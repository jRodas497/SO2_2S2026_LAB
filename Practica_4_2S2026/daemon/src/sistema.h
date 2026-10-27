/* Hilo de monitoreo: metricas de memoria (/proc/meminfo) y tabla de procesos. */
#ifndef SISTEMA_H
#define SISTEMA_H

#include <stddef.h>
#include "cJSON.h"

void sistema_init(int intervalo_s, int forzar_proc);

/* Cuerpo del hilo: muestrea cada "intervalo" y publica por SSE. */
void *sistema_thread(void *arg);

/* JSON del enunciado (memoria_usada, memoria_libre, memoria_cache, swap_usada,
 * procesos[], eventos_seguridad[]) mas campos auxiliares. Lo libera quien llama. */
cJSON *datos_json(void);

/* sys_get_process_info. Devuelve 0 y rellena *out; si falla, motivo en err. */
int proceso_json(int pid, cJSON **out, char *err, size_t errn);

#endif
