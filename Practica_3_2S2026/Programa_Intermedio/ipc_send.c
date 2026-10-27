#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_IPC_CHANNEL_SEND 548

int main(int argc, char *argv[])
{
    long ret;

    if (argc != 2) {
        fprintf(stderr, "Uso: %s \"mensaje\"\n", argv[0]);
        return EXIT_FAILURE;
    }

    errno = 0;
    ret = syscall(SYS_IPC_CHANNEL_SEND, argv[1]);

    if (ret == -1) {
        fprintf(stderr, "Error ipc_channel_send: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Mensaje enviado correctamente.\n");
    printf("Contenido: %s\n", argv[1]);
    return EXIT_SUCCESS;
}
