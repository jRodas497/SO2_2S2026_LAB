#define _GNU_SOURCE
#include "blacklist.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"

#define BUCKETS      1024
#define MAX_FIRMAS   100000
#define MAX_JSON     (1 << 20)

typedef struct node {
	char hash[65];          /* siempre en minusculas */
	threat_t t;
	struct node *next;
} node_t;

static node_t *table[BUCKETS];
static int g_count;
static char g_path[PATH_MAX];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash_str(const char *s)
{
	unsigned h = 2166136261u;           /* FNV-1a */

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h % BUCKETS;
}

static int valid_hex64(const char *s)
{
	if (!s || strlen(s) != 64)
		return 0;
	for (int i = 0; i < 64; i++)
		if (!isxdigit((unsigned char)s[i]))
			return 0;
	return 1;
}

static void lower64(const char *src, char *dst)
{
	for (int i = 0; i < 64; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[64] = '\0';
}

static void table_free(node_t **t)
{
	for (int i = 0; i < BUCKETS; i++) {
		node_t *n = t[i];

		while (n) {
			node_t *next = n->next;

			free(n);
			n = next;
		}
		t[i] = NULL;
	}
}

/* Construye una tabla nueva. No toca la tabla vigente. */
static int build(const char *json, size_t len, node_t **t, int *count,
		 char *err, size_t errn)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	cJSON *it;
	int n = 0;

	if (!root) {
		snprintf(err, errn, "JSON invalido (error de sintaxis)");
		return -1;
	}
	if (!cJSON_IsObject(root)) {
		snprintf(err, errn, "El JSON debe ser un objeto {\"NombreAmenaza\": {\"hash\": ..., \"descripcion\": ...}}");
		goto fail;
	}

	cJSON_ArrayForEach(it, root) {
		cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "hash");
		cJSON *d = cJSON_GetObjectItemCaseSensitive(it, "descripcion");
		node_t *node;
		unsigned b;

		if (!it->string || !*it->string || strlen(it->string) >= sizeof(node->t.nombre)) {
			snprintf(err, errn, "Nombre de amenaza vacio o demasiado largo");
			goto fail;
		}
		if (!cJSON_IsObject(it)) {
			snprintf(err, errn, "'%s': el valor debe ser un objeto con 'hash' y 'descripcion'", it->string);
			goto fail;
		}
		if (!cJSON_IsString(h) || !valid_hex64(h->valuestring)) {
			snprintf(err, errn, "'%s': 'hash' debe ser un SHA-256 (64 caracteres hexadecimales)", it->string);
			goto fail;
		}
		if (d && !cJSON_IsString(d)) {
			snprintf(err, errn, "'%s': 'descripcion' debe ser texto", it->string);
			goto fail;
		}
		if (++n > MAX_FIRMAS) {
			snprintf(err, errn, "Demasiadas firmas (maximo %d)", MAX_FIRMAS);
			goto fail;
		}

		node = calloc(1, sizeof(*node));
		if (!node) {
			snprintf(err, errn, "Sin memoria");
			goto fail;
		}
		lower64(h->valuestring, node->hash);
		snprintf(node->t.nombre, sizeof(node->t.nombre), "%s", it->string);
		snprintf(node->t.descripcion, sizeof(node->t.descripcion), "%s",
			 d ? d->valuestring : "");
		b = hash_str(node->hash);
		node->next = t[b];
		t[b] = node;
	}
	if (n == 0) {
		snprintf(err, errn, "La blacklist esta vacia");
		goto fail;
	}
	*count = n;
	cJSON_Delete(root);
	return 0;
fail:
	table_free(t);
	cJSON_Delete(root);
	return -1;
}

static int persist_file(const char *json, size_t len)
{
	char tmp[PATH_MAX + 8];
	FILE *f;

	if (!g_path[0])
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	if (fwrite(json, 1, len, f) != len || fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	return rename(tmp, g_path);
}

int bl_replace(const char *json, size_t len, int persist, char *err, size_t errn)
{
	node_t *fresh[BUCKETS] = { NULL };
	int count = 0;

	if (len > MAX_JSON) {
		snprintf(err, errn, "Archivo demasiado grande");
		return -1;
	}
	if (build(json, len, fresh, &count, err, errn) < 0)
		return -1;

	pthread_mutex_lock(&mu);
	for (int i = 0; i < BUCKETS; i++) {
		node_t *old = table[i];

		table[i] = fresh[i];
		fresh[i] = old;         /* lo viejo se libera fuera del lock */
	}
	g_count = count;
	pthread_mutex_unlock(&mu);
	table_free(fresh);

	if (persist && persist_file(json, len) < 0)
		log_msg("blacklist: no se pudo guardar en %s: %s", g_path, strerror(errno));
	log_msg("blacklist: %d firmas cargadas", count);
	return count;
}

int bl_load_file(const char *path, char *err, size_t errn)
{
	char *text = NULL;
	size_t len = 0;
	int rc;

	snprintf(g_path, sizeof(g_path), "%s", path);
	if (read_file(path, &text, &len, MAX_JSON) < 0) {
		snprintf(err, errn, "No se pudo leer %s: %s", path, strerror(errno));
		return -1;
	}
	rc = bl_replace(text, len, 0, err, errn);
	free(text);
	return rc;
}

int bl_lookup(const char *hash_hex, threat_t *out)
{
	char key[65];
	int found = 0;

	if (!valid_hex64(hash_hex))
		return 0;
	lower64(hash_hex, key);

	pthread_mutex_lock(&mu);
	for (node_t *n = table[hash_str(key)]; n; n = n->next) {
		if (strcmp(n->hash, key) == 0) {
			if (out)
				*out = n->t;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&mu);
	return found;
}

int bl_count(void)
{
	int c;

	pthread_mutex_lock(&mu);
	c = g_count;
	pthread_mutex_unlock(&mu);
	return c;
}
