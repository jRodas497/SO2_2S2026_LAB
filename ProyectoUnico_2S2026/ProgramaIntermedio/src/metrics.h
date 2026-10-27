/* Hilo 1: metricas del sistema (/proc), top 20 de procesos y control de procesos. */
#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"

void metrics_init(int interval_sec);

/* Cuerpo del hilo de monitoreo. */
void *monitor_thread(void *arg);

/* Interruptores (solo administrador). "quien" es el usuario que lo pidio. */
void monitor_set_enabled(bool on, const char *quien);
void procscan_set_enabled(bool on, const char *quien);
bool monitor_is_enabled(void);
bool procscan_is_enabled(void);

/* JSON para el dashboard */
cJSON *metrics_last_json(void);      /* ultima muestra (o {"disponible":false}) */
cJSON *metrics_history_json(void);   /* ultimas muestras, de la mas vieja a la mas nueva */
cJSON *procs_last_json(void);        /* {"resumen":{...},"top":[...]} */

/* sys_get_process_info. Devuelve 0 y rellena *out; si falla escribe el motivo en err. */
int proc_info_json(int pid, cJSON **out, char *err, size_t errn);

/* sys_kill con las protecciones: no PID 1, no el propio daemon, no hilos del kernel. */
int proc_kill(int pid, const char *quien, char *err, size_t errn);

#endif
