# Informe Técnico
## Práctica 2 - Información de procesos con nuevas llamadas al sistema

**Curso:** Sistemas Operativos 2  
**Carné:** 202200389  
**Kernel base:** Linux 6.12.69  
**Kernel compilado:** `6.12.69-202200389-so2-p2`  
**Arquitectura:** x86_64  

---

# 1. Introducción y objetivos

## 1.1 Introducción

En esta práctica se extendió la funcionalidad del kernel Linux 6.12.69 mediante la implementación de dos nuevas llamadas al sistema relacionadas con la consulta de procesos.

La primera llamada, `get_process_info`, permite consultar un proceso mediante su PID y obtener su nombre, PID, tiempo de ejecución y uso aproximado de memoria. La segunda, `get_processes_state`, recorre la lista de procesos del sistema para obtener la cantidad total de procesos y clasificarlos en estados Running, Sleeping/IDLE y Zombie.

La práctica también permitió comprobar la separación entre espacio de usuario y espacio kernel. Los datos son construidos dentro del kernel y posteriormente transferidos mediante `copy_to_user()`.

## 1.2 Objetivo general

Implementar llamadas al sistema personalizadas en Linux 6.12.69 para consultar información de procesos desde espacio de usuario de forma segura.

## 1.3 Objetivos específicos

- Implementar `get_process_info`.
- Buscar procesos por PID utilizando estructuras internas del kernel.
- Obtener nombre, PID, tiempo de ejecución y memoria aproximada.
- Implementar `get_processes_state`.
- Recorrer procesos mediante `for_each_process`.
- Clasificar procesos en Running, Sleeping/IDLE y Zombie.
- Utilizar `copy_to_user()` para transferir datos.
- Compilar e instalar el kernel modificado.
- Crear un programa en C para probar las syscalls.
- Validar la implementación mediante varias ejecuciones y `dmesg`.

---

# 2. Entorno de trabajo

| Elemento | Configuración |
|---|---|
| Sistema anfitrión | Fedora Linux |
| Virtualización | QEMU/KVM |
| Sistema invitado | Ubuntu 24.04 LTS |
| Arquitectura | x86_64 |
| Kernel base | Linux 6.12.69 |
| Kernel personalizado | `6.12.69-202200389-so2-p2` |
| Lenguaje | C |
| Compilador | GCC |
| RAM de la VM | Aproximadamente 4 GB |
| CPU de la VM | 4 vCPU |

---

# 3. Guía paso a paso de los archivos modificados

## 3.1 `kernel/sys.c`

En este archivo se implementaron las dos llamadas al sistema.

### `get_process_info`

El flujo utilizado fue:

1. Validar PID y puntero recibido.
2. Localizar el proceso mediante `find_get_pid()`.
3. Obtener su `task_struct` mediante `get_pid_task()`.
4. Obtener nombre y PID.
5. Calcular el tiempo de ejecución usando `start_boottime`.
6. Obtener información de memoria mediante `get_task_mm()` y `get_mm_rss()`.
7. Transferir la estructura con `copy_to_user()`.
8. Registrar la ejecución mediante `pr_info()`.

La estructura utilizada fue:

```c
struct so2_process_info {
    char name[TASK_COMM_LEN];
    pid_t pid;
    u64 runtime_ns;
    unsigned long memory_kb;
};
```

![Implementación de get_process_info](../img/02_codigo_process_info.png)

### `get_processes_state`

La syscall recorre la lista de procesos mediante `for_each_process()`.

Cada proceso se clasifica como:

- Zombie mediante `EXIT_ZOMBIE`.
- Running mediante `TASK_RUNNING`.
- Sleeping/IDLE mediante `TASK_INTERRUPTIBLE` o `TASK_UNINTERRUPTIBLE`.

La estructura utilizada fue:

```c
struct so2_processes_state {
    unsigned long total;
    unsigned long running;
    unsigned long sleeping;
    unsigned long zombie;
};
```

Al final se utiliza `copy_to_user()` para transferir los resultados.

![Implementación de get_processes_state](../img/03_codigo_processes_state.png)

## 3.2 `include/linux/syscalls.h`

Se agregaron los prototipos de las dos nuevas llamadas al sistema.

## 3.3 `arch/x86/entry/syscalls/syscall_64.tbl`

Se verificó previamente que 548 y 549 estaban libres y se registraron:

```text
548    common    get_process_info       sys_get_process_info
549    common    get_processes_state    sys_get_processes_state
```

![Registro de las syscalls](../img/04_registro_syscalls.png)

---

# 4. Configuración del kernel

Se utilizó una configuración funcional de Linux 6.12.69 como base y se estableció:

```text
CONFIG_LOCALVERSION="-202200389-so2-p2"
```

Esto permitió conservar separado el kernel personalizado de la práctica anterior.

También se deshabilitó información de depuración innecesaria para reducir el consumo de recursos durante la compilación.

La versión resultante fue:

```text
6.12.69-202200389-so2-p2
```

![Configuración con menuconfig](../img/01_menuconfig.png)

---

# 5. Proceso de compilación

Primero se ejecutaron:

```bash
make olddefconfig
make prepare
```

Después se comprobó específicamente el archivo modificado:

```bash
make -j1 kernel/sys.o
```

Resultado:

```text
CC      kernel/sys.o
```

Posteriormente se compiló el kernel completo:

```bash
make -j1 2>&1 | tee ~/SO2/Practica_2_2S2026/compilacion-kernel-p2.log
```

La compilación finalizó con código de salida 0.

![Compilación del kernel](../img/05_compilacion_inicial_kernel_01.png)

Finalmente se verificaron los artefactos:

```text
arch/x86/boot/bzImage   14 MB
vmlinux                 67 MB
```

![Compilación final](../img/06_compilacion_final_kernel_02.png)

---

# 6. Instalación y arranque

Los módulos se instalaron con:

```bash
sudo make modules_install
```

![Instalación de módulos](../img/07_modules_install.png)

Luego:

```bash
sudo make install
```

Se generaron correctamente:

```text
/boot/vmlinuz-6.12.69-202200389-so2-p2
/boot/System.map-6.12.69-202200389-so2-p2
/boot/config-6.12.69-202200389-so2-p2
/boot/initrd.img-6.12.69-202200389-so2-p2
```

![Kernel instalado](../img/08_kernel_instalado.png)

GRUB detectó el nuevo kernel.

![Kernel disponible en GRUB](../img/09_grub_kernel.png)

Luego de iniciar Ubuntu se comprobó:

```bash
uname -r
```

Resultado:

```text
6.12.69-202200389-so2-p2
```

![Kernel en ejecución](../img/10_uname_kernel.png)

---

# 7. Programa de usuario

Se creó `Programa_Intermedio/prueba.c`.

El programa utiliza:

```c
#define SYS_GET_PROCESS_INFO 548
#define SYS_GET_PROCESSES_STATE 549
```

y llama a las syscalls mediante `syscall()`.

Compilación:

```bash
gcc -Wall -Wextra prueba.c -o prueba
```

Ejemplo:

```bash
./prueba $$
```

---

# 8. Pruebas realizadas

Se realizaron varias pruebas exitosas.

En una prueba con Bash se obtuvo:

```text
Nombre                : bash
PID                   : 2460
Tiempo de ejecucion   : 108.660 segundos
Memoria aproximada    : 5452 KB

Total                 : 261
Running               : 2
Sleeping / IDLE       : 259
Zombie                : 0
```

En ejecuciones posteriores el tiempo aumentó a 119.866 y 129.230 segundos. Esto demuestra que el tiempo de ejecución se actualiza correctamente.

Los valores Running y Sleeping también cambiaron entre ejecuciones debido a que el estado de los procesos es dinámico.

Además se realizaron pruebas con otros procesos, incluyendo PID 1 (`systemd`).

![Pruebas 1](../img/12_pruebas_01.png)

![Pruebas 2](../img/12_pruebas_02.png)

![Pruebas 3](../img/12_pruebas_03.png)

---

# 9. Validación mediante `dmesg`

Las syscalls generan mensajes mediante `pr_info()`.

La validación se realizó con:

```bash
sudo dmesg | grep "SO2 P2:"
```

Se observaron registros de ambas llamadas, por ejemplo consultas a:

```text
pid=2460 name=bash
pid=1 name=systemd
```

y los conteos generados por `get_processes_state`.

![Validación mediante dmesg](../img/13_dmesg.png)

---

# 10. Diferencia entre espacio de usuario y espacio kernel

El programa de usuario y el kernel trabajan en espacios de memoria separados.

El puntero entregado a una syscall pertenece al espacio del proceso de usuario. El kernel no debe escribir directamente sobre esa dirección. Por esa razón primero se construye una estructura dentro del kernel y luego se utiliza:

```c
copy_to_user(...)
```

para copiar los datos al programa.

Este mecanismo mantiene el aislamiento entre ambos espacios y evita accesos inseguros a memoria privilegiada.

---

# 11. Conclusiones

1. Se implementaron correctamente dos nuevas llamadas al sistema en Linux 6.12.69 para consultar información de procesos.

2. El uso de `task_struct`, `get_task_mm()` y `for_each_process()` permitió trabajar directamente con estructuras internas del kernel.

3. `copy_to_user()` permitió transferir la información de manera segura desde espacio kernel hacia espacio de usuario.

4. Las pruebas mostraron que métricas como tiempo de ejecución y estado de los procesos cambian dinámicamente entre llamadas.

5. El identificador `6.12.69-202200389-so2-p2` permitió conservar separado este kernel del utilizado en la práctica anterior.

---

# 12. Lecciones aprendidas

Se aprendió que agregar una syscall requiere modificar de forma coordinada su implementación, declaración y registro en la tabla de llamadas al sistema.

También fue útil compilar primero `kernel/sys.o` antes de iniciar la compilación completa, porque esto permitió validar rápidamente la parte modificada.

Finalmente, la práctica ayudó a comprender de forma práctica la separación entre espacio de usuario y espacio kernel y la necesidad de utilizar funciones seguras para transferir información entre ambos.
