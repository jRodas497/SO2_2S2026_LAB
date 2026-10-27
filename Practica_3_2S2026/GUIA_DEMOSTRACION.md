# Guía de Ejecución y Demostración - Práctica 3

## Objetivo

Esta guía permite comprobar rápidamente que la Práctica 3 está correctamente implementada y preparada para una demostración.

Kernel requerido:

```text
6.12.69-202200389-so2-p3
```

> Los programas utilizan directamente las syscalls 548 a 551. Deben ejecutarse con el kernel personalizado activo.

## 1. Opción recomendada para otro equipo

Para una demostración en otra computadora, lo más seguro es transportar la máquina virtual completa donde el kernel ya está compilado e instalado.

La VM conserva `/boot`, los módulos, GRUB, el kernel y los programas de prueba. Si solamente se copia el repositorio a otra VM, será necesario compilar e instalar Linux 6.12.69 nuevamente.

## 2. Comprobación inicial

```bash
uname -r
```

Debe mostrar:

```text
6.12.69-202200389-so2-p3
```

Si aparece otro kernel, reiniciar y seleccionar en GRUB:

```text
Advanced options for Ubuntu
Ubuntu, with Linux 6.12.69-202200389-so2-p3
```

## 3. Comprobar syscalls

```bash
cd ~/SO2/Practica_3_2S2026/workspace/linux-6.12.69

grep -nE '^(548|549|550|551)[[:space:]]' arch/x86/entry/syscalls/syscall_64.tbl
```

Resultado esperado:

```text
548  common  ipc_channel_send       sys_ipc_channel_send
549  common  ipc_channel_receive    sys_ipc_channel_receive
550  common  start_log_watch        sys_start_log_watch
551  common  stop_log_watch         sys_stop_log_watch
```

Verificar implementaciones:

```bash
grep -nE 'SYSCALL_DEFINE[0-9]\((ipc_channel|start_log_watch|stop_log_watch)' kernel/sys.c
```

## 4. Compilar programas de usuario

```bash
cd ~/SO2/Practica_3_2S2026/Programa_Intermedio

gcc -Wall -Wextra -O2 ipc_send.c -o ipc_send
gcc -Wall -Wextra -O2 ipc_receive.c -o ipc_receive
gcc -Wall -Wextra -O2 start_log_watch.c -o start_log_watch
gcc -Wall -Wextra -O2 stop_log_watch.c -o stop_log_watch
```

## 5. Demostración 1 - IPC bloqueante

Terminal 1:

```bash
./ipc_receive
```

Debe quedar bloqueado mostrando que espera un mensaje.

Terminal 2:

```bash
./ipc_send "Hola desde la Practica 3"
```

El receptor debe despertar automáticamente y mostrar el mensaje recibido.

Comprobar kernel:

```bash
sudo dmesg | grep "SO2 P3:" | tail -n 10
```

## 6. Demostración 2 - Monitoreo de dos logs

```bash
BASE="$HOME/SO2/Practica_3_2S2026/pruebas_logs"
mkdir -p "$BASE"
printf 'INFO app1 iniciada
' > "$BASE/app1.log"
printf 'INFO app2 iniciada
' > "$BASE/app2.log"
: > "$BASE/central.log"

cd ~/SO2/Practica_3_2S2026/Programa_Intermedio
rm -f monitor.id
```

Iniciar:

```bash
./start_log_watch ERROR "$BASE/central.log" "$BASE/app1.log" "$BASE/app2.log"
```

Generar eventos:

```bash
echo "ERROR fallo encontrado en aplicacion 1" >> "$BASE/app1.log"
echo "ERROR acceso rechazado en aplicacion 2" >> "$BASE/app2.log"
sleep 3
```

Comprobar:

```bash
cat "$BASE/central.log"
```

Deben aparecer dos entradas, una de cada archivo.

Detener:

```bash
./stop_log_watch
```

## 7. Demostración 3 - Corrección de truncamiento

```bash
BASE="$HOME/SO2/Practica_3_2S2026/pruebas_logs"
printf 'INFO inicio de app1
' > "$BASE/app1.log"
: > "$BASE/central.log"
cd ~/SO2/Practica_3_2S2026/Programa_Intermedio
rm -f monitor.id

./start_log_watch ERROR "$BASE/central.log" "$BASE/app1.log"

echo "ERROR antes de truncar" >> "$BASE/app1.log"
sleep 2
: > "$BASE/app1.log"
sleep 2
echo "ERROR despues de truncar" >> "$BASE/app1.log"
sleep 3
cat "$BASE/central.log"
```

Deben aparecer ambos eventos.

Comprobar:

```bash
sudo dmesg | grep "archivo truncado" | tail
```

Detener:

```bash
./stop_log_watch
```

## 8. Demostración 4 - Rechazo de auto monitoreo

```bash
BASE="$HOME/SO2/Practica_3_2S2026/pruebas_logs"
: > "$BASE/loop.log"
cd ~/SO2/Practica_3_2S2026/Programa_Intermedio
rm -f monitor.id

./start_log_watch ERROR "$BASE/loop.log" "$BASE/loop.log"
```

Debe responder:

```text
Error start_log_watch: Invalid argument
```

Comprobar:

```bash
ls -l monitor.id
sudo dmesg | grep "log central no puede" | tail
```

`monitor.id` no debe existir y `dmesg` debe indicar que el log central no puede ser archivo vigilado.

## 9. Secuencia corta para presentación

```bash
uname -r
```

Luego IPC en dos terminales:

```bash
./ipc_receive
```

```bash
./ipc_send "Prueba de IPC"
```

Después:

```bash
BASE="$HOME/SO2/Practica_3_2S2026/pruebas_logs"
mkdir -p "$BASE"
: > "$BASE/app1.log"
: > "$BASE/central.log"

./start_log_watch ERROR "$BASE/central.log" "$BASE/app1.log"
echo "ERROR prueba final" >> "$BASE/app1.log"
sleep 2
cat "$BASE/central.log"
./stop_log_watch

sudo dmesg | grep "SO2 P3:" | tail -n 20
```

Con esto se demuestra kernel personalizado, IPC bloqueante, envío y recepción, kthread, detección de eventos, escritura en log central, detención segura y trazas en `dmesg`.

## 10. Si se usa otra VM desde cero

Se requiere Linux 6.12.69 limpio y colocar los archivos modificados en las mismas rutas del árbol del kernel:

```text
linux 6.12.69/kernel/sys.c
linux 6.12.69/include/linux/syscalls.h
linux 6.12.69/arch/x86/entry/syscalls/syscall_64.tbl
```

Luego se debe configurar, compilar, instalar y arrancar el kernel personalizado. Para una presentación se recomienda una copia de la VM ya preparada.

## 11. Checklist final

- [ ] `uname -r` muestra `6.12.69-202200389-so2-p3`.
- [ ] Los cuatro programas compilan.
- [ ] `ipc_receive` queda bloqueado cuando no hay mensajes.
- [ ] `ipc_send` despierta al receptor.
- [ ] `start_log_watch` devuelve un ID.
- [ ] Se detectan eventos de varios archivos.
- [ ] El log central contiene archivo, palabra y línea.
- [ ] `stop_log_watch` termina el monitor.
- [ ] El truncamiento no impide futuras detecciones.
- [ ] El log central no puede vigilarse a sí mismo.
- [ ] `dmesg` contiene mensajes `SO2 P3`.
