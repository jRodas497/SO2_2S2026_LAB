/* Servidor HTTP minimo: API JSON de solo lectura, SSE en tiempo real y dashboard estatico. */
#ifndef HTTP_H
#define HTTP_H

int http_start(const char *bind_addr, int port, const char *dashboard_dir);
void http_stop(void);

/* Envia un evento SSE a todos los dashboards conectados. json: una sola linea. */
void sse_broadcast(const char *event, const char *json);

#endif
