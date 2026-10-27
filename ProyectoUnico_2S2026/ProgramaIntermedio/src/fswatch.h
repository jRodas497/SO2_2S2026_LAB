/* Hilo 2: sensor eBPF de archivos, SHA-256, listado de archivos y cuarentena. */
#ifndef FSWATCH_H
#define FSWATCH_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/* Carga el programa eBPF (sin engancharlo). dir puede ser NULL. 0 = ok. */
int fs_init(const char *dir);

/* Cuerpo del hilo: consume el ring buffer. */
void *fs_thread(void *arg);

void fs_shutdown(void);

/* Activar/desactivar el escaneo = attach/detach de los programas eBPF (libbpf). */
int fs_set_enabled(bool on, char *err, size_t errn);
bool fs_is_enabled(void);
bool fs_is_available(void);

/* Ruta a monitorear: actualiza el mapa eBPF "cfg_map". Solo archivos directos. */
int fs_set_dir(const char *dir, char *err, size_t errn);
void fs_get_dir(char *out, size_t n);

/* Cuarentena manual (chmod 0000). Solo archivos ya analizados dentro del directorio. */
int fs_quarantine(const char *path, const char *quien, char *err, size_t errn);

/* Tras actualizar la blacklist: reevalua los archivos ya conocidos con su hash guardado. */
void fs_reevaluate(void);

cJSON *fs_files_json(void);         /* todos los archivos analizados */
cJSON *fs_threats_json(void);       /* amenazas detectadas */
cJSON *fs_quarantine_json(void);    /* archivos en cuarentena */

#endif
