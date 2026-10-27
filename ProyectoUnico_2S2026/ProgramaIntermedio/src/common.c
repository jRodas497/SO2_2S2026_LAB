#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile sig_atomic_t g_running = 1;

static FILE *g_log;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *path)
{
	if (path) {
		FILE *f = fopen(path, "a");

		if (f) {
			g_log = f;
			return;
		}
		fprintf(stderr, "No se pudo abrir el log %s: %s (se usa stderr)\n",
			path, strerror(errno));
	}
	g_log = stderr;
}

void fmt_time(time_t t, char *buf, size_t n)
{
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void log_msg(const char *fmt, ...)
{
	char ts[32];
	va_list ap;

	fmt_time(time(NULL), ts, sizeof(ts));
	pthread_mutex_lock(&log_mu);
	if (!g_log)
		g_log = stderr;
	fprintf(g_log, "%s ", ts);
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
	pthread_mutex_unlock(&log_mu);
}

void msleep_run(int ms)
{
	while (ms > 0 && g_running) {
		int step = ms > 100 ? 100 : ms;

		usleep(step * 1000);
		ms -= step;
	}
}

int read_file(const char *path, char **out, size_t *len, size_t max)
{
	/* No se confia en st_size: los archivos de /proc reportan tamano 0. */
	size_t cap = max < 4096 ? max : 4096, got = 0;
	char *buf = malloc(cap + 1);
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		free(buf);
		return -1;
	}
	if (!buf) {
		close(fd);
		return -1;
	}
	for (;;) {
		ssize_t r;

		if (got == cap) {
			char *bigger;

			if (cap >= max) {
				char extra;

				/* justo en el limite: solo es error si todavia hay mas datos */
				r = read(fd, &extra, 1);
				if (r == 0)
					break;
				free(buf);
				close(fd);
				errno = EFBIG;
				return -1;
			}
			cap = cap * 2 > max ? max : cap * 2;
			bigger = realloc(buf, cap + 1);
			if (!bigger) {
				free(buf);
				close(fd);
				return -1;
			}
			buf = bigger;
		}
		r = read(fd, buf + got, cap - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	buf[got] = '\0';
	*out = buf;
	if (len)
		*len = got;
	return 0;
}
