#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_START_LOG_WATCH 550
#define MAX_LOG_FILES 8

int main(int argc, char *argv[])
{
    char *rutas[MAX_LOG_FILES];
    const char *palabra;
    const char *central;
    int cantidad;
    long id;
    FILE *id_file;

    if (argc < 4) {
        fprintf(stderr,
                "Uso: %s PALABRA_CLAVE LOG_CENTRAL LOG1 [LOG2 ... LOG8]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    palabra = argv[1];
    central = argv[2];
    cantidad = argc - 3;

    if (cantidad > MAX_LOG_FILES) {
        fprintf(stderr, "Maximo %d archivos de log.\n", MAX_LOG_FILES);
        return EXIT_FAILURE;
    }

    if (central[0] != '/') {
        fprintf(stderr, "LOG_CENTRAL debe ser una ruta absoluta.\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < cantidad; ++i) {
        rutas[i] = argv[i + 3];
        if (rutas[i][0] != '/') {
            fprintf(stderr, "La ruta del log %d debe ser absoluta: %s\n",
                    i + 1, rutas[i]);
            return EXIT_FAILURE;
        }
    }

    errno = 0;
    id = syscall(SYS_START_LOG_WATCH, rutas, cantidad, central, palabra);

    if (id == -1) {
        fprintf(stderr, "Error start_log_watch: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Monitoreo iniciado correctamente.\n");
    printf("ID de monitoreo: %ld\n", id);
    printf("Palabra clave: %s\n", palabra);
    printf("Log central: %s\n", central);
    printf("Archivos vigilados: %d\n", cantidad);
    for (int i = 0; i < cantidad; ++i)
        printf("  [%d] %s\n", i + 1, rutas[i]);

    id_file = fopen("monitor.id", "w");
    if (id_file) {
        fprintf(id_file, "%ld\n", id);
        fclose(id_file);
        printf("ID guardado en monitor.id\n");
    } else {
        fprintf(stderr, "Advertencia: no se pudo crear monitor.id; anote el ID.\n");
    }

    return EXIT_SUCCESS;
}
