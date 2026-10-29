

## **Arquivos de dados** 

| Arquivo | Chave (única) | Campos repetidos | Registro |
| ----- | ----- | ----- | ----- |
| produtos.dat | product\_id | marca, categoria, preço | 78 bytes: chave(8) ativo(1) marca(20) categoria(40) preço(8, double) \\n(1) |
| acessos.dat | id sequencial gerado na carga original | tipo de evento, product\_id, user\_id | 101 bytes: chave(8) ativo(1) product\_id(8) user\_id(8) tipo(16) tempo(23) sessão(36) \\n(1) |

## **Arquivos de índice parcial – 2 níveis**

- ### produtos.idx, acessos.idx

Layout: \[cabeçalho\]\[entradas nível 2\]\[entradas nível 1\]; entrada \= {chave, posição} (16 bytes).

* Nível 1: 1 entrada a cada P1 \= 100 registros do arquivo de dados (posição \= nº do registro).  
* Nível 2: 1 entrada a cada P2 \= 50 entradas do nível 1, ou seja, a cada 5.000 registros (posição \= nº da entrada no nível 1).  
* Cabeçalho: nº de registros do arquivo de dados na criação (usado para detectar índice desatualizado), tamanhos dos níveis e passos.

Consulta: Começa fazendo a busca binária no nível 2\. Após encontrar a parte correspondente, faz busca binária no trecho (≤ 50 entradas) do nível 1\. Usa fseek no arquivo de dados, com varredura de no máximo 100 registros. Nada é carregado inteiro em RAM. O índice é gravado em arquivo, é verificado e reconstruído se faltar ou estiver desatualizado.

## **Inserção e remoção**

* **Inserção:** busca binária acha a posição; os registros seguintes são deslocados uma posição dentro do arquivo (em blocos de 1024); o índice é reconstruído a cada inserção, pois as posições mudam. Se a chave existe e está removida logicamente, o registro é reativado.  
* **Remoção:** tem uma variável de controle, que guarda um limite de remoções lógicas que podem ocorrer. Vai remover logicamente (ativo \= 0), mantendo ordenação, até atingir esse limite. Após, ocorre a compactação física e a reconstrução do índice. Essa reconstrução também pode ser feita através do menu com uma opção própria, caso queira reconstruir antes desse limite de remoções lógicas.

## **Perguntas (consultas)**

1. Qual é a marca mais vendida? Uma marca tem mais de 50% das compras? (opção 8; usa o índice de produtos para cada compra)  
2. Qual a distribuição dos tipos de evento e a taxa de conversão view → purchase? (opção 9\)  
3. Depois de inserir um product\_id inédito e atualizar o índice, o sistema consegue localizá-lo? Para essa pergunta, temos duas hipóteses:   
   1. A estratégia de inserção mantém a integridade do arquivo binário de tamanho fixo.   
   2. A reconstrução do índice remapeia os intervalos sem corromper as consultas. 

