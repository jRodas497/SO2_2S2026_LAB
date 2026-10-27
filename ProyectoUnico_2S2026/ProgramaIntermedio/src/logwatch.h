/* Hilo 3: canal IPC bloqueante con log_watch + gestion de monitoreos de logs. */
#ifndef LOGWATCH_H
#define LOGWATCH_H

#include <stddef.h>
#include "cJSON.h"

#define LW_MAX_RUTAS 8

/* sys_start_log_watch. Devuelve el id (>0) o -1 con el motivo en err. */
int lw_start(const char *central, const char *const *rutas, int nrutas,
	     const char *palabra, const char *quien, char *err, size_t errn);

/* sys_stop_log_watch de un monitoreo concreto. 0 = ok. */
int lw_stop(int id, const char *quien, char *err, size_t errn);

/* Al terminar el daemon: detiene todos los monitoreos activos. */
void lw_stop_all(void);

cJSON *lw_list_json(void);

/* Cuerpo del hilo: bloqueado en sys_ipc_channel_receive, SIN polling. */
void *ipc_thread(void *arg);

#endif
