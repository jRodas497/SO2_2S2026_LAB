# Práctica 3 - Sistemas Operativos 2

## Sincronización e Instrumentación: Comunicación entre Procesos y Monitoreo de Eventos

**Carné:** 202200389  
**Kernel utilizado:** Linux 6.12.69  
**Kernel personalizado:** `6.12.69-202200389-so2-p3`

## Descripción

La práctica implementa dos mecanismos dentro del kernel de Linux:

1. Un canal de comunicación entre procesos basado en una cola FIFO bloqueante.
2. Un sistema de monitoreo de archivos de log ejecutado mediante un hilo del kernel.

Se aplican sincronización, comunicación entre espacio de usuario y espacio de kernel, wait queues, mutex, hilos de kernel y acceso controlado a archivos.

## Llamadas al sistema implementadas

| Número | Syscall | Función |
|---:|---|---|
| 548 | `ipc_channel_send` | Envía un mensaje a una cola FIFO protegida. |
| 549 | `ipc_channel_receive` | Espera sin polling hasta que exista un mensaje y lo entrega al receptor. |
| 550 | `start_log_watch` | Inicia un hilo de kernel que monitorea archivos de log. |
| 551 | `stop_log_watch` | Detiene de forma segura un monitoreo activo. |

## Estructura de entrega

```text
Practica_3_2S2026/
├── README.md
├── GUIA_DEMOSTRACION.md
├── docs/
│   ├── Informe_Tecnico.md
│   └── ERRORES.md
├── img/
├── linux 6.12.69/
│   ├── arch/x86/entry/syscalls/syscall_64.tbl
│   ├── include/linux/syscalls.h
│   └── kernel/sys.c
└── Programa_Intermedio/
    ├── ipc_send.c
    ├── ipc_receive.c
    ├── start_log_watch.c
    └── stop_log_watch.c
```

## Archivos modificados del kernel

### `kernel/sys.c`

Contiene la implementación de las cuatro syscalls, la cola FIFO del canal IPC, la wait queue, los mutex y la estructura utilizada para administrar monitores de logs.

### `include/linux/syscalls.h`

Contiene los prototipos de las cuatro llamadas al sistema.

### `arch/x86/entry/syscalls/syscall_64.tbl`

Registra las syscalls 548, 549, 550 y 551 para x86-64.

## Programas de usuario

- `ipc_send.c`: invoca la syscall 548 para enviar mensajes.
- `ipc_receive.c`: invoca la syscall 549 y queda bloqueado si la cola está vacía.
- `start_log_watch.c`: invoca la syscall 550 e inicia un monitor.
- `stop_log_watch.c`: invoca la syscall 551 y detiene un monitor.

## Compilación de programas de usuario

```bash
cd Programa_Intermedio

gcc -Wall -Wextra -O2 ipc_send.c -o ipc_send
gcc -Wall -Wextra -O2 ipc_receive.c -o ipc_receive
gcc -Wall -Wextra -O2 start_log_watch.c -o start_log_watch
gcc -Wall -Wextra -O2 stop_log_watch.c -o stop_log_watch
```

## Verificación del kernel

```bash
uname -r
```

Resultado esperado:

```text
6.12.69-202200389-so2-p3
```

## Prueba rápida del canal IPC

Terminal 1:

```bash
./ipc_receive
```

Terminal 2:

```bash
./ipc_send "Hola desde la Practica 3"
```

El receptor debe despertar y mostrar el mensaje recibido.

## Prueba rápida de log_watch

```bash
BASE="$HOME/SO2/Practica_3_2S2026/pruebas_logs"
mkdir -p "$BASE"
printf 'INFO app1 iniciada
' > "$BASE/app1.log"
printf 'INFO app2 iniciada
' > "$BASE/app2.log"
: > "$BASE/central.log"

./start_log_watch ERROR "$BASE/central.log" "$BASE/app1.log" "$BASE/app2.log"

echo "ERROR fallo encontrado en aplicacion 1" >> "$BASE/app1.log"
echo "ERROR acceso rechazado en aplicacion 2" >> "$BASE/app2.log"
sleep 3
cat "$BASE/central.log"

./stop_log_watch
```

## Documentación

- Informe técnico: `docs/Informe_Tecnico.md`
- Errores encontrados: `docs/ERRORES.md`
- Guía para demostración: `GUIA_DEMOSTRACION.md`
