#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Global command counter for unique IDs */
static unsigned long g_command_id = 0;

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

static char *generate_fifo_path(pid_t pid) {
    static char path[RUNNER_FIFO_PATH_MAX];
    snprintf(path, sizeof(path), "/tmp/so_runner_%d_%lu.fifo", pid, 
             (unsigned long)time(NULL) * 1000000 + g_command_id++);
    return path;
}

static int send_message(const message_t *msg) {
    int fd;
    ssize_t written;

    fd = open(CONTROLLER_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        print_error("Cannot connect to controller");
        return -1;
    }

    written = write(fd, msg, sizeof(*msg));
    close(fd);

    if (written != (ssize_t)sizeof(*msg)) {
        print_error("Failed to send message to controller");
        return -1;
    }

    return 0;
}

static int receive_response(const char *fifo_path, response_t *resp) {
    int fd;
    ssize_t n;

    /* Create the reply FIFO */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        print_error("Cannot create reply FIFO");
        return -1;
    }

    /* Open for reading (this will block until the other side opens for writing) */
    fd = open(fifo_path, O_RDONLY);
    if (fd < 0) {
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return -1;
    }

    n = read(fd, resp, sizeof(*resp));
    close(fd);
    unlink(fifo_path);

    if (n != (ssize_t)sizeof(*resp)) {
        print_error("Failed to receive response from controller");
        return -1;
    }

    return 0;
}

static int cmd_execute(int user_id, const char *command) {
    message_t msg;
    response_t resp;
    char *fifo_path;
    pid_t pid;

    pid = getpid();
    fifo_path = generate_fifo_path(pid);

    /* Create and send the SUBMIT message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SUBMIT;
    msg.user_id = user_id;
    msg.command_id = g_command_id;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);
    strncpy(msg.command, command, sizeof(msg.command) - 1);

    if (send_message(&msg) < 0) {
        return 1;
    }

    printf("Submitting command (ID: %lu, User: %d)\n", msg.command_id, user_id);
    printf("Command: %s\n", command);

    /* Wait for response */
    if (receive_response(fifo_path, &resp) < 0) {
        return 1;
    }

    if (!resp.ok) {
        fprintf(stderr, "Command submission failed: %s\n", resp.info);
        return 1;
    }

    if (resp.type == MSG_START) {
        print_info("Command accepted and started by controller");
        printf("Command ID: %lu\n", msg.command_id);
        return 0;
    } else if (resp.type == MSG_ERROR) {
        fprintf(stderr, "Error from controller: %s\n", resp.info);
        return 1;
    }

    print_error("Unexpected response type from controller");
    return 1;
}

static int cmd_check_status(void) {
    message_t msg;
    response_t resp;
    char fifo_path[RUNNER_FIFO_PATH_MAX];
    pid_t pid;
    int i;

    pid = getpid();
    snprintf(fifo_path, sizeof(fifo_path), "/tmp/so_runner_status_%d.fifo", pid);

    /* Create and send the STATUS_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_STATUS_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        return 1;
    }

    printf("Requesting controller status...\n");

    /* Wait for response */
    if (receive_response(fifo_path, &resp) < 0) {
        return 1;
    }

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

    pid = getpid();
    snprintf(fifo_path, sizeof(fifo_path), "/tmp/so_runner_shutdown_%d.fifo", pid);

    /* Create and send the SHUTDOWN_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SHUTDOWN_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        return 1;
    }

    printf("Requesting controller shutdown...\n");

    /* Wait for response */
    if (receive_response(fifo_path, &resp) < 0) {
        return 1;
    }

    if (!resp.ok) {
        fprintf(stderr, "Shutdown request failed: %s\n", resp.info);
        return 1;
    }

    if (resp.type == MSG_SHUTDOWN_ACK) {
        print_info("Controller acknowledged shutdown request");
        printf("The controller will shut down after finishing all queued jobs.\n");
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
