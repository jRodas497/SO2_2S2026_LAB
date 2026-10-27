#define _GNU_SOURCE
#include "alerts.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "http.h"

#define MAX_ALERTS 500

typedef struct {
	unsigned long id;
	time_t ts;
	char tipo[16];
	char severidad[8];
	char titulo[64];
	char descripcion[512];
} alert_t;

static alert_t ring[MAX_ALERTS];
static int head;            /* siguiente posicion a escribir */
static int count;
static unsigned long next_id = 1;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void alerts_init(void)
{
	pthread_mutex_lock(&mu);
	head = count = 0;
	next_id = 1;
	pthread_mutex_unlock(&mu);
}

static cJSON *to_json(const alert_t *a)
{
	char ts[32];
	cJSON *o = cJSON_CreateObject();

	fmt_time(a->ts, ts, sizeof(ts));
	cJSON_AddNumberToObject(o, "id", (double)a->id);
	cJSON_AddStringToObject(o, "tipo", a->tipo);
	cJSON_AddStringToObject(o, "severidad", a->severidad);
	cJSON_AddStringToObject(o, "titulo", a->titulo);
	cJSON_AddStringToObject(o, "descripcion", a->descripcion);
	cJSON_AddStringToObject(o, "timestamp", ts);
	cJSON_AddNumberToObject(o, "ts", (double)a->ts);
	return o;
}

void alert_add(const char *severidad, const char *tipo, const char *titulo,
	       const char *fmt, ...)
{
	alert_t a;
	va_list ap;
	cJSON *j;
	char *s;

	memset(&a, 0, sizeof(a));
	a.ts = time(NULL);
	snprintf(a.tipo, sizeof(a.tipo), "%s", tipo);
	snprintf(a.severidad, sizeof(a.severidad), "%s", severidad);
	snprintf(a.titulo, sizeof(a.titulo), "%s", titulo);
	va_start(ap, fmt);
	vsnprintf(a.descripcion, sizeof(a.descripcion), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&mu);
	a.id = next_id++;
	ring[head] = a;
	head = (head + 1) % MAX_ALERTS;
	if (count < MAX_ALERTS)
		count++;
	pthread_mutex_unlock(&mu);

	log_msg("ALERTA [%s] (%s) %s: %s", a.severidad, a.tipo, a.titulo, a.descripcion);

	j = to_json(&a);
	s = cJSON_PrintUnformatted(j);
	if (s) {
		sse_broadcast("alert", s);
		free(s);
	}
	cJSON_Delete(j);
}

cJSON *alerts_json(int max)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	if (max > count)
		max = count;
	for (int i = 0; i < max; i++) {
		int idx = (head - 1 - i + MAX_ALERTS) % MAX_ALERTS;

		cJSON_AddItemToArray(arr, to_json(&ring[idx]));
	}
	pthread_mutex_unlock(&mu);
	return arr;
}
