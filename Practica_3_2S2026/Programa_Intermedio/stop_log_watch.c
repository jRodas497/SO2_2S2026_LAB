#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_STOP_LOG_WATCH 551

static int leer_id_archivo(void)
{
    FILE *f = fopen("monitor.id", "r");
    int id;

    if (!f)
        return -1;

    if (fscanf(f, "%d", &id) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return id;
}

int main(int argc, char *argv[])
{
    int id;
    long ret;

    if (argc == 2) {
        char *end = NULL;
        long parsed = strtol(argv[1], &end, 10);

        if (!argv[1][0] || (end && *end != '\0') || parsed <= 0) {
            fprintf(stderr, "ID invalido: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        id = (int)parsed;
    } else if (argc == 1) {
        id = leer_id_archivo();
        if (id <= 0) {
            fprintf(stderr,
                    "No se proporciono un ID y no se pudo leer monitor.id\n");
            return EXIT_FAILURE;
        }
        printf("ID leido desde monitor.id: %d\n", id);
    } else {
        fprintf(stderr, "Uso: %s [ID_MONITOREO]\n", argv[0]);
        return EXIT_FAILURE;
    }

    errno = 0;
    ret = syscall(SYS_STOP_LOG_WATCH, id);

    if (ret == -1) {
        fprintf(stderr, "Error stop_log_watch(%d): %s\n", id, strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Monitoreo %d detenido correctamente.\n", id);
    remove("monitor.id");
    return EXIT_SUCCESS;
}
