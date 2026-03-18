BEGIN {
    FS = ","
    runs = 0
    sum_wait = 0
    sum_total = 0
}

NF >= 7 {
    user = $1 + 0
    wait = $6 + 0
    total = $7 + 0

    runs++
    sum_wait += wait
    sum_total += total

    user_runs[user]++
    user_wait[user] += wait
    user_total[user] += total
}

END {
    if (runs == 0) {
        print "no runs"
        exit 0
    }

    printf("runs=%d\n", runs)
    printf("avg_wait_ms=%.2f\n", sum_wait / runs)
    printf("avg_total_ms=%.2f\n", sum_total / runs)

    min_user_avg = -1
    max_user_avg = -1
    min_user = -1
    max_user = -1

    for (u in user_runs) {
        avg_w = user_wait[u] / user_runs[u]
        avg_t = user_total[u] / user_runs[u]
        printf("user=%d runs=%d avg_wait_ms=%.2f avg_total_ms=%.2f\n",
               u, user_runs[u], avg_w, avg_t)

        if (min_user_avg < 0 || avg_w < min_user_avg) {
            min_user_avg = avg_w
            min_user = u
        }
        if (max_user_avg < 0 || avg_w > max_user_avg) {
            max_user_avg = avg_w
            max_user = u
        }
    }

    printf("fairness_wait_gap_ms=%.2f (max_user=%d min_user=%d)\n",
           max_user_avg - min_user_avg, max_user, min_user)
}
