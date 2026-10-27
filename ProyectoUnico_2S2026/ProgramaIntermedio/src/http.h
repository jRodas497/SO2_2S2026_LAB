/* Servidor HTTP minimo (hilo por conexion): API JSON, SSE en tiempo real y dashboard estatico. */
#ifndef HTTP_H
#define HTTP_H

/* Abre el puerto y lanza el hilo que acepta conexiones. 0 = ok. */
int http_start(int port, const char *dashboard_dir);
void http_stop(void);

/* Envia un evento SSE a todos los dashboards conectados. json: una sola linea. */
void sse_broadcast(const char *event, const char *json);

/* Informa a los dashboards del estado de los interruptores (monitoreo, procesos, archivos). */
void http_publish_flags(void);

#endif
