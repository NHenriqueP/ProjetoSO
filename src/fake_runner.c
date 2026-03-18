#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void print_usage(void) {
    const char *msg =
        "usage:\n"
        "  ./bin/fake_runner submit <user-id> <command-id> [sleep-seconds]\n"
        "  ./bin/fake_runner status\n"
        "  ./bin/fake_runner shutdown\n";
    (void)write(STDERR_FILENO, msg, strlen(msg));
}

static int write_request(const message_t *msg) {
    int fd;
    ssize_t n;

    fd = open(CONTROLLER_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        perror("open controller fifo");
        return -1;
    }

    n = write(fd, msg, sizeof(*msg));
    close(fd);
    if (n != (ssize_t)sizeof(*msg)) {
        perror("write controller fifo");
        return -1;
    }

    return 0;
}

static int read_response(const char *fifo_path, response_t *resp) {
    int fd;
    ssize_t n;

    fd = open(fifo_path, O_RDONLY);
    if (fd < 0) {
        perror("open reply fifo");
        return -1;
    }

    n = read(fd, resp, sizeof(*resp));
    close(fd);
    if (n != (ssize_t)sizeof(*resp)) {
        perror("read reply fifo");
        return -1;
    }

    return 0;
}

static int do_status_or_shutdown(int is_shutdown) {
    message_t req;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    snprintf(fifo_path, sizeof(fifo_path), "/tmp/so_fake_runner_%d", (int)getpid());
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo reply");
        return 1;
    }

    req.type = is_shutdown ? MSG_SHUTDOWN_REQ : MSG_STATUS_REQ;
    req.runner_pid = getpid();
    strncpy(req.reply_fifo, fifo_path, sizeof(req.reply_fifo) - 1);

    if (write_request(&req) != 0) {
        unlink(fifo_path);
        return 1;
    }

    if (read_response(fifo_path, &resp) != 0) {
        unlink(fifo_path);
        return 1;
    }

    if (is_shutdown) {
        dprintf(STDOUT_FILENO, "shutdown_ack: ok=%d\n", resp.ok);
    } else {
        int i;
        dprintf(STDOUT_FILENO, "--Executing (%d)\n", resp.executing_count);
        for (i = 0; i < resp.executing_count; i++) {
            dprintf(STDOUT_FILENO,
                    "user-id %d - command-id %lu\n",
                    resp.executing[i].user_id,
                    resp.executing[i].command_id);
        }
        dprintf(STDOUT_FILENO, "--Scheduled (%d)\n", resp.scheduled_count);
        for (i = 0; i < resp.scheduled_count; i++) {
            dprintf(STDOUT_FILENO,
                    "user-id %d - command-id %lu\n",
                    resp.scheduled[i].user_id,
                    resp.scheduled[i].command_id);
        }
    }

    unlink(fifo_path);
    return 0;
}

static int do_submit(int user_id, unsigned long command_id, int sleep_seconds) {
    message_t req;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    snprintf(fifo_path,
             sizeof(fifo_path),
             "/tmp/so_fake_runner_%d_%lu",
             (int)getpid(),
             command_id);
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo reply");
        return 1;
    }

    req.type = MSG_SUBMIT;
    req.user_id = user_id;
    req.command_id = command_id;
    req.runner_pid = getpid();
    strncpy(req.reply_fifo, fifo_path, sizeof(req.reply_fifo) - 1);
    strncpy(req.command, "fake-command", sizeof(req.command) - 1);

    if (write_request(&req) != 0) {
        unlink(fifo_path);
        return 1;
    }

    if (read_response(fifo_path, &resp) != 0) {
        unlink(fifo_path);
        return 1;
    }

    if (resp.type != MSG_START || !resp.ok) {
        dprintf(STDERR_FILENO,
                "submit failed: response type=%d ok=%d info=%s\n",
                resp.type,
                resp.ok,
                resp.info);
        unlink(fifo_path);
        return 1;
    }

    dprintf(STDOUT_FILENO,
            "started: user-id=%d command-id=%lu (sleep=%d)\n",
            user_id,
            command_id,
            sleep_seconds);

    if (sleep_seconds > 0) {
        sleep((unsigned int)sleep_seconds);
    }

    memset(&req, 0, sizeof(req));
    req.type = MSG_FINISH;
    req.user_id = user_id;
    req.command_id = command_id;
    req.runner_pid = getpid();

    if (write_request(&req) != 0) {
        unlink(fifo_path);
        return 1;
    }

    dprintf(STDOUT_FILENO, "finished: user-id=%d command-id=%lu\n", user_id, command_id);
    unlink(fifo_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        return do_status_or_shutdown(0);
    }

    if (strcmp(argv[1], "shutdown") == 0) {
        return do_status_or_shutdown(1);
    }

    if (strcmp(argv[1], "submit") == 0) {
        int user_id;
        unsigned long command_id;
        int sleep_seconds;

        if (argc < 4) {
            print_usage();
            return 1;
        }

        user_id = atoi(argv[2]);
        command_id = strtoul(argv[3], NULL, 10);
        sleep_seconds = 0;
        if (argc >= 5) {
            sleep_seconds = atoi(argv[4]);
            if (sleep_seconds < 0) {
                sleep_seconds = 0;
            }
        }

        return do_submit(user_id, command_id, sleep_seconds);
    }

    print_usage();
    return 1;
}
