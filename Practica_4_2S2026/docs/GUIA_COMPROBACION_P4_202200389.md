# Guía sencilla de comprobación — Práctica 4

## Sistemas Operativos 2 — Carné 202200389

Esta guía sirve para comprobar, de forma rápida y ordenada, que el kernel personalizado de la Práctica 4 está arrancando y que las seis syscalls integradas funcionan.

Kernel esperado:

```text
6.12.69-202200389-so2-p4
```

Numeración utilizada:

```text
548  get_process_info
549  get_processes_state
550  ipc_channel_send
551  ipc_channel_receive
552  start_log_watch
553  stop_log_watch
```

---

# 1. Confirmar que se está usando el kernel de Práctica 4

Ejecutar:

```bash
uname -r
```

Debe aparecer:

```text
6.12.69-202200389-so2-p4
```

También se puede comprobar con:

```bash
uname -a
```

Si no aparece `6.12.69-202200389-so2-p4`, no continuar con las pruebas porque las syscalls personalizadas no estarán disponibles.

---

# 2. Entrar al Programa Intermedio

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
```

Comprobar los archivos:

```bash
ls -lh
```

Deben existir al menos:

```text
prueba.c
ipc_send.c
ipc_receive.c
start_log_watch.c
stop_log_watch.c
```

---

# 3. Actualizar los números de syscall de los programas de Práctica 3

Los programas copiados desde Práctica 3 todavía utilizan la numeración anterior.

Ejecutar:

```bash
sed -i 's/#define SYS_IPC_CHANNEL_SEND 548/#define SYS_IPC_CHANNEL_SEND 550/' ipc_send.c

sed -i 's/#define SYS_IPC_CHANNEL_RECEIVE 549/#define SYS_IPC_CHANNEL_RECEIVE 551/' ipc_receive.c

sed -i 's/#define SYS_START_LOG_WATCH 550/#define SYS_START_LOG_WATCH 552/' start_log_watch.c

sed -i 's/#define SYS_STOP_LOG_WATCH 551/#define SYS_STOP_LOG_WATCH 553/' stop_log_watch.c
```

`prueba.c` no se modifica porque Práctica 2 conserva:

```text
548 get_process_info
549 get_processes_state
```

Comprobar la numeración:

```bash
grep -nE 'SYS_' *.c
```

Se debe observar:

```text
ipc_send.c        -> 550
ipc_receive.c     -> 551
start_log_watch.c -> 552
stop_log_watch.c  -> 553
prueba.c          -> 548 y 549
```

---

# 4. Compilar los programas de prueba

Ejecutar:

```bash
gcc -Wall -Wextra prueba.c -o prueba
gcc -Wall -Wextra ipc_send.c -o ipc_send
gcc -Wall -Wextra ipc_receive.c -o ipc_receive
gcc -Wall -Wextra start_log_watch.c -o start_log_watch
gcc -Wall -Wextra stop_log_watch.c -o stop_log_watch
```

Comprobar:

```bash
ls -lh prueba ipc_send ipc_receive start_log_watch stop_log_watch
```

Si no aparecen errores de GCC, continuar.

---

# 5. Probar las syscalls de procesos

Ejecutar:

```bash
./prueba
```

El programa utiliza:

```text
548 -> get_process_info
549 -> get_processes_state
```

Debe mostrar la información de procesos solicitada por el programa.

Después revisar los mensajes del kernel:

```bash
sudo dmesg | tail -n 30
```

Se esperan mensajes relacionados con:

```text
SO2 P2: get_process_info
SO2 P2: get_processes_state
```

Con esto se comprueba que las syscalls de Práctica 2 siguen funcionando dentro del kernel P4.

---

# 6. Probar IPC bloqueante

Esta prueba necesita dos terminales.

## Terminal 1

Entrar a la carpeta:

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
```

Ejecutar:

```bash
./ipc_receive
```

El programa debe quedarse esperando.

Esto es correcto.

No debe imprimir mensajes repetidamente ni consumir CPU mediante polling.

## Terminal 2

Entrar a la misma carpeta:

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
```

Enviar un mensaje:

```bash
./ipc_send "Hola desde Practica 4"
```

Al enviarlo, la Terminal 1 debe despertar y mostrar el mensaje.

Resultado esperado conceptualmente:

```text
Terminal 1:
Esperando mensaje...
Hola desde Practica 4
```

Esto demuestra que:

```text
ipc_channel_receive
        |
        | espera bloqueado
        |
ipc_channel_send
        |
wake_up_interruptible
        |
receptor despierta
```

---

# 7. Preparar archivos para probar log_watch

Crear un entorno temporal:

```bash
BASE=/tmp/so2_p4_test

rm -rf "$BASE"
mkdir -p "$BASE"

touch "$BASE/app1.log"
touch "$BASE/app2.log"
touch "$BASE/central.log"
```

Comprobar:

```bash
ls -lh "$BASE"
```

---

# 8. Prueba principal de Práctica 4: log_watch -> IPC

Esta es la prueba más importante de la integración.

Se recomienda utilizar tres terminales.

---

## Terminal 1 — receptor IPC

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
./ipc_receive
```

Debe quedarse bloqueado esperando un evento.

No cerrar esta terminal.

---

## Terminal 2 — iniciar log_watch

Definir nuevamente la variable:

```bash
BASE=/tmp/so2_p4_test
```

Entrar a los programas:

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
```

Iniciar el monitoreo:

```bash
./start_log_watch ERROR "$BASE/central.log" "$BASE/app1.log" "$BASE/app2.log"
```

El programa debe mostrar el identificador del monitor iniciado.

Guardar mentalmente o anotar ese ID.

---

## Terminal 3 — escribir una línea normal

Definir:

```bash
BASE=/tmp/so2_p4_test
```

Escribir una línea que NO contiene la palabra `ERROR`:

```bash
echo "INFO sistema funcionando correctamente" >> "$BASE/app1.log"
```

Esperar aproximadamente 2 segundos.

La Terminal 1 debe continuar bloqueada porque todavía no existe una coincidencia.

---

# 9. Generar un evento real

En la Terminal 3:

```bash
echo "ERROR acceso no autorizado detectado" >> "$BASE/app1.log"
```

Esperar aproximadamente 1 a 2 segundos.

Ahora la Terminal 1 debe despertar y recibir un mensaje similar a:

```text
SECURITY|1|/tmp/so2_p4_test/app1.log|ERROR|ERROR acceso no autorizado detectado
```

El número después de `SECURITY` puede cambiar porque corresponde al identificador del monitor.

Esta prueba demuestra el flujo requerido:

```text
app1.log
    |
se escribe ERROR
    |
log_watch detecta coincidencia
    |
so2_log_write_match
    |
so2_ipc_enqueue_kernel
    |
cola IPC
    |
wake_up_interruptible
    |
ipc_receive despierta
```

---

# 10. Comprobar el log central

Ejecutar:

```bash
cat /tmp/so2_p4_test/central.log
```

Debe existir una entrada correspondiente a la línea detectada.

Por ejemplo:

```text
archivo=/tmp/so2_p4_test/app1.log
palabra=ERROR
linea=ERROR acceso no autorizado detectado
```

El formato exacto puede incluir también el identificador del monitor.

---

# 11. Revisar mensajes del kernel

Ejecutar:

```bash
sudo dmesg | grep -E 'SO2 P2|SO2 P3|SO2 P4' | tail -n 50
```

Durante la prueba de eventos deben aparecer mensajes relacionados con:

```text
ipc_enqueue
evento de seguridad notificado via IPC
match
ipc_receive
```

Esto permite comprobar que la comunicación realmente pasó por el kernel.

---

# 12. Detener log_watch

En una terminal:

```bash
cd ~/SO2/Practica_4_2S2026/Programa_Intermedio
```

Ejecutar:

```bash
./stop_log_watch
```

Cuando el programa solicite el identificador, ingresar el ID mostrado anteriormente por `start_log_watch`.

Debe indicar que el monitor fue detenido correctamente.

Si el programa muestra directamente su sintaxis de uso, seguir esa sintaxis utilizando el mismo ID obtenido en el paso anterior.

---

# 13. Prueba adicional con el segundo archivo

Para demostrar que se pueden monitorear varios archivos, iniciar nuevamente el receptor:

```bash
./ipc_receive
```

Luego agregar un evento al segundo archivo:

```bash
echo "ERROR evento generado desde app2" >> /tmp/so2_p4_test/app2.log
```

El receptor debe despertar nuevamente y el mensaje debe indicar:

```text
/tmp/so2_p4_test/app2.log
```

---

# 14. Verificar que las seis syscalls estén registradas

Entrar al árbol del kernel:

```bash
cd ~/SO2/Practica_4_2S2026/workspace/linux-6.12.69
```

Ejecutar:

```bash
grep -nE \
'^(548|549|550|551|552|553)[[:space:]]' \
arch/x86/entry/syscalls/syscall_64.tbl
```

Debe aparecer:

```text
548 common get_process_info      sys_get_process_info
549 common get_processes_state   sys_get_processes_state
550 common ipc_channel_send      sys_ipc_channel_send
551 common ipc_channel_receive   sys_ipc_channel_receive
552 common start_log_watch       sys_start_log_watch
553 common stop_log_watch        sys_stop_log_watch
```

---

# 15. Verificación rápida del kernel compilado

Ejecutar:

```bash
make kernelrelease
```

Debe mostrar:

```text
6.12.69-202200389-so2-p4
```

Después:

```bash
ls -lh arch/x86/boot/bzImage vmlinux System.map
```

También puede comprobarse la compilación mediante:

```bash
grep "Kernel: arch/x86/boot/bzImage is ready" \
~/SO2/Practica_4_2S2026/logs/compilacion-kernel-p4.log
```

Resultado esperado:

```text
Kernel: arch/x86/boot/bzImage is ready (#1)
```

---

# 16. Evidencias recomendadas

Las evidencias existentes son:

```text
01_integracion_logwatch_ipc.png
02_kernel_compilado.png
03_kernel_instalado.png
04_grub_p4.png
05_uname_p4.png
```

Las siguientes capturas recomendadas son:

```text
06_prueba_procesos.png
07_ipc_bloqueado.png
08_ipc_envio_recepcion.png
09_logwatch_evento_ipc.png
10_log_central_dmesg.png
```

La evidencia más importante de esta etapa es `09_logwatch_evento_ipc.png`, porque debe mostrar que el receptor estaba bloqueado y fue despertado después de escribir una línea que contiene `ERROR`.

---

# 17. Qué demuestra cada prueba

| Prueba | Funcionalidad demostrada |
|---|---|
| `uname -r` | El sistema está ejecutando el kernel P4 |
| `./prueba` | Funcionamiento de `get_process_info` y `get_processes_state` |
| `./ipc_receive` + `./ipc_send` | Canal IPC bloqueante |
| `start_log_watch` | Creación del hilo de monitoreo |
| Línea sin `ERROR` | No se genera una falsa notificación |
| Línea con `ERROR` | `log_watch` detecta el evento |
| Receptor despierta | Integración `log_watch -> IPC` |
| `central.log` | Conservación del registro central de P3 |
| `stop_log_watch` | Detención segura del monitor |
| `dmesg` | Evidencia de ejecución dentro del kernel |

---

# 18. Resultado esperado de la parte kernel

Si todas las pruebas anteriores funcionan, se puede considerar validada la parte de kernel de la Práctica 4:

```text
[OK] Kernel 6.12.69-202200389-so2-p4 activo
[OK] get_process_info
[OK] get_processes_state
[OK] ipc_channel_send
[OK] ipc_channel_receive bloqueante
[OK] start_log_watch
[OK] stop_log_watch
[OK] log_watch genera evento IPC
[OK] receptor despierta sin polling ciego
[OK] central.log registra coincidencias
```

Después de estas pruebas todavía quedan las capas de daemon y dashboard para completar la práctica completa.
