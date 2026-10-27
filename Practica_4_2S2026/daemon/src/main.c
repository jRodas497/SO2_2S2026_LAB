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

#include "cJSON.h"
#include "common.h"
#include "eventos.h"
#include "http.h"
#include "sistema.h"

#define MAX_LW_CFG 8

struct lw_cfg {
	char central[256];
	char palabra[64];
	char rutas[EV_MAX_RUTAS][256];
	int n;
};

struct config {
	char bind[48];
	int port;
	int intervalo;
	bool forzar_proc;
	char dashboard_dir[PATH_MAX];
	char log_file[PATH_MAX];
	char pid_file[PATH_MAX];
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
	(void)sig;      /* solo interrumpe la syscall dormida del hilo IPC */
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Uso: %s [-c config.json] [-f]\n"
		"  -c  archivo de configuración (por defecto config/daemon.json junto al ejecutable)\n"
		"  -f  primer plano (no demonizar; el log sale por stderr)\n", prog);
}

/* Si p es relativa se resuelve contra el directorio del archivo de configuración. */
static void resolver(const char *base, const char *p, char *out, size_t n)
{
	if (p[0] == '/')
		snprintf(out, n, "%s", p);
	else
		snprintf(out, n, "%s/%s", base, p);
}

static void cargar_config(const char *path, struct config *c)
{
	char base[PATH_MAX], tmp[PATH_MAX], *texto = NULL;
	cJSON *root, *it, *lw;

	memset(c, 0, sizeof(*c));
	snprintf(c->bind, sizeof(c->bind), "0.0.0.0");
	c->port = 8080;
	c->intervalo = 2;
	snprintf(c->log_file, sizeof(c->log_file), "/tmp/so2_p4.log");
	snprintf(c->pid_file, sizeof(c->pid_file), "/tmp/so2_p4.pid");

	snprintf(tmp, sizeof(tmp), "%s", path);
	snprintf(base, sizeof(base), "%s", dirname(tmp));
	if (read_file(path, &texto, NULL, 1 << 20) < 0) {
		fprintf(stderr, "No se pudo leer la configuración %s: %s\n", path, strerror(errno));
		exit(1);
	}
	root = cJSON_Parse(texto);
	free(texto);
	if (!root) {
		fprintf(stderr, "Configuración inválida (JSON) en %s\n", path);
		exit(1);
	}

	it = cJSON_GetObjectItemCaseSensitive(root, "dashboard_dir");
	if (cJSON_IsString(it))
		resolver(base, it->valuestring, c->dashboard_dir, sizeof(c->dashboard_dir));
	it = cJSON_GetObjectItemCaseSensitive(root, "log_file");
	if (cJSON_IsString(it))
		resolver(base, it->valuestring, c->log_file, sizeof(c->log_file));
	it = cJSON_GetObjectItemCaseSensitive(root, "pid_file");
	if (cJSON_IsString(it))
		resolver(base, it->valuestring, c->pid_file, sizeof(c->pid_file));
	it = cJSON_GetObjectItemCaseSensitive(root, "bind");
	if (cJSON_IsString(it))
		snprintf(c->bind, sizeof(c->bind), "%s", it->valuestring);
	it = cJSON_GetObjectItemCaseSensitive(root, "port");
	if (cJSON_IsNumber(it) && it->valueint > 0 && it->valueint < 65536)
		c->port = it->valueint;
	it = cJSON_GetObjectItemCaseSensitive(root, "intervalo_s");
	if (cJSON_IsNumber(it))
		c->intervalo = it->valueint;
	it = cJSON_GetObjectItemCaseSensitive(root, "forzar_proc");
	if (cJSON_IsBool(it))
		c->forzar_proc = cJSON_IsTrue(it);

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
			if (cJSON_IsString(r) && l->n < EV_MAX_RUTAS)
				snprintf(l->rutas[l->n++], sizeof(l->rutas[0]), "%s", r->valuestring);
		if (l->n > 0)
			c->nlw++;
	}
	cJSON_Delete(root);
}

static int demonizar(void)
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

static void esperar_ipc(pthread_t t)
{
	/* El hilo IPC duerme dentro del kernel: una señal interrumpe la espera. */
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
	log_msg("el hilo IPC no terminó a tiempo");
}

int main(int argc, char **argv)
{
	char cfgpath[PATH_MAX], exe[PATH_MAX], dash[PATH_MAX], err[256];
	struct config cfg;
	struct sigaction sa;
	pthread_t t_sis, t_ipc;
	bool primer_plano = false;
	int opt;

	cfgpath[0] = '\0';
	while ((opt = getopt(argc, argv, "c:fh")) != -1) {
		switch (opt) {
		case 'c': snprintf(cfgpath, sizeof(cfgpath), "%s", optarg); break;
		case 'f': primer_plano = true; break;
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
		fprintf(stderr, "No existe la configuración %s\n", cfgpath);
		return 1;
	}
	snprintf(cfgpath, sizeof(cfgpath), "%s", exe);
	cargar_config(cfgpath, &cfg);
	if (!realpath(cfg.dashboard_dir, dash)) {
		fprintf(stderr, "No existe el directorio del dashboard: %s\n", cfg.dashboard_dir);
		return 1;
	}

	if (!primer_plano && demonizar() < 0) {
		fprintf(stderr, "No se pudo demonizar: %s\n", strerror(errno));
		return 1;
	}
	log_init(primer_plano ? NULL : cfg.log_file);
	if (!primer_plano) {
		FILE *f = fopen(cfg.pid_file, "w");

		if (f) {
			fprintf(f, "%d\n", getpid());
			fclose(f);
		}
	}
	log_msg("== so2_p4 iniciado (PID %d, config %s) ==", getpid(), cfgpath);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;                    /* sin SA_RESTART */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	sistema_init(cfg.intervalo, cfg.forzar_proc);

	/* 1) iniciar log_watch y 2) quedar dormido esperando sus notificaciones por IPC */
	for (int i = 0; i < cfg.nlw; i++) {
		const char *rutas[EV_MAX_RUTAS];

		for (int k = 0; k < cfg.lw[i].n; k++)
			rutas[k] = cfg.lw[i].rutas[k];
		if (logwatch_start(cfg.lw[i].central, rutas, cfg.lw[i].n, cfg.lw[i].palabra,
				   err, sizeof(err)) < 0)
			log_msg("no se pudo iniciar log_watch: %s", err);
	}

	if (pthread_create(&t_sis, NULL, sistema_thread, NULL) ||
	    pthread_create(&t_ipc, NULL, ipc_thread, NULL)) {
		log_msg("no se pudieron crear los hilos: %s", strerror(errno));
		return 1;
	}
	if (http_start(cfg.bind, cfg.port, dash) < 0)
		g_running = 0;

	while (g_running)
		msleep_run(500);

	log_msg("apagando...");
	esperar_ipc(t_ipc);
	pthread_join(t_sis, NULL);
	logwatch_stop_all();                /* sys_stop_log_watch de cada monitoreo */
	http_stop();
	if (!primer_plano)
		unlink(cfg.pid_file);
	log_msg("== so2_p4 terminado ==");
	return 0;
}
