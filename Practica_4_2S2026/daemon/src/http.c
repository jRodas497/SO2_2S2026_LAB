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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"
#include "sistema.h"

/*
 * API de solo lectura (la P4 no pide autenticacion ni acciones):
 *   GET /api/ping           -> {"ok":true}
 *   GET /api/datos          -> JSON del enunciado (memoria, procesos, eventos_seguridad)
 *   GET /api/proceso?pid=N  -> detalle de un proceso (sys_get_process_info)
 *   GET /api/stream         -> Server-Sent Events: "datos" cada intervalo y "evento" al instante
 *   cualquier otra ruta     -> archivos del dashboard
 */

#define HDR_MAX    8192
#define MAX_CONN   64
#define MAX_SSE    32
#define STATIC_MAX (8 << 20)

struct req {
	char method[8];
	char path[768];
	char query[256];
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
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
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

static void respond(int fd, int status, const char *ctype, const char *body, size_t len)
{
	char hdr[384];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
			 "Connection: close\r\n\r\n", status, status_text(status), ctype, len);

	send_all(fd, hdr, (size_t)n);
	if (len)
		send_all(fd, body, len);
}

static void send_json(int fd, int status, cJSON *j)
{
	char *s = cJSON_PrintUnformatted(j);

	if (s) {
		respond(fd, status, "application/json; charset=utf-8", s, strlen(s));
		free(s);
	} else {
		respond(fd, 500, "text/plain", "error", 5);
	}
	cJSON_Delete(j);
}

static void send_err(int fd, int status, const char *msg)
{
	cJSON *o = cJSON_CreateObject();

	cJSON_AddBoolToObject(o, "ok", 0);
	cJSON_AddStringToObject(o, "error", msg);
	send_json(fd, status, o);
}

/* ------------------------------------------------------------------- SSE */

void sse_broadcast(const char *event, const char *json)
{
	char *msg;
	int n = asprintf(&msg, "event: %s\ndata: %s\n\n", event, json);

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
		if (g_sse[slot].active && send(fd, ": ping\n\n", 8, MSG_NOSIGNAL) != 8) {
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

static int read_request(int fd, struct req *r)
{
	char hdr[HDR_MAX + 1], target[768], *q;
	size_t have = 0;

	memset(r, 0, sizeof(*r));
	while (!memmem(hdr, have, "\r\n\r\n", 4)) {
		ssize_t n;

		if (have >= HDR_MAX)
			return -1;
		n = recv(fd, hdr + have, HDR_MAX - have, 0);
		if (n <= 0)
			return -1;
		have += (size_t)n;
	}
	hdr[have] = '\0';
	if (sscanf(hdr, "%7s %767s", r->method, target) != 2)
		return -1;
	q = strchr(target, '?');
	if (q) {
		*q++ = '\0';
		snprintf(r->query, sizeof(r->query), "%s", q);
	}
	snprintf(r->path, sizeof(r->path), "%s", target);
	return 0;
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

static void api(int fd, const struct req *r)
{
	char err[256];

	if (strcmp(r->method, "GET") != 0) {
		send_err(fd, 405, "Método no permitido");
		return;
	}
	if (strcmp(r->path, "/api/ping") == 0) {
		cJSON *o = cJSON_CreateObject();

		cJSON_AddBoolToObject(o, "ok", 1);
		send_json(fd, 200, o);
	} else if (strcmp(r->path, "/api/datos") == 0) {
		send_json(fd, 200, datos_json());
	} else if (strcmp(r->path, "/api/proceso") == 0) {
		cJSON *info = NULL;

		if (proceso_json(query_int(r->query, "pid"), &info, err, sizeof(err)) < 0)
			send_err(fd, 400, err);
		else
			send_json(fd, 200, info);
	} else if (strcmp(r->path, "/api/stream") == 0) {
		handle_stream(fd);
	} else {
		send_err(fd, 404, "Ruta no encontrada");
	}
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
		send_err(fd, 405, "Método no permitido");
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
	respond(fd, 200, mime_of(full), data, len);
	free(data);
}

/* --------------------------------------------------------------- servidor */

static void *conn_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	struct timeval tv = { .tv_sec = 10 };
	struct req r;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (read_request(fd, &r) < 0) {
		send_err(fd, 400, "Petición inválida");
	} else if (strncmp(r.path, "/api/", 5) == 0) {
		api(fd, &r);
	} else {
		serve_static(fd, &r);
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

int http_start(const char *bind_addr, int port, const char *dashboard_dir)
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
	a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) {
		log_msg("HTTP: dirección de escucha inválida: %s", bind_addr);
		close(g_listen);
		g_listen = -1;
		return -1;
	}
	if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 64) < 0) {
		log_msg("HTTP: no se pudo escuchar en %s:%d: %s", bind_addr, port, strerror(errno));
		close(g_listen);
		g_listen = -1;
		return -1;
	}
	if (pthread_create(&g_accept_thr, NULL, accept_thread, NULL) != 0) {
		close(g_listen);
		g_listen = -1;
		return -1;
	}
	log_msg("HTTP: escuchando en %s:%d (dashboard: %s)", bind_addr, port, g_dash);
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
