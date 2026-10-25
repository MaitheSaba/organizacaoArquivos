/*
Arquivos de dados (binarios, registros de tamanho fixo, ordenados pela chave):
    produtos.dat : chave = product_id (unica)       -> marca/categoria repetem
    acessos.dat  : chave = id sequencial (unica)    -> tipo de evento repete

Indices parciais (binarios, 2 niveis): produtos.idx e acessos.idx
*/
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>

// Forca o compilador a alinhar os membros em limites de 1 byte, colando um membro logo após o outro sem deixar espaços
#pragma pack(push, 1)

typedef long long ll;

typedef struct {
    ll     chave;
    char   ativo;
    char   marca[20];
    char   categoria[40];
    double preco;
    char   nl;
} Produto;

typedef struct {
    ll   chave;
    char ativo;
    ll   product_id;
    ll   user_id;
    char tipo[16];
    char tempo[23];
    char sessao[36];
    char nl;
} Acesso;

typedef struct {
    ll n_regs;
    ll n1, n2;
    int p1, p2;
} IdxCab;

typedef struct {
    ll chave;
    ll pos;
} IdxEnt;

// Restaura o alinhamento "padrão", para que o que está depois das structs não seja afetado
#pragma pack(pop)

/* Parametros do indice parcial:
   nivel 1 = 1 entrada a cada P1 registros do arquivo de dados
   nivel 2 = 1 entrada a cada P2 entradas do nivel 1 (ou seja, a cada P1*P2 registros) */
#define P1 100
#define P2 50
#define MAXREG 128
#define LIMITE_COMPACTA 10   /* remocoes logicas pendentes que disparam compactacao */
#define CHUNK 100000         /* produtos ordenados em RAM por vez na carga (ordenacao externa) */

/* Descritor generico de um arquivo de dados + indice */
typedef struct {
    const char *dados, *indice;
    size_t tam;
    void (*imprimir)(const void *);
} Arq;

static void imp_produto(const void *p);
static void imp_acesso(const void *p);
static const Arq ARQ_PROD = {"produtos.dat", "produtos.idx", sizeof(Produto), imp_produto};
static const Arq ARQ_ACES = {"acessos.dat",  "acessos.idx",  sizeof(Acesso),  imp_acesso};
static int pendentes[2];
static int id_arq(const Arq *a) {
    return a == &ARQ_PROD ? 0 : 1;
}

//Funcoes auxiliares
//Identifica qual a chave nos primeiros bytes do arquivo
static ll chave_de(const void *p) {
    ll k;
    memcpy(&k, p, sizeof k);
    return k;
}
//Identifica se está ativo ou inativo (se foi removido)
static char ativo_de(const void *p) {
    return ((const char *)p)[sizeof(ll)];
}

static void set_ativo(void *p, char v) {
    ((char *)p)[sizeof(ll)] = v;
}

//Copia texto para campo de tamanho fixo, completando com espacos (garante o tamanho fixo)
static void copiar_campo(char *dst, size_t n, const char *src) {
    size_t l = strlen(src);
    memset(dst, ' ', n);
    memcpy(dst, src, l < n ? l : n);
}

//Compara campo de tamanho fixo, com espacos a direita, com string
static int campo_eh(const char *campo, size_t n, const char *alvo) {
    size_t l = strlen(alvo);
    return l <= n && strncmp(campo, alvo, l) == 0 && (l == n || campo[l] == ' ');
}

//Tamanho do arquivo dividido pelo tamanho do registro
static ll num_regs(const Arq *a) {
    FILE *f = fopen(a->dados, "rb");
    if (!f){
        return 0;
    }
    fseeko(f, 0, SEEK_END);
    ll n = (ll)(ftello(f) / (off_t)a->tam);
    fclose(f);
    return n;
}

//Acesso aleatorio, nao sequencial
static int ler_reg(FILE *f, const Arq *a, ll i, void *buf) {
    if (fseeko(f, (off_t)i * (off_t)a->tam, SEEK_SET)){
        return 0;
    }
    return fread(buf, a->tam, 1, f) == 1;
}

// imp_produto e imp_acesso cortam ou completam a saida para ficar do tamanho certo
static void imp_produto(const void *p) {
    const Produto *x = p;
    printf("%-10lld | %-20.20s | %-40.40s | %10.2f\n", x->chave, x->marca, x->categoria, x->preco);
}
static void imp_acesso(const void *p) {
    const Acesso *x = p;
    printf("%-10lld | prod %-9lld | user %-10lld | %-16.16s | %-23.23s | %.36s\n",
           x->chave, x->product_id, x->user_id, x->tipo, x->tempo, x->sessao);
}

//pesquisa binaria direto no arquivo de dados (*comp recebe o numero de arquivos lidos)
static int busca_binaria(const Arq *a, ll chave, ll *pos, void *buf, ll *comp) {
    ll c = 0;
    FILE *f = fopen(a->dados, "rb");
    if (!f) {
        *pos = 0;
        if (comp) {
            *comp = 0;
        }
        return 0;
    }
    fseeko(f, 0, SEEK_END);
    ll lo = 0, hi = (ll)(ftello(f) / (off_t)a->tam) - 1;
    while (lo <= hi) {
        ll mid = lo + (hi - lo) / 2;
        if (!ler_reg(f, a, mid, buf)){
            break;
        }
        c++;
        ll k = chave_de(buf);
        if (k == chave) {
            *pos = mid;
            fclose(f);
            if (comp){
                *comp = c;
            }
            return 1;
        }
        if (k < chave) {
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    *pos = lo;
    fclose(f);
    if (comp){
        *comp = c;
    }
    return 0;
}


// Indice parcial em arquivo (2 niveis)
// Layout: [IdxCab][n2 entradas nivel 2][n1 entradas nivel 1]
static int indice_construir(const Arq *a) {
    FILE *d = fopen(a->dados, "rb");
    if (!d){
        return 0;
    }
    fseeko(d, 0, SEEK_END);
    ll n = (ll)(ftello(d) / (off_t)a->tam);
    IdxCab c = {n, (n + P1 - 1) / P1, 0, P1, P2};
    c.n2 = (c.n1 + P2 - 1) / P2;
    FILE *x = fopen(a->indice, "wb");
    if (!x) {
        fclose(d);
        return 0;
    }
    fwrite(&c, sizeof c, 1, x);
    char buf[MAXREG];
    for (ll i = 0; i < c.n2; i++) {                 /* nivel 2 */
        ll e1 = i * P2;
        ler_reg(d, a, e1 * P1, buf);
        IdxEnt e = {chave_de(buf), e1};
        fwrite(&e, sizeof e, 1, x);
    }
    for (ll i = 0; i < c.n1; i++) {                 /* nivel 1 */
        ler_reg(d, a, i * P1, buf);
        IdxEnt e = {chave_de(buf), i * P1};
        fwrite(&e, sizeof e, 1, x);
    }
    fclose(d);
    fclose(x);
    return 1;
}

// Carrega o indice ao abrir o programa: se nao existe ou esta desatualizado, reconstroi
static void indice_garantir(const Arq *a) {
    ll n = num_regs(a);
    if (n == 0){
       return;
    }
    IdxCab c;
    FILE *x = fopen(a->indice, "rb");
    int ok = x && fread(&c, sizeof c, 1, x) == 1 && c.n_regs == n && c.p1 == P1 && c.p2 == P2;
    if (x){
        fclose(x);
    }
    if (!ok) {
        printf("[indice] (re)construindo %s ...\n", a->indice);
        indice_construir(a);
    }
}

/* Consulta usando o indice parcial
   1) busca binaria no nivel 2 do indice (fseek no arquivo .idx)
   2) busca binaria no trecho correspondente do nivel 1
   3) fseek no arquivo de dados e varredura de no maximo P1 registros */
static int busca_indice(const Arq *a, ll chave, void *buf, ll *pos, ll *ci, ll *cd) {
    *ci = *cd = 0;
    IdxCab c;
    FILE *x = fopen(a->indice, "rb");
    if (!x) {
        return 0;
    }
    if (fread(&c, sizeof c, 1, x) != 1 || c.n1 == 0) {
        fclose(x);
        return 0;
    }
    off_t off2 = sizeof c, off1 = off2 + (off_t)c.n2 * (off_t)sizeof(IdxEnt);
    IdxEnt e, e2 = {0, -1}, e1 = {0, -1};

    ll lo = 0, hi = c.n2 - 1;
    while (lo <= hi) {
        ll mid = lo + (hi - lo) / 2;
        fseeko(x, off2 + (off_t)mid * (off_t)sizeof e, SEEK_SET);
        if (fread(&e, sizeof e, 1, x) != 1){
            break;
        }
        (*ci)++;
        if (e.chave <= chave) {
            e2 = e;
            lo = mid + 1;
        }
        else{
            hi = mid - 1;
        }
    }
    if (e2.pos < 0) {
        fclose(x);
        return 0;
    }

    lo = e2.pos;
    hi = lo + c.p2 - 1;
    if (hi >= c.n1){
        hi = c.n1 - 1;
    }
    while (lo <= hi) {
        ll mid = lo + (hi - lo) / 2;
        fseeko(x, off1 + (off_t)mid * (off_t)sizeof e, SEEK_SET);
        if (fread(&e, sizeof e, 1, x) != 1){
            break;
        }
        (*ci)++;
        if (e.chave <= chave) {
            e1 = e;
            lo = mid + 1;
        }
        else{
            hi = mid - 1;
        }
    }
    fclose(x);
    if (e1.pos < 0){
        return 0;
    }

    FILE *d = fopen(a->dados, "rb");
    if (!d){
        return 0;
    }
    fseeko(d, (off_t)e1.pos * (off_t)a->tam, SEEK_SET);
    for (ll i = 0; i < c.p1 && e1.pos + i < c.n_regs; i++) {
        if (fread(buf, a->tam, 1, d) != 1){
            break;
        }
        (*cd)++;
        ll k = chave_de(buf);
        if (k == chave) {
            *pos = e1.pos + i;
            fclose(d);
            return 1;
        }
        if (k > chave){
            break;
        }
    }
    fclose(d);
    return 0;
}


// Mostrar dados (parcial ou total)
static void mostrar(const Arq *a, ll ini, ll qtd) {
    FILE *f = fopen(a->dados, "rb");
    if (!f) {
        printf("Arquivo %s nao existe.\n", a->dados);
        return;
    }
    char buf[MAXREG];
    ll n = num_regs(a), shown = 0;
    printf("%s: %lld registros (incluindo removidos logicamente)\n", a->dados, n);
    fseeko(f, (off_t)ini * (off_t)a->tam, SEEK_SET);
    for (ll i = ini; i < n && (qtd < 0 || shown < qtd); i++) {
        if (fread(buf, a->tam, 1, f) != 1){
           break;
        }
        if (!ativo_de(buf)){
            continue;
        }
        printf("[%lld] ", i);
        a->imprimir(buf);
        shown++;
    }
    fclose(f);
}

/* Inserir registros
   Regras:
    - chave ja existente e ativa: recusa
    - chave existente e removida: reativa o registro (indice igual)
    - senao:
        acha a posicao por busca binaria, desloca os registros seguintes uma posicao (em blocos, dentro do proprio arquivo) e grava,
        o indice e reconstruido a cada insercao (as posicoes mudaram) */
static int inserir(const Arq *a, const void *reg) {
    char buf[MAXREG];
    ll pos, comp;
    if (busca_binaria(a, chave_de(reg), &pos, buf, &comp)) {
        if (ativo_de(buf)) {
            printf("Chave %lld ja existe.\n", chave_de(reg));
            return 0;
        }
        FILE *f = fopen(a->dados, "rb+");
        fseeko(f, (off_t)pos * (off_t)a->tam, SEEK_SET);
        fwrite(reg, a->tam, 1, f);
        fclose(f);
        if (pendentes[id_arq(a)] > 0){
            pendentes[id_arq(a)]--;
        }
        printf("Registro removido anteriormente foi reativado.\n");
        return 1;
    }
    FILE *f = fopen(a->dados, "rb+");
    if (!f){
        f = fopen(a->dados, "wb+");
    }
    if (!f) {
        perror("fopen");
        return 0;
    }
    fseeko(f, 0, SEEK_END);
    ll n = (ll)(ftello(f) / (off_t)a->tam);
    const ll B = 1024;
    char *blk = malloc((size_t)B * a->tam);
    ll fim = n;
    while (fim > pos) {
        ll ini = fim - B < pos ? pos : fim - B;
        ll cnt = fim - ini;
        fseeko(f, (off_t)ini * (off_t)a->tam, SEEK_SET);
        if (fread(blk, a->tam, (size_t)cnt, f) != (size_t)cnt){
            break;
        }
        fseeko(f, (off_t)(ini + 1) * (off_t)a->tam, SEEK_SET);
        fwrite(blk, a->tam, (size_t)cnt, f);
        fim = ini;
    }
    free(blk);
    fseeko(f, (off_t)pos * (off_t)a->tam, SEEK_SET);
    fwrite(reg, a->tam, 1, f);
    fclose(f);
    indice_construir(a);
    printf("Inserido na posicao %lld (indice reconstruido).\n", pos);
    return 1;
}

// Compactacao: elimina fisicamente os removidos e reconstroi o indice
static void compactar(const Arq *a) {
    char tmp[256], buf[MAXREG];
    snprintf(tmp, sizeof tmp, "%s.tmp", a->dados);
    FILE *d = fopen(a->dados, "rb"), *t = fopen(tmp, "wb");
    if (!d || !t) {
        if (d){
            fclose(d);
        }
        if (t){
            fclose(t);
        }
        return;
    }
    ll mantidos = 0, descartados = 0;
    while (fread(buf, a->tam, 1, d) == 1) {
        if (ativo_de(buf)) {
            fwrite(buf, a->tam, 1, t);
            mantidos++;
        } else {
            descartados++;
        }
    }
    fclose(d);
    fclose(t);
    remove(a->dados);
    rename(tmp, a->dados);
    pendentes[id_arq(a)] = 0;
    indice_construir(a);
    printf("Compactacao: %lld registros mantidos, %lld eliminados. Indice reconstruido.\n", mantidos, descartados);
}

// Remover (remocao LOGICA). Ao acumular LIMITE_COMPACTA remocoes, faz a remocao fisica (compactacao) e reconstroi o indice.
static int remover(const Arq *a, ll chave) {
    char buf[MAXREG];
    ll pos, comp;
    if (!busca_binaria(a, chave, &pos, buf, &comp) || !ativo_de(buf)) {
        printf("Chave %lld nao encontrada.\n", chave);
        return 0;
    }
    set_ativo(buf, 0);
    FILE *f = fopen(a->dados, "rb+");
    fseeko(f, (off_t)pos * (off_t)a->tam, SEEK_SET);
    fwrite(buf, a->tam, 1, f);
    fclose(f);
    printf("Registro %lld removido logicamente (posicao %lld).\n", chave, pos);
    if (++pendentes[id_arq(a)] >= LIMITE_COMPACTA){
        compactar(a);
    }
    return 1;
}

/* Carga inicial a partir do CSV do Kaggle
   - acessos.dat: gravado direto, chave sequencial (ja ordenada)
   - produtos.dat: ordenacao externa (runs ordenadas + merge k-vias com eliminacao de duplicatas), sem manter tudo em RAM */
static int split_csv(char *s, char **c, int max) {
    int n = 0;
    c[n++] = s;
    for (; *s; s++) {
        if (*s == ',') {
            *s = 0;
            if (n < max) {
                c[n++] = s + 1;
            }
        }
        else if (*s == '\n' || *s == '\r') {
            *s = 0;
            break;
        }
    }
    return n;
}

static int cmp_prod(const void *x, const void *y) {
    ll a = chave_de(x), b = chave_de(y);
    return (a > b) - (a < b);
}

static int gravar_run(Produto *v, size_t n, int run) {
    qsort(v, n, sizeof *v, cmp_prod);
    size_t m = 0;
    for (size_t i = 0; i < n; i++){
        if (m == 0 || v[i].chave != v[m - 1].chave){
            v[m++] = v[i];
        }
    }
    char nome[64];
    snprintf(nome, sizeof nome, "run_%d.tmp", run);
    FILE *f = fopen(nome, "wb");
    if (!f){
        return 0;
    }
    fwrite(v, sizeof *v, m, f);
    fclose(f);
    return 1;
}

static void merge_runs(int k, const char *saida) {
    FILE **r = malloc(k * sizeof *r);
    Produto *cur = malloc(k * sizeof *cur);
    int *vivo = malloc(k * sizeof *vivo);
    char nome[64];
    for (int i = 0; i < k; i++) {
        snprintf(nome, sizeof nome, "run_%d.tmp", i);
        r[i] = fopen(nome, "rb");
        vivo[i] = r[i] && fread(&cur[i], sizeof *cur, 1, r[i]) == 1;
    }
    FILE *out = fopen(saida, "wb");
    ll ultima = LLONG_MIN, total = 0;
    for (;;) {
        int m = -1;
        for (int i = 0; i < k; i++){
            if (vivo[i] && (m < 0 || cur[i].chave < cur[m].chave)){
                m = i;
            }
        }
        if (m < 0){
            break;
        }
        if (cur[m].chave != ultima) {
            fwrite(&cur[m], sizeof *cur, 1, out);
            ultima = cur[m].chave;
            total++;
        }
        vivo[m] = fread(&cur[m], sizeof *cur, 1, r[m]) == 1;
    }
    fclose(out);
    for (int i = 0; i < k; i++) {
        if (r[i]){
            fclose(r[i]);
        }
        snprintf(nome, sizeof nome, "run_%d.tmp", i);
        remove(nome);
    }
    free(r);
    free(cur);
    free(vivo);
    printf("produtos.dat: %lld produtos distintos.\n", total);
}

static void carga_inicial(const char *csv, ll limite) {
    char linha[4096], *c[9];
    ll seq = 0;
    size_t produtosChunkCont = 0;
    int runs = 0;

    FILE *in = fopen(csv, "r");
    if (!in) {
        perror(csv);
        return;
    }
    FILE *fa = fopen(ARQ_ACES.dados, "wb");
    Produto *chunk = malloc(CHUNK * sizeof *chunk);

    while (fgets(linha, sizeof linha, in) && (limite <= 0 || seq < limite)) {
        if (!isdigit((unsigned char)linha[0])) {
            continue;
        }
        if (split_csv(linha, c, 9) < 9){
            continue;
        }

        Acesso a;
        memset(&a, 0, sizeof a);

        a.chave = ++seq; a.ativo = 1;
        a.product_id = atoll(c[2]);
        a.user_id = atoll(c[7]);

        copiar_campo(a.tipo, sizeof a.tipo, c[1]);
        copiar_campo(a.tempo, sizeof a.tempo, c[0]);
        copiar_campo(a.sessao, sizeof a.sessao, c[8]);

        a.nl = '\n';
        fwrite(&a, sizeof a, 1, fa);

        Produto p;
        memset(&p, 0, sizeof p);
        p.chave = a.product_id; p.ativo = 1;
        copiar_campo(p.marca, sizeof p.marca, c[5][0] ? c[5] : "desconhecida");
        copiar_campo(p.categoria, sizeof p.categoria, c[4][0] ? c[4] : "desconhecida");
        p.preco = atof(c[6]);
        p.nl = '\n';
        chunk[produtosChunkCont++] = p;

        if (produtosChunkCont == CHUNK) {
            gravar_run(chunk, produtosChunkCont, runs++);
            produtosChunkCont = 0;
        }
        if (seq % 1000000 == 0){
            printf("  %lld linhas processadas...\n", seq);
        }
    }
    if (produtosChunkCont){
       gravar_run(chunk, produtosChunkCont, runs++);
    }
    fclose(in);
    fclose(fa);
    free(chunk);

    printf("acessos.dat: %lld registros.\n", seq);
    merge_runs(runs, ARQ_PROD.dados);
    pendentes[0] = pendentes[1] = 0;
    indice_construir(&ARQ_PROD);
    indice_construir(&ARQ_ACES);
    printf("Indices gerados: %s e %s.\n", ARQ_PROD.indice, ARQ_ACES.indice);
}

/* Consultas (perguntas) sobre os dados
   P1: Qual a marca mais vendida? Ela tem mais de 50% das vendas?
   P2: Qual a distribuicao dos tipos de evento (view/cart/purchase)?
   Leitura em blocos de 4096 registros; produto pesquisado via indice */
typedef struct {
    char nome[21];
    ll compras;
    double receita;
} MarcaCnt;
#define MAXM 2000
static MarcaCnt marcas[MAXM];

static int cmp_marca(const void *x, const void *y) {
    const MarcaCnt *a = x, *b = y;
    return (b->compras > a->compras) - (b->compras < a->compras);
}

static void consulta_marcas(void) {
    FILE *f = fopen(ARQ_ACES.dados, "rb");
    if (!f) {
        printf("Execute a carga inicial primeiro.\n");
        return;
    }
    enum { BLK = 4096 };
    Acesso *blk = malloc(BLK * sizeof *blk);
    int nm = 0;
    ll total = 0, nao_ach = 0, ci, cd, pos;
    double receita = 0;
    size_t lidos;
    while ((lidos = fread(blk, sizeof *blk, BLK, f)) > 0) {
        for (size_t i = 0; i < lidos; i++) {
            if (!blk[i].ativo || !campo_eh(blk[i].tipo, sizeof blk[i].tipo, "purchase")){
                continue;
            }
            Produto p;
            if (!busca_indice(&ARQ_PROD, blk[i].product_id, &p, &pos, &ci, &cd) || !p.ativo) {
                nao_ach++;
                continue;
            }
            char nome[21];
            memcpy(nome, p.marca, 20); nome[20] = 0;
            for (int j = 19; j >= 0 && nome[j] == ' '; j--){
                nome[j] = 0;
            }
            int j;
            for (j = 0; j < nm; j++){
                if (strcmp(marcas[j].nome, nome) == 0){
                    break;
                }
            }
            if (j == nm) {
                if (nm == MAXM){
                    continue;
                }
                strcpy(marcas[nm].nome, nome);
                marcas[nm].compras = 0;
                marcas[nm].receita = 0;
                nm++;
            }
            marcas[j].compras++;
            marcas[j].receita += p.preco;
            total++;
            receita += p.preco;
        }
    }
    fclose(f);
    free(blk);
    if (!total) {
        printf("Nenhuma compra encontrada.\n");
        return;
    }
    qsort(marcas, nm, sizeof *marcas, cmp_marca);
    printf("\nMarcas mais vendidas (compras: %lld, receita total: %.2f)\n", total, receita);
    for (int i = 0; i < nm && i < 5; i++){
        printf("%d. %-20s %8lld compras (%5.1f%%)  receita %.2f\n", i + 1, marcas[i].nome,marcas[i].compras, 100.0 * marcas[i].compras / total, marcas[i].receita);
    }
    double pct = 100.0 * marcas[0].compras / total;
    printf("Hipotese 'uma marca lidera com mais de 50%%': %s (lider: %.1f%%)\n", pct > 50 ? "CONFIRMADA" : "REFUTADA", pct);
    if (nao_ach){
        printf("(%lld compras ignoradas: produto nao encontrado)\n", nao_ach);
    }
}

static void consulta_eventos(void) {
    FILE *f = fopen(ARQ_ACES.dados, "rb");
    if (!f) {
        printf("Execute a carga inicial primeiro.\n");
        return;
    }
    const char *tipos[] = {"view", "cart", "remove_from_cart", "purchase"};
    ll cont[5] = {0}, total = 0;
    enum { BLK = 4096 };
    Acesso *blk = malloc(BLK * sizeof *blk);
    size_t lidos;
    while ((lidos = fread(blk, sizeof *blk, BLK, f)) > 0){
        for (size_t i = 0; i < lidos; i++) {
            if (!blk[i].ativo){
                continue;
            }
            int t;
            for (t = 0; t < 4; t++){
                if (campo_eh(blk[i].tipo, sizeof blk[i].tipo, tipos[t])){
                    break;
                }
            }
            cont[t]++; total++;
        }
    }
    fclose(f); free(blk);
    printf("\n Distribuicao dos eventos (%lld) \n", total);
    for (int t = 0; t < 4; t++){
        printf("%-18s %10lld (%5.2f%%)\n", tipos[t], cont[t], total ? 100.0 * cont[t] / total : 0.0);
    }
    if (cont[0]){
        printf("Taxa de conversao view -> purchase: %.2f%%\n", 100.0 * cont[3] / cont[0]);
    }
}

static void encerrar(int code) {
    indice_garantir(&ARQ_PROD);
    indice_garantir(&ARQ_ACES);
    printf("Indices salvos. Ate logo.\n");
    exit(code);
}
static void ler_linha(const char *prompt, char *dst, size_t n) {
    printf("%s", prompt);
    if (!fgets(dst, (int)n, stdin)){
        encerrar(0);
    }
    dst[strcspn(dst, "\r\n")] = 0;
}
static ll ler_ll(const char *prompt) {
    char l[128];
    ler_linha(prompt, l, sizeof l);
    return strtoll(l, NULL, 10);
}
static const Arq *escolher(void) {
    ll o = ler_ll("Arquivo (1 = produtos, 2 = acessos): ");
    return o == 1 ? &ARQ_PROD : o == 2 ? &ARQ_ACES : NULL;
}

static void menu_inserir(const Arq *a) {
    char s[128];
    if (a == &ARQ_PROD) {
        Produto p;
        memset(&p, 0, sizeof p);
        p.chave = ler_ll("product_id: ");
        p.ativo = 1;
        p.nl = '\n';
        ler_linha("marca: ", s, sizeof s);
        copiar_campo(p.marca, sizeof p.marca, s);
        ler_linha("categoria: ", s, sizeof s);
        copiar_campo(p.categoria, sizeof p.categoria, s);
        ler_linha("preco: ", s, sizeof s);
        p.preco = atof(s);
        inserir(a, &p);
    } else {
        Acesso x;
        memset(&x, 0, sizeof x);
        x.chave = ler_ll("chave (0 = proxima sequencial): ");
        if (x.chave == 0){
           x.chave = num_regs(a) + 1;
        }
        x.ativo = 1;
        x.nl = '\n';
        x.product_id = ler_ll("product_id: ");
        x.user_id = ler_ll("user_id: ");
        ler_linha("tipo (view/cart/remove_from_cart/purchase): ", s, sizeof s);
        copiar_campo(x.tipo, sizeof x.tipo, s);
        ler_linha("data/hora (2019-11-01 00:00:02 UTC): ", s, sizeof s);
        copiar_campo(x.tempo, sizeof x.tempo, s);
        ler_linha("user_session: ", s, sizeof s);
        copiar_campo(x.sessao, sizeof x.sessao, s);
        inserir(a, &x);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "carga") == 0) {
        carga_inicial(argv[2], argc > 3 ? atoll(argv[3]) : 0);
        return 0;
    }
    indice_garantir(&ARQ_PROD);
    indice_garantir(&ARQ_ACES);
    for (;;) {
        printf("\n MENU \n"
               "1) Carga inicial (CSV)      2) Mostrar dados\n"
               "3) Pesquisa binaria         4) Consulta pelo indice\n"
               "5) Inserir registro         6) Remover registro\n"
               "7) Compactar + reconstruir indice\n"
               "8) Marca mais vendida       9) Distribuicao de eventos\n"
               "0) Sair\n");
        ll op = ler_ll("Escolha uma opcao: ");
        const Arq *a;
        char buf[MAXREG], s[256];
        ll pos, c1, c2, k;
        switch (op) {
        case 0:
            encerrar(0);
            break;
        case 1:
            ler_linha("Arquivo CSV: ", s, sizeof s);
            carga_inicial(s, ler_ll("Limite de linhas (0 = todas): "));
            break;
        case 2:
            if ((a = escolher())) {
                ll ini = ler_ll("Registro inicial: ");
                mostrar(a, ini, ler_ll("Quantidade (-1 = todos): "));
            }
            break;
        case 3:
            if ((a = escolher())) {
                k = ler_ll("Chave: ");
                if (busca_binaria(a, k, &pos, buf, &c1) && ativo_de(buf)) {
                    printf("Achou na posicao %lld (%lld leituras):\n", pos, c1);
                    a->imprimir(buf);
                }
                else {
                    printf("Nao encontrado (%lld leituras).\n", c1);
                }
            }
            break;
        case 4:
            if ((a = escolher())) {
                k = ler_ll("Chave: ");
                if (busca_indice(a, k, buf, &pos, &c1, &c2) && ativo_de(buf)) {
                    printf("Achou na posicao %lld (%lld comparacoes no indice, %lld registros lidos nos dados):\n", pos, c1, c2);
                    a->imprimir(buf);
                } else {
                    printf("Nao encontrado (%lld comparacoes no indice, %lld registros lidos).\n", c1, c2);
                }
            }
            break;
        case 5:
            if ((a = escolher())){
              menu_inserir(a);
              break;
            }
        case 6:
            if ((a = escolher())){
                remover(a, ler_ll("Chave a remover: "));
                break;
            }
        case 7:
            if ((a = escolher())){
                compactar(a);
                break;
            }
        case 8:
            consulta_marcas();
            break;
        case 9:
            consulta_eventos();
            break;
        default: printf("Opcao invalida.\n");
        }
    }
}
