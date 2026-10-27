#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_IPC_CHANNEL_RECEIVE 551
#define IPC_BUFFER_SIZE 256

int main(void)
{
    char buffer[IPC_BUFFER_SIZE] = {0};
    long ret;

    printf("Esperando mensaje IPC...\n");
    printf("El proceso queda bloqueado sin polling hasta que llegue un mensaje.\n");
    fflush(stdout);

    errno = 0;
    ret = syscall(SYS_IPC_CHANNEL_RECEIVE, buffer);

    if (ret == -1) {
        fprintf(stderr, "Error ipc_channel_receive: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Mensaje recibido correctamente.\n");
    printf("Bytes recibidos: %ld\n", ret);
    printf("Contenido: %s\n", buffer);
    return EXIT_SUCCESS;
}
