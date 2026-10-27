#define _GNU_SOURCE
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "alerts.h"
#include "auth.h"
#include "blacklist.h"
#include "cJSON.h"
#include "common.h"
#include "fswatch.h"
#include "logwatch.h"
#include "metrics.h"

#define HDR_MAX    16384
#define BODY_MAX   (1 << 20)
#define MAX_CONN   64
#define MAX_SSE    32
#define STATIC_MAX (8 << 20)

struct req {
	char method[8];
	char path[768];
	char query[256];
	char token[65];
	char *body;
	size_t blen;
};

struct who {
	char user[48];
	role_t role;
};

typedef struct {
	int fd;
	int active;
} sse_client_t;

static int g_listen = -1;
static pthread_t g_accept_thr;
static char g_dash[PATH_MAX];
static atomic_int g_conns;
static sse_client_t g_sse[MAX_SSE];
static pthread_mutex_t sse_mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------- respuestas */

static const char *status_text(int s)
{
	switch (s) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 413: return "Payload Too Large";
	case 503: return "Service Unavailable";
	default:  return "Internal Server Error";
	}
}

static void send_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

		if (w <= 0)
			return;
		p += w;
		n -= (size_t)w;
	}
}

static void respond(int fd, int status, const char *ctype, const char *body, size_t len,
		    const char *extra)
{
	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
			 "Connection: close\r\n%s\r\n",
			 status, status_text(status), ctype, len, extra ? extra : "");

	send_all(fd, hdr, (size_t)n);
	if (len)
		send_all(fd, body, len);
}

static void send_json(int fd, int status, cJSON *j, const char *extra)
{
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		respond(fd, status, "application/json; charset=utf-8", s, strlen(s), extra);
		free(s);
	} else {
		respond(fd, 500, "text/plain", "error", 5, NULL);
	}
	cJSON_Delete(j);
}

static void send_err(int fd, int status, const char *msg)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", msg);
	send_json(fd, status, o, NULL);
}

static void send_ok(int fd)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddBoolToObject(o, "ok", 1);
	send_json(fd, 200, o, NULL);
}

/* ------------------------------------------------------------------- SSE */

void sse_broadcast(const char *event, const char *json)
{
	char *msg;
	int n;

	n = asprintf(&msg, "event: %s\ndata: %s\n\n", event, json);
	if (n < 0)
		return;
	pthread_mutex_lock(&sse_mu);
	for (int i = 0; i < MAX_SSE; i++) {
		if (!g_sse[i].active)
			continue;
		if (send(g_sse[i].fd, msg, (size_t)n, MSG_NOSIGNAL) != n) {
			g_sse[i].active = 0;            /* el hilo de la conexion la cierra */
			shutdown(g_sse[i].fd, SHUT_RDWR);
		}
	}
	pthread_mutex_unlock(&sse_mu);
	free(msg);
}

static cJSON *flags_json(void)
{
	char dir[PATH_MAX];
	cJSON *o = cJSON_CreateObject();

	fs_get_dir(dir, sizeof(dir));
	cJSON_AddBoolToObject(o, "monitoreo", monitor_is_enabled());
	cJSON_AddBoolToObject(o, "procesos", procscan_is_enabled());
	cJSON_AddBoolToObject(o, "archivos", fs_is_enabled());
	cJSON_AddBoolToObject(o, "sensor_disponible", fs_is_available());
	cJSON_AddStringToObject(o, "ruta", dir);
	return o;
}

void http_publish_flags(void)
{
	cJSON *j = flags_json();
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		sse_broadcast("flags", s);
		free(s);
	}
	cJSON_Delete(j);
}

static void publish_logwatches(void)
{
	cJSON *j = lw_list_json();
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		sse_broadcast("logwatches", s);
		free(s);
	}
	cJSON_Delete(j);
}

static void handle_stream(int fd)
{
	static const char hdr[] =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
		"Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\nretry: 3000\n\n";
	struct timeval tv = { .tv_sec = 2 };
	int slot = -1;

	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	pthread_mutex_lock(&sse_mu);
	for (int i = 0; i < MAX_SSE; i++)
		if (!g_sse[i].active) {
			g_sse[i].fd = fd;
			g_sse[i].active = 1;
			slot = i;
			break;
		}
	pthread_mutex_unlock(&sse_mu);
	if (slot < 0) {
		send_err(fd, 503, "Demasiadas conexiones en tiempo real");
		return;
	}
	send_all(fd, hdr, sizeof(hdr) - 1);

	while (g_running) {
		int alive;

		for (int i = 0; i < 150 && g_running; i++) {    /* 15 s entre latidos */
			usleep(100000);
			pthread_mutex_lock(&sse_mu);
			alive = g_sse[slot].active;
			pthread_mutex_unlock(&sse_mu);
			if (!alive)
				goto out;
		}
		pthread_mutex_lock(&sse_mu);
		if (g_sse[slot].active &&
		    send(fd, ": ping\n\n", 8, MSG_NOSIGNAL) != 8) {
			g_sse[slot].active = 0;
			shutdown(fd, SHUT_RDWR);
		}
		alive = g_sse[slot].active;
		pthread_mutex_unlock(&sse_mu);
		if (!alive)
			break;
	}
out:
	pthread_mutex_lock(&sse_mu);
	g_sse[slot].active = 0;
	pthread_mutex_unlock(&sse_mu);
}

/* ------------------------------------------------------------- peticiones */

static int header_value(const char *hs, const char *name, char *out, size_t n)
{
	char key[48];
	const char *p, *e;

	snprintf(key, sizeof(key), "\r\n%s:", name);
	p = strcasestr(hs, key);
	if (!p)
		return -1;
	p += strlen(key);
	while (*p == ' ' || *p == '\t')
		p++;
	e = strstr(p, "\r\n");
	if (!e)
		e = p + strlen(p);
	if ((size_t)(e - p) >= n)
		return -1;
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = '\0';
	return 0;
}

/* 0 = ok, -1 = mala peticion, -2 = cuerpo demasiado grande */
static int read_request(int fd, struct req *r)
{
	char hdr[HDR_MAX + 1], target[768], val[HDR_MAX], *hs, *q;
	size_t have = 0, hlen, extra, clen = 0;
	char *end;

	memset(r, 0, sizeof(*r));
	while (!(end = memmem(hdr, have, "\r\n\r\n", 4))) {
		ssize_t n;

		if (have >= HDR_MAX)
			return -1;
		n = recv(fd, hdr + have, HDR_MAX - have, 0);
		if (n <= 0)
			return -1;
		have += (size_t)n;
	}
	hlen = (size_t)(end - hdr) + 4;
	hs = strndup(hdr, hlen);
	if (!hs)
		return -1;

	if (sscanf(hs, "%7s %767s", r->method, target) != 2) {
		free(hs);
		return -1;
	}
	q = strchr(target, '?');
	if (q) {
		*q++ = '\0';
		snprintf(r->query, sizeof(r->query), "%s", q);
	}
	snprintf(r->path, sizeof(r->path), "%s", target);

	if (header_value(hs, "Cookie", val, sizeof(val)) == 0) {
		char *c = strstr(val, "so2sid=");

		if (c) {
			c += 7;
			for (size_t i = 0; i < 64 && c[i] && c[i] != ';' && c[i] != ' '; i++)
				r->token[i] = c[i];
		}
	}
	if (header_value(hs, "Content-Length", val, sizeof(val)) == 0)
		clen = strtoul(val, NULL, 10);
	free(hs);

	if (clen > BODY_MAX)
		return -2;
	if (clen) {
		size_t got;

		r->body = malloc(clen + 1);
		if (!r->body)
			return -1;
		extra = have - hlen;
		got = extra < clen ? extra : clen;
		memcpy(r->body, hdr + hlen, got);
		while (got < clen) {
			ssize_t n = recv(fd, r->body + got, clen - got, 0);

			if (n <= 0) {
				free(r->body);
				r->body = NULL;
				return -1;
			}
			got += (size_t)n;
		}
		r->body[clen] = '\0';
		r->blen = clen;
	}
	return 0;
}

static cJSON *parse_body(const struct req *r)
{
	return r->body ? cJSON_ParseWithLength(r->body, r->blen) : NULL;
}

static const char *str_field(cJSON *j, const char *name)
{
	cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);

	return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* Devuelve 0 si "name" es booleano en j. */
static int bool_field(cJSON *j, const char *name, bool *out)
{
	cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);

	if (!cJSON_IsBool(it))
		return -1;
	*out = cJSON_IsTrue(it);
	return 0;
}

static int int_field(cJSON *j, const char *name, int *out)
{
	cJSON *it = cJSON_GetObjectItemCaseSensitive(j, name);

	if (!cJSON_IsNumber(it) || it->valuedouble < 1 || it->valuedouble > 2147483647.0)
		return -1;
	*out = it->valueint;
	return 0;
}

static cJSON *build_state(const struct who *w)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddStringToObject(o, "usuario", w->user);
	cJSON_AddStringToObject(o, "rol", role_name(w->role));
	cJSON_AddItemToObject(o, "flags", flags_json());
	cJSON_AddItemToObject(o, "metricas", metrics_last_json());
	cJSON_AddItemToObject(o, "historial", metrics_history_json());
	cJSON_AddItemToObject(o, "procesos", procs_last_json());
	cJSON_AddItemToObject(o, "alertas", alerts_json(200));
	cJSON_AddItemToObject(o, "archivos", fs_files_json());
	cJSON_AddItemToObject(o, "amenazas", fs_threats_json());
	cJSON_AddItemToObject(o, "logwatches", lw_list_json());
	cJSON_AddNumberToObject(o, "firmas", bl_count());
	return o;
}

static int query_int(const char *query, const char *key)
{
	char k[32];
	const char *p;

	snprintf(k, sizeof(k), "%s=", key);
	p = strstr(query, k);
	if (!p || (p != query && p[-1] != '&'))
		return -1;
	return atoi(p + strlen(k));
}

static void api_login(int fd, const struct req *r)
{
	cJSON *b = parse_body(r);
	const char *u = b ? str_field(b, "usuario") : NULL;
	const char *p = b ? str_field(b, "password") : NULL;
	char err[160], token[65], cookie[160], uname[48];
	role_t role;

	if (!u || !p) {
		cJSON_Delete(b);
		send_err(fd, 400, "Se requieren 'usuario' y 'password'");
		return;
	}
	snprintf(uname, sizeof(uname), "%.40s", u);
	if (auth_check(u, p, &role, err, sizeof(err)) < 0) {
		cJSON_Delete(b);
		alert_add(SEV_MEDIUM, TIPO_SESION, "Inicio de sesión fallido",
			  "Intento fallido del usuario '%s': %s", uname, err);
		send_err(fd, 401, err);
		return;
	}
	cJSON_Delete(b);
	if (session_create(uname, role, token) < 0) {
		send_err(fd, 500, "No se pudo crear la sesion");
		return;
	}
	alert_add(SEV_LOW, TIPO_SESION, "Inicio de sesión exitoso",
		  "El usuario %s inició sesión como %s", uname, role_name(role));

	snprintf(cookie, sizeof(cookie),
		 "Set-Cookie: so2sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800\r\n", token);
	{
		cJSON *o = cJSON_CreateObject();

		cJSON_AddBoolToObject(o, "ok", 1);
		cJSON_AddStringToObject(o, "usuario", uname);
		cJSON_AddStringToObject(o, "rol", role_name(role));
		send_json(fd, 200, o, cookie);
	}
}

static void api(int fd, const struct req *r)
{
	struct who w;
	int authed;
	bool is_post = strcmp(r->method, "POST") == 0;
	bool is_get = strcmp(r->method, "GET") == 0;
	const char *p = r->path;
	char err[256];
	cJSON *b = NULL;

	if (is_get && strcmp(p, "/api/ping") == 0) {
		send_ok(fd);
		return;
	}
	if (is_post && strcmp(p, "/api/login") == 0) {
		api_login(fd, r);
		return;
	}

	authed = session_lookup(r->token, w.user, sizeof(w.user), &w.role) == 0;
	if (!authed) {
		send_err(fd, 401, "No autenticado");
		return;
	}

	/* ---- permitido a administrador y usuario comun ---- */
	if (is_post && strcmp(p, "/api/logout") == 0) {
		session_destroy(r->token);
		send_json(fd, 200, cJSON_CreateObject(),
			  "Set-Cookie: so2sid=; Path=/; Max-Age=0\r\n");
		return;
	}
	if (is_get && strcmp(p, "/api/me") == 0) {
		cJSON *o = cJSON_CreateObject();

		cJSON_AddStringToObject(o, "usuario", w.user);
		cJSON_AddStringToObject(o, "rol", role_name(w.role));
		send_json(fd, 200, o, NULL);
		return;
	}
	if (is_get && strcmp(p, "/api/state") == 0) {
		send_json(fd, 200, build_state(&w), NULL);
		return;
	}
	if (is_get && strcmp(p, "/api/stream") == 0) {
		handle_stream(fd);
		return;
	}

	/* ---- solo administrador: el control de permisos se hace aqui, en el daemon ---- */
	if (w.role != ROLE_ADMIN) {
		send_err(fd, 403, "Permiso denegado: se requiere rol administrador");
		return;
	}

	if (is_get && strcmp(p, "/api/quarantine") == 0) {
		send_json(fd, 200, fs_quarantine_json(), NULL);
		return;
	}
	if (is_get && strcmp(p, "/api/process") == 0) {
		cJSON *info = NULL;
		int pid = query_int(r->query, "pid");

		if (proc_info_json(pid, &info, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else
			send_json(fd, 200, info, NULL);
		return;
	}

	if (!is_post) {
		send_err(fd, 405, "Metodo no permitido");
		return;
	}

	if (strcmp(p, "/api/blacklist") == 0) {          /* el cuerpo ES el archivo JSON */
		int n;

		if (!r->body) {
			send_err(fd, 400, "Falta el archivo JSON");
			return;
		}
		n = bl_replace(r->body, r->blen, 1, err, sizeof(err));
		if (n < 0) {
			send_err(fd, 400, err);
			return;
		}
		alert_add(SEV_MEDIUM, TIPO_SISTEMA, "Actualización base de datos local",
			  "%s actualizó la base de hashes malignos (%d firmas)", w.user, n);
		fs_reevaluate();
		{
			cJSON *o = cJSON_CreateObject();

			cJSON_AddBoolToObject(o, "ok", 1);
			cJSON_AddNumberToObject(o, "firmas", n);
			send_json(fd, 200, o, NULL);
		}
		return;
	}

	b = parse_body(r);
	if (!b) {
		send_err(fd, 400, "Cuerpo JSON invalido");
		return;
	}

	if (strcmp(p, "/api/monitoring") == 0 || strcmp(p, "/api/procscan") == 0 ||
	    strcmp(p, "/api/filescan") == 0) {
		bool on;

		if (bool_field(b, "activo", &on) < 0) {
			send_err(fd, 400, "Se requiere 'activo' (true/false)");
		} else if (strcmp(p, "/api/monitoring") == 0) {
			monitor_set_enabled(on, w.user);
			send_ok(fd);
		} else if (strcmp(p, "/api/procscan") == 0) {
			procscan_set_enabled(on, w.user);
			send_ok(fd);
		} else if (fs_set_enabled(on, err, sizeof(err)) < 0) {
			send_err(fd, 400, err);
		} else {
			send_ok(fd);
		}
	} else if (strcmp(p, "/api/path") == 0) {
		const char *ruta = str_field(b, "ruta");

		if (!ruta)
			send_err(fd, 400, "Se requiere 'ruta'");
		else if (fs_set_dir(ruta, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else {
			http_publish_flags();
			send_ok(fd);
		}
	} else if (strcmp(p, "/api/kill") == 0) {
		int pid;

		if (int_field(b, "pid", &pid) < 0)
			send_err(fd, 400, "Se requiere 'pid' numerico");
		else if (proc_kill(pid, w.user, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else
			send_ok(fd);
	} else if (strcmp(p, "/api/quarantine") == 0) {
		const char *ruta = str_field(b, "ruta");

		if (!ruta)
			send_err(fd, 400, "Se requiere 'ruta'");
		else if (fs_quarantine(ruta, w.user, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else
			send_ok(fd);
	} else if (strcmp(p, "/api/logwatch") == 0) {
		const char *rutas[LW_MAX_RUTAS];
		cJSON *arr = cJSON_GetObjectItemCaseSensitive(b, "rutas"), *it;
		const char *central = str_field(b, "central"), *palabra = str_field(b, "palabra");
		int n = 0, id;

		if (!central || !palabra || !cJSON_IsArray(arr)) {
			send_err(fd, 400, "Se requieren 'central', 'palabra' y 'rutas' (lista)");
			goto done;
		}
		cJSON_ArrayForEach(it, arr) {
			if (!cJSON_IsString(it) || n >= LW_MAX_RUTAS) {
				send_err(fd, 400, "'rutas' debe ser una lista de hasta 8 textos");
				goto done;
			}
			rutas[n++] = it->valuestring;
		}
		id = lw_start(central, rutas, n, palabra, w.user, err, sizeof(err));
		if (id < 0) {
			send_err(fd, 400, err);
		} else {
			cJSON *o = cJSON_CreateObject();

			cJSON_AddBoolToObject(o, "ok", 1);
			cJSON_AddNumberToObject(o, "id", id);
			publish_logwatches();
			send_json(fd, 200, o, NULL);
		}
	} else if (strcmp(p, "/api/logwatch/stop") == 0) {
		int id;

		if (int_field(b, "id", &id) < 0)
			send_err(fd, 400, "Se requiere 'id' numerico");
		else if (lw_stop(id, w.user, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else {
			publish_logwatches();
			send_ok(fd);
		}
	} else {
		send_err(fd, 404, "Ruta no encontrada");
	}
done:
	cJSON_Delete(b);
}

/* ------------------------------------------------------------- estaticos */

static const char *mime_of(const char *path)
{
	const char *e = strrchr(path, '.');

	if (!e) return "application/octet-stream";
	if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(e, ".js"))   return "application/javascript; charset=utf-8";
	if (!strcmp(e, ".css"))  return "text/css; charset=utf-8";
	if (!strcmp(e, ".json")) return "application/json; charset=utf-8";
	if (!strcmp(e, ".svg"))  return "image/svg+xml";
	if (!strcmp(e, ".png"))  return "image/png";
	if (!strcmp(e, ".ico"))  return "image/x-icon";
	return "application/octet-stream";
}

static void serve_static(int fd, const struct req *r)
{
	char full[PATH_MAX + 800];
	const char *p = r->path;
	char *data = NULL;
	size_t len = 0;
	struct stat st;

	if (strcmp(r->method, "GET") != 0) {
		send_err(fd, 405, "Metodo no permitido");
		return;
	}
	if (strcmp(p, "/") == 0)
		p = "/index.html";
	if (p[0] != '/' || strstr(p, "..") || strstr(p, "//")) {
		send_err(fd, 404, "No encontrado");
		return;
	}
	for (const char *c = p; *c; c++)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
		      (*c >= '0' && *c <= '9') || *c == '.' || *c == '/' || *c == '-' || *c == '_')) {
			send_err(fd, 404, "No encontrado");
			return;
		}

	snprintf(full, sizeof(full), "%s%s", g_dash, p);
	if (stat(full, &st) < 0 || !S_ISREG(st.st_mode) ||
	    read_file(full, &data, &len, STATIC_MAX) < 0) {
		send_err(fd, 404, "No encontrado");
		return;
	}
	respond(fd, 200, mime_of(full), data, len, NULL);
	free(data);
}

/* --------------------------------------------------------------- servidor */

static void *conn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	struct timeval tv = { .tv_sec = 10 };
	struct req r;
	int rc;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	rc = read_request(fd, &r);
	if (rc == -2) {
		send_err(fd, 413, "Archivo demasiado grande");
	} else if (rc < 0) {
		send_err(fd, 400, "Peticion invalida");
	} else {
		if (strncmp(r.path, "/api/", 5) == 0)
			api(fd, &r);
		else
			serve_static(fd, &r);
		free(r.body);
	}
	close(fd);
	atomic_fetch_sub(&g_conns, 1);
	return NULL;
}

static void *accept_thread(void *arg)
{
	(void)arg;
	while (g_running) {
		struct pollfd pfd = { .fd = g_listen, .events = POLLIN };
		pthread_t t;
		pthread_attr_t at;
		int fd;

		if (poll(&pfd, 1, 500) <= 0)
			continue;
		fd = accept4(g_listen, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0)
			continue;
		if (atomic_load(&g_conns) >= MAX_CONN) {
			send_err(fd, 503, "Servidor ocupado");
			close(fd);
			continue;
		}
		atomic_fetch_add(&g_conns, 1);
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd) != 0) {
			atomic_fetch_sub(&g_conns, 1);
			close(fd);
		}
		pthread_attr_destroy(&at);
	}
	return NULL;
}

int http_start(int port, const char *dashboard_dir)
{
	struct sockaddr_in a;
	int one = 1;

	snprintf(g_dash, sizeof(g_dash), "%s", dashboard_dir);
	g_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (g_listen < 0)
		return -1;
	setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)port);
	if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 64) < 0) {
		log_msg("HTTP: no se pudo escuchar en el puerto %d: %s", port, strerror(errno));
		close(g_listen);
		g_listen = -1;
		return -1;
	}
	if (pthread_create(&g_accept_thr, NULL, accept_thread, NULL) != 0) {
		close(g_listen);
		g_listen = -1;
		return -1;
	}
	log_msg("HTTP: escuchando en el puerto %d (dashboard: %s)", port, g_dash);
	return 0;
}

void http_stop(void)
{
	if (g_listen < 0)
		return;
	pthread_join(g_accept_thr, NULL);
	close(g_listen);
	g_listen = -1;
	for (int i = 0; i < 40 && atomic_load(&g_conns) > 0; i++)
		usleep(100000);                 /* esperar a que terminen las conexiones */
}
