#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_GETPID_COUNTER 548

int main(void)
{
    long pid;
    long contador;

    printf("=== Prueba de sys_getpid y getpid_counter ===\n\n");

    for (int i = 1; i <= 5; i++) {
        pid = syscall(SYS_getpid);
        contador = syscall(SYS_GETPID_COUNTER);

        printf(
            "Llamada %d -> PID: %ld | Contador: %ld\n",
            i,
            pid,
            contador
        );
    }

    return 0;
}

