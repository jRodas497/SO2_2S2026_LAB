# Práctica 2 - Información de procesos con nuevas llamadas al sistema

**Curso:** Sistemas Operativos 2  
**Carné:** 202200389  
**Kernel base:** Linux 6.12.69  
**Kernel compilado:** `6.12.69-202200389-so2-p2`  
**Rama de entrega:** `202200389`

## Descripción

En esta práctica se modificó el kernel Linux 6.12.69 para implementar dos nuevas llamadas al sistema:

- `get_process_info`: recibe un PID y devuelve nombre, PID, tiempo de ejecución y uso aproximado de memoria.
- `get_processes_state`: devuelve el total de procesos y la cantidad en estados Running, Sleeping/IDLE y Zombie.

La transferencia de información del kernel al programa de usuario se realiza mediante `copy_to_user()`.

## Syscalls implementadas

| Número | Syscall |
|---:|---|
| 548 | `get_process_info` |
| 549 | `get_processes_state` |

## Estructura

```text
Practica_2_2S2026/
├── README.md
├── docs/
│   ├── Informe_Tecnico.md
│   └── EVIDENCIAS.md
├── img/
├── linux 6.12.69/
│   ├── arch/x86/entry/syscalls/syscall_64.tbl
│   ├── include/linux/syscalls.h
│   └── kernel/sys.c
└── Programa_Intermedio/
    └── prueba.c
```

## Programa de prueba

```bash
cd Programa_Intermedio
gcc -Wall -Wextra prueba.c -o prueba
./prueba <PID>
```

Ejemplo:

```bash
./prueba $$
```

## Verificación del kernel

```bash
uname -r
```

Resultado esperado:

```text
6.12.69-202200389-so2-p2
```

## Informe técnico

[Ver Informe Técnico](docs/Informe_Tecnico.md)
