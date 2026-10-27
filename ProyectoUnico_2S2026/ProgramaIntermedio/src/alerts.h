/* Sistema de alertas: anillo de las ultimas alertas + aviso en vivo por SSE. */
#ifndef ALERTS_H
#define ALERTS_H

#include <time.h>
#include "cJSON.h"

#define SEV_LOW    "LOW"
#define SEV_MEDIUM "MEDIUM"
#define SEV_HIGH   "HIGH"

/* tipos de evento: archivos, sistema, sesion, logs */
#define TIPO_ARCHIVOS "archivos"
#define TIPO_SISTEMA  "sistema"
#define TIPO_SESION   "sesion"
#define TIPO_LOGS     "logs"

void alerts_init(void);

/* Registra una alerta y la envia a los dashboards conectados. No retener locks al llamar. */
void alert_add(const char *severidad, const char *tipo, const char *titulo,
	       const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Ultimas "max" alertas, las mas recientes primero. */
cJSON *alerts_json(int max);

#endif
