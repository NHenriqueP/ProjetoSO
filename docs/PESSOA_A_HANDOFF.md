# Pessoa A - Controller Handoff

## Estado atual

Controller funcional com:
- FIFO publico do controller (`/tmp/so_controller_fifo`)
- Escalonamento com duas politicas: `FCFS` e `RR_USER`
- Limite de paralelismo (`parallel-commands`)
- Pedido de estado (`MSG_STATUS_REQ`)
- Shutdown gracioso (`MSG_SHUTDOWN_REQ`) com ACK apenas no encerramento real
- Registo persistente de execucoes em `tmp/executions.log`
- Validacao de politica no arranque (aceita apenas `FCFS` e `RR_USER`)
- Tratamento defensivo de `SIGPIPE` no envio de respostas

## Protocolo IPC

Definido em `include/protocol.h`.

Mensagens de pedido (`message_t`):
- `MSG_SUBMIT`
- `MSG_FINISH`
- `MSG_STATUS_REQ`
- `MSG_SHUTDOWN_REQ`

Mensagens de resposta (`response_t`):
- `MSG_START`
- `MSG_STATUS_RESP`
- `MSG_SHUTDOWN_ACK`
- `MSG_ERROR`

## Formato de log

Ficheiro: `tmp/executions.log`

Cada linha:
`user_id,command_id,submit_ms,start_ms,end_ms,wait_ms,total_ms`

Onde:
- `submit_ms`, `start_ms`, `end_ms` sao epoch em milissegundos
- `wait_ms = start - submit`
- `total_ms = end - submit`

## Teste rapido (sem runner final)

Compilar:
`make && make fake_runner`

Executar controller:
`./bin/controller 1 FCFS`

Noutro terminal:
`./bin/fake_runner submit 1 1001 2`
`./bin/fake_runner submit 2 1002 0`
`./bin/fake_runner status`
`./bin/fake_runner shutdown`

## Experimentos automáticos

Script principal:
`./scripts/run_policy_experiment.sh <FCFS|RR_USER> [parallel-commands]`

Exemplos:
`./scripts/run_policy_experiment.sh FCFS 1`
`./scripts/run_policy_experiment.sh RR_USER 1`

Resumo de metricas (espera e turnaround) e gap de justica:
`awk -f scripts/summarize_executions.awk tmp/executions.log`

Nota: `fairness_wait_gap_ms` mais baixo tende a indicar melhor justica entre utilizadores.

## Integracao com runner real

Requisitos para a Pessoa B:
- Criar FIFO privada por processo runner
- Enviar `MSG_SUBMIT` com `reply_fifo` preenchido
- Bloquear a espera de `MSG_START`
- No fim da execucao, enviar `MSG_FINISH`
- Para `-c`, enviar `MSG_STATUS_REQ` e imprimir `response_t`
- Para `-s`, enviar `MSG_SHUTDOWN_REQ` e aguardar `MSG_SHUTDOWN_ACK`

## Notas

- `RR_USER` tenta escolher proximo comando de um utilizador diferente do ultimo escalonado.
- Se nao existir utilizador diferente na fila, faz fallback para cabeca da fila.
- Durante shutdown, novos `MSG_SUBMIT` recebem `MSG_ERROR`.

## To-do da Pessoa A antes da integracao da Pessoa B

- Adicionar mais uma politica (ex.: `SJF_PROXY`, com base em historico por utilizador/comando).
- Escrever `tmp/events.log` com transicoes (`SUBMIT`, `START`, `FINISH`) para facilitar debug.
- Acrescentar script com 3-5 repeticoes por politica para reduzir ruido de medicao.
- Criar uma tabela final para o relatorio com `avg_wait_ms`, `avg_total_ms`, `fairness_wait_gap_ms`.
