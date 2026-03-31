#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_signal = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop_signal = 1;
}

static void print_usage(void) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  runner -e <user_id> <command>          Execute a command\n");
    fprintf(stderr, "  runner -c                              Check controller status\n");
    fprintf(stderr, "  runner -s                              Shutdown controller\n");
}

static void print_error(const char *msg) {
    fprintf(stderr, "ERROR: %s\n", msg);
}

static void print_info(const char *msg) {
    fprintf(stdout, "INFO: %s\n", msg);
}

static void generate_fifo_path(pid_t pid, unsigned long cmd_id, char *buffer, size_t bufsize) {
    snprintf(buffer, bufsize, "/tmp/so_runner_%d_%lu.fifo", pid, cmd_id);
}

static int write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
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

static int read_full(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
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

static unsigned long generate_unique_id(pid_t pid) {
    struct timespec ts;
    uint64_t usec;
    uint64_t id;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* Fallback: still unique enough across concurrent runners due to pid. */
        return (unsigned long)(((uint64_t)(uint32_t)pid << 32) ^ (uint64_t)time(NULL));
    }

    usec = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
    /* Compose an ID that stays unique even with concurrent runners. */
    id = ((uint64_t)(uint32_t)pid << 32) | (usec & 0xFFFFFFFFULL);
    return (unsigned long)id;
}

static int send_message(const message_t *msg) {
    int fd;

    fd = open(CONTROLLER_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        print_error("Cannot connect to controller");
        return -1;
    }

    if (write_full(fd, msg, sizeof(*msg)) != 0) {
        close(fd);
        print_error("Failed to send message to controller");
        return -1;
    }

    close(fd);

    return 0;
}

static int cmd_execute(int user_id, const char *command) {
    message_t msg;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];
    pid_t pid;
    pid_t cmd_pid;
    int status;
    unsigned long cmd_id;
    int reply_fd;
    
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    pid = getpid();
    cmd_id = generate_unique_id(pid);
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO FIRST, before sending message */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
        return 1;
    }

    /*
     * Open reply FIFO in RDWR: prevents immediate EOF reads before controller opens for writing,
     * and allows controller to open its end with O_NONBLOCK without ever blocking.
     */
    reply_fd = open(fifo_path, O_RDWR);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    /* Create and send the SUBMIT message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SUBMIT;
    msg.user_id = user_id;
    msg.command_id = cmd_id;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);
    strncpy(msg.command, command, sizeof(msg.command) - 1);

    printf("Submitting command (ID: %lu, User: %d)\n", cmd_id, user_id);
    printf("Command: %s\n", command);
    fflush(stdout);

    if (send_message(&msg) < 0) {
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    /* Read the response */
    if (read_full(reply_fd, &resp, sizeof(resp)) != 0) {
        print_error("Failed to receive response from controller");
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    close(reply_fd);
    unlink(fifo_path);

    if (!resp.ok) {
        fprintf(stderr, "Command submission failed: %s\n", resp.info);
        return 1;
    }

    if (resp.type != MSG_START) {
        if (resp.type == MSG_ERROR) {
            fprintf(stderr, "Error from controller: %s\n", resp.info);
        } else {
            print_error("Unexpected response type from controller");
        }
        return 1;
    }

    print_info("Command accepted and started by controller");
    printf("Command ID: %lu, PID: %d\n", cmd_id, pid);
    fflush(stdout);

    /* Execute the command */
    cmd_pid = fork();
    if (cmd_pid < 0) {
        perror("fork");
        return 1;
    }

    if (cmd_pid == 0) {
        /* Child process: execute the command via shell to preserve quoting/pipes/redirections. */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        perror("execl");
        fprintf(stderr, "ERROR: Failed to execute: %s\n", command);
        exit(127);
    } else {
        /* Parent process: wait for child to complete */
        if (waitpid(cmd_pid, &status, 0) < 0) {
            perror("waitpid");
            return 1;
        }

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code != 0) {
                fprintf(stderr, "Command exited with code %d\n", exit_code);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            fprintf(stderr, "Command terminated by signal %d\n", sig);
        }

        /* Send FINISH message to controller */
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_FINISH;
        msg.user_id = user_id;
        msg.command_id = cmd_id;
        msg.runner_pid = pid;

        if (send_message(&msg) < 0) {
            print_error("Failed to notify controller of command completion");
            return 1;
        }

        print_info("Command completed and controller notified");
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
    }
}

static int cmd_check_status(void) {
    message_t msg;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];
    pid_t pid;
    int i;
    int reply_fd;
    unsigned long cmd_id;

    pid = getpid();
    cmd_id = generate_unique_id(pid);
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO first */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
        return 1;
    }

    reply_fd = open(fifo_path, O_RDWR);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    /* Create and send the STATUS_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_STATUS_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    printf("Requesting controller status...\n");
    fflush(stdout);

    if (read_full(reply_fd, &resp, sizeof(resp)) != 0) {
        print_error("Failed to receive response from controller");
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    close(reply_fd);
    unlink(fifo_path);

    if (!resp.ok) {
        fprintf(stderr, "Status request failed: %s\n", resp.info);
        return 1;
    }

    /* Display status */
    printf("\n=== Controller Status ===\n");
    printf("Executing: %d job(s)\n", resp.executing_count);
    for (i = 0; i < resp.executing_count; i++) {
        printf("  [%d] User %d, Command %lu\n", 
               i + 1, 
               resp.executing[i].user_id, 
               resp.executing[i].command_id);
    }

    printf("Scheduled: %d job(s)\n", resp.scheduled_count);
    for (i = 0; i < resp.scheduled_count; i++) {
        printf("  [%d] User %d, Command %lu\n", 
               i + 1, 
               resp.scheduled[i].user_id, 
               resp.scheduled[i].command_id);
    }
    printf("========================\n");

    return 0;
}

static int cmd_shutdown(void) {
    message_t msg;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];
    pid_t pid;
    int reply_fd;
    unsigned long cmd_id;

    pid = getpid();
    cmd_id = generate_unique_id(pid);
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO first */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
        return 1;
    }

    reply_fd = open(fifo_path, O_RDWR);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    /* Create and send the SHUTDOWN_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SHUTDOWN_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    printf("Requesting controller shutdown...\n");
    fflush(stdout);

    if (read_full(reply_fd, &resp, sizeof(resp)) != 0) {
        print_error("Failed to receive response from controller");
        close(reply_fd);
        unlink(fifo_path);
        return 1;
    }

    close(reply_fd);
    unlink(fifo_path);

    if (!resp.ok) {
        fprintf(stderr, "Shutdown request failed: %s\n", resp.info);
        return 1;
    }

    if (resp.type == MSG_SHUTDOWN_ACK) {
        print_info("Controller acknowledged shutdown request");
        printf("The controller is shutting down.\n");
        return 0;
    }

    print_error("Unexpected response type from controller");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "-e") == 0) {
        /* Execute mode: runner -e <user_id> <command> */
        if (argc < 4) {
            print_usage();
            return 1;
        }

        int user_id = atoi(argv[2]);
        if (user_id <= 0) {
            print_error("Invalid user_id (must be positive)");
            return 1;
        }

        /* Reconstruct the command from remaining arguments */
        char command[COMMAND_TEXT_MAX] = {0};
        for (int i = 3; i < argc; i++) {
            if (i > 3) {
                strncat(command, " ", sizeof(command) - strlen(command) - 1);
            }
            strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
        }

        return cmd_execute(user_id, command);

    } else if (strcmp(argv[1], "-c") == 0) {
        /* Check status mode: runner -c */
        return cmd_check_status();

    } else if (strcmp(argv[1], "-s") == 0) {
        /* Shutdown mode: runner -s */
        return cmd_shutdown();

    } else {
        print_usage();
        return 1;
    }
}
