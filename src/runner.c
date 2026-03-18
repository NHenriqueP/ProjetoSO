#include <string.h>
#include <unistd.h>

int main(void) {
    const char *msg =
        "runner: implementação base pendente (Pessoa B)\n";
    (void)write(STDOUT_FILENO, msg, strlen(msg));
    return 0;
}
