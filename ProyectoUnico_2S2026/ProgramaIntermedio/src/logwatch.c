#define _GNU_SOURCE
#include "logwatch.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "alerts.h"
#include "common.h"
#include "ksys.h"

#define MAX_LW        32
#define PATH_LEN      256     /* SO2_MAX_PATH del kernel */
#define KEYWORD_LEN   64      /* SO2_MAX_KEYWORD del kernel */

typedef struct {
	int id;
	char central[PATH_LEN];
	char palabra[KEYWORD_LEN];
	int nrutas;
	char rutas[LW_MAX_RUTAS][PATH_LEN];
	char por[40];
	time_t inicio;
	int used;
} lw_t;

static lw_t g_lw[MAX_LW];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

int lw_start(const char *central, const char *const *rutas, int nrutas,
	     const char *palabra, const char *quien, char *err, size_t errn)
{
	const char *krutas[LW_MAX_RUTAS];
	lw_t *slot = NULL;
	long id;

	if (!central || central[0] != '/' || strlen(central) >= PATH_LEN) {
		snprintf(err, errn, "El log central debe ser una ruta absoluta de menos de %d caracteres", PATH_LEN);
		return -1;
	}
	if (nrutas < 1 || nrutas > LW_MAX_RUTAS) {
		snprintf(err, errn, "Debe indicar de 1 a %d archivos a vigilar", LW_MAX_RUTAS);
		return -1;
	}
	for (int i = 0; i < nrutas; i++) {
		if (!rutas[i] || rutas[i][0] != '/' || strlen(rutas[i]) >= PATH_LEN) {
			snprintf(err, errn, "La ruta %d debe ser absoluta y de menos de %d caracteres", i + 1, PATH_LEN);
			return -1;
		}
		if (strcmp(rutas[i], central) == 0) {
			snprintf(err, errn, "El log central no puede ser uno de los archivos vigilados");
			return -1;
		}
		krutas[i] = rutas[i];
	}
	/* '|' separa los campos del mensaje que llega por IPC */
	if (!palabra || !*palabra || strlen(palabra) >= KEYWORD_LEN || strchr(palabra, '|')) {
		snprintf(err, errn, "La palabra clave es obligatoria, de menos de %d caracteres y sin '|'", KEYWORD_LEN);
		return -1;
	}

	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_LW; i++)
		if (!g_lw[i].used) {
			slot = &g_lw[i];
			break;
		}
	if (!slot) {
		pthread_mutex_unlock(&mu);
		snprintf(err, errn, "Demasiados monitoreos activos");
		return -1;
	}
	slot->used = 1;                     /* reservar mientras se llama al kernel */
	pthread_mutex_unlock(&mu);

	errno = 0;
	id = syscall(SYS_START_LOG_WATCH, krutas, nrutas, central, palabra);
	if (id < 0) {
		int e = errno;

		pthread_mutex_lock(&mu);
		slot->used = 0;
		pthread_mutex_unlock(&mu);
		snprintf(err, errn, "start_log_watch fallo: %s", strerror(e));
		return -1;
	}

	pthread_mutex_lock(&mu);
	memset(slot, 0, sizeof(*slot));
	slot->used = 1;
	slot->id = (int)id;
	snprintf(slot->central, sizeof(slot->central), "%s", central);
	snprintf(slot->palabra, sizeof(slot->palabra), "%s", palabra);
	slot->nrutas = nrutas;
	for (int i = 0; i < nrutas; i++)
		snprintf(slot->rutas[i], PATH_LEN, "%s", rutas[i]);
	snprintf(slot->por, sizeof(slot->por), "%s", quien);
	slot->inicio = time(NULL);
	pthread_mutex_unlock(&mu);

	log_msg("logwatch %ld iniciado por %s: central=%s palabra=%s archivos=%d",
		id, quien, central, palabra, nrutas);
	return (int)id;
}

int lw_stop(int id, const char *quien, char *err, size_t errn)
{
	lw_t *slot = NULL;

	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_LW; i++)
		if (g_lw[i].used && g_lw[i].id == id) {
			slot = &g_lw[i];
			break;
		}
	pthread_mutex_unlock(&mu);
	if (!slot) {
		snprintf(err, errn, "No existe el monitoreo %d", id);
		return -1;
	}

	errno = 0;
	if (syscall(SYS_STOP_LOG_WATCH, id) < 0) {
		snprintf(err, errn, "stop_log_watch fallo: %s", strerror(errno));
		return -1;
	}
	pthread_mutex_lock(&mu);
	if (slot->used && slot->id == id)
		slot->used = 0;
	pthread_mutex_unlock(&mu);
	log_msg("logwatch %d detenido por %s", id, quien);
	return 0;
}

void lw_stop_all(void)
{
	for (int i = 0; i < MAX_LW; i++) {
		int id = 0;

		pthread_mutex_lock(&mu);
		if (g_lw[i].used)
			id = g_lw[i].id;
		pthread_mutex_unlock(&mu);
		if (id) {
			char err[128];

			if (lw_stop(id, "daemon", err, sizeof(err)) < 0)
				log_msg("no se pudo detener logwatch %d: %s", id, err);
		}
	}
}

cJSON *lw_list_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_LW; i++) {
		char ts[32];
		cJSON *o, *rs;

		if (!g_lw[i].used)
			continue;
		o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", g_lw[i].id);
		cJSON_AddStringToObject(o, "central", g_lw[i].central);
		cJSON_AddStringToObject(o, "palabra", g_lw[i].palabra);
		rs = cJSON_AddArrayToObject(o, "rutas");
		for (int k = 0; k < g_lw[i].nrutas; k++)
			cJSON_AddItemToArray(rs, cJSON_CreateString(g_lw[i].rutas[k]));
		fmt_time(g_lw[i].inicio, ts, sizeof(ts));
		cJSON_AddStringToObject(o, "iniciado", ts);
		cJSON_AddStringToObject(o, "por", g_lw[i].por);
		cJSON_AddItemToArray(arr, o);
	}
	pthread_mutex_unlock(&mu);
	return arr;
}

/* Formato del kernel: SECURITY|ID|ARCHIVO|PALABRA|CONTENIDO (CONTENIDO puede traer '|') */
static int parse_msg(char *msg, char *f[5])
{
	char *p = msg;

	for (int i = 0; i < 4; i++) {
		f[i] = p;
		p = strchr(p, '|');
		if (!p)
			return -1;
		*p++ = '\0';
	}
	f[4] = p;
	return strcmp(f[0], "SECURITY") == 0 ? 0 : -1;
}

static void sig_noop(int s) { (void)s; }

void *ipc_thread(void *arg)
{
	char buf[KS_IPC_MSGLEN * 2];
	struct sigaction sa;

	(void)arg;
	/* SIGUSR1 sin SA_RESTART: interrumpe la syscall bloqueada al apagar el daemon. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_noop;
	sigaction(SIGUSR1, &sa, NULL);

	log_msg("hilo IPC iniciado: bloqueado en sys_ipc_channel_receive (sin polling)");
	while (g_running) {
		char *f[5];
		long n;

		memset(buf, 0, sizeof(buf));
		errno = 0;
		n = syscall(SYS_IPC_CHANNEL_RECEIVE, buf);       /* duerme hasta que llegue un mensaje */
		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_msg("ipc_channel_receive fallo: %s", strerror(errno));
			msleep_run(1000);       /* evita un bucle caliente si el error persiste */
			continue;
		}
		buf[sizeof(buf) - 1] = '\0';

		if (parse_msg(buf, f) < 0) {
			log_msg("mensaje IPC ajeno al formato SECURITY|...: \"%s\"", buf);
			continue;
		}
		alert_add(SEV_MEDIUM, TIPO_LOGS, "Palabra clave detectada en log",
			  "Archivo: %s | Palabra: %s | Línea: %s | Monitoreo #%s",
			  f[2], f[3], f[4], f[1]);
	}
	log_msg("hilo IPC terminado");
	return NULL;
}
