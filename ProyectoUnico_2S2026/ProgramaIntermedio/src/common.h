/* Utilidades comunes: log, tiempo, lectura de archivos y bandera de ejecucion. */
#ifndef COMMON_H
#define COMMON_H

#include <signal.h>
#include <stddef.h>
#include <time.h>

/* 0 cuando se pidio terminar (SIGINT/SIGTERM). Todos los hilos la consultan. */
extern volatile sig_atomic_t g_running;

void log_init(const char *path);        /* NULL = stderr */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fmt_time(time_t t, char *buf, size_t n);   /* "AAAA-MM-DD HH:MM:SS" */

/* Duerme en tramos de 100 ms; vuelve antes si g_running pasa a 0. */
void msleep_run(int ms);

/* Lee un archivo completo (max bytes). *out se libera con free(). 0 = ok. */
int read_file(const char *path, char **out, size_t *len, size_t max);

#endif
