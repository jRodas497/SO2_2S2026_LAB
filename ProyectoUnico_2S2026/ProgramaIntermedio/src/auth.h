/* Autenticacion con PAM, roles por grupo del sistema y sesiones del dashboard. */
#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

#define GRUPO_ADMIN  "admin_user"
#define GRUPO_COMUN  "common_user"

typedef enum { ROLE_NONE = 0, ROLE_COMMON = 1, ROLE_ADMIN = 2 } role_t;

void auth_init(const char *pam_service);

/*
 * Valida usuario y contrasena con PAM y determina el rol por grupo.
 * Si pertenece a ambos grupos es administrador. Si no pertenece a ninguno,
 * rechaza el acceso aunque la contrasena sea correcta.
 * 0 = acceso concedido (rellena *role); -1 = denegado (motivo en err).
 */
int auth_check(const char *user, const char *pass, role_t *role, char *err, size_t errn);

/* Sesiones (cookie "so2sid"). token debe tener 65 bytes. */
int session_create(const char *user, role_t role, char token[65]);
int session_lookup(const char *token, char *user_out, size_t un, role_t *role);
void session_destroy(const char *token);

const char *role_name(role_t r);

#endif
