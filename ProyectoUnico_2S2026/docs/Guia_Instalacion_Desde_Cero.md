# Guía de instalación desde cero

Proyecto Único · Sistemas Operativos 2 · Carné 202200389

Esta guía levanta **todo el sistema en una máquina nueva**: compila el kernel Linux 6.12.69 con las syscalls del proyecto y
soporte eBPF/BTF, lo instala, crea los usuarios y pone en marcha el daemon y el dashboard.

> **Cómo leerla.** Cada paso indica *qué ejecutar*, *qué debe salir* y *qué hacer si no sale*. Los pasos del kernel están
> automatizados con los scripts de `scripts/`; ejecútalos **en orden** y no pases al siguiente hasta ver el resultado esperado.

---

## Índice

0. [Resumen y tiempos](#0-resumen-y-tiempos)
1. [Requisitos de la máquina](#1-requisitos-de-la-máquina)
2. [Obtener el código](#2-obtener-el-código)
3. [Parte A — El kernel](#3-parte-a--el-kernel)
4. [Parte B — El sistema de monitoreo](#4-parte-b--el-sistema-de-monitoreo)
5. [Parte C — Verificación completa](#5-parte-c--verificación-completa)
6. [Problemas frecuentes](#6-problemas-frecuentes)
7. [Resumen de comandos](#7-resumen-de-comandos)
8. [Volver atrás](#8-volver-atrás)

---

## 0. Resumen y tiempos

```text
máquina nueva ─► dependencias ─► código del kernel ─► + archivos del proyecto ─► configurar (BTF+eBPF)
              ─► compilar ─► instalar ─► reiniciar ─► verificar ─► daemon + usuarios + dashboard ─► pruebas
```

| Etapa | Tiempo aproximado |
|---|---|
| Dependencias y descargas | 5–10 min |
| Compilar el kernel (4 vCPU, 8 GiB) | 15–30 min |
| Resto (instalar, reiniciar, daemon, usuarios) | 10–15 min |
| **Total** | **≈ 1 hora** |

## 1. Requisitos de la máquina

| Requisito | Detalle |
|---|---|
| Sistema | **Ubuntu 24.04 LTS** (o una distribución basada en Debian). La guía se probó en Ubuntu 24.04 |
| CPU / RAM | 4 núcleos y **8 GiB de RAM** recomendados. Con menos RAM la compilación puede cerrarse sola (ver §6) |
| Disco | 40 GB, con **al menos 25 GB libres** antes de empezar (el árbol compilado ocupa ~6–7 GB) |
| Red | Acceso a internet (apt, kernel.org y, para clonar, GitLab) |
| Usuario | Un usuario normal con `sudo` (no trabajes como `root`) |
| Arranque | **Secure Boot desactivado**: un kernel compilado por ti no está firmado y no arrancaría con él activado |

**Compilar y ejecutar en la misma máquina.** El script de configuración reduce el kernel a los módulos que *esa máquina* usa
(`localmodconfig`), lo que hace posible compilar con BTF en poco disco. Si piensas compilar aquí y **llevar el kernel a otro
equipo**, ejecuta el paso de configuración con `USE_LOCALMOD=0` (más lento y más grande, ver §3.5).

Si la máquina es una VM en **virt-manager (QEMU/KVM)**: usa video **QXL** (evita la pantalla negra), reparte 4 vCPU y 8 GiB, y
un disco de 40 GB.

## 2. Obtener el código

```bash
sudo apt update && sudo apt install -y git
mkdir -p ~/SO2
git clone -b 202200389 git@gitlab.com:ingenieria.usac/sistemas/so2/mapa.git ~/SO2/mapa
```

- La URL SSH necesita que tu clave pública esté registrada en GitLab. Si usas HTTPS:
  `git clone -b 202200389 https://gitlab.com/ingenieria.usac/sistemas/so2/mapa.git ~/SO2/mapa`
- Si no puedes clonar, copia la carpeta `ProyectoUnico_2S2026/` desde otro equipo con `scp -r`.

Trabaja sobre una **copia** fuera del repositorio (el árbol del kernel pesa varios GB y así no ensucia el repo):

```bash
cp -r ~/SO2/mapa/ProyectoUnico_2S2026 ~/SO2/
cd ~/SO2/ProyectoUnico_2S2026/scripts
chmod +x *.sh ../ProgramaIntermedio/scripts/*.sh
```

> En una máquina nueva **no ejecutes** `01_limpieza_p4.sh` ni `99_eliminar_kernel_p4.sh`: son de limpieza de la VM original.

---

## 3. Parte A — El kernel

Todos los comandos de esta parte se ejecutan desde `~/SO2/ProyectoUnico_2S2026/scripts`.

### 3.1 Diagnóstico

```bash
./00_diagnostico.sh
```

Muestra el kernel actual, RAM, disco y qué herramientas faltan. **Es normal** que falten `clang`, `bpftool`, `libbpf` o
`pam.h` en una máquina nueva: el siguiente paso las instala. Comprueba que `free -h` muestre ≈ 8 GiB y `df -h /` al menos
25 GB disponibles.

### 3.2 Dependencias

```bash
./02_dependencias.sh
```

Instala (con `apt`): `build-essential bc bison flex libssl-dev libelf-dev libncurses-dev dwarves cpio rsync git
zlib1g-dev pkg-config curl xz-utils ca-certificates clang llvm libbpf-dev libpam0g-dev`, más `bpftool` (o
`linux-tools-common` y `linux-tools-generic` si no existe el paquete). `dwarves` aporta `pahole`, imprescindible para BTF.

**Debe terminar** con las versiones de `pahole` (≥ 1.25), `clang` (18), `libbpf` (1.3), `openssl` y la línea `pam OK`. El aviso
`WARNING: bpftool not found for kernel …` es **normal**: es un envoltorio que no corre con kernels personalizados; el `bpftool`
real queda en `/usr/lib/linux-tools/*/bpftool` y el proyecto lo usa directamente.

### 3.3 Descargar el kernel y la configuración base

```bash
./02a_obtener_fuentes.sh
```

Descarga `linux-6.12.69.tar.xz` (≈ 148 MB) de kernel.org en `~/SO2/recursos/`, **verifica su SHA-256** contra el publicado
por kernel.org y guarda como configuración base la del kernel genérico de Ubuntu instalado en la máquina
(`~/SO2/recursos/configs/config-base`).

**Debe salir:** `OK` seguido del hash `4b493657f218703239c4f22415f027b3644949bf2761abd18b849f0aad5f7665` y `Listo`.
Si el hash no coincide, borra el `.tar.xz` y repite.

### 3.4 Aplicar los archivos del proyecto al kernel

```bash
./03_preparar_arbol.sh
```

Extrae Linux limpio en `workspace/linux-6.12.69/`, comprueba que los números de syscall **548–553 estén libres** y copia encima los
tres archivos de `Kernel/`. El repositorio contiene solo los archivos modificados, no el kernel completo.

**Debe salir:**

```text
Libres.
kernel/sys.c                                  +784 -0 lineas
include/linux/syscalls.h                      +33 -0 lineas
arch/x86/entry/syscalls/syscall_64.tbl        +6 -0 lineas
548 … get_process_info   549 … get_processes_state   550 … ipc_channel_send
551 … ipc_channel_receive   552 … start_log_watch   553 … stop_log_watch
```

Los originales quedan en `workspace/originales/`. Si dice «Ya existe el árbol del kernel», es porque lo ejecutaste antes:
`rm -rf ~/SO2/ProyectoUnico_2S2026/workspace` y repite.

### 3.5 Configurar (BTF + eBPF)

```bash
./04_configurar_kernel.sh
```

Parte de la configuración base, fija el nombre del kernel (`-202200389-so2-final`), reduce los módulos a los que usa la
máquina y **activa BTF y eBPF**. Al final **verifica** cada opción exigida por el enunciado y aborta si alguna falla.

**Debe terminar** con estas líneas (todas `=y`) y el nombre del kernel:

```text
CONFIG_BPF=y   CONFIG_BPF_SYSCALL=y   CONFIG_BPF_EVENTS=y
CONFIG_DEBUG_INFO_BTF=y   CONFIG_FTRACE_SYSCALLS=y   CONFIG_TRACEPOINTS=y
kernelrelease: 6.12.69-202200389-so2-final
Modulos a compilar: <un número de decenas a pocos cientos>
```

- Si dice `Falta pahole`, repite el paso 3.2.
- Para usar la configuración **completa** de Ubuntu (kernel portable a otros equipos, pero ≥ 20 GB de compilación y mucho
  más lento): `USE_LOCALMOD=0 ./04_configurar_kernel.sh`.

### 3.6 Compilar

Hazlo dentro de `tmux` para que no se corte si se cierra la terminal:

```bash
sudo apt install -y tmux          # si no lo tienes
tmux new -s kernel
cd ~/SO2/ProyectoUnico_2S2026/scripts
./05_compilar.sh                  # opcional: JOBS=3 ./05_compilar.sh
```

(`Ctrl+B` y luego `D` para salir de `tmux` sin detenerlo; `tmux attach -t kernel` para volver.)

Primero compila `kernel/sys.o` (si hay un error en las syscalls aparece aquí, en segundos) y luego el kernel completo. El
registro queda en `~/SO2/ProyectoUnico_2S2026/logs/compilacion-kernel.log`. Por defecto usa 2 trabajos en paralelo; con 8 GiB
puedes subir a `JOBS=3` o `4`.

**Debe terminar** con `Kernel: arch/x86/boot/bzImage is ready`, `sin errores`, los tres archivos (`bzImage` ≈ 15 MB, `vmlinux`
≈ 400 MB, `System.map`) y el disco libre. Si dice menos de 10 GB libres, aborta antes de empezar.

### 3.7 Instalar

```bash
./06_instalar.sh
```

Ejecuta `modules_install` (con `INSTALL_MOD_STRIP=1`, que quita la información de depuración de los módulos para que no ocupen
GB), `make install` y `update-grub`. No reinicia. **Debe mostrar** `/lib/modules/6.12.69-202200389-so2-final`, los archivos de
`/boot` con ese nombre y la entrada del kernel en `grub.cfg`.

### 3.8 Reiniciar con el kernel nuevo

```bash
sudo reboot
```

En el menú de GRUB elige **Advanced options for Ubuntu** → **Ubuntu, with Linux 6.12.69-202200389-so2-final** (no el que termina
en `.old` ni el de `recovery mode`). GRUB arranca por defecto el kernel con la versión más alta, y según los kernels que tengas instalados puede
ser otro (en la VM original era un `7.x-generic`). Comprueba siempre con `uname -r` y, si hace falta, **elígelo a mano**.

- Si el menú no aparece, mantén pulsada **Shift** (BIOS) o **Esc** (UEFI) al encender, o hazlo visible:
  `sudo sed -i 's/^GRUB_TIMEOUT_STYLE=.*/GRUB_TIMEOUT_STYLE=menu/' /etc/default/grub && sudo update-grub`
- Opcional, para arrancarlo siempre por defecto (comprueba después con `uname -r`):
  ```bash
  sudo sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 6.12.69-202200389-so2-final"/' /etc/default/grub
  sudo update-grub
  ```

Ya dentro:

```bash
uname -r                       # debe ser 6.12.69-202200389-so2-final
ls -lh /sys/kernel/btf/vmlinux # debe existir (≈ 5–6 MB)
```

### 3.9 Verificar el kernel

```bash
cd ~/SO2/ProyectoUnico_2S2026/scripts
./07_verificar_kernel.sh
```

Pide la contraseña de `sudo` (usa `bpftool`). **Debe mostrar:**

- `OK release` y `OK BTF`.
- Soporte eBPF: `bpf() syscall is available`, `program_type kprobe/tracepoint is available`, `map_type ringbuf is available`.
- Los tres tracepoints `OK` (`sys_enter_openat`, `sys_enter_write`, `sys_enter_close`).
- `Generado …/ebpf/vmlinux.h` (≈ 145 000 líneas).
- Los 5 programas de prueba compilados y la salida de `prueba` con `ESTADO GENERAL DE LOS PROCESOS`, `Entradas devueltas` y un
  **Top 5 por memoria**: eso confirma que las syscalls 548 y 549 funcionan.

Prueba además el canal IPC bloqueante, con dos terminales:

```bash
# Terminal 1 (se queda esperando, sin consumir CPU)
~/SO2/ProyectoUnico_2S2026/workspace/bin/ipc_receive
# Terminal 2
~/SO2/ProyectoUnico_2S2026/workspace/bin/ipc_send "Hola"
```

La terminal 1 debe despertar y mostrar `Contenido: Hola`. **Con esto el kernel está listo.**

---

## 4. Parte B — El sistema de monitoreo

Desde aquí trabajas dentro de `~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio` y **con el kernel nuevo arrancado**.

### 4.1 Compilar el daemon y el sensor eBPF

```bash
cd ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio
make
```

Genera el esqueleto del sensor eBPF con `clang` y `bpftool` (usa el BTF del kernel en ejecución) y compila `./so2monitor`. **Debe
terminar** con la línea de enlace `cc -o so2monitor …` y **sin** líneas `warning`. (El repositorio incluye un `so2monitor` ya
compilado, pero recompílalo en tu máquina.)

### 4.2 Preparar el sistema (grupos, autenticación y carpetas)

```bash
sudo ./scripts/instalar_sistema.sh
```

Hace, una sola vez:

| Acción | Para qué |
|---|---|
| Crea los grupos `admin_user` y `common_user` | Definen el rol de cada usuario en el dashboard |
| Instala el servicio PAM `/etc/pam.d/so2-monitor` | El daemon autentica contra las cuentas del sistema |
| Crea `/tmp/monitor` (`1777`) | Carpeta vigilada por el sensor |
| Crea `/var/log/so2_app.log` y `/var/log/so2_central.log` | Logs de ejemplo del monitoreo de palabras clave |

**Debe mostrar** `grupo creado: admin_user`, `grupo creado: common_user`, `servicio PAM instalado` y las rutas creadas.

### 4.3 Crear los usuarios

Los usuarios son cuentas normales del sistema (`useradd`) que pertenecen a uno de los dos grupos:

```bash
sudo ./scripts/crear_usuario.sh admin1 admin        # administrador: te pide su contraseña
sudo ./scripts/crear_usuario.sh usuario1 common     # usuario común:  te pide su contraseña
```

Elige y **anota** las contraseñas (se escriben sin que se vean). Comprueba los grupos:

```bash
id admin1       # debe incluir admin_user
id usuario1     # debe incluir common_user
```

- Un usuario en **ambos** grupos es administrador; uno en **ninguno** no puede entrar al dashboard.
- Puedes crear más usuarios cuando quieras, incluso con el daemon en marcha (no hay que reiniciarlo).
- Para agregar una cuenta que ya existe a un rol: `sudo usermod -aG admin_user NOMBRE`.
- Para cambiar una contraseña: `sudo passwd NOMBRE`.

### 4.4 Iniciar el daemon

```bash
cd ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio
sudo ./so2monitor -f
```

`-f` lo deja en primer plano y muestra su registro. Necesita `sudo` (cargar el sensor eBPF, autenticar con PAM, aislar archivos).
**Debe mostrar**, sin errores:

```text
blacklist: 6 firmas cargadas
sensor eBPF cargado (PID del daemon registrado: …)
ruta a monitorear: /tmp/monitor (generacion 1)
hilo de monitoreo iniciado … hilo de archivos iniciado … hilo IPC iniciado: bloqueado en sys_ipc_channel_receive
HTTP: escuchando en el puerto 8080
logwatch 1 iniciado por configuracion: central=/var/log/so2_central.log palabra=ERROR …
```

Para dejarlo **en segundo plano** como servicio, ejecútalo sin `-f` (registro en `/var/log/so2monitor.log`, PID en
`/run/so2monitor.pid`) y deténlo con `sudo kill $(cat /run/so2monitor.pid)`.

### 4.5 Abrir el dashboard

Averigua la IP de la máquina y abre el puerto 8080 desde un navegador (en la propia máquina o en otra de la misma red):

```bash
hostname -I                    # p. ej. 192.168.122.96
```

```text
http://IP_DE_LA_MAQUINA:8080        (en la propia máquina: http://127.0.0.1:8080)
```

Inicia sesión con `admin1` (verás todos los controles) o con `usuario1` (solo lectura). Arriba a la derecha debe decir **En vivo**
en verde. Si hay un firewall activo (`sudo ufw status`), permite el puerto: `sudo ufw allow 8080/tcp`.

> **VM con red NAT (libvirt):** la IP suele ser `192.168.122.x` y se puede abrir directamente desde el anfitrión.

---

## 5. Parte C — Verificación completa

Con el daemon corriendo, en **otra terminal**:

```bash
cd ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio
./scripts/probar_api.sh
```

Pide el usuario administrador y el común con sus contraseñas y ejecuta **58 comprobaciones** de extremo a extremo: sesión y
roles, permisos, escaneo de archivos y cuarentena con las 6 muestras, blacklist, procesos, logwatch con IPC e interruptores.
**Resultado esperado: `58 correctas, 0 fallidas`.**

Demostración rápida a mano (con el escaneo activado desde el dashboard: *Archivos analizados* → ruta `/tmp/monitor` →
**Aplicar** → interruptor **Escaneo de archivos (eBPF)**):

```bash
cp ~/SO2/ProyectoUnico_2S2026/Drive/"Archivos de Prueba"/*.txt /tmp/monitor/
echo "ERROR acceso rechazado" >> /var/log/so2_app.log
```

Las 6 muestras aparecen como **En cuarentena** y se generan alertas HIGH; la línea del log genera una alerta MEDIUM. Si al
arrancar ves alertas de logs viejos, es normal (el monitoreo lee el archivo desde el principio); vacíalo con
`: > /var/log/so2_app.log`.

La guía de uso del dashboard está en el [Manual de Usuario](Manual_Usuario.md).

**Opcional — Práctica 4:** el daemon de la Práctica 4 (`Practica_4_2S2026/daemon`) también funciona con este kernel
(`make && ./so2_p4 -f`), pero **no a la vez** que `so2monitor`: ambos usan el puerto 8080 y la misma cola IPC del kernel.

---

## 6. Problemas frecuentes

| Síntoma | Causa y solución |
|---|---|
| `pahole: command not found` o BTF queda desactivado | Falta `dwarves`. Ejecuta `./02_dependencias.sh` y repite `./04_configurar_kernel.sh` |
| `No space left on device` al compilar | Poco disco. `df -h /`; libera espacio o amplía el disco. Necesitas ≥ 25 GB libres |
| La compilación termina con `Killed` / `signal 9` | Se quedó sin RAM. Usa `JOBS=1 ./05_compilar.sh` o añade swap (`sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile`) |
| Error de compilación en `kernel/sys.c` | Aparece en el primer paso de `05` (en segundos). Verifica que `03` se ejecutó sin avisos y que `Kernel/` es el del repositorio |
| El kernel nuevo no arranca (`bad shim signature`, error de firma) | **Secure Boot** activado. Desactívalo en el firmware o arranca con el kernel anterior desde GRUB |
| El menú de GRUB no aparece | Mantén **Shift** o **Esc** al encender, o haz visible el menú (ver §3.8) |
| Pantalla negra en la VM | Cambia el video a **QXL** en virt-manager |
| El sistema no arranca tras `localmodconfig` (falta un controlador) | Arranca con otro kernel desde GRUB y repite `04` con `USE_LOCALMOD=0`, o carga ese hardware antes de configurar para que quede en `lsmod` |
| `failed to find valid kernel BTF` al iniciar el daemon | No arrancaste con el kernel `…-so2-final`. Reinicia y elígelo en GRUB (`uname -r`) |
| `Address already in use` | Otro programa usa el puerto 8080 (p. ej. el daemon de la P4). Deténlo |
| Warning de `bpftool` («not found for kernel») | Normal: es el envoltorio de `/usr/sbin/bpftool`. El proyecto usa `/usr/lib/linux-tools/*/bpftool` |
| `make` del daemon: falta `bpf/libbpf.h` o `security/pam_appl.h` | Falta `libbpf-dev` o `libpam0g-dev`. Ejecuta `./02_dependencias.sh` |
| No puedes entrar al dashboard con una cuenta válida | No está en `admin_user` ni `common_user` (`id NOMBRE`), o la contraseña es incorrecta (`su - NOMBRE` para probarla) |
| El dashboard no carga desde otro equipo | Revisa la IP (`hostname -I`), que el daemon corra y el firewall (`sudo ufw allow 8080/tcp`) |
| `./03_preparar_arbol.sh` dice «Ya existe el árbol del kernel» | Ya lo ejecutaste. `rm -rf ~/SO2/ProyectoUnico_2S2026/workspace` y repite desde `03` |
| Un script no se ejecuta (`Permiso denegado`) | `chmod +x ~/SO2/ProyectoUnico_2S2026/scripts/*.sh ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio/scripts/*.sh` |

## 7. Resumen de comandos

Para quien ya leyó la guía y solo quiere los comandos (en orden, esperando el resultado de cada uno):

```bash
# --- Preparación
sudo apt update && sudo apt install -y git
mkdir -p ~/SO2 && git clone -b 202200389 git@gitlab.com:ingenieria.usac/sistemas/so2/mapa.git ~/SO2/mapa
cp -r ~/SO2/mapa/ProyectoUnico_2S2026 ~/SO2/ && cd ~/SO2/ProyectoUnico_2S2026/scripts
chmod +x *.sh ../ProgramaIntermedio/scripts/*.sh

# --- Parte A: kernel
./00_diagnostico.sh
./02_dependencias.sh
./02a_obtener_fuentes.sh
./03_preparar_arbol.sh
./04_configurar_kernel.sh
./05_compilar.sh                  # dentro de tmux
./06_instalar.sh
sudo reboot                       # GRUB: Advanced options → …-so2-final
uname -r && ls /sys/kernel/btf/vmlinux
./07_verificar_kernel.sh

# --- Parte B: sistema de monitoreo
cd ../ProgramaIntermedio && make
sudo ./scripts/instalar_sistema.sh
sudo ./scripts/crear_usuario.sh admin1 admin
sudo ./scripts/crear_usuario.sh usuario1 common
sudo ./so2monitor -f              # y abrir http://IP:8080

# --- Parte C: verificación (otra terminal)
./scripts/probar_api.sh           # 58 correctas, 0 fallidas
```

## 8. Volver atrás

El kernel original **no se toca**: sigue instalado y se elige desde GRUB (*Advanced options*). Para quitar el kernel del proyecto,
arranca primero con otro kernel (`uname -r` no debe ser el `…-so2-final`) y ejecuta:

```bash
R=6.12.69-202200389-so2-final
sudo rm -rf /boot/vmlinuz-$R* /boot/initrd.img-$R* /boot/System.map-$R* /boot/config-$R* /lib/modules/$R /var/lib/initramfs-tools/$R
sudo update-grub
```

(Usa siempre el nombre **completo**, nunca un comodín como `6.12.69-202200389-so2*`, que también coincidiría con otros kernels.)
Para empezar la compilación desde cero: `rm -rf ~/SO2/ProyectoUnico_2S2026/workspace`.
