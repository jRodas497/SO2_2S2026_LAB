#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alerts.h"
#include "auth.h"
#include "blacklist.h"
#include "cJSON.h"
#include "common.h"
#include "fswatch.h"
#include "http.h"
#include "logwatch.h"
#include "metrics.h"

#define MAX_LW_CFG 8

struct lw_cfg {
	char central[256];
	char palabra[64];
	char rutas[LW_MAX_RUTAS][256];
	int n;
};

struct config {
	int port;
	int interval;
	char dashboard_dir[PATH_MAX];
	char blacklist[PATH_MAX];
	char monitor_dir[PATH_MAX];
	char log_file[PATH_MAX];
	char pid_file[PATH_MAX];
	char pam_service[64];
	bool auto_monitor, auto_procs, auto_files;
	struct lw_cfg lw[MAX_LW_CFG];
	int nlw;
};

static void on_term(int sig)
{
	(void)sig;
	g_running = 0;
}

static void on_usr1(int sig)
{
	(void)sig;      /* solo interrumpe la syscall bloqueada del hilo IPC */
}

/* Si p es relativa se resuelve contra el directorio del archivo de configuracion. */
static void resolve(const char *base, const char *p, char *out, size_t n)
{
	if (p[0] == '/')
		snprintf(out, n, "%s", p);
	else
		snprintf(out, n, "%s/%s", base, p);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Uso: %s [-c config.json] [-f]\n"
		"  -c  archivo de configuracion (por defecto config/daemon.json junto al ejecutable)\n"
		"  -f  primer plano (no demonizar; el log sale por stderr)\n", prog);
}

static void load_config(const char *path, struct config *c)
{
	char base[PATH_MAX], *text = NULL, tmp[PATH_MAX];
	cJSON *root, *it, *lw;

	memset(c, 0, sizeof(*c));
	c->port = 8080;
	c->interval = 2;
	c->auto_monitor = c->auto_procs = true;
	snprintf(c->pam_service, sizeof(c->pam_service), "so2-monitor");
	snprintf(c->log_file, sizeof(c->log_file), "/var/log/so2monitor.log");
	snprintf(c->pid_file, sizeof(c->pid_file), "/run/so2monitor.pid");

	snprintf(tmp, sizeof(tmp), "%s", path);
	snprintf(base, sizeof(base), "%s", dirname(tmp));

	if (read_file(path, &text, NULL, 1 << 20) < 0) {
		fprintf(stderr, "No se pudo leer la configuracion %s: %s\n", path, strerror(errno));
		exit(1);
	}
	root = cJSON_Parse(text);
	free(text);
	if (!root) {
		fprintf(stderr, "Configuracion invalida (JSON) en %s\n", path);
		exit(1);
	}

#define STR(key, dst) do { it = cJSON_GetObjectItemCaseSensitive(root, key); \
	if (cJSON_IsString(it)) resolve(base, it->valuestring, dst, sizeof(dst)); } while (0)
	STR("dashboard_dir", c->dashboard_dir);
	STR("blacklist_file", c->blacklist);
	STR("log_file", c->log_file);
	STR("pid_file", c->pid_file);
	it = cJSON_GetObjectItemCaseSensitive(root, "monitor_dir");
	if (cJSON_IsString(it))
		snprintf(c->monitor_dir, sizeof(c->monitor_dir), "%s", it->valuestring);
	it = cJSON_GetObjectItemCaseSensitive(root, "pam_service");
	if (cJSON_IsString(it))
		snprintf(c->pam_service, sizeof(c->pam_service), "%s", it->valuestring);
	it = cJSON_GetObjectItemCaseSensitive(root, "port");
	if (cJSON_IsNumber(it) && it->valueint > 0 && it->valueint < 65536)
		c->port = it->valueint;
	it = cJSON_GetObjectItemCaseSensitive(root, "intervalo_metricas_s");
	if (cJSON_IsNumber(it))
		c->interval = it->valueint;

	it = cJSON_GetObjectItemCaseSensitive(root, "autoinicio");
	if (cJSON_IsObject(it)) {
		cJSON *x;

		x = cJSON_GetObjectItemCaseSensitive(it, "monitoreo");
		if (cJSON_IsBool(x)) c->auto_monitor = cJSON_IsTrue(x);
		x = cJSON_GetObjectItemCaseSensitive(it, "procesos");
		if (cJSON_IsBool(x)) c->auto_procs = cJSON_IsTrue(x);
		x = cJSON_GetObjectItemCaseSensitive(it, "archivos");
		if (cJSON_IsBool(x)) c->auto_files = cJSON_IsTrue(x);
	}

	lw = cJSON_GetObjectItemCaseSensitive(root, "logwatch");
	cJSON_ArrayForEach(it, lw) {
		cJSON *central = cJSON_GetObjectItemCaseSensitive(it, "central");
		cJSON *palabra = cJSON_GetObjectItemCaseSensitive(it, "palabra");
		cJSON *rutas = cJSON_GetObjectItemCaseSensitive(it, "rutas"), *r;
		struct lw_cfg *l;

		if (c->nlw >= MAX_LW_CFG || !cJSON_IsString(central) || !cJSON_IsString(palabra) ||
		    !cJSON_IsArray(rutas))
			continue;
		l = &c->lw[c->nlw];
		snprintf(l->central, sizeof(l->central), "%s", central->valuestring);
		snprintf(l->palabra, sizeof(l->palabra), "%s", palabra->valuestring);
		cJSON_ArrayForEach(r, rutas)
			if (cJSON_IsString(r) && l->n < LW_MAX_RUTAS)
				snprintf(l->rutas[l->n++], sizeof(l->rutas[0]), "%s", r->valuestring);
		if (l->n > 0)
			c->nlw++;
	}
	cJSON_Delete(root);
}

static int daemonize(void)
{
	pid_t pid = fork();
	int fd;

	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);
	if (setsid() < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);
	umask(027);
	if (chdir("/") < 0)
		return -1;
	fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	return 0;
}

/* /tmp se vacia al reiniciar: si el directorio vigilado de la configuracion no existe, se crea. */
static void ensure_monitor_dir(const char *dir)
{
	struct stat st;

	if (!dir[0] || stat(dir, &st) == 0)
		return;
	if (mkdir(dir, 0777) < 0) {
		log_msg("no se pudo crear el directorio vigilado %s: %s", dir, strerror(errno));
		return;
	}
	chmod(dir, 01777);          /* sticky: cualquiera deja archivos, nadie borra los ajenos */
	log_msg("directorio vigilado creado: %s", dir);
}

static void write_pidfile(const char *path)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		log_msg("no se pudo escribir el pidfile %s: %s", path, strerror(errno));
		return;
	}
	fprintf(f, "%d\n", getpid());
	fclose(f);
}

static void join_ipc(pthread_t t)
{
	/* El hilo IPC esta dormido en el kernel: una senal interrumpe la espera. */
	for (int i = 0; i < 50; i++) {
		struct timespec ts;

		pthread_kill(t, SIGUSR1);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		if (pthread_timedjoin_np(t, NULL, &ts) == 0)
			return;
	}
	log_msg("el hilo IPC no termino a tiempo");
}

int main(int argc, char **argv)
{
	char cfgpath[PATH_MAX], exe[PATH_MAX], err[256], dash[PATH_MAX];
	struct config cfg;
	struct sigaction sa;
	pthread_t t_mon, t_fs, t_ipc;
	bool foreground = false;
	int opt;

	cfgpath[0] = '\0';
	while ((opt = getopt(argc, argv, "c:fh")) != -1) {
		switch (opt) {
		case 'c': snprintf(cfgpath, sizeof(cfgpath), "%s", optarg); break;
		case 'f': foreground = true; break;
		default:  usage(argv[0]); return opt == 'h' ? 0 : 1;
		}
	}
	if (!cfgpath[0]) {
		ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

		if (n < 0) {
			usage(argv[0]);
			return 1;
		}
		exe[n] = '\0';
		snprintf(cfgpath, sizeof(cfgpath), "%s/config/daemon.json", dirname(exe));
	}
	/* rutas absolutas ANTES de demonizar (el daemon hace chdir("/")) */
	if (!realpath(cfgpath, exe)) {
		fprintf(stderr, "No existe la configuracion %s\n", cfgpath);
		return 1;
	}
	snprintf(cfgpath, sizeof(cfgpath), "%s", exe);
	load_config(cfgpath, &cfg);
	if (!realpath(cfg.dashboard_dir, dash)) {
		fprintf(stderr, "No existe el directorio del dashboard: %s\n", cfg.dashboard_dir);
		return 1;
	}
	if (geteuid() != 0)
		fprintf(stderr, "ADVERTENCIA: sin root fallaran eBPF, PAM, chmod y kill.\n");

	if (!foreground && daemonize() < 0) {
		fprintf(stderr, "No se pudo demonizar: %s\n", strerror(errno));
		return 1;
	}
	log_init(foreground ? NULL : cfg.log_file);
	if (!foreground)
		write_pidfile(cfg.pid_file);
	log_msg("== so2monitor iniciado (PID %d, config %s) ==", getpid(), cfgpath);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;                    /* sin SA_RESTART */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	alerts_init();
	auth_init(cfg.pam_service);
	metrics_init(cfg.interval);
	if (cfg.blacklist[0] && bl_load_file(cfg.blacklist, err, sizeof(err)) < 0)
		log_msg("ADVERTENCIA blacklist: %s", err);
	ensure_monitor_dir(cfg.monitor_dir);
	if (fs_init(cfg.monitor_dir[0] ? cfg.monitor_dir : NULL) < 0)
		log_msg("ADVERTENCIA: el escaneo de archivos no estara disponible");

	/* Los tres hilos obligatorios: monitoreo, sistema de archivos (eBPF) e IPC. */
	if (pthread_create(&t_mon, NULL, monitor_thread, NULL) ||
	    pthread_create(&t_fs, NULL, fs_thread, NULL) ||
	    pthread_create(&t_ipc, NULL, ipc_thread, NULL)) {
		log_msg("no se pudieron crear los hilos: %s", strerror(errno));
		return 1;
	}

	if (http_start(cfg.port, dash) < 0) {
		g_running = 0;
	} else {
		if (!cfg.auto_monitor)
			monitor_set_enabled(false, "configuracion");
		if (!cfg.auto_procs)
			procscan_set_enabled(false, "configuracion");
		if (cfg.auto_files && fs_set_enabled(true, err, sizeof(err)) < 0)
			log_msg("autoinicio del escaneo de archivos: %s", err);
		for (int i = 0; i < cfg.nlw; i++) {
			const char *rutas[LW_MAX_RUTAS];

			for (int k = 0; k < cfg.lw[i].n; k++)
				rutas[k] = cfg.lw[i].rutas[k];
			if (lw_start(cfg.lw[i].central, rutas, cfg.lw[i].n, cfg.lw[i].palabra,
				     "configuracion", err, sizeof(err)) < 0)
				log_msg("autoinicio de logwatch: %s", err);
		}
	}

	while (g_running)
		msleep_run(500);

	log_msg("apagando...");
	join_ipc(t_ipc);
	pthread_join(t_mon, NULL);
	pthread_join(t_fs, NULL);
	lw_stop_all();                  /* sys_stop_log_watch de cada monitoreo activo */
	fs_shutdown();
	http_stop();
	if (!foreground)
		unlink(cfg.pid_file);
	log_msg("== so2monitor terminado ==");
	return 0;
}
