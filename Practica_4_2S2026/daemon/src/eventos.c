#define _GNU_SOURCE
#include "eventos.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"
#include "http.h"
#include "ksys.h"

#define MAX_EVENTOS 200
#define MAX_LW      16
#define PATH_LEN    256     /* SO2_MAX_PATH del kernel */
#define KEYWORD_LEN 64      /* SO2_MAX_KEYWORD del kernel */

typedef struct {
	unsigned long id;
	int monitoreo;
	char archivo[256];
	char palabra[64];
	char contenido[1024];
	time_t ts;
} evento_t;

static evento_t g_ev[MAX_EVENTOS];
static int g_ev_head, g_ev_n;
static unsigned long g_ev_next = 1;
static int g_lw[MAX_LW];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------- log_watch */

int logwatch_start(const char *central, const char *const *rutas, int n,
		   const char *palabra, char *err, size_t errn)
{
	const char *krutas[EV_MAX_RUTAS];
	long id;
	int slot = -1;

	if (!central || central[0] != '/' || strlen(central) >= PATH_LEN) {
		snprintf(err, errn, "El log central debe ser una ruta absoluta de menos de %d caracteres", PATH_LEN);
		return -1;
	}
	if (n < 1 || n > EV_MAX_RUTAS) {
		snprintf(err, errn, "Debe haber de 1 a %d archivos a vigilar", EV_MAX_RUTAS);
		return -1;
	}
	for (int i = 0; i < n; i++) {
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

	errno = 0;
	id = syscall(SYS_START_LOG_WATCH, krutas, n, central, palabra);
	if (id < 0) {
		snprintf(err, errn, "sys_start_log_watch falló: %s", strerror(errno));
		return -1;
	}
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_LW; i++)
		if (!g_lw[i]) {
			g_lw[i] = (int)id;
			slot = i;
			break;
		}
	pthread_mutex_unlock(&mu);
	if (slot < 0)
		log_msg("aviso: demasiados monitoreos; el %ld no se detendra al salir", id);
	log_msg("log_watch %ld iniciado: central=%s palabra=%s archivos=%d", id, central, palabra, n);
	return (int)id;
}

void logwatch_stop_all(void)
{
	for (int i = 0; i < MAX_LW; i++) {
		int id;

		pthread_mutex_lock(&mu);
		id = g_lw[i];
		g_lw[i] = 0;
		pthread_mutex_unlock(&mu);
		if (!id)
			continue;
		if (syscall(SYS_STOP_LOG_WATCH, id) < 0)
			log_msg("no se pudo detener log_watch %d: %s", id, strerror(errno));
		else
			log_msg("log_watch %d detenido (sys_stop_log_watch)", id);
	}
}

/* --------------------------------------------------------------- eventos */

static cJSON *evento_a_json(const evento_t *e)
{
	char ts[32];
	cJSON *o = cJSON_CreateObject();

	fmt_time(e->ts, ts, sizeof(ts));
	cJSON_AddNumberToObject(o, "id", (double)e->id);
	cJSON_AddStringToObject(o, "archivo_origen", e->archivo);
	cJSON_AddStringToObject(o, "palabra_detectada", e->palabra);
	cJSON_AddStringToObject(o, "contenido", e->contenido);
	cJSON_AddStringToObject(o, "timestamp", ts);
	cJSON_AddNumberToObject(o, "monitoreo", e->monitoreo);
	return o;
}

cJSON *eventos_json(int max)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	if (max > g_ev_n)
		max = g_ev_n;
	for (int i = 0; i < max; i++) {
		int idx = (g_ev_head - 1 - i + MAX_EVENTOS) % MAX_EVENTOS;

		cJSON_AddItemToArray(arr, evento_a_json(&g_ev[idx]));
	}
	pthread_mutex_unlock(&mu);
	return arr;
}

static void registrar_evento(int monitoreo, const char *archivo, const char *palabra,
			     const char *contenido)
{
	evento_t e;
	cJSON *j;
	char *s;

	memset(&e, 0, sizeof(e));
	e.monitoreo = monitoreo;
	e.ts = time(NULL);
	snprintf(e.archivo, sizeof(e.archivo), "%s", archivo);
	snprintf(e.palabra, sizeof(e.palabra), "%s", palabra);
	snprintf(e.contenido, sizeof(e.contenido), "%s", contenido);

	pthread_mutex_lock(&mu);
	e.id = g_ev_next++;
	g_ev[g_ev_head] = e;
	g_ev_head = (g_ev_head + 1) % MAX_EVENTOS;
	if (g_ev_n < MAX_EVENTOS)
		g_ev_n++;
	pthread_mutex_unlock(&mu);

	log_msg("EVENTO DE SEGURIDAD [monitoreo %d] %s | %s | %s", monitoreo, archivo, palabra, contenido);

	/* Se empuja al dashboard en el mismo instante: no hay que refrescar ni sondear. */
	j = evento_a_json(&e);
	s = cJSON_PrintUnformatted(j);
	if (s) {
		sse_broadcast("evento", s);
		free(s);
	}
	cJSON_Delete(j);
}

/* Formato del kernel: SECURITY|ID|ARCHIVO|PALABRA|CONTENIDO (CONTENIDO puede traer '|') */
static int parsear(char *msg, char *campo[5])
{
	char *p = msg;

	for (int i = 0; i < 4; i++) {
		campo[i] = p;
		p = strchr(p, '|');
		if (!p)
			return -1;
		*p++ = '\0';
	}
	campo[4] = p;
	return strcmp(campo[0], "SECURITY") == 0 ? 0 : -1;
}

static void sig_noop(int s) { (void)s; }

void *ipc_thread(void *arg)
{
	char buf[KS_IPC_MSGLEN * 2];
	struct sigaction sa;
	int enosys_avisado = 0;

	(void)arg;
	/* SIGUSR1 sin SA_RESTART: interrumpe la syscall dormida al apagar el daemon. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_noop;
	sigaction(SIGUSR1, &sa, NULL);

	log_msg("hilo IPC iniciado: dormido en sys_ipc_channel_receive (sin polling)");
	while (g_running) {
		char *c[5];
		long n;

		memset(buf, 0, sizeof(buf));
		errno = 0;
		n = syscall(SYS_IPC_CHANNEL_RECEIVE, buf);   /* el proceso duerme hasta que llegue un mensaje */
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ENOSYS) {
				if (!enosys_avisado)
					log_msg("sys_ipc_channel_receive no existe en este kernel: sin eventos de seguridad");
				enosys_avisado = 1;
				msleep_run(5000);
				continue;
			}
			log_msg("sys_ipc_channel_receive falló: %s", strerror(errno));
			msleep_run(1000);       /* evita un bucle caliente si el error persiste */
			continue;
		}
		buf[sizeof(buf) - 1] = '\0';

		if (parsear(buf, c) < 0) {
			log_msg("mensaje IPC con otro formato (se ignora): \"%s\"", buf);
			continue;
		}
		registrar_evento(atoi(c[1]), c[2], c[3], c[4]);
	}
	log_msg("hilo IPC terminado");
	return NULL;
}
