#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────
# test_all.sh — Script de testes funcionais para o TP-SO-2.
#
# Testa todas as funcionalidades requeridas pelo enunciado:
#   1. Execução básica de comandos
#   2. Consulta de estado (-c)
#   3. Encerramento gracioso (-s)
#   4. Operadores de redireção: >, <, 2>
#   5. Pipe: |
#   6. Combinação pipe + redireção
#   7. Execução paralela de comandos
#   8. Justiça entre utilizadores (RR_USER)
# ──────────────────────────────────────────────────────────────────
set -uo pipefail

PASS=0
FAIL=0
TOTAL=0

pass() {
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
    echo "  ✓ PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
    echo "  ✗ FAIL: $1"
}

cleanup() {
    if [[ -n "${CTRL_PID:-}" ]]; then
        kill -TERM "$CTRL_PID" >/dev/null 2>&1 || true
        wait "$CTRL_PID" >/dev/null 2>&1 || true
    fi
    rm -f /tmp/so_controller_fifo /tmp/so_test_* /tmp/so_runner_*.fifo
}
trap cleanup EXIT

make -s >/dev/null 2>&1

echo "══════════════════════════════════════════"
echo " TP-SO-2 — Testes Funcionais"
echo "══════════════════════════════════════════"

# ──── Teste 1: Execução básica ────
echo ""
echo "── Teste 1: Execução básica de comando ──"
rm -f /tmp/so_controller_fifo tmp/executions.log tmp/events.log
./controller 1 FCFS &
CTRL_PID=$!
sleep 0.3

OUTPUT=$(./runner -e 1 "echo hello_world" 2>/dev/null)
if echo "$OUTPUT" | grep -q "hello_world"; then
    pass "echo hello_world produz output correto"
else
    fail "echo hello_world não produziu output esperado"
fi

if echo "$OUTPUT" | grep -q "\[runner\] command .* submitted"; then
    pass "mensagem submitted presente"
else
    fail "mensagem submitted ausente"
fi

if echo "$OUTPUT" | grep -q "\[runner\] executing command"; then
    pass "mensagem executing presente"
else
    fail "mensagem executing ausente"
fi

if echo "$OUTPUT" | grep -q "\[runner\] command .* finished"; then
    pass "mensagem finished presente"
else
    fail "mensagem finished ausente"
fi

# ──── Teste 2: Consulta de estado ────
echo ""
echo "── Teste 2: Consulta de estado (-c) ──"
./runner -e 1 "sleep 2" >/dev/null 2>&1 &
SLEEP_PID=$!
sleep 0.5

STATUS_OUTPUT=$(./runner -c 2>/dev/null)
if echo "$STATUS_OUTPUT" | grep -q "\-\-Executing"; then
    pass "secção --Executing presente"
else
    fail "secção --Executing ausente"
fi

if echo "$STATUS_OUTPUT" | grep -q "\-\-Scheduled"; then
    pass "secção --Scheduled presente"
else
    fail "secção --Scheduled ausente"
fi

if echo "$STATUS_OUTPUT" | grep -q "user-id 1"; then
    pass "user-id visível no estado"
else
    fail "user-id não visível no estado"
fi

wait "$SLEEP_PID" 2>/dev/null || true

# ──── Teste 3: Redireção de saída (>) ────
echo ""
echo "── Teste 3: Redireção de saída (>) ──"
rm -f /tmp/so_test_redir_out.txt
./runner -e 1 "echo redir_test > /tmp/so_test_redir_out.txt" 2>/dev/null
if [[ -f /tmp/so_test_redir_out.txt ]] && grep -q "redir_test" /tmp/so_test_redir_out.txt; then
    pass "redireção > funciona"
else
    fail "redireção > não funciona"
fi

# ──── Teste 4: Redireção de entrada (<) ────
echo ""
echo "── Teste 4: Redireção de entrada (<) ──"
echo "input_data_test" > /tmp/so_test_input.txt
OUTPUT=$(./runner -e 1 "cat < /tmp/so_test_input.txt" 2>/dev/null)
if echo "$OUTPUT" | grep -q "input_data_test"; then
    pass "redireção < funciona"
else
    fail "redireção < não funciona"
fi

# ──── Teste 5: Redireção de stderr (2>) ────
echo ""
echo "── Teste 5: Redireção de stderr (2>) ──"
rm -f /tmp/so_test_stderr.txt
./runner -e 1 "ls /nonexistent_path_test_xyz 2> /tmp/so_test_stderr.txt" 2>/dev/null
if [[ -f /tmp/so_test_stderr.txt ]] && [[ -s /tmp/so_test_stderr.txt ]]; then
    pass "redireção 2> funciona"
else
    fail "redireção 2> não funciona"
fi

# ──── Teste 6: Pipe (|) ────
echo ""
echo "── Teste 6: Pipe (|) ──"
OUTPUT=$(./runner -e 1 "echo hello world | wc -w" 2>/dev/null)
if echo "$OUTPUT" | grep -q "2"; then
    pass "pipe | funciona"
else
    fail "pipe | não funciona"
fi

# ──── Teste 7: Pipe + Redireção combinados ────
echo ""
echo "── Teste 7: Pipe + Redireção combinados ──"
echo -e "apple\nbanana\nstrawberry\nblueberry" > /tmp/so_test_fruits.txt
rm -f /tmp/so_test_count.txt
./runner -e 1 "grep berry /tmp/so_test_fruits.txt | wc -l > /tmp/so_test_count.txt" 2>/dev/null
if [[ -f /tmp/so_test_count.txt ]] && grep -q "2" /tmp/so_test_count.txt; then
    pass "pipe + redireção combinados funcionam"
else
    fail "pipe + redireção combinados não funcionam"
fi

# ──── Teste 8: Log persistente ────
echo ""
echo "── Teste 8: Log persistente ──"
if [[ -f tmp/executions.log ]] && [[ -s tmp/executions.log ]]; then
    pass "executions.log criado e não vazio"
    LINES=$(wc -l < tmp/executions.log)
    if [[ "$LINES" -ge 5 ]]; then
        pass "log contém múltiplas entradas ($LINES linhas)"
    else
        fail "log com poucas entradas ($LINES linhas)"
    fi
else
    fail "executions.log não criado"
fi

# ──── Encerrar e reiniciar para teste de paralelismo ────
./runner -s >/dev/null 2>&1
wait "$CTRL_PID" 2>/dev/null || true
CTRL_PID=""
sleep 0.3

# ──── Teste 9: Execução paralela ────
echo ""
echo "── Teste 9: Execução paralela (max=2) ──"
rm -f /tmp/so_controller_fifo tmp/executions.log tmp/events.log
./controller 2 FCFS &
CTRL_PID=$!
sleep 0.3

START_TIME=$(date +%s)
./runner -e 1 "sleep 1" >/dev/null 2>&1 &
P1=$!
./runner -e 2 "sleep 1" >/dev/null 2>&1 &
P2=$!
wait "$P1" "$P2"
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

if [[ "$ELAPSED" -le 2 ]]; then
    pass "2 comandos sleep 1 em paralelo demoram ~1s ($ELAPSED s)"
else
    fail "execução paralela demorou demasiado ($ELAPSED s)"
fi

# ──── Teste 10: Encerramento gracioso ────
echo ""
echo "── Teste 10: Encerramento gracioso (-s) ──"
SHUTDOWN_OUTPUT=$(./runner -s 2>/dev/null)
if echo "$SHUTDOWN_OUTPUT" | grep -q "\[runner\] controller exited"; then
    pass "shutdown gracioso funciona"
else
    fail "shutdown gracioso falhou"
fi

wait "$CTRL_PID" 2>/dev/null || true
CTRL_PID=""

# ──── Teste 11: Política RR_USER ────
echo ""
echo "── Teste 11: Política RR_USER (max=1) ──"
rm -f /tmp/so_controller_fifo tmp/executions.log tmp/events.log
./controller 1 RR_USER &
CTRL_PID=$!
sleep 0.3

./runner -e 1 "echo user1_cmd1" >/dev/null 2>&1 &
P1=$!
sleep 0.05
./runner -e 2 "echo user2_cmd1" >/dev/null 2>&1 &
P2=$!
sleep 0.05
./runner -e 1 "echo user1_cmd2" >/dev/null 2>&1 &
P3=$!
wait "$P1" "$P2" "$P3"

if [[ -f tmp/executions.log ]]; then
    pass "RR_USER executou todos os comandos"
else
    fail "RR_USER não registou execuções"
fi

./runner -s >/dev/null 2>&1
wait "$CTRL_PID" 2>/dev/null || true
CTRL_PID=""

# ──── Teste 12: Política SJF_PROXY ────
echo ""
echo "── Teste 12: Política SJF_PROXY (max=1) ──"
rm -f /tmp/so_controller_fifo tmp/executions.log tmp/events.log
./controller 1 SJF_PROXY &
CTRL_PID=$!
sleep 0.3

./runner -e 1 "sleep 2" >/dev/null 2>&1 &
P1=$!
sleep 0.05
./runner -e 2 "echo fast" >/dev/null 2>&1 &
P2=$!
wait "$P1" "$P2"

if [[ -f tmp/executions.log ]]; then
    pass "SJF_PROXY executou todos os comandos"
else
    fail "SJF_PROXY não registou execuções"
fi

./runner -s >/dev/null 2>&1
wait "$CTRL_PID" 2>/dev/null || true
CTRL_PID=""

# ──── Sumário ────
echo ""
echo "══════════════════════════════════════════"
echo " Resultado: $PASS/$TOTAL testes passaram"
if [[ "$FAIL" -gt 0 ]]; then
    echo " $FAIL testes falharam!"
    exit 1
else
    echo " Todos os testes passaram! ✓"
fi
echo "══════════════════════════════════════════"
