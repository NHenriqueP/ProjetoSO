#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct job {
    int user_id;
    unsigned long command_id;
    pid_t runner_pid;
    char reply_fifo[RUNNER_FIFO_PATH_MAX];
    char command[COMMAND_TEXT_MAX];
    struct timeval submit_ts;
    struct timeval start_ts;
    struct job *next;
} job_t;

typedef struct controller_state {
    int max_parallel;
    int current_running;
    int shutdown_requested;
    int shutdown_ack_pending;
    char policy[32];
    int has_last_user;
    int last_user_id;
    char shutdown_reply_fifo[RUNNER_FIFO_PATH_MAX];
    job_t *scheduled_head;
    job_t *scheduled_tail;
    job_t *executing_head;
} controller_state_t;

static volatile sig_atomic_t g_stop_signal = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop_signal = 1;
}

static void safe_write_text(int fd, const char *text) {
    if (text == NULL) {
        return;
    }
    (void)write(fd, text, strlen(text));
}

static void usage(void) {
    safe_write_text(
        STDERR_FILENO,
        "usage: ./controller <parallel-commands> <sched-policy>\n"
        "  sched-policy: FCFS | RR_USER\n");
}

static int is_valid_policy(const char *policy) {
    return strcmp(policy, "FCFS") == 0 || strcmp(policy, "RR_USER") == 0 ||
           strcmp(policy, "RR-U") == 0;
}

static int write_full(int fd, const void *buf, size_t len) {
    const char *p;
    size_t total;

    p = (const char *)buf;
    total = 0;
    while (total < len) {
        ssize_t n;
        n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

static int send_response(const char *fifo_path, const response_t *resp) {
    int fd;

    /*
     * Open non-blocking to ensure the controller never gets stuck on a misbehaving runner.
     * Runners should open their reply FIFO in O_RDWR mode before sending a request.
     */
    fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (write_full(fd, resp, sizeof(*resp)) != 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static job_t *create_job(const message_t *msg) {
    job_t *job;

    job = (job_t *)calloc(1, sizeof(*job));
    if (job == NULL) {
        return NULL;
    }

    job->user_id = msg->user_id;
    job->command_id = msg->command_id;
    job->runner_pid = msg->runner_pid;
    strncpy(job->reply_fifo, msg->reply_fifo, sizeof(job->reply_fifo) - 1);
    strncpy(job->command, msg->command, sizeof(job->command) - 1);
    gettimeofday(&job->submit_ts, NULL);
    job->next = NULL;

    return job;
}

static void enqueue_scheduled(controller_state_t *state, job_t *job) {
    if (state->scheduled_tail == NULL) {
        state->scheduled_head = job;
        state->scheduled_tail = job;
        return;
    }

    state->scheduled_tail->next = job;
    state->scheduled_tail = job;
}

static job_t *dequeue_scheduled_head(controller_state_t *state) {
    job_t *job;

    job = state->scheduled_head;
    if (job == NULL) {
        return NULL;
    }

    state->scheduled_head = job->next;
    if (state->scheduled_head == NULL) {
        state->scheduled_tail = NULL;
    }
    job->next = NULL;
    return job;
}

static int is_rr_user_policy(const controller_state_t *state) {
    return strcmp(state->policy, "RR_USER") == 0 ||
           strcmp(state->policy, "RR-U") == 0;
}

static job_t *dequeue_scheduled_rr_user(controller_state_t *state) {
    job_t *prev;
    job_t *cur;
    job_t *pick_prev;
    job_t *pick;

    prev = NULL;
    cur = state->scheduled_head;
    pick_prev = NULL;
    pick = NULL;

    while (cur != NULL) {
        if (!state->has_last_user || cur->user_id != state->last_user_id) {
            pick_prev = prev;
            pick = cur;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    if (pick == NULL) {
        return dequeue_scheduled_head(state);
    }

    if (pick_prev == NULL) {
        state->scheduled_head = pick->next;
    } else {
        pick_prev->next = pick->next;
    }

    if (pick == state->scheduled_tail) {
        state->scheduled_tail = pick_prev;
    }

    pick->next = NULL;
    return pick;
}

static job_t *dequeue_next_scheduled(controller_state_t *state) {
    if (is_rr_user_policy(state)) {
        return dequeue_scheduled_rr_user(state);
    }
    return dequeue_scheduled_head(state);
}

static void add_executing(controller_state_t *state, job_t *job) {
    job->next = state->executing_head;
    state->executing_head = job;
}

static long long elapsed_ms(const struct timeval *start, const struct timeval *end) {
    long long secs;
    long long usecs;

    secs = (long long)(end->tv_sec - start->tv_sec);
    usecs = (long long)(end->tv_usec - start->tv_usec);
    return secs * 1000LL + usecs / 1000LL;
}

static long long epoch_ms(const struct timeval *ts) {
    return (long long)ts->tv_sec * 1000LL + (long long)ts->tv_usec / 1000LL;
}

static void log_finished_job(const job_t *job) {
    int fd;
    struct timeval end_ts;
    char line[512];
    int len;
    long long submit_ms;
    long long start_ms;
    long long end_ms;
    long long wait_ms;
    long long total_ms;

    gettimeofday(&end_ts, NULL);
    submit_ms = epoch_ms(&job->submit_ts);
    start_ms = epoch_ms(&job->start_ts);
    end_ms = epoch_ms(&end_ts);
    wait_ms = elapsed_ms(&job->submit_ts, &job->start_ts);
    total_ms = elapsed_ms(&job->submit_ts, &end_ts);

    fd = open("tmp/executions.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }

    len = snprintf(line,
                   sizeof(line),
                   "%d,%lu,%lld,%lld,%lld,%lld,%lld\n",
                   job->user_id,
                   job->command_id,
                   submit_ms,
                   start_ms,
                   end_ms,
                   wait_ms,
                   total_ms);
    if (len > 0) {
        (void)write(fd, line, (size_t)len);
    }

    close(fd);
}

static int finish_executing(controller_state_t *state, int user_id, unsigned long command_id) {
    job_t *prev;
    job_t *cur;

    prev = NULL;
    cur = state->executing_head;
    while (cur != NULL) {
        if (cur->user_id == user_id && cur->command_id == command_id) {
            if (prev == NULL) {
                state->executing_head = cur->next;
            } else {
                prev->next = cur->next;
            }

            if (state->current_running > 0) {
                state->current_running--;
            }
            log_finished_job(cur);
            free(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    return -1;
}

static void build_status_response(const controller_state_t *state, response_t *resp) {
    const job_t *cur;
    int i;

    memset(resp, 0, sizeof(*resp));
    resp->type = MSG_STATUS_RESP;
    resp->ok = 1;

    i = 0;
    cur = state->executing_head;
    while (cur != NULL && i < STATUS_MAX_ITEMS) {
        resp->executing[i].user_id = cur->user_id;
        resp->executing[i].command_id = cur->command_id;
        i++;
        cur = cur->next;
    }
    resp->executing_count = i;

    i = 0;
    cur = state->scheduled_head;
    while (cur != NULL && i < STATUS_MAX_ITEMS) {
        resp->scheduled[i].user_id = cur->user_id;
        resp->scheduled[i].command_id = cur->command_id;
        i++;
        cur = cur->next;
    }
    resp->scheduled_count = i;
}

static void schedule_jobs(controller_state_t *state) {
    job_t *job;
    response_t resp;

    while (state->current_running < state->max_parallel) {
        job = dequeue_next_scheduled(state);
        if (job == NULL) {
            return;
        }

        gettimeofday(&job->start_ts, NULL);
        add_executing(state, job);
        state->current_running++;
        state->has_last_user = 1;
        state->last_user_id = job->user_id;

        memset(&resp, 0, sizeof(resp));
        resp.type = MSG_START;
        resp.ok = 1;
        
        if (send_response(job->reply_fifo, &resp) != 0) {
            /* If the runner is gone, free the slot to keep controller healthy. */
            (void)finish_executing(state, job->user_id, job->command_id);
        }
    }
}

static int can_exit(const controller_state_t *state) {
    return state->shutdown_requested && state->current_running == 0 &&
           state->scheduled_head == NULL;
}

int main(int argc, char **argv) {
    controller_state_t state;
    struct sigaction sa;
    int request_fd;
    struct pollfd pfd;
    int fifo_keepalive_fd;

    if (argc != 3) {
        usage();
        return 1;
    }

    memset(&state, 0, sizeof(state));
    state.max_parallel = atoi(argv[1]);
    if (state.max_parallel <= 0) {
        safe_write_text(STDERR_FILENO, "invalid parallel-commands value\n");
        return 1;
    }
    strncpy(state.policy, argv[2], sizeof(state.policy) - 1);
    if (!is_valid_policy(state.policy)) {
        safe_write_text(STDERR_FILENO, "invalid sched-policy. use FCFS or RR_USER\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal SIGPIPE");
        return 1;
    }

    unlink(CONTROLLER_FIFO_PATH);
    if (mkfifo(CONTROLLER_FIFO_PATH, 0666) < 0) {
        perror("mkfifo controller");
        return 1;
    }

    request_fd = open(CONTROLLER_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (request_fd < 0) {
        perror("open controller fifo");
        unlink(CONTROLLER_FIFO_PATH);
        return 1;
    }

    fifo_keepalive_fd = open(CONTROLLER_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fifo_keepalive_fd < 0 && errno != ENXIO) {
        perror("open fifo keepalive");
    }

    pfd.fd = request_fd;
    pfd.events = POLLIN;

    while (1) {
        int pr;

        if (g_stop_signal) {
            state.shutdown_requested = 1;
        }

        schedule_jobs(&state);
        if (can_exit(&state)) {
            if (state.shutdown_ack_pending) {
                response_t ack;
                memset(&ack, 0, sizeof(ack));
                ack.type = MSG_SHUTDOWN_ACK;
                ack.ok = 1;
                (void)send_response(state.shutdown_reply_fifo, &ack);
            }
            break;
        }

        pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (pr == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        while (1) {
            message_t msg;
            ssize_t n;

            n = read(request_fd, &msg, sizeof(msg));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("read request");
                break;
            }

            if (n == 0) {
                break;
            }

            if (n != (ssize_t)sizeof(msg)) {
                continue;
            }

            if (msg.type == MSG_SUBMIT) {
                job_t *job;
                response_t err_resp;

                if (state.shutdown_requested) {
                    memset(&err_resp, 0, sizeof(err_resp));
                    err_resp.type = MSG_ERROR;
                    err_resp.ok = 0;
                    strncpy(err_resp.info, "controller shutting down", sizeof(err_resp.info) - 1);
                    (void)send_response(msg.reply_fifo, &err_resp);
                    continue;
                }

                job = create_job(&msg);
                if (job == NULL) {
                    memset(&err_resp, 0, sizeof(err_resp));
                    err_resp.type = MSG_ERROR;
                    err_resp.ok = 0;
                    strncpy(err_resp.info, "controller out of memory", sizeof(err_resp.info) - 1);
                    (void)send_response(msg.reply_fifo, &err_resp);
                    continue;
                }

                enqueue_scheduled(&state, job);
            } else if (msg.type == MSG_FINISH) {
                (void)finish_executing(&state, msg.user_id, msg.command_id);
            } else if (msg.type == MSG_STATUS_REQ) {
                response_t status;
                build_status_response(&state, &status);
                (void)send_response(msg.reply_fifo, &status);
            } else if (msg.type == MSG_SHUTDOWN_REQ) {
                state.shutdown_requested = 1;
                state.shutdown_ack_pending = 1;
                strncpy(state.shutdown_reply_fifo,
                        msg.reply_fifo,
                        sizeof(state.shutdown_reply_fifo) - 1);
            }
        }
    }

    close(request_fd);
    if (fifo_keepalive_fd >= 0) {
        close(fifo_keepalive_fd);
    }
    unlink(CONTROLLER_FIFO_PATH);
    return 0;
}
