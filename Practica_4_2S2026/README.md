# Práctica 4 — Sistema de monitoreo con dashboard

Carné: 202200389 · Kernel base: Linux 6.12.69 (`6.12.69-202200389-so2-p4`)

## Compilar y ejecutar el sistema completo

```bash
# 1) kernel con las syscalls 548-553: ver "linux 6.12.69/" y scripts/ (ya instalado en la VM)
# 2) daemon (en la VM)
cd daemon && make && ./so2_p4 -f
# 3) dashboard: abrir http://IP_DE_LA_VM:8080/ en el navegador
# 4) provocar un evento de seguridad:
echo "ERROR acceso rechazado" >> /var/log/so2_app.log
```

Estructura: `daemon/` (programa intermedio en C), `dashboard/` (HTML/CSS/JS),
`Programa_Intermedio/` (programas de prueba de las syscalls), `linux 6.12.69/` (archivos del
kernel modificados), `docs/` (informe técnico y `GUIA_DAEMON_DASHBOARD_P4_202200389.md`, con la
arquitectura, el diseño del canal IPC, las instrucciones detalladas y los errores encontrados).

---

# Práctica 4 - Bootstrap

Carné: 202200389
Kernel base: Linux 6.12.69

Este paquete prepara la VM antes de integrar y compilar la Práctica 4.

Orden:
1. scripts/00_diagnostico.sh
2. scripts/01_respaldo_config.sh
3. Reiniciar con kernel genérico si uname -r muestra un kernel personalizado.
4. scripts/02_limpieza_segura.sh
5. scripts/03_preparar_p4.sh
6. scripts/04_preparar_config.sh

Plan de syscalls para P4:
548 get_process_info
549 get_processes_state
550 ipc_channel_send
551 ipc_channel_receive
552 start_log_watch
553 stop_log_watch

No se compila nada durante estos pasos.
