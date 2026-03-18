#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>

#define CONTROLLER_FIFO_PATH "/tmp/so_controller_fifo"
#define RUNNER_FIFO_PATH_MAX 128
#define COMMAND_TEXT_MAX 512
#define STATUS_MAX_ITEMS 64

typedef enum message_type {
    MSG_SUBMIT = 1,
    MSG_START = 2,
    MSG_FINISH = 3,
    MSG_STATUS_REQ = 4,
    MSG_STATUS_RESP = 5,
    MSG_SHUTDOWN_REQ = 6,
    MSG_SHUTDOWN_ACK = 7,
    MSG_ERROR = 8
} message_type_t;

typedef struct message {
    int type;
    int user_id;
    unsigned long command_id;
    pid_t runner_pid;
    char reply_fifo[RUNNER_FIFO_PATH_MAX];
    char command[COMMAND_TEXT_MAX];
} message_t;

typedef struct status_item {
    int user_id;
    unsigned long command_id;
} status_item_t;

typedef struct response {
    int type;
    int ok;
    int executing_count;
    int scheduled_count;
    status_item_t executing[STATUS_MAX_ITEMS];
    status_item_t scheduled[STATUS_MAX_ITEMS];
    char info[128];
} response_t;

#endif
