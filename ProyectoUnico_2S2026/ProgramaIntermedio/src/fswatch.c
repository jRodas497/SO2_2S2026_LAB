#define _GNU_SOURCE
#include "fswatch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/types.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <openssl/evp.h>

#include "alerts.h"
#include "blacklist.h"
#include "common.h"
#include "http.h"
#include "sensor.h"
#include "sensor.skel.h"

#define MAX_FILES    1000
#define MAX_THREATS  200

#define ST_BENIGNO     "benigno"
#define ST_INFECTADO   "infectado"
#define ST_CUARENTENA  "cuarentena"

typedef struct {
	char path[PATH_MAX];
	char status[12];
	char sha[65];
	char amenaza[96];
	char descripcion[256];
	time_t mtime;
	time_t analizado;
	int used;
} fileinfo_t;

typedef struct {
	char amenaza[96];
	char archivo[PATH_MAX];
	char descripcion[256];
	char sha[65];
	time_t ts;
} threat_rec_t;

static struct sensor_bpf *g_skel;
static struct ring_buffer *g_rb;
static bool g_attached;
static __u32 g_gen;
static char g_dir[PATH_MAX];

static fileinfo_t g_files[MAX_FILES];
static threat_rec_t g_threats[MAX_THREATS];
static int g_nthreats, g_threat_head;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------- libbpf */

static int libbpf_log(enum libbpf_print_level lvl, const char *fmt, va_list ap)
{
	char buf[1024];
	size_t n;

	if (lvl == LIBBPF_DEBUG)
		return 0;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	log_msg("libbpf: %s", buf);
	return 0;
}

/* ---------------------------------------------------------------- SHA-256 */

/* Calcula el SHA-256 del archivo ya abierto. 0 = ok. */
static int sha256_fd(int fd, char out[65])
{
	static const char hexd[] = "0123456789abcdef";
	unsigned char buf[65536], md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	int rc = -1;

	if (!ctx)
		return -1;
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
		goto out;
	for (;;) {
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (r == 0)
			break;
		if (EVP_DigestUpdate(ctx, buf, (size_t)r) != 1)
			goto out;
	}
	if (EVP_DigestFinal_ex(ctx, md, &mdlen) != 1 || mdlen != 32)
		goto out;
	for (unsigned i = 0; i < 32; i++) {
		out[i * 2] = hexd[md[i] >> 4];
		out[i * 2 + 1] = hexd[md[i] & 15];
	}
	out[64] = '\0';
	rc = 0;
out:
	EVP_MD_CTX_free(ctx);
	return rc;
}

/* ---------------------------------------------------------------- tabla */

static fileinfo_t *find_file(const char *path)
{
	for (int i = 0; i < MAX_FILES; i++)
		if (g_files[i].used && strcmp(g_files[i].path, path) == 0)
			return &g_files[i];
	return NULL;
}

/* Devuelve la entrada de path, o una libre / la mas antigua si hay que crearla. */
static fileinfo_t *get_slot(const char *path, int *existed)
{
	fileinfo_t *f = find_file(path), *oldest = NULL;

	*existed = f != NULL;
	if (f)
		return f;
	for (int i = 0; i < MAX_FILES; i++) {
		if (!g_files[i].used)
			return &g_files[i];
		if (!oldest || g_files[i].analizado < oldest->analizado)
			oldest = &g_files[i];
	}
	return oldest;
}

static cJSON *file_json(const fileinfo_t *f)
{
	char ts[32], an[32];
	cJSON *o = cJSON_CreateObject();

	fmt_time(f->mtime, ts, sizeof(ts));
	fmt_time(f->analizado, an, sizeof(an));
	cJSON_AddStringToObject(o, "archivo", f->path);
	cJSON_AddStringToObject(o, "estado", f->status);
	cJSON_AddStringToObject(o, "sha256", f->sha);
	cJSON_AddStringToObject(o, "amenaza", f->amenaza);
	cJSON_AddStringToObject(o, "descripcion", f->descripcion);
	cJSON_AddStringToObject(o, "ultima_modificacion", ts);
	cJSON_AddStringToObject(o, "analizado", an);
	return o;
}

static cJSON *threat_json(const threat_rec_t *t)
{
	char ts[32];
	cJSON *o = cJSON_CreateObject();

	fmt_time(t->ts, ts, sizeof(ts));
	cJSON_AddStringToObject(o, "amenaza", t->amenaza);
	cJSON_AddStringToObject(o, "archivo", t->archivo);
	cJSON_AddStringToObject(o, "descripcion", t->descripcion);
	cJSON_AddStringToObject(o, "sha256", t->sha);
	cJSON_AddStringToObject(o, "timestamp", ts);
	return o;
}

static void publish(const char *event, cJSON *j)
{
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		sse_broadcast(event, s);
		free(s);
	}
	cJSON_Delete(j);
}

/* Registra la deteccion (llamar con mu tomado). Devuelve copia para publicar. */
static threat_rec_t record_threat_locked(const char *path, const threat_t *t, const char *sha)
{
	threat_rec_t *r = &g_threats[g_threat_head];

	memset(r, 0, sizeof(*r));
	snprintf(r->amenaza, sizeof(r->amenaza), "%s", t->nombre);
	snprintf(r->archivo, sizeof(r->archivo), "%s", path);
	snprintf(r->descripcion, sizeof(r->descripcion), "%s", t->descripcion);
	snprintf(r->sha, sizeof(r->sha), "%s", sha);
	r->ts = time(NULL);
	g_threat_head = (g_threat_head + 1) % MAX_THREATS;
	if (g_nthreats < MAX_THREATS)
		g_nthreats++;
	return *r;
}

/* ---------------------------------------------------------- procesar evento */

/* El sensor solo manda ruta/PID; aqui se decide todo: hash, severidad y cuarentena. */
static void process_file(const char *name, const struct sensor_event *e)
{
	char path[PATH_MAX], dir[PATH_MAX], sha[65];
	struct stat st;
	threat_t th;
	fileinfo_t snapshot;
	int fd, infected, existed, was_quarantined = 0, changed = 0, quarantined_now = 0;
	char old_sha[65] = "";
	time_t old_mtime = 0;
	threat_rec_t rec;

	memset(&rec, 0, sizeof(rec));
	pthread_mutex_lock(&mu);
	snprintf(dir, sizeof(dir), "%s", g_dir);
	pthread_mutex_unlock(&mu);

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return;

	/* O_NOFOLLOW: no seguir enlaces; O_NONBLOCK: no quedarse en una FIFO. */
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;                         /* ya no existe: se borro o renombro */
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || sha256_fd(fd, sha) < 0) {
		close(fd);
		return;
	}

	infected = bl_lookup(sha, &th);
	if (infected && fchmod(fd, 0) == 0)
		quarantined_now = 1;
	close(fd);

	pthread_mutex_lock(&mu);
	{
		fileinfo_t *f = get_slot(path, &existed);

		if (existed) {
			snprintf(old_sha, sizeof(old_sha), "%s", f->sha);
			old_mtime = f->mtime;
			was_quarantined = strcmp(f->status, ST_CUARENTENA) == 0;
		}
		changed = !existed || strcmp(old_sha, sha) != 0 || old_mtime != st.st_mtime;

		memset(f, 0, sizeof(*f));
		f->used = 1;
		snprintf(f->path, sizeof(f->path), "%s", path);
		snprintf(f->sha, sizeof(f->sha), "%s", sha);
		f->mtime = st.st_mtime;
		f->analizado = time(NULL);
		if (infected) {
			snprintf(f->amenaza, sizeof(f->amenaza), "%s", th.nombre);
			snprintf(f->descripcion, sizeof(f->descripcion), "%s", th.descripcion);
			snprintf(f->status, sizeof(f->status), "%s",
				 (quarantined_now || was_quarantined) ? ST_CUARENTENA : ST_INFECTADO);
			rec = record_threat_locked(path, &th, sha);
		} else {
			/* un archivo ya puesto en cuarentena sigue en cuarentena */
			snprintf(f->status, sizeof(f->status), "%s",
				 was_quarantined ? ST_CUARENTENA : ST_BENIGNO);
		}
		snapshot = *f;
	}
	pthread_mutex_unlock(&mu);

	if (infected) {
		alert_add(SEV_HIGH, TIPO_ARCHIVOS, "Hash malicioso detectado",
			  "Amenaza %s en %s (SHA-256 %s). Proceso %s PID %u",
			  th.nombre, path, sha, e->comm, e->pid);
		if (quarantined_now && !was_quarantined)
			alert_add(SEV_HIGH, TIPO_ARCHIVOS, "Archivo puesto en cuarentena",
				  "Cuarentena automática de %s (permisos 0000)", path);
		publish("threat", threat_json(&rec));
	} else if (changed) {
		alert_add(SEV_LOW, TIPO_ARCHIVOS, "Archivo modificado",
			  "%s %s (proceso %s, PID %u)", path,
			  existed ? "fue modificado" : "fue creado", e->comm, e->pid);
	}
	publish("file", file_json(&snapshot));
}

static int on_event(void *ctx, void *data, size_t len)
{
	const struct sensor_event *e = data;
	char name[SENSOR_NAME_LEN];
	__u32 gen;

	(void)ctx;
	if (len < sizeof(*e))
		return 0;
	memcpy(name, e->name, sizeof(name));
	name[sizeof(name) - 1] = '\0';

	pthread_mutex_lock(&mu);
	gen = g_gen;
	pthread_mutex_unlock(&mu);
	if (e->gen != gen || strchr(name, '/') || !name[0])
		return 0;                       /* evento de una configuracion anterior */

	process_file(name, e);
	return 0;
}

/* ---------------------------------------------------------------- control */

int fs_init(const char *dir)
{
	__u32 zero = 0, pid = (__u32)getpid();
	char err[256];

	libbpf_set_print(libbpf_log);

	g_skel = sensor_bpf__open_and_load();
	if (!g_skel) {
		log_msg("sensor eBPF: no se pudo cargar (se necesita root y un kernel con BTF/eBPF)");
		return -1;
	}
	/* El sensor descarta los eventos de este PID: evita el bucle al calcular el hash. */
	if (bpf_map_update_elem(bpf_map__fd(g_skel->maps.daemon_pid), &zero, &pid, BPF_ANY) < 0) {
		log_msg("sensor eBPF: no se pudo registrar el PID del daemon: %s", strerror(errno));
		sensor_bpf__destroy(g_skel);
		g_skel = NULL;
		return -1;
	}
	g_rb = ring_buffer__new(bpf_map__fd(g_skel->maps.events), on_event, NULL, NULL);
	if (!g_rb) {
		log_msg("sensor eBPF: no se pudo crear el ring buffer");
		sensor_bpf__destroy(g_skel);
		g_skel = NULL;
		return -1;
	}
	log_msg("sensor eBPF cargado (PID del daemon registrado: %u)", pid);

	if (dir && *dir && fs_set_dir(dir, err, sizeof(err)) < 0)
		log_msg("ruta a monitorear inicial '%s' rechazada: %s", dir, err);
	return 0;
}

void *fs_thread(void *arg)
{
	(void)arg;
	log_msg("hilo de archivos iniciado");
	while (g_running) {
		if (!g_rb) {
			msleep_run(500);
			continue;
		}
		int n = ring_buffer__poll(g_rb, 200 /* ms, bloquea en epoll */);

		if (n < 0 && n != -EINTR) {
			log_msg("ring buffer: %s", strerror(-n));
			msleep_run(500);
		}
	}
	log_msg("hilo de archivos terminado");
	return NULL;
}

void fs_shutdown(void)
{
	if (g_skel && g_attached)
		sensor_bpf__detach(g_skel);
	g_attached = false;
	if (g_rb)
		ring_buffer__free(g_rb);
	g_rb = NULL;
	if (g_skel)
		sensor_bpf__destroy(g_skel);
	g_skel = NULL;
}

bool fs_is_available(void) { return g_skel != NULL; }
bool fs_is_enabled(void) { return g_attached; }

int fs_set_enabled(bool on, char *err, size_t errn)
{
	int rc;

	if (!g_skel) {
		snprintf(err, errn, "El sensor eBPF no esta disponible (no se pudo cargar al iniciar)");
		return -1;
	}
	if (on == g_attached)
		return 0;
	if (on) {
		if (!g_dir[0]) {
			snprintf(err, errn, "Primero configure la ruta a monitorear");
			return -1;
		}
		rc = sensor_bpf__attach(g_skel);
		if (rc) {
			snprintf(err, errn, "No se pudo enganchar el sensor: %s", strerror(-rc));
			return -1;
		}
		g_attached = true;
		log_msg("escaneo de archivos activado (eBPF enganchado)");
	} else {
		sensor_bpf__detach(g_skel);
		g_attached = false;
		log_msg("escaneo de archivos desactivado (eBPF desenganchado)");
	}
	http_publish_flags();
	return 0;
}

/* "/tmp/monitor" -> comp[0]="monitor", comp[1]="tmp", ncomp=2 */
static int build_cfg(const char *resolved_in, struct sensor_cfg *cfg, __u32 gen,
		     char *err, size_t errn)
{
	char resolved[PATH_MAX];
	char *comps[SENSOR_MAX_COMPS];
	char *save = NULL, *tok;
	int n = 0;

	snprintf(resolved, sizeof(resolved), "%s", resolved_in);
	memset(cfg, 0, sizeof(*cfg));
	for (tok = strtok_r(resolved, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (n >= SENSOR_MAX_COMPS) {
			snprintf(err, errn, "Directorio demasiado profundo (maximo %d niveles)", SENSOR_MAX_COMPS);
			return -1;
		}
		if (strlen(tok) >= SENSOR_COMP_LEN) {
			snprintf(err, errn, "El nombre '%s' es demasiado largo (maximo %d caracteres)",
				 tok, SENSOR_COMP_LEN - 1);
			return -1;
		}
		comps[n++] = tok;
	}
	if (n == 0) {
		snprintf(err, errn, "No se permite monitorear la raiz '/'");
		return -1;
	}
	for (int i = 0; i < n; i++)
		snprintf(cfg->comp[i], SENSOR_COMP_LEN, "%s", comps[n - 1 - i]);
	cfg->ncomp = (__u32)n;
	cfg->gen = gen;
	return 0;
}

int fs_set_dir(const char *dir, char *err, size_t errn)
{
	char resolved[PATH_MAX];
	struct sensor_cfg cfg;
	struct stat st;
	char old_dir[PATH_MAX];
	__u32 zero = 0, gen, old_gen;

	if (!g_skel) {
		snprintf(err, errn, "El sensor eBPF no esta disponible");
		return -1;
	}
	if (!dir || dir[0] != '/') {
		snprintf(err, errn, "La ruta debe ser absoluta");
		return -1;
	}
	if (!realpath(dir, resolved)) {
		snprintf(err, errn, "No se pudo resolver '%s': %s", dir, strerror(errno));
		return -1;
	}
	if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
		snprintf(err, errn, "'%s' no es un directorio", resolved);
		return -1;
	}

	pthread_mutex_lock(&mu);
	gen = g_gen + 1;
	pthread_mutex_unlock(&mu);

	/* Primero se valida todo; el estado solo cambia si la ruta es aceptable. */
	if (build_cfg(resolved, &cfg, gen, err, errn) < 0)
		return -1;

	pthread_mutex_lock(&mu);
	old_gen = g_gen;
	snprintf(old_dir, sizeof(old_dir), "%s", g_dir);
	g_gen = gen;
	snprintf(g_dir, sizeof(g_dir), "%s", resolved);
	pthread_mutex_unlock(&mu);

	if (bpf_map_update_elem(bpf_map__fd(g_skel->maps.cfg_map), &zero, &cfg, BPF_ANY) < 0) {
		snprintf(err, errn, "No se pudo actualizar el mapa eBPF: %s", strerror(errno));
		pthread_mutex_lock(&mu);
		g_gen = old_gen;
		snprintf(g_dir, sizeof(g_dir), "%s", old_dir);
		pthread_mutex_unlock(&mu);
		return -1;
	}
	log_msg("ruta a monitorear: %s (generacion %u)", resolved, gen);
	return 0;
}

void fs_get_dir(char *out, size_t n)
{
	pthread_mutex_lock(&mu);
	snprintf(out, n, "%s", g_dir);
	pthread_mutex_unlock(&mu);
}

int fs_quarantine(const char *path, const char *quien, char *err, size_t errn)
{
	fileinfo_t *f;
	int fd, rc = -1;

	if (!path || !*path) {
		snprintf(err, errn, "Falta la ruta del archivo");
		return -1;
	}

	/* Solo se pueden poner en cuarentena archivos que el sensor ya analizo:
	 * evita que un administrador deje sin permisos archivos arbitrarios del sistema. */
	pthread_mutex_lock(&mu);
	f = find_file(path);
	if (!f) {
		pthread_mutex_unlock(&mu);
		snprintf(err, errn, "El archivo no esta en la lista de archivos analizados");
		return -1;
	}
	if (strcmp(f->status, ST_CUARENTENA) == 0) {
		pthread_mutex_unlock(&mu);
		snprintf(err, errn, "El archivo ya esta en cuarentena");
		return -1;
	}
	pthread_mutex_unlock(&mu);

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errn, "No se pudo abrir el archivo: %s", strerror(errno));
		return -1;
	}
	if (fchmod(fd, 0) < 0) {
		snprintf(err, errn, "chmod 0000 fallo: %s", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	{
		cJSON *j = NULL;

		pthread_mutex_lock(&mu);
		f = find_file(path);
		if (f) {
			snprintf(f->status, sizeof(f->status), "%s", ST_CUARENTENA);
			j = file_json(f);
			rc = 0;
		}
		pthread_mutex_unlock(&mu);
		if (j)
			publish("file", j);
	}
	if (rc == 0)
		alert_add(SEV_HIGH, TIPO_ARCHIVOS, "Archivo puesto en cuarentena",
			  "Cuarentena manual de %s por %s (permisos 0000)", path, quien);
	else
		snprintf(err, errn, "El archivo desaparecio de la lista");
	return rc;
}

void fs_reevaluate(void)
{
	/* Recorre solo la tabla en memoria (usa el SHA-256 guardado, no relee el disco). */
	for (int i = 0; i < MAX_FILES; i++) {
		char path[PATH_MAX], sha[65];
		threat_t th;
		int hit = 0;

		pthread_mutex_lock(&mu);
		if (g_files[i].used && strcmp(g_files[i].status, ST_BENIGNO) == 0) {
			memcpy(path, g_files[i].path, sizeof(path));
			path[sizeof(path) - 1] = '\0';
			memcpy(sha, g_files[i].sha, sizeof(sha));
			hit = 1;
		}
		pthread_mutex_unlock(&mu);
		if (!hit || !bl_lookup(sha, &th))
			continue;

		{
			int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
			int quarantined = fd >= 0 && fchmod(fd, 0) == 0;
			threat_rec_t rec;
			cJSON *j = NULL;

			if (fd >= 0)
				close(fd);
			pthread_mutex_lock(&mu);
			if (g_files[i].used && strcmp(g_files[i].path, path) == 0) {
				snprintf(g_files[i].amenaza, sizeof(g_files[i].amenaza), "%s", th.nombre);
				snprintf(g_files[i].descripcion, sizeof(g_files[i].descripcion), "%s", th.descripcion);
				snprintf(g_files[i].status, sizeof(g_files[i].status), "%s",
					 quarantined ? ST_CUARENTENA : ST_INFECTADO);
				rec = record_threat_locked(path, &th, sha);
				j = file_json(&g_files[i]);
			}
			pthread_mutex_unlock(&mu);
			if (!j)
				continue;
			alert_add(SEV_HIGH, TIPO_ARCHIVOS, "Hash malicioso detectado",
				  "Amenaza %s en %s (detectada al actualizar la base de datos)",
				  th.nombre, path);
			if (quarantined)
				alert_add(SEV_HIGH, TIPO_ARCHIVOS, "Archivo puesto en cuarentena",
					  "Cuarentena automática de %s (permisos 0000)", path);
			publish("threat", threat_json(&rec));
			publish("file", j);
		}
	}
}

/* ------------------------------------------------------------------ JSON */

cJSON *fs_files_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_FILES; i++)
		if (g_files[i].used)
			cJSON_AddItemToArray(arr, file_json(&g_files[i]));
	pthread_mutex_unlock(&mu);
	return arr;
}

cJSON *fs_threats_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	for (int i = 0; i < g_nthreats; i++) {
		int idx = (g_threat_head - 1 - i + MAX_THREATS) % MAX_THREATS;

		cJSON_AddItemToArray(arr, threat_json(&g_threats[idx]));
	}
	pthread_mutex_unlock(&mu);
	return arr;
}

cJSON *fs_quarantine_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_FILES; i++)
		if (g_files[i].used && strcmp(g_files[i].status, ST_CUARENTENA) == 0)
			cJSON_AddItemToArray(arr, file_json(&g_files[i]));
	pthread_mutex_unlock(&mu);
	return arr;
}
