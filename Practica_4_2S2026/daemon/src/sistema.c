#define _GNU_SOURCE
#include "sistema.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"
#include "eventos.h"
#include "http.h"
#include "ksys.h"

#define HISTORIAL 120

typedef struct {
	time_t ts;
	unsigned long total, libre, disponible, buffers, cache;      /* kB */
	unsigned long swap_total, swap_libre, usada, swap_usada;      /* kB */
} mem_t;

typedef struct {
	int pid;
	char state;
	char name[KS_COMM_LEN];
	unsigned long rss_kb;
} proc_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int g_intervalo = 2;
static int g_forzar_proc;

static mem_t g_mem, g_hist[HISTORIAL];
static int g_tiene_mem, g_hist_n, g_hist_head;

static proc_t *g_procs;
static int g_nprocs;
static struct ks_processes_state g_resumen;
static char g_fuente[160] = "sin datos";

void sistema_init(int intervalo_s, int forzar_proc)
{
	if (intervalo_s >= 1 && intervalo_s <= 60)
		g_intervalo = intervalo_s;
	g_forzar_proc = forzar_proc;
}

/* ------------------------------------------------------------ /proc/meminfo */

static int meminfo_valor(const char *texto, const char *clave, unsigned long *out)
{
	size_t kl = strlen(clave);
	const char *p = texto;

	while (p && *p) {
		if (strncmp(p, clave, kl) == 0 && p[kl] == ':') {
			*out = strtoul(p + kl + 1, NULL, 10);
			return 0;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return -1;
}

static int leer_memoria(mem_t *m)
{
	char *txt = NULL;
	unsigned long cached = 0;
	int rc = -1;

	memset(m, 0, sizeof(*m));
	if (read_file("/proc/meminfo", &txt, NULL, 1 << 16) < 0) {
		log_msg("memoria: no se pudo leer /proc/meminfo: %s", strerror(errno));
		return -1;
	}
	if (meminfo_valor(txt, "MemTotal", &m->total) || meminfo_valor(txt, "MemFree", &m->libre) ||
	    meminfo_valor(txt, "MemAvailable", &m->disponible) || meminfo_valor(txt, "Buffers", &m->buffers) ||
	    meminfo_valor(txt, "Cached", &cached) || meminfo_valor(txt, "SwapTotal", &m->swap_total) ||
	    meminfo_valor(txt, "SwapFree", &m->swap_libre)) {
		log_msg("memoria: /proc/meminfo con formato inesperado");
		goto out;
	}
	m->ts = time(NULL);
	m->cache = cached + m->buffers;              /* caché de páginas + buffers */
	{
		unsigned long no_usada = m->libre + m->cache;

		m->usada = m->total > no_usada ? m->total - no_usada : 0;
		m->swap_usada = m->swap_total > m->swap_libre ? m->swap_total - m->swap_libre : 0;
	}
	rc = 0;
out:
	free(txt);
	return rc;
}

/* --------------------------------------------------------------- procesos */

static const char *estado_texto(char c)
{
	switch (c) {
	case 'R': return "En ejecución";
	case 'S': return "Durmiendo";
	case 'D': return "Espera de E/S";
	case 'I': return "Inactivo";
	case 'T': return "Detenido";
	case 't': return "Trazado";
	case 'Z': return "Zombie";
	case 'X': return "Muerto";
	default:  return "Desconocido";
	}
}

static int cmp_pid(const void *a, const void *b)
{
	return ((const proc_t *)a)->pid - ((const proc_t *)b)->pid;
}

/* Lee estado y RSS de /proc/PID/stat. Devuelve 0 si el proceso existe. */
static int leer_stat(int pid, char *state, char *comm, size_t cn, unsigned long *rss_kb)
{
	char ruta[64], buf[1024], *a, *b;
	long rss_pag = 0;
	size_t n;
	FILE *f;

	snprintf(ruta, sizeof(ruta), "/proc/%d/stat", pid);
	f = fopen(ruta, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	a = strchr(buf, '(');
	b = strrchr(buf, ')');
	if (!a || !b || b < a)
		return -1;
	if (comm) {
		size_t l = (size_t)(b - a - 1);

		if (l >= cn)
			l = cn - 1;
		memcpy(comm, a + 1, l);
		comm[l] = '\0';
	}
	if (sscanf(b + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
		   state, &rss_pag) < 1)
		return -1;
	if (rss_kb)
		*rss_kb = rss_pag > 0 ? (unsigned long)rss_pag * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024) : 0;
	return 0;
}

/*
 * Lista alternativa para kernels cuya sys_get_processes_state solo devuelve
 * contadores (el de la Practica 4): se recorren los PID de /proc y, por cada
 * uno, se llama a sys_get_process_info para obtener nombre y memoria.
 */
static int lista_desde_proc(proc_t *out, int max)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)) && n < max) {
		struct ks_process_info info;
		char st = '?', comm[KS_COMM_LEN] = "";
		unsigned long rss = 0;
		int pid;
		const char *c;

		for (c = e->d_name; *c && isdigit((unsigned char)*c); c++)
			;
		if (*c || !e->d_name[0])
			continue;
		pid = atoi(e->d_name);
		if (leer_stat(pid, &st, comm, sizeof(comm), &rss) < 0)
			continue;                           /* el proceso termino */

		memset(&info, 0, sizeof(info));
		if (syscall(SYS_GET_PROCESS_INFO, pid, &info) == 0) {
			snprintf(out[n].name, sizeof(out[n].name), "%s", info.name);
			out[n].rss_kb = info.memory_kb;
		} else {
			snprintf(out[n].name, sizeof(out[n].name), "%s", comm);
			out[n].rss_kb = rss;
		}
		out[n].pid = pid;
		out[n].state = st;
		n++;
	}
	closedir(d);
	return n;
}

static int muestrear_procesos(void)
{
	struct ks_processes_state st;
	struct ks_process_entry *ent = calloc(KS_MAX_PROCS, sizeof(*ent));
	proc_t *lista = calloc(KS_MAX_PROCS, sizeof(*lista));
	long r;
	int n = 0, sys_ok = 0;
	const char *fuente;

	if (!ent || !lista) {
		free(ent);
		free(lista);
		return -1;
	}
	memset(&st, 0, sizeof(st));
	r = g_forzar_proc ? -1 : syscall(SYS_GET_PROCESSES_STATE, &st, ent, KS_MAX_PROCS);
	sys_ok = r == 0;

	if (sys_ok && st.count > 0) {
		/* kernel con lista por proceso: la syscall alimenta la tabla directamente */
		for (unsigned long i = 0; i < st.count && n < KS_MAX_PROCS; i++) {
			lista[n].pid = ent[i].pid;
			lista[n].state = ent[i].state;
			snprintf(lista[n].name, sizeof(lista[n].name), "%s", ent[i].name);
			lista[n].rss_kb = ent[i].rss_kb;
			n++;
		}
		fuente = "sys_get_processes_state (lista por proceso)";
	} else {
		n = lista_desde_proc(lista, KS_MAX_PROCS);
		if (n < 0) {
			log_msg("procesos: no se pudo recorrer /proc: %s", strerror(errno));
			free(ent);
			free(lista);
			return -1;
		}
		if (sys_ok) {
			fuente = "sys_get_processes_state (contadores) + /proc + sys_get_process_info";
		} else {
			/* sin la syscall: los contadores se calculan desde la lista */
			memset(&st, 0, sizeof(st));
			for (int i = 0; i < n; i++) {
				st.total++;
				if (lista[i].state == 'R')
					st.running++;
				else if (lista[i].state == 'Z')
					st.zombie++;
				else
					st.sleeping++;
			}
			fuente = "/proc (la syscall no esta disponible)";
		}
	}
	free(ent);
	qsort(lista, (size_t)n, sizeof(*lista), cmp_pid);

	pthread_mutex_lock(&mu);
	free(g_procs);
	g_procs = lista;
	g_nprocs = n;
	g_resumen = st;
	snprintf(g_fuente, sizeof(g_fuente), "%s", fuente);
	pthread_mutex_unlock(&mu);
	return 0;
}

/* ------------------------------------------------------------------- JSON */

cJSON *datos_json(void)
{
	cJSON *o = cJSON_CreateObject(), *procs, *hist, *res;
	char ts[32];

	pthread_mutex_lock(&mu);
	fmt_time(g_tiene_mem ? g_mem.ts : time(NULL), ts, sizeof(ts));
	/* Campos del enunciado (kB) */
	cJSON_AddNumberToObject(o, "memoria_usada", (double)g_mem.usada);
	cJSON_AddNumberToObject(o, "memoria_libre", (double)g_mem.libre);
	cJSON_AddNumberToObject(o, "memoria_cache", (double)g_mem.cache);
	cJSON_AddNumberToObject(o, "swap_usada", (double)g_mem.swap_usada);
	/* Auxiliares */
	cJSON_AddNumberToObject(o, "memoria_total", (double)g_mem.total);
	cJSON_AddNumberToObject(o, "swap_total", (double)g_mem.swap_total);
	cJSON_AddStringToObject(o, "unidad", "kB");
	cJSON_AddStringToObject(o, "timestamp", ts);

	procs = cJSON_AddArrayToObject(o, "procesos");
	for (int i = 0; i < g_nprocs; i++) {
		char st[2] = { g_procs[i].state, 0 };
		cJSON *p = cJSON_CreateObject();

		cJSON_AddStringToObject(p, "nombre", g_procs[i].name);
		cJSON_AddNumberToObject(p, "pid", g_procs[i].pid);
		cJSON_AddStringToObject(p, "estado", estado_texto(g_procs[i].state));
		cJSON_AddStringToObject(p, "estado_codigo", st);
		cJSON_AddNumberToObject(p, "rss_kb", (double)g_procs[i].rss_kb);
		cJSON_AddItemToArray(procs, p);
	}
	res = cJSON_AddObjectToObject(o, "resumen_procesos");
	cJSON_AddNumberToObject(res, "total", (double)g_resumen.total);
	cJSON_AddNumberToObject(res, "running", (double)g_resumen.running);
	cJSON_AddNumberToObject(res, "sleeping", (double)g_resumen.sleeping);
	cJSON_AddNumberToObject(res, "zombie", (double)g_resumen.zombie);
	cJSON_AddStringToObject(o, "fuente_procesos", g_fuente);

	hist = cJSON_AddArrayToObject(o, "historial");
	for (int i = 0; i < g_hist_n; i++) {
		int idx = (g_hist_head - g_hist_n + i + HISTORIAL) % HISTORIAL;
		cJSON *h = cJSON_CreateObject();

		fmt_time(g_hist[idx].ts, ts, sizeof(ts));
		cJSON_AddStringToObject(h, "timestamp", ts);
		cJSON_AddNumberToObject(h, "memoria_usada", (double)g_hist[idx].usada);
		cJSON_AddNumberToObject(h, "swap_usada", (double)g_hist[idx].swap_usada);
		cJSON_AddItemToArray(hist, h);
	}
	pthread_mutex_unlock(&mu);

	cJSON_AddItemToObject(o, "eventos_seguridad", eventos_json(100));
	return o;
}

int proceso_json(int pid, cJSON **out, char *err, size_t errn)
{
	struct ks_process_info info;
	cJSON *o;

	if (pid <= 0) {
		snprintf(err, errn, "PID inválido");
		return -1;
	}
	memset(&info, 0, sizeof(info));
	if (syscall(SYS_GET_PROCESS_INFO, pid, &info) < 0) {
		if (errno == ESRCH)
			snprintf(err, errn, "El proceso %d no existe", pid);
		else
			snprintf(err, errn, "sys_get_process_info falló: %s", strerror(errno));
		return -1;
	}
	o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "pid", info.pid);
	cJSON_AddStringToObject(o, "nombre", info.name);
	cJSON_AddNumberToObject(o, "tiempo_ejecucion_s", (double)info.runtime_ns / 1e9);
	cJSON_AddNumberToObject(o, "memoria_kb", (double)info.memory_kb);
	*out = o;
	return 0;
}

/* ---------------------------------------------------------------- el hilo */

void *sistema_thread(void *arg)
{
	(void)arg;
	log_msg("hilo de monitoreo iniciado (intervalo %d s)", g_intervalo);
	while (g_running) {
		mem_t m;

		if (leer_memoria(&m) == 0) {
			pthread_mutex_lock(&mu);
			g_mem = m;
			g_tiene_mem = 1;
			g_hist[g_hist_head] = m;
			g_hist_head = (g_hist_head + 1) % HISTORIAL;
			if (g_hist_n < HISTORIAL)
				g_hist_n++;
			pthread_mutex_unlock(&mu);
		}
		if (muestrear_procesos() < 0)
			log_msg("procesos: muestreo fallido (se reintenta en el siguiente intervalo)");

		{
			cJSON *j = datos_json();
			char *s = cJSON_PrintUnformatted(j);

			if (s) {
				sse_broadcast("datos", s);
				free(s);
			}
			cJSON_Delete(j);
		}
		msleep_run(g_intervalo * 1000);
	}
	log_msg("hilo de monitoreo terminado");
	return NULL;
}
