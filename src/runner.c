#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Global command counter for unique IDs */
static unsigned long g_command_id = 0;
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

/* Parse command string into argv array */
static char **parse_command(const char *cmd_str, int *argc_out) {
    char *cmd_copy;
    char *saveptr;
    char *token;
    char **argv;
    int count = 0;
    int capacity = 10;

    cmd_copy = strdup(cmd_str);
    if (!cmd_copy) {
        return NULL;
    }

    argv = malloc((capacity + 1) * sizeof(char *));
    if (!argv) {
        free(cmd_copy);
        return NULL;
    }

    token = strtok_r(cmd_copy, " ", &saveptr);
    while (token) {
        if (count >= capacity) {
            capacity *= 2;
            char **new_argv = realloc(argv, (capacity + 1) * sizeof(char *));
            if (!new_argv) {
                for (int i = 0; i < count; i++) {
                    free(argv[i]);
                }
                free(argv);
                free(cmd_copy);
                return NULL;
            }
            argv = new_argv;
        }
        argv[count] = strdup(token);
        if (!argv[count]) {
            for (int i = 0; i < count; i++) {
                free(argv[i]);
            }
            free(argv);
            free(cmd_copy);
            return NULL;
        }
        count++;
        token = strtok_r(NULL, " ", &saveptr);
    }
    free(cmd_copy);
    argv[count] = NULL;
    *argc_out = count;
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) {
        free(argv[i]);
    }
    free(argv);
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
    cmd_id = g_command_id++;
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO FIRST, before sending message */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
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
        unlink(fifo_path);
        return 1;
    }

    /* Now open the reply FIFO for reading - this will block until controller opens for writing */
    reply_fd = open(fifo_path, O_RDONLY);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    /* Read the response */
    ssize_t n = read(reply_fd, &resp, sizeof(resp));
    close(reply_fd);
    unlink(fifo_path);

    if (n != (ssize_t)sizeof(resp)) {
        print_error("Failed to receive response from controller");
        return 1;
    }

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
        /* Child process: execute the command */
        int argc;
        char **argv = parse_command(command, &argc);
        
        if (!argv || argc == 0) {
            fprintf(stderr, "ERROR: Failed to parse command: %s\n", command);
            exit(127);
        }

        /* Execute the command */
        execvp(argv[0], argv);
        
        /* execvp only returns on error */
        perror("execvp");
        fprintf(stderr, "ERROR: Failed to execute: %s\n", command);
        free_argv(argv);
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
    ssize_t n;
    unsigned long cmd_id;

    pid = getpid();
    cmd_id = g_command_id++;
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO first */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
        return 1;
    }

    /* Create and send the STATUS_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_STATUS_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        unlink(fifo_path);
        return 1;
    }

    printf("Requesting controller status...\n");
    fflush(stdout);

    /* Open the reply FIFO for reading */
    reply_fd = open(fifo_path, O_RDONLY);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    n = read(reply_fd, &resp, sizeof(resp));
    close(reply_fd);
    unlink(fifo_path);

    if (n != (ssize_t)sizeof(resp)) {
        print_error("Failed to receive response from controller");
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
    int reply_fd;
    ssize_t n;
    unsigned long cmd_id;

    pid = getpid();
    cmd_id = g_command_id++;
    generate_fifo_path(pid, cmd_id, fifo_path, sizeof(fifo_path));

    /* Create the reply FIFO first */
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        print_error("Cannot create reply FIFO");
        return 1;
    }

    /* Create and send the SHUTDOWN_REQ message */
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SHUTDOWN_REQ;
    msg.runner_pid = pid;
    strncpy(msg.reply_fifo, fifo_path, sizeof(msg.reply_fifo) - 1);

    if (send_message(&msg) < 0) {
        unlink(fifo_path);
        return 1;
    }

    printf("Requesting controller shutdown...\n");
    fflush(stdout);

    /* Open the reply FIFO for reading */
    reply_fd = open(fifo_path, O_RDONLY);
    if (reply_fd < 0) {
        perror("open");
        print_error("Cannot open reply FIFO");
        unlink(fifo_path);
        return 1;
    }

    n = read(reply_fd, &resp, sizeof(resp));
    close(reply_fd);
    unlink(fifo_path);

    if (n != (ssize_t)sizeof(resp)) {
        print_error("Failed to receive response from controller");
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
