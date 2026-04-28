/**
 * @file protocol.h
 * @brief Definição do protocolo de comunicação entre runner e controller.
 *
 * Este ficheiro define as estruturas de mensagem e resposta trocadas via
 * named pipes (FIFOs). O runner envia mensagens (message_t) para o FIFO
 * público do controller e recebe respostas (response_t) num FIFO privado.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>

/* ───── Caminhos e limites ───── */
#define CONTROLLER_FIFO_PATH "/tmp/so_controller_fifo"
#define RUNNER_FIFO_PATH_MAX 128
#define COMMAND_TEXT_MAX 512
#define STATUS_MAX_ITEMS 64

/* ───── Tipos de mensagem ───── */
typedef enum message_type {
    MSG_SUBMIT       = 1,   /* Runner → Controller: submeter comando    */
    MSG_START        = 2,   /* Controller → Runner:  pode iniciar exec  */
    MSG_FINISH       = 3,   /* Runner → Controller: execução concluída  */
    MSG_STATUS_REQ   = 4,   /* Runner → Controller: pedir estado        */
    MSG_STATUS_RESP  = 5,   /* Controller → Runner:  resposta de estado */
    MSG_SHUTDOWN_REQ = 6,   /* Runner → Controller: pedir encerramento  */
    MSG_SHUTDOWN_ACK = 7,   /* Controller → Runner:  confirmação fecho  */
    MSG_ERROR        = 8    /* Controller → Runner:  erro genérico      */
} message_type_t;

/* ───── Mensagem de pedido (runner → controller) ───── */
typedef struct message {
    int type;                               /* message_type_t              */
    int user_id;                            /* identificador do utilizador */
    unsigned long command_id;               /* identificador único do cmd  */
    pid_t runner_pid;                       /* PID do runner               */
    char reply_fifo[RUNNER_FIFO_PATH_MAX];  /* FIFO privado de resposta   */
    char command[COMMAND_TEXT_MAX];          /* comando + argumentos        */
} message_t;

/* ───── Item de estado (para listagem) ───── */
typedef struct status_item {
    int user_id;
    unsigned long command_id;
} status_item_t;

/* ───── Mensagem de resposta (controller → runner) ───── */
typedef struct response {
    int type;                                 /* message_type_t            */
    int ok;                                   /* 1 = sucesso, 0 = erro    */
    int executing_count;                      /* nº comandos em execução  */
    int scheduled_count;                      /* nº comandos em espera    */
    status_item_t executing[STATUS_MAX_ITEMS];
    status_item_t scheduled[STATUS_MAX_ITEMS];
    char info[128];                           /* informação extra / erro  */
} response_t;

#endif /* PROTOCOL_H */
