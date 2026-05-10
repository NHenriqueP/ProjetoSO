#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────
# run_all_experiments.sh — Executa experimentos com todas as políticas
# e diferentes níveis de paralelismo, gerando dados para o relatório.
#
# Uso: ./scripts/run_all_experiments.sh [repetitions]
#
# Gera: tmp/experiment_results.csv  (dados brutos)
#       tmp/experiment_summary.txt  (tabela resumo para relatório)
# ──────────────────────────────────────────────────────────────────
set -uo pipefail

REPETITIONS="${1:-3}"
RESULTS_CSV="tmp/experiment_results.csv"
SUMMARY_TXT="tmp/experiment_summary.txt"

make -s >/dev/null 2>&1
mkdir -p tmp

echo "policy,parallel,run,avg_wait_ms,avg_total_ms,fairness_gap_ms" > "$RESULTS_CSV"

run_experiment() {
    local policy="$1"
    local parallel="$2"
    local run_num="$3"

    # Limpar logs anteriores
    : > tmp/executions.log
    : > tmp/events.log
    rm -f /tmp/so_controller_fifo

    # Iniciar controller
    ./controller "$parallel" "$policy" >/dev/null 2>&1 &
    local ctrl_pid=$!
    sleep 0.3

    # Carga de trabalho mista: 3 utilizadores com comandos variados
    # Utilizador 1: 2 comandos (sleep 2 + true)
    # Utilizador 2: 2 comandos (sleep 1 + true)
    # Utilizador 3: 1 comando  (true)
    ./runner -e 1 "sleep 2" >/dev/null 2>&1 &
    local p1=$!
    sleep 0.02
    ./runner -e 1 "true"    >/dev/null 2>&1 &
    local p2=$!
    sleep 0.02
    ./runner -e 2 "sleep 1" >/dev/null 2>&1 &
    local p3=$!
    sleep 0.02
    ./runner -e 3 "true"    >/dev/null 2>&1 &
    local p4=$!
    sleep 0.02
    ./runner -e 2 "true"    >/dev/null 2>&1 &
    local p5=$!

    wait "$p1" "$p2" "$p3" "$p4" "$p5" 2>/dev/null

    # Desligar controller
    ./runner -s >/dev/null 2>&1
    wait "$ctrl_pid" 2>/dev/null || true

    # Calcular métricas
    if [[ ! -s tmp/executions.log ]]; then
        echo "$policy,$parallel,$run_num,0,0,0" >> "$RESULTS_CSV"
        return
    fi

    local summary
    summary=$(awk -f scripts/summarize_executions.awk tmp/executions.log 2>/dev/null)

    local avg_wait avg_total fairness
    avg_wait=$(echo "$summary" | awk -F= '/^avg_wait_ms=/{print $2}' | tr ',' '.')
    avg_total=$(echo "$summary" | awk -F= '/^avg_total_ms=/{print $2}' | tr ',' '.')
    fairness=$(echo "$summary" | awk -F= '/^fairness_wait_gap_ms=/{print $2}' | awk '{print $1}' | tr ',' '.')

    echo "$policy,$parallel,$run_num,${avg_wait:-0},${avg_total:-0},${fairness:-0}" >> "$RESULTS_CSV"
}

echo "═══════════════════════════════════════════════════════════"
echo " Experimentos de escalonamento — TP-SO-2"
echo " Repetições por configuração: $REPETITIONS"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Testar todas as combinações
for policy in FCFS RR_USER SJF_PROXY; do
    for parallel in 1 2 4; do
        echo -n "  $policy (parallel=$parallel): "
        for run in $(seq 1 "$REPETITIONS"); do
            run_experiment "$policy" "$parallel" "$run"
            echo -n "."
        done
        echo " done"
    done
done

echo ""
echo "─── Resultados (médias por configuração) ───"
echo ""

# Gerar tabela resumo
{
    printf "%-12s  %-8s  %12s  %12s  %16s\n" \
        "Política" "Parallel" "Avg Wait(ms)" "Avg Total(ms)" "Fairness Gap(ms)"
    printf "%-12s  %-8s  %12s  %12s  %16s\n" \
        "──────────" "────────" "────────────" "──────────────" "────────────────"

    awk -F, 'NR>1 {
        key=$1","$2
        w[key]+=$4; t[key]+=$5; f[key]+=$6; n[key]++
    }
    END {
        for (k in n) {
            split(k, a, ",")
            printf "%-12s  %-8s  %12.1f  %12.1f  %16.1f\n",
                a[1], a[2], w[k]/n[k], t[k]/n[k], f[k]/n[k]
        }
    }' "$RESULTS_CSV" | sort
} | tee "$SUMMARY_TXT"

echo ""
echo "Dados brutos: $RESULTS_CSV"
echo "Tabela resumo: $SUMMARY_TXT"
echo ""
echo "═══════════════════════════════════════════════════════════"
echo " Experimentos concluídos com sucesso!"
echo "═══════════════════════════════════════════════════════════"
