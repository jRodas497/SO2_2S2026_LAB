#define _GNU_SOURCE
#include "auth.h"

#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define MAX_SESSIONS   64
#define SESSION_TTL    (8 * 3600)

static char g_service[64] = "so2-monitor";

typedef struct {
	char token[65];
	char user[48];
	role_t role;
	time_t expira;
	int used;
} session_t;

static session_t g_sess[MAX_SESSIONS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void auth_init(const char *pam_service)
{
	if (pam_service && *pam_service)
		snprintf(g_service, sizeof(g_service), "%s", pam_service);
}

const char *role_name(role_t r)
{
	return r == ROLE_ADMIN ? "admin" : r == ROLE_COMMON ? "common" : "ninguno";
}

/* ------------------------------------------------------------------ PAM */

struct pam_data {
	const char *pass;
};

static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	const struct pam_data *d = data;
	struct pam_response *r;

	if (n <= 0 || n > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;
	r = calloc((size_t)n, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;
	for (int i = 0; i < n; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			r[i].resp = strdup(d->pass);
			if (!r[i].resp) {
				for (int k = 0; k < i; k++)
					free(r[k].resp);
				free(r);
				return PAM_BUF_ERR;
			}
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

/* Busca el rol del usuario por los grupos del sistema (incluido el grupo primario). */
static role_t role_of(const char *user)
{
	struct passwd *pw = getpwnam(user);
	gid_t *groups;
	int ng = 32;
	role_t role = ROLE_NONE;

	if (!pw)
		return ROLE_NONE;
	groups = malloc(sizeof(gid_t) * (size_t)ng);
	if (!groups)
		return ROLE_NONE;
	while (getgrouplist(user, pw->pw_gid, groups, &ng) < 0) {
		gid_t *bigger = realloc(groups, sizeof(gid_t) * (size_t)ng);

		if (!bigger) {
			free(groups);
			return ROLE_NONE;
		}
		groups = bigger;
	}
	for (int i = 0; i < ng; i++) {
		struct group *g = getgrgid(groups[i]);

		if (!g)
			continue;
		if (strcmp(g->gr_name, GRUPO_ADMIN) == 0)
			role = ROLE_ADMIN;              /* ambos grupos => administrador */
		else if (strcmp(g->gr_name, GRUPO_COMUN) == 0 && role == ROLE_NONE)
			role = ROLE_COMMON;
	}
	free(groups);
	return role;
}

int auth_check(const char *user, const char *pass, role_t *role, char *err, size_t errn)
{
	pam_handle_t *h = NULL;
	struct pam_data data = { pass };
	struct pam_conv pc = { conv, &data };
	int rc;
	role_t r;

	if (!user || !pass || !*user || strlen(user) > 32 || strlen(pass) > 256 ||
	    strpbrk(user, "\r\n/ ")) {
		snprintf(err, errn, "Credenciales invalidas");
		return -1;
	}

	rc = pam_start(g_service, user, &pc, &h);
	if (rc != PAM_SUCCESS) {
		log_msg("PAM: pam_start fallo: %s", pam_strerror(h, rc));
		snprintf(err, errn, "Error interno de autenticacion");
		return -1;
	}
	rc = pam_authenticate(h, 0);
	if (rc == PAM_SUCCESS)
		rc = pam_acct_mgmt(h, 0);
	pam_end(h, rc);
	if (rc != PAM_SUCCESS) {
		snprintf(err, errn, "Usuario o contraseña incorrectos");
		return -1;
	}

	r = role_of(user);
	if (r == ROLE_NONE) {
		snprintf(err, errn, "El usuario no pertenece a %s ni a %s", GRUPO_ADMIN, GRUPO_COMUN);
		return -1;
	}
	*role = r;
	return 0;
}

/* -------------------------------------------------------------- sesiones */

static int random_token(char out[65])
{
	static const char hexd[] = "0123456789abcdef";
	unsigned char b[32];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, b, sizeof(b));
	close(fd);
	if (r != (ssize_t)sizeof(b))
		return -1;
	for (int i = 0; i < 32; i++) {
		out[i * 2] = hexd[b[i] >> 4];
		out[i * 2 + 1] = hexd[b[i] & 15];
	}
	out[64] = '\0';
	return 0;
}

int session_create(const char *user, role_t role, char token[65])
{
	time_t now = time(NULL);
	session_t *slot = NULL;

	if (random_token(token) < 0)
		return -1;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (!g_sess[i].used || g_sess[i].expira < now) {
			slot = &g_sess[i];
			break;
		}
		if (!slot || g_sess[i].expira < slot->expira)
			slot = &g_sess[i];          /* si esta llena, se reemplaza la mas antigua */
	}
	memset(slot, 0, sizeof(*slot));
	slot->used = 1;
	snprintf(slot->token, sizeof(slot->token), "%s", token);
	snprintf(slot->user, sizeof(slot->user), "%s", user);
	slot->role = role;
	slot->expira = now + SESSION_TTL;
	pthread_mutex_unlock(&mu);
	return 0;
}

int session_lookup(const char *token, char *user_out, size_t un, role_t *role)
{
	time_t now = time(NULL);
	int found = -1;

	if (!token || strlen(token) != 64)
		return -1;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (g_sess[i].used && g_sess[i].expira >= now &&
		    strcmp(g_sess[i].token, token) == 0) {
			if (user_out)
				snprintf(user_out, un, "%s", g_sess[i].user);
			if (role)
				*role = g_sess[i].role;
			found = 0;
			break;
		}
	}
	pthread_mutex_unlock(&mu);
	return found;
}

void session_destroy(const char *token)
{
	if (!token)
		return;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < MAX_SESSIONS; i++)
		if (g_sess[i].used && strcmp(g_sess[i].token, token) == 0)
			memset(&g_sess[i], 0, sizeof(g_sess[i]));
	pthread_mutex_unlock(&mu);
}
