/* Base de firmas (blacklist.json): tabla hash en memoria con SHA-256 -> amenaza. */
#ifndef BLACKLIST_H
#define BLACKLIST_H

#include <stddef.h>

typedef struct {
	char nombre[96];
	char descripcion[256];
} threat_t;

/* Carga la blacklist desde archivo y recuerda la ruta para persistir actualizaciones. */
int bl_load_file(const char *path, char *err, size_t errn);

/*
 * Reemplaza la blacklist con el JSON recibido (subido desde el dashboard).
 * Valida el formato completo ANTES de reemplazar nada. Si persist != 0 guarda
 * el JSON en el archivo de la blacklist. Devuelve el numero de firmas o -1.
 */
int bl_replace(const char *json, size_t len, int persist, char *err, size_t errn);

/* hash_hex: 64 caracteres hexadecimales (cualquier capitalizacion). 1 = encontrado. */
int bl_lookup(const char *hash_hex, threat_t *out);

int bl_count(void);

#endif
