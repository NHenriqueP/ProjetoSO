# Orquestração de um ambiente multi-runner
 
Projeto da unidade curricular **Sistemas Operativos** (Licenciatura em Engenharia Informática, Universidade do Minho, 2025/2026).
 
Sistema em C para Linux que coordena a execução de comandos de vários utilizadores através de um processo central. Cada utilizador usa um **runner** para pedir a execução de um comando; o **controller** recebe os pedidos de vários runners em simultâneo, escalona-os segundo uma política configurável e autoriza cada execução quando houver capacidade disponível.
 
Toda a comunicação entre processos é feita com **named pipes (FIFOs)** e as primitivas de sistema POSIX (`fork`, `execvp`, `pipe`, `dup2`, `open`, `read`, `write`), sem recurso a `system()` nem a um interpretador de comandos.
 
## Arquitetura
 
```
 runner (user 1) ──┐                         ┌── FIFO privado ──> runner (user 1)
 runner (user 2) ──┼──> FIFO público ──> controller ──┼── FIFO privado ──> runner (user 2)
 runner (user N) ──┘   /tmp/so_controller_fifo         └── FIFO privado ──> runner (user N)
```
 
- **Controller**: escalona os pedidos, mantém a lista de comandos em execução e em espera, controla o limite de paralelismo e regista cada execução em ficheiro. Não executa os comandos.
- **Runner**: cria um FIFO privado para receber respostas, submete o pedido, espera pela autorização, executa o comando e notifica o controller quando termina.
- **Protocolo** (`include/protocol.h`): mensagens tipadas (`SUBMIT`, `START`, `FINISH`, `STATUS_REQ`, `STATUS_RESP`, `SHUTDOWN_REQ`, `SHUTDOWN_ACK`, `ERROR`) trocadas como estruturas binárias de tamanho fixo.
## Funcionalidades
 
- Execução de comandos com autorização prévia do controller.
- Consulta dos comandos em execução e em espera, pela ordem de escalonamento.
- Execução de **vários comandos em paralelo**, com limite definido no arranque.
- **Três políticas de escalonamento**:
  - `FCFS`: por ordem de chegada.
  - `RR_USER`: round-robin entre utilizadores, para evitar que um utilizador monopolize o sistema.
  - `SJF_PROXY`: dá prioridade aos comandos com menor duração estimada, com base no histórico de execuções anteriores.
- Suporte aos operadores **`|`, `>`, `<` e `2>`**, incluindo pipelines combinadas com redirecionamentos.
- **Encerramento gracioso**: o controller só termina depois de todos os comandos em curso acabarem.
- Registo persistente de cada execução (`tmp/executions.log`) com utilizador, comando, tempo de espera e duração total, e registo de eventos (`tmp/events.log`).
- Tratamento defensivo de erros, incluindo `SIGPIPE` quando um runner desaparece.
## Compilar
 
Requisitos: Linux, `gcc` e `make`.
 
```bash
make        # compila para bin/ e cria atalhos ./controller e ./runner
make clean  # limpa binários e ficheiros temporários
```
 
O código compila com `-Wall -Wextra -Werror -std=c11`.
 
## Utilização
 
**Iniciar o controller:**
 
```bash
./controller <parallel-commands> <sched-policy>
 
# Exemplo: até 2 comandos em paralelo, round-robin entre utilizadores
./controller 2 RR_USER
```
 
**Executar um comando** (noutro terminal):
 
```bash
$ ./runner -e 1 "echo hello"
[runner] command 98301 submitted
[runner] executing command 98301...
hello
[runner] command 98301 finished
 
$ ./runner -e 1 "grep system /etc/passwd | wc -l > out.txt"
```
 
**Consultar o estado:**
 
```bash
$ ./runner -c
---
Executing
user-id 1 - command-id 98301
---
Scheduled
user-id 4 - command-id 97650
user-id 1 - command-id 98720
```
 
**Encerrar o controller:**
 
```bash
$ ./runner -s
[runner] sent shutdown notification
[runner] waiting for controller to shutdown...
[runner] controller exited.
```
 
## Testes e avaliação experimental
 
**Testes funcionais** (execução básica, consulta, shutdown, redirecionamentos, pipes, paralelismo e justiça entre utilizadores):
 
```bash
./scripts/test_all.sh
```
 
**Comparação de políticas de escalonamento**, com vários runners concorrentes e diferentes níveis de paralelismo:
 
```bash
# Uma política, com N comandos em paralelo e R repetições
./scripts/run_policy_experiment.sh <FCFS|RR_USER|SJF_PROXY> [parallel] [repetições]
 
# Todas as políticas e níveis de paralelismo
./scripts/run_all_experiments.sh [repetições]
```
 
Os resultados são guardados em `tmp/experiment_results.csv` e resumidos em `tmp/experiment_summary.txt`, com tempo médio de espera, tempo médio total e diferença de tempos entre utilizadores (métrica de justiça).
 
## Estrutura
 
```
.
├── include/protocol.h   # Protocolo de mensagens runner <-> controller
├── src/controller.c     # Escalonamento, estado e registo de execuções
├── src/runner.c         # Submissão, execução de pipelines e redirecionamentos
├── scripts/             # Testes funcionais e avaliação de políticas
├── tests/               # Scripts de teste auxiliares
├── docs/                # Documentação interna
└── Makefile
```
 
## Tecnologias
 
C · Linux · POSIX (FIFOs, fork/exec, pipes, dup2) · Bash · AWK · Makefile · Git
 
## Autores
 
- João Pedro Cunha Cerqueira
- Rafael Figueiras Esteves
- Nuno Henrique Dias Pereira
 
