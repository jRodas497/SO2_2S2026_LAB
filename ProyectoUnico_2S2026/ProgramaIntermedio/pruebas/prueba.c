#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_GET_PROCESS_INFO 548
#define SYS_GET_PROCESSES_STATE 549
#define PROCESS_NAME_LEN 16

struct so2_process_info {
    char name[PROCESS_NAME_LEN];
    int pid;
    uint64_t runtime_ns;
    unsigned long memory_kb;
};

#define MAX_PROCS 4096

struct so2_process_entry {
    int pid;
    char state;
    char name[PROCESS_NAME_LEN];
    unsigned long rss_kb;
};

struct so2_processes_state {
    unsigned long total;
    unsigned long running;
    unsigned long sleeping;
    unsigned long zombie;
    unsigned long count;
};

static int cmp_rss_desc(const void *a, const void *b)
{
    const struct so2_process_entry *x = a, *y = b;
    return (y->rss_kb > x->rss_kb) - (y->rss_kb < x->rss_kb);
}

static int probar_process_info(pid_t pid)
{
    struct so2_process_info info = {0};
    long ret = syscall(SYS_GET_PROCESS_INFO, pid, &info);

    if (ret == -1) {
        fprintf(stderr, "Error get_process_info(%d): %s\n", pid, strerror(errno));
        return 1;
    }

    printf("\n============================================================\n");
    printf(" INFORMACION DEL PROCESO\n");
    printf("============================================================\n");
    printf("Nombre                : %s\n", info.name);
    printf("PID                   : %d\n", info.pid);
    printf("Tiempo de ejecucion   : %.3f segundos\n",
           (double)info.runtime_ns / 1000000000.0);
    printf("Memoria aproximada    : %lu KB\n", info.memory_kb);
    return 0;
}

static int probar_processes_state(void)
{
    struct so2_processes_state state = {0};
    struct so2_process_entry *procs = calloc(MAX_PROCS, sizeof(*procs));
    long ret;

    if (!procs) {
        perror("calloc");
        return 1;
    }

    ret = syscall(SYS_GET_PROCESSES_STATE, &state, procs, MAX_PROCS);

    if (ret == -1) {
        fprintf(stderr, "Error get_processes_state: %s\n", strerror(errno));
        free(procs);
        return 1;
    }

    printf("\n============================================================\n");
    printf(" ESTADO GENERAL DE LOS PROCESOS\n");
    printf("============================================================\n");
    printf("Total                 : %lu\n", state.total);
    printf("Running               : %lu\n", state.running);
    printf("Sleeping / IDLE       : %lu\n", state.sleeping);
    printf("Zombie                : %lu\n", state.zombie);
    printf("Entradas devueltas    : %lu\n", state.count);

    qsort(procs, state.count, sizeof(*procs), cmp_rss_desc);
    printf("\nTop 5 por memoria (RSS):\n");
    printf("%-8s %-5s %-16s %s\n", "PID", "EST", "NOMBRE", "RSS (KB)");
    for (unsigned long i = 0; i < state.count && i < 5; i++)
        printf("%-8d %-5c %-16s %lu\n", procs[i].pid, procs[i].state,
               procs[i].name, procs[i].rss_kb);

    free(procs);
    return 0;
}

int main(int argc, char *argv[])
{
    pid_t pid;

    if (argc > 2) {
        fprintf(stderr, "Uso: %s [PID]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        char *end = NULL;
        long parsed = strtol(argv[1], &end, 10);
        if (!argv[1][0] || (end && *end != '\0') || parsed <= 0) {
            fprintf(stderr, "PID invalido: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        pid = (pid_t)parsed;
    } else {
        pid = getpid();
    }

    printf("Practica 2 - Sistemas Operativos 2\n");
    printf("Syscalls: get_process_info=%d, get_processes_state=%d\n",
           SYS_GET_PROCESS_INFO, SYS_GET_PROCESSES_STATE);
    printf("PID solicitado: %d\n", pid);

    if (probar_process_info(pid) != 0)
        return EXIT_FAILURE;

    if (probar_processes_state() != 0)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
