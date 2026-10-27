#define _GNU_SOURCE
#include "metrics.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "alerts.h"
#include "common.h"
#include "http.h"
#include "ksys.h"

#define HISTORY   120
#define TOP_N     20
#define PF_KTHREAD_FLAG 0x00200000UL

typedef struct {
	time_t ts;
	unsigned long mem_total, mem_free, mem_avail, buffers, cached;   /* kB */
	unsigned long active, inactive, swap_total, swap_free;           /* kB */
	unsigned long mem_used, swap_used;                               /* kB */
	unsigned long pgfault, pgmajfault;                               /* acumulados */
	unsigned long pgfault_d, pgmajfault_d;                           /* delta del intervalo */
} sample_t;

typedef struct {
	int pid;
	char state;
	char name[KS_COMM_LEN];
	unsigned long rss_kb;
	double pct;
} proc_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int g_interval = 2;
static volatile int g_monitoring = 1;
static volatile int g_procscan = 1;

static sample_t g_last, g_hist[HISTORY];
static int g_have_last, g_hist_n, g_hist_head;
static unsigned long g_prev_pgf, g_prev_pgm;
static int g_have_prev;

static proc_t g_top[TOP_N];
static int g_ntop;
static struct ks_processes_state g_sum;
static int g_have_procs;

void metrics_init(int interval_sec)
{
	if (interval_sec >= 1 && interval_sec <= 60)
		g_interval = interval_sec;
}

/* ---------------------------------------------------------------- /proc */

static int meminfo_value(const char *text, const char *key, unsigned long *out)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (strncmp(p, key, kl) == 0 && p[kl] == ':') {
			*out = strtoul(p + kl + 1, NULL, 10);
			return 0;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return -1;
}

static int vmstat_value(const char *text, const char *key, unsigned long *out)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (strncmp(p, key, kl) == 0 && p[kl] == ' ') {
			*out = strtoul(p + kl + 1, NULL, 10);
			return 0;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return -1;
}

static int take_sample(sample_t *s)
{
	char *mi = NULL, *vm = NULL;
	int rc = -1;

	memset(s, 0, sizeof(*s));
	if (read_file("/proc/meminfo", &mi, NULL, 1 << 16) < 0 ||
	    read_file("/proc/vmstat", &vm, NULL, 1 << 20) < 0) {
		log_msg("metricas: no se pudo leer /proc/meminfo o /proc/vmstat: %s",
			strerror(errno));
		goto out;
	}

	s->ts = time(NULL);
	if (meminfo_value(mi, "MemTotal", &s->mem_total) ||
	    meminfo_value(mi, "MemFree", &s->mem_free) ||
	    meminfo_value(mi, "MemAvailable", &s->mem_avail) ||
	    meminfo_value(mi, "Buffers", &s->buffers) ||
	    meminfo_value(mi, "Cached", &s->cached) ||
	    meminfo_value(mi, "Active", &s->active) ||
	    meminfo_value(mi, "Inactive", &s->inactive) ||
	    meminfo_value(mi, "SwapTotal", &s->swap_total) ||
	    meminfo_value(mi, "SwapFree", &s->swap_free)) {
		log_msg("metricas: /proc/meminfo con formato inesperado");
		goto out;
	}
	if (vmstat_value(vm, "pgfault", &s->pgfault) ||
	    vmstat_value(vm, "pgmajfault", &s->pgmajfault)) {
		log_msg("metricas: /proc/vmstat sin pgfault/pgmajfault");
		goto out;
	}

	{
		unsigned long used = s->mem_free + s->buffers + s->cached;

		s->mem_used = s->mem_total > used ? s->mem_total - used : 0;
		s->swap_used = s->swap_total > s->swap_free ? s->swap_total - s->swap_free : 0;
	}
	rc = 0;
out:
	free(mi);
	free(vm);
	return rc;
}

static cJSON *sample_json(const sample_t *s)
{
	char ts[32];
	cJSON *o = cJSON_CreateObject();

	fmt_time(s->ts, ts, sizeof(ts));
	cJSON_AddBoolToObject(o, "disponible", 1);
	cJSON_AddStringToObject(o, "timestamp", ts);
	cJSON_AddNumberToObject(o, "ts", (double)s->ts);
	cJSON_AddNumberToObject(o, "mem_total_kb", (double)s->mem_total);
	cJSON_AddNumberToObject(o, "mem_libre_kb", (double)s->mem_free);
	cJSON_AddNumberToObject(o, "mem_disponible_kb", (double)s->mem_avail);
	cJSON_AddNumberToObject(o, "buffers_kb", (double)s->buffers);
	cJSON_AddNumberToObject(o, "cache_kb", (double)s->cached);
	cJSON_AddNumberToObject(o, "mem_usada_kb", (double)s->mem_used);
	cJSON_AddNumberToObject(o, "activas_kb", (double)s->active);
	cJSON_AddNumberToObject(o, "inactivas_kb", (double)s->inactive);
	cJSON_AddNumberToObject(o, "swap_total_kb", (double)s->swap_total);
	cJSON_AddNumberToObject(o, "swap_libre_kb", (double)s->swap_free);
	cJSON_AddNumberToObject(o, "swap_usada_kb", (double)s->swap_used);
	cJSON_AddNumberToObject(o, "pgfault_total", (double)s->pgfault);
	cJSON_AddNumberToObject(o, "pgmajfault_total", (double)s->pgmajfault);
	cJSON_AddNumberToObject(o, "pgfault_delta", (double)s->pgfault_d);
	cJSON_AddNumberToObject(o, "pgmajfault_delta", (double)s->pgmajfault_d);
	return o;
}

cJSON *metrics_last_json(void)
{
	cJSON *o;

	pthread_mutex_lock(&mu);
	if (g_have_last) {
		o = sample_json(&g_last);
	} else {
		o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "disponible", 0);
	}
	pthread_mutex_unlock(&mu);
	return o;
}

cJSON *metrics_history_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	for (int i = 0; i < g_hist_n; i++) {
		int idx = (g_hist_head - g_hist_n + i + HISTORY) % HISTORY;

		cJSON_AddItemToArray(arr, sample_json(&g_hist[idx]));
	}
	pthread_mutex_unlock(&mu);
	return arr;
}

/* --------------------------------------------------------------- procesos */

static int cmp_rss_desc(const void *a, const void *b)
{
	const struct ks_process_entry *x = a, *y = b;

	if (x->rss_kb != y->rss_kb)
		return x->rss_kb < y->rss_kb ? 1 : -1;
	return x->pid - y->pid;
}

/* Llama a sys_get_processes_state y filtra el top 20 por RSS (desde espacio de usuario). */
static int take_procs(void)
{
	struct ks_processes_state st;
	struct ks_process_entry *buf;
	unsigned long mem_total = 0;
	proc_t top[TOP_N];
	int n;

	buf = calloc(KS_MAX_PROCS, sizeof(*buf));
	if (!buf)
		return -1;
	memset(&st, 0, sizeof(st));

	if (syscall(SYS_GET_PROCESSES_STATE, &st, buf, KS_MAX_PROCS) < 0) {
		log_msg("procesos: get_processes_state fallo: %s", strerror(errno));
		free(buf);
		return -1;
	}

	pthread_mutex_lock(&mu);
	mem_total = g_have_last ? g_last.mem_total : 0;
	pthread_mutex_unlock(&mu);
	if (!mem_total) {       /* monitoreo apagado: leer MemTotal directamente */
		char *mi = NULL;

		if (read_file("/proc/meminfo", &mi, NULL, 1 << 16) == 0) {
			meminfo_value(mi, "MemTotal", &mem_total);
			free(mi);
		}
	}

	qsort(buf, st.count, sizeof(*buf), cmp_rss_desc);
	n = st.count < TOP_N ? (int)st.count : TOP_N;
	for (int i = 0; i < n; i++) {
		top[i].pid = buf[i].pid;
		top[i].state = buf[i].state;
		snprintf(top[i].name, sizeof(top[i].name), "%s", buf[i].name);
		top[i].rss_kb = buf[i].rss_kb;
		top[i].pct = mem_total ? 100.0 * (double)buf[i].rss_kb / (double)mem_total : 0.0;
	}
	free(buf);

	pthread_mutex_lock(&mu);
	memcpy(g_top, top, sizeof(proc_t) * (size_t)n);
	g_ntop = n;
	g_sum = st;
	g_have_procs = 1;
	pthread_mutex_unlock(&mu);
	return 0;
}

static const char *state_name(char c)
{
	switch (c) {
	case 'R': return "En ejecucion";
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

cJSON *procs_last_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON *res = cJSON_AddObjectToObject(o, "resumen");
	cJSON *arr = cJSON_AddArrayToObject(o, "top");

	pthread_mutex_lock(&mu);
	cJSON_AddBoolToObject(o, "disponible", g_have_procs);
	cJSON_AddNumberToObject(res, "total", (double)g_sum.total);
	cJSON_AddNumberToObject(res, "running", (double)g_sum.running);
	cJSON_AddNumberToObject(res, "sleeping", (double)g_sum.sleeping);
	cJSON_AddNumberToObject(res, "zombie", (double)g_sum.zombie);
	for (int i = 0; i < g_ntop; i++) {
		char st[2] = { g_top[i].state, 0 };
		cJSON *p = cJSON_CreateObject();

		cJSON_AddNumberToObject(p, "pid", g_top[i].pid);
		cJSON_AddStringToObject(p, "nombre", g_top[i].name);
		cJSON_AddStringToObject(p, "estado", st);
		cJSON_AddStringToObject(p, "estado_texto", state_name(g_top[i].state));
		cJSON_AddNumberToObject(p, "rss_kb", (double)g_top[i].rss_kb);
		cJSON_AddNumberToObject(p, "porcentaje", g_top[i].pct);
		cJSON_AddItemToArray(arr, p);
	}
	pthread_mutex_unlock(&mu);
	return o;
}

int proc_info_json(int pid, cJSON **out, char *err, size_t errn)
{
	struct ks_process_info info;
	cJSON *o;

	if (pid <= 0) {
		snprintf(err, errn, "PID invalido");
		return -1;
	}
	memset(&info, 0, sizeof(info));
	if (syscall(SYS_GET_PROCESS_INFO, pid, &info) < 0) {
		if (errno == ESRCH)
			snprintf(err, errn, "El proceso %d no existe", pid);
		else
			snprintf(err, errn, "get_process_info falló: %s", strerror(errno));
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

/* Hilo del kernel: PF_KTHREAD en el campo "flags" de /proc/PID/stat. */
static int is_kernel_thread(int pid)
{
	char path[64], buf[1024];
	unsigned long flags = 0;
	char *p;
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	p = strrchr(buf, ')');
	if (!p)
		return -1;
	/* tras ')' : estado ppid pgrp sesion tty tpgid flags */
	if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %lu", &flags) != 1)
		return -1;
	return (flags & PF_KTHREAD_FLAG) ? 1 : 0;
}

int proc_kill(int pid, const char *quien, char *err, size_t errn)
{
	char name[KS_COMM_LEN] = "?";
	struct ks_process_info info;
	int kt;

	if (pid <= 0) {
		snprintf(err, errn, "PID invalido");
		return -1;
	}
	if (pid == 1) {
		snprintf(err, errn, "No se permite terminar el proceso con PID 1");
		return -1;
	}
	if (pid == getpid()) {
		snprintf(err, errn, "No se permite terminar el propio daemon");
		return -1;
	}
	kt = is_kernel_thread(pid);
	if (kt < 0) {
		snprintf(err, errn, "El proceso %d no existe", pid);
		return -1;
	}
	if (kt > 0) {
		snprintf(err, errn, "No se permite terminar hilos del kernel");
		return -1;
	}

	memset(&info, 0, sizeof(info));
	if (syscall(SYS_GET_PROCESS_INFO, pid, &info) == 0)
		snprintf(name, sizeof(name), "%s", info.name);

	if (syscall(SYS_kill, pid, SIGKILL) < 0) {
		snprintf(err, errn, "kill(%d) fallo: %s", pid, strerror(errno));
		return -1;
	}
	alert_add(SEV_MEDIUM, TIPO_SISTEMA, "Proceso matado",
		  "El usuario %s terminó el proceso %s (PID %d)", quien, name, pid);
	return 0;
}

/* -------------------------------------------------------- interruptores */

void monitor_set_enabled(bool on, const char *quien)
{
	if ((bool)g_monitoring == on)
		return;
	g_monitoring = on;
	if (on)
		alert_add(SEV_LOW, TIPO_SISTEMA, "Monitoreo del sistema activado",
			  "El usuario %s activó el monitoreo de métricas", quien);
	else
		alert_add(SEV_MEDIUM, TIPO_SISTEMA, "Monitoreo del sistema desactivado",
			  "El usuario %s desactivó el monitoreo de métricas", quien);
	http_publish_flags();
}

void procscan_set_enabled(bool on, const char *quien)
{
	if ((bool)g_procscan == on)
		return;
	g_procscan = on;
	log_msg("escaneo de procesos %s por %s", on ? "activado" : "desactivado", quien);
	http_publish_flags();
}

bool monitor_is_enabled(void) { return g_monitoring; }
bool procscan_is_enabled(void) { return g_procscan; }

/* -------------------------------------------------------------- el hilo */

static void publish(const char *event, cJSON *j)
{
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		sse_broadcast(event, s);
		free(s);
	}
	cJSON_Delete(j);
}

void *monitor_thread(void *arg)
{
	(void)arg;
	log_msg("hilo de monitoreo iniciado (intervalo %d s)", g_interval);

	while (g_running) {
		if (g_monitoring) {
			sample_t s;

			if (take_sample(&s) == 0) {
				pthread_mutex_lock(&mu);
				if (g_have_prev) {
					s.pgfault_d = s.pgfault >= g_prev_pgf ? s.pgfault - g_prev_pgf : 0;
					s.pgmajfault_d = s.pgmajfault >= g_prev_pgm ? s.pgmajfault - g_prev_pgm : 0;
				}
				g_prev_pgf = s.pgfault;
				g_prev_pgm = s.pgmajfault;
				g_have_prev = 1;
				g_last = s;
				g_have_last = 1;
				g_hist[g_hist_head] = s;
				g_hist_head = (g_hist_head + 1) % HISTORY;
				if (g_hist_n < HISTORY)
					g_hist_n++;
				pthread_mutex_unlock(&mu);

				publish("metrics", metrics_last_json());
			}
		}
		if (g_procscan && take_procs() == 0)
			publish("procs", procs_last_json());

		msleep_run(g_interval * 1000);
	}
	log_msg("hilo de monitoreo terminado");
	return NULL;
}
