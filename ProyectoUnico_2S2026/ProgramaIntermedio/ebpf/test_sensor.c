// SPDX-License-Identifier: GPL-2.0
/*
 * Cargador de prueba del sensor eBPF (independiente del daemon).
 *
 *   sudo ./test_sensor /ruta/a/vigilar [segundos]
 *
 * Carga el programa, registra su propio PID para que el sensor lo ignore,
 * configura el directorio en el mapa "cfg_map", engancha los tracepoints y
 * muestra los eventos que llegan por el ring buffer.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/types.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "sensor.h"
#include "sensor.skel.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* "/tmp/monitor" -> comp[0]="monitor", comp[1]="tmp", ncomp=2 */
static int build_cfg(const char *dir, struct sensor_cfg *cfg, __u32 gen)
{
	char resolved[PATH_MAX];
	char *comps[SENSOR_MAX_COMPS + 1];
	struct stat st;
	char *save = NULL, *tok;
	int n = 0;

	if (!realpath(dir, resolved)) {
		fprintf(stderr, "No se pudo resolver '%s': %s\n", dir, strerror(errno));
		return -1;
	}
	if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "'%s' no es un directorio\n", resolved);
		return -1;
	}

	memset(cfg, 0, sizeof(*cfg));
	for (tok = strtok_r(resolved, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (n >= SENSOR_MAX_COMPS) {
			fprintf(stderr, "Directorio demasiado profundo (max %d niveles)\n",
				SENSOR_MAX_COMPS);
			return -1;
		}
		if (strlen(tok) >= SENSOR_COMP_LEN) {
			fprintf(stderr, "Componente '%s' demasiado largo (max %d)\n",
				tok, SENSOR_COMP_LEN - 1);
			return -1;
		}
		comps[n++] = tok;
	}
	for (int i = 0; i < n; i++)
		strncpy(cfg->comp[i], comps[n - 1 - i], SENSOR_COMP_LEN - 1);
	cfg->ncomp = n;
	cfg->gen = gen;
	return 0;
}

static int on_event(void *ctx, void *data, size_t len)
{
	const struct sensor_event *e = data;
	const char *dir = ctx;

	(void)len;
	/* El kernel limpia O_CREAT/O_TRUNC de f_flags al abrir: crear vs. modificar
	 * lo decide el daemon segun si ya conocia el archivo. */
	printf("[gen %u] ESCRITURA  %s/%s  (pid=%u comm=%s uid=%u flags=0%o)\n",
	       e->gen, dir, e->name, e->pid, e->comm, e->uid, e->flags);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	struct sensor_bpf *skel;
	struct ring_buffer *rb;
	struct sensor_cfg cfg;
	char resolved[PATH_MAX];
	__u32 zero = 0, pid = getpid();
	int seconds = argc > 2 ? atoi(argv[2]) : 0;
	int err = 1;

	if (argc < 2) {
		fprintf(stderr, "Uso: sudo %s <directorio> [segundos]\n", argv[0]);
		return 1;
	}
	if (build_cfg(argv[1], &cfg, 1) < 0)
		return 1;
	if (!realpath(argv[1], resolved))
		return 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	skel = sensor_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "No se pudo cargar el programa eBPF (se necesita root)\n");
		return 1;
	}

	/* Orden importante: PID del daemon y config ANTES de enganchar. */
	if (bpf_map_update_elem(bpf_map__fd(skel->maps.daemon_pid), &zero, &pid, BPF_ANY) ||
	    bpf_map_update_elem(bpf_map__fd(skel->maps.cfg_map), &zero, &cfg, BPF_ANY)) {
		fprintf(stderr, "No se pudieron actualizar los mapas: %s\n", strerror(errno));
		goto out;
	}

	rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, resolved, NULL);
	if (!rb) {
		fprintf(stderr, "No se pudo crear el ring buffer\n");
		goto out;
	}

	if (sensor_bpf__attach(skel)) {
		fprintf(stderr, "No se pudo enganchar al kernel: %s\n", strerror(errno));
		goto out_rb;
	}

	printf("Vigilando %s (solo archivos directos, sin subdirectorios). PID propio=%u. Ctrl+C para salir.\n",
	       resolved, pid);

	for (int t = 0; !stop && (seconds == 0 || t < seconds * 10); t++) {
		int n = ring_buffer__poll(rb, 100 /* ms */);

		if (n < 0 && n != -EINTR) {
			fprintf(stderr, "ring_buffer__poll: %s\n", strerror(-n));
			break;
		}
	}
	err = 0;

out_rb:
	ring_buffer__free(rb);
out:
	sensor_bpf__destroy(skel);
	return err;
}
