/* Hilo IPC: eventos de seguridad de log_watch (bloqueado en sys_ipc_channel_receive). */
#ifndef EVENTOS_H
#define EVENTOS_H

#include <stddef.h>
#include "cJSON.h"

#define EV_MAX_RUTAS 8

/* sys_start_log_watch. Devuelve el id (>0) o -1 con el motivo en err. */
int logwatch_start(const char *central, const char *const *rutas, int n,
		   const char *palabra, char *err, size_t errn);
void logwatch_stop_all(void);

/* Ultimos "max" eventos, el mas reciente primero. */
cJSON *eventos_json(int max);

/* Cuerpo del hilo: duerme en el kernel hasta que log_watch notifica. SIN polling. */
void *ipc_thread(void *arg);

#endif
