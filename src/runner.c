/**
 * @file runner.c
 * @brief Programa runner — interface do utilizador para submissão de comandos.
 *
 * O runner permite ao utilizador:
 *   -e <user-id> "<comando>": submeter um comando para execução
 *   -c:                       consultar comandos em execução e em espera
 *   -s:                       pedir o encerramento do controller
 *
 * Comunicação com o controller:
 *   - Envia pedidos para o FIFO público do controller
 *   - Recebe respostas num FIFO privado criado por cada instância
 *
 * Suporte a operadores da linha de comandos:
 *   - Pipes:                cmd1 | cmd2 | cmd3
 *   - Redireção de entrada: cmd < ficheiro
 *   - Redireção de saída:   cmd > ficheiro
 *   - Redireção de stderr:  cmd 2> ficheiro
 */

#define _POSIX_C_SOURCE 200809L

#include "protocol.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
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

static int write_full(int fd, const void *buf, size_t len);

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

/* ─── Geração do caminho do FIFO privado para resposta ─── */
static void generate_fifo_path(pid_t pid, unsigned long cmd_id, char *buffer, size_t bufsize) {
    snprintf(buffer, bufsize, "/tmp/so_runner_%d_%lu.fifo", pid, cmd_id);
}

static int write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                struct timespec ts = {0, 1000000}; /* 1ms */
                nanosleep(&ts, NULL);
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

/**
 * Gera um identificador único para o comando.
 * Combina o PID do processo com timestamp monotónico para garantir unicidade
 * mesmo com múltiplos runners concorrentes.
 */
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

/* ═══════════════════════════════════════════════════════════════════
 * Parsing de comandos — tokenização e construção de pipeline
 * ═══════════════════════════════════════════════════════════════════ */

#define MAX_CMDS 20    /* máx. comandos num pipeline  */
#define MAX_ARGS 50    /* máx. argumentos por comando */
#define MAX_TOKENS 256 /* máx. tokens na tokenização  */

/* Estrutura de um comando individual dentro de um pipeline. */
typedef struct parsed_command {
    char *argv[MAX_ARGS];  /* vetor de argumentos (NULL-terminated) */
    int argc;              /* número de argumentos                  */
    char *in_file;         /* ficheiro para redireção de stdin      */
    char *out_file;        /* ficheiro para redireção de stdout     */
    char *err_file;        /* ficheiro para redireção de stderr     */
} parsed_command_t;

static void free_tokens(char *tokens[], int token_count) {
    int i;

    for (i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
}

static int push_token(char *tokens[], int *token_count, const char *token) {
    char *copy;

    if (*token_count >= MAX_TOKENS) {
        return -1;
    }

    copy = strdup(token);
    if (copy == NULL) {
        return -1;
    }

    tokens[*token_count] = copy;
    (*token_count)++;
    return 0;
}

static int is_operator_token(const char *token) {
    return strcmp(token, "|") == 0 || strcmp(token, "<") == 0 ||
           strcmp(token, ">") == 0 || strcmp(token, "2>") == 0;
}

static int is_operator_start(const char *cmdline, size_t index) {
    return cmdline[index] == '|' || cmdline[index] == '<' ||
           cmdline[index] == '>' ||
           (cmdline[index] == '2' && cmdline[index + 1] == '>');
}

static int append_char(char *buf, size_t *len, char c, size_t max_len) {
    if (*len + 1 >= max_len) {
        return -1;
    }
    buf[*len] = c;
    (*len)++;
    return 0;
}

static int tokenize_command(const char *cmdline, char *tokens[], int *token_count) {
    size_t i;

    *token_count = 0;
    i = 0;
    while (cmdline[i] != '\0') {
        char token_buf[COMMAND_TEXT_MAX];
        size_t len;

        while (isspace((unsigned char)cmdline[i])) {
            i++;
        }

        if (cmdline[i] == '\0') {
            break;
        }

        if (cmdline[i] == '|') {
            if (push_token(tokens, token_count, "|") != 0) {
                free_tokens(tokens, *token_count);
                return -1;
            }
            i++;
            continue;
        }
        if (cmdline[i] == '<') {
            if (push_token(tokens, token_count, "<") != 0) {
                free_tokens(tokens, *token_count);
                return -1;
            }
            i++;
            continue;
        }
        if (cmdline[i] == '>') {
            if (push_token(tokens, token_count, ">") != 0) {
                free_tokens(tokens, *token_count);
                return -1;
            }
            i++;
            continue;
        }
        if (cmdline[i] == '2' && cmdline[i + 1] == '>') {
            if (push_token(tokens, token_count, "2>") != 0) {
                free_tokens(tokens, *token_count);
                return -1;
            }
            i += 2;
            continue;
        }

        len = 0;
        while (cmdline[i] != '\0' && !isspace((unsigned char)cmdline[i]) &&
               !is_operator_start(cmdline, i)) {
            if (cmdline[i] == '\\') {
                if (cmdline[i + 1] == '\0' ||
                    append_char(token_buf,
                                &len,
                                cmdline[i + 1],
                                sizeof(token_buf)) != 0) {
                    free_tokens(tokens, *token_count);
                    return -1;
                }
                i += 2;
                continue;
            }

            if (cmdline[i] == '\'') {
                i++;
                while (cmdline[i] != '\0' && cmdline[i] != '\'') {
                    if (append_char(token_buf,
                                    &len,
                                    cmdline[i],
                                    sizeof(token_buf)) != 0) {
                        free_tokens(tokens, *token_count);
                        return -1;
                    }
                    i++;
                }
                if (cmdline[i] != '\'') {
                    free_tokens(tokens, *token_count);
                    return -1;
                }
                i++;
                continue;
            }

            if (cmdline[i] == '"') {
                i++;
                while (cmdline[i] != '\0' && cmdline[i] != '"') {
                    if (cmdline[i] == '\\' && cmdline[i + 1] != '\0') {
                        if (append_char(token_buf,
                                        &len,
                                        cmdline[i + 1],
                                        sizeof(token_buf)) != 0) {
                            free_tokens(tokens, *token_count);
                            return -1;
                        }
                        i += 2;
                        continue;
                    }

                    if (append_char(token_buf,
                                    &len,
                                    cmdline[i],
                                    sizeof(token_buf)) != 0) {
                        free_tokens(tokens, *token_count);
                        return -1;
                    }
                    i++;
                }
                if (cmdline[i] != '"') {
                    free_tokens(tokens, *token_count);
                    return -1;
                }
                i++;
                continue;
            }

            if (append_char(token_buf, &len, cmdline[i], sizeof(token_buf)) != 0) {
                free_tokens(tokens, *token_count);
                return -1;
            }
            i++;
        }

        token_buf[len] = '\0';
        if (push_token(tokens, token_count, token_buf) != 0) {
            free_tokens(tokens, *token_count);
            return -1;
        }
    }

    return 0;
}

static int parse_pipeline(char *tokens[],
                          int token_count,
                          parsed_command_t commands[],
                          int *command_count) {
    int i;
    int current;

    memset(commands, 0, sizeof(parsed_command_t) * MAX_CMDS);
    current = 0;

    for (i = 0; i < token_count; i++) {
        char *tok;

        tok = tokens[i];
        if (strcmp(tok, "|") == 0) {
            if (commands[current].argc == 0) {
                return -1;
            }
            commands[current].argv[commands[current].argc] = NULL;
            current++;
            if (current >= MAX_CMDS) {
                return -1;
            }
            continue;
        }

        if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 ||
            strcmp(tok, "2>") == 0) {
            char *target;

            if (i + 1 >= token_count || is_operator_token(tokens[i + 1])) {
                return -1;
            }
            target = tokens[++i];
            if (strcmp(tok, "<") == 0) {
                commands[current].in_file = target;
            } else if (strcmp(tok, ">") == 0) {
                commands[current].out_file = target;
            } else {
                commands[current].err_file = target;
            }
            continue;
        }

        if (commands[current].argc >= MAX_ARGS - 1) {
            return -1;
        }

        commands[current].argv[commands[current].argc] = tok;
        commands[current].argc++;
    }

    if (commands[current].argc == 0) {
        return -1;
    }

    commands[current].argv[commands[current].argc] = NULL;
    *command_count = current + 1;
    return 0;
}

/**
 * Executa um pipeline de comandos com suporte a |, <, >, 2>.
 * Cria processos-filho para cada comando do pipeline, configurando
 * pipes entre eles e aplicando redireções de ficheiro conforme solicitado.
 * Retorna o exit code do último comando do pipeline.
 */
static int execute_pipeline(const char *cmdline) {
    parsed_command_t commands[MAX_CMDS];
    char *tokens[MAX_TOKENS];
    int pipe_fds[MAX_CMDS - 1][2];
    pid_t child_pids[MAX_CMDS];
    int token_count;
    int command_count;
    int i;
    int last_status;

    memset(tokens, 0, sizeof(tokens));
    last_status = -1;

    if (tokenize_command(cmdline, tokens, &token_count) != 0 || token_count == 0) {
        print_error("Invalid command syntax");
        return 127;
    }

    if (parse_pipeline(tokens, token_count, commands, &command_count) != 0) {
        free_tokens(tokens, token_count);
        print_error("Invalid command syntax");
        return 127;
    }

    for (i = 0; i < command_count - 1; i++) {
        if (pipe(pipe_fds[i]) < 0) {
            free_tokens(tokens, token_count);
            print_error("Failed to create pipe");
            return 127;
        }
    }

    for (i = 0; i < command_count; i++) {
        child_pids[i] = fork();
        if (child_pids[i] < 0) {
            int j;

            for (j = 0; j < command_count - 1; j++) {
                close(pipe_fds[j][0]);
                close(pipe_fds[j][1]);
            }
            free_tokens(tokens, token_count);
            print_error("Failed to fork process");
            return 127;
        }

        if (child_pids[i] == 0) {
            int j;

            if (i > 0 && dup2(pipe_fds[i - 1][0], STDIN_FILENO) < 0) {
                _exit(127);
            }
            if (i < command_count - 1 && dup2(pipe_fds[i][1], STDOUT_FILENO) < 0) {
                _exit(127);
            }

            for (j = 0; j < command_count - 1; j++) {
                close(pipe_fds[j][0]);
                close(pipe_fds[j][1]);
            }

            if (commands[i].in_file != NULL) {
                int fd;
                fd = open(commands[i].in_file, O_RDONLY);
                if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) {
                    if (fd >= 0) {
                        close(fd);
                    }
                    _exit(127);
                }
                close(fd);
            }

            if (commands[i].out_file != NULL) {
                int fd;
                fd = open(commands[i].out_file,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0644);
                if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
                    if (fd >= 0) {
                        close(fd);
                    }
                    _exit(127);
                }
                close(fd);
            }

            if (commands[i].err_file != NULL) {
                int fd;
                fd = open(commands[i].err_file,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          0644);
                if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) {
                    if (fd >= 0) {
                        close(fd);
                    }
                    _exit(127);
                }
                close(fd);
            }

            execvp(commands[i].argv[0], commands[i].argv);
            _exit(127);
        }
    }

    for (i = 0; i < command_count - 1; i++) {
        close(pipe_fds[i][0]);
        close(pipe_fds[i][1]);
    }

    for (i = 0; i < command_count; i++) {
        int status;

        if (waitpid(child_pids[i], &status, 0) < 0) {
            continue;
        }

        if (child_pids[i] == child_pids[command_count - 1]) {
            last_status = status;
        }
    }

    free_tokens(tokens, token_count);

    if (last_status >= 0 && WIFEXITED(last_status)) {
        return WEXITSTATUS(last_status);
    }
    if (last_status >= 0 && WIFSIGNALED(last_status)) {
        return 128 + WTERMSIG(last_status);
    }
    return 127;
}

/* ═══════════════════════════════════════════════════════════════════
 * Modos de operação do runner: -e, -c, -s
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * Modo -e: submete um comando ao controller e executa-o após autorização.
 * 1. Cria FIFO privado para receber resposta
 * 2. Envia MSG_SUBMIT ao controller
 * 3. Aguarda MSG_START
 * 4. Executa o pipeline em processo-filho
 * 5. Envia MSG_FINISH ao controller
 */
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

    {
        char buf[128];
        int msg_len = snprintf(buf, sizeof(buf), "[runner] command %lu submitted\n", cmd_id);
        write(STDOUT_FILENO, buf, msg_len);
    }

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

    {
        char buf[128];
        int msg_len = snprintf(buf, sizeof(buf), "[runner] executing command %lu...\n", cmd_id);
        write(STDOUT_FILENO, buf, msg_len);
    }

    /* Execute the command */
    cmd_pid = fork();
    if (cmd_pid < 0) {
        perror("fork");
        return 1;
    }

    if (cmd_pid == 0) {
        int cmd_rc;

        cmd_rc = execute_pipeline(command);
        _exit(cmd_rc);
    } else {
        /* Parent process: wait for child to complete */
        if (waitpid(cmd_pid, &status, 0) < 0) {
            perror("waitpid");
            return 1;
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

        {
            char buf[128];
            int msg_len = snprintf(buf, sizeof(buf), "[runner] command %lu finished\n", cmd_id);
            write(STDOUT_FILENO, buf, msg_len);
        }
        
        return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
    }
}

/**
 * Modo -c: consulta o estado do controller (comandos em execução e em espera).
 * Envia MSG_STATUS_REQ e apresenta a resposta ao utilizador.
 */
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
    write(STDOUT_FILENO, "--Executing\n", 12);
    for (i = 0; i < resp.executing_count; i++) {
        char buf[128];
        int msg_len = snprintf(buf, sizeof(buf), "user-id %d - command-id %lu\n", 
                               resp.executing[i].user_id, 
                               resp.executing[i].command_id);
        write(STDOUT_FILENO, buf, msg_len);
    }

    write(STDOUT_FILENO, "--Scheduled\n", 12);
    for (i = 0; i < resp.scheduled_count; i++) {
        char buf[128];
        int msg_len = snprintf(buf, sizeof(buf), "user-id %d - command-id %lu\n", 
                               resp.scheduled[i].user_id, 
                               resp.scheduled[i].command_id);
        write(STDOUT_FILENO, buf, msg_len);
    }

    return 0;
}

/**
 * Modo -s: pede o encerramento gracioso do controller.
 * Envia MSG_SHUTDOWN_REQ e aguarda MSG_SHUTDOWN_ACK.
 * O controller só encerra após todos os runners terminarem.
 */
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

    write(STDOUT_FILENO, "[runner] sent shutdown notification\n", 36);
    write(STDOUT_FILENO, "[runner] waiting for controller to shutdown...\n", 47);

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
        write(STDOUT_FILENO, "[runner] controller exited.\n", 28);
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
