# Proyecto Único — Sistema Integral de Monitoreo y Seguridad

Sistemas Operativos 2 · Carné 202200389 · Linux 6.12.69

Plataforma de monitoreo y seguridad a nivel kernel: syscalls reutilizadas de las prácticas 2–4, un sensor **eBPF** que
intercepta la escritura de archivos, un **daemon en C** (3 hilos) que calcula SHA-256, compara contra una blacklist,
aísla archivos y genera alertas LOW/MEDIUM/HIGH, autenticación con **PAM** (roles `admin_user` / `common_user`) y un
**dashboard web** en tiempo real.

## Documentación

- [Manual Técnico](docs/Manual_Tecnico.md): arquitectura, syscalls, sensor eBPF, daemon, seguridad, pruebas y errores.
- [Manual de Usuario](docs/Manual_Usuario.md): encender el sistema, iniciar sesión y usar el dashboard (con capturas).
- [Guía de instalación desde cero](docs/Guia_Instalacion_Desde_Cero.md): kernel, usuarios, daemon y dashboard en una máquina nueva, paso a paso.

## Inicio rápido (VM Ubuntu con el kernel `6.12.69-202200389-so2-final`)

```bash
cd ProgramaIntermedio
make                                          # sensor eBPF + daemon (o usar el ejecutable ./so2monitor incluido)
sudo ./scripts/instalar_sistema.sh            # grupos, servicio PAM, directorio vigilado, logs de ejemplo
sudo ./scripts/crear_usuario.sh admin1 admin
sudo ./scripts/crear_usuario.sh usuario1 common
sudo ./so2monitor -f                          # luego abrir http://IP_DE_LA_VM:8080
./scripts/probar_api.sh                       # (otra terminal) 58 comprobaciones de extremo a extremo
```

## Estructura

| Carpeta | Contenido |
|---|---|
| `Kernel/` | Archivos modificados del kernel 6.12.69 (`sys.c`, `syscalls.h`, `syscall_64.tbl`) |
| `ProgramaIntermedio/` | Daemon (`src/`), sensor eBPF (`ebpf/`), configuración, scripts, ejecutable `so2monitor` |
| `Dashboard/` | Frontend en HTML, CSS y JavaScript (Chart.js incluido en `vendor/`) |
| `docs/` | Manual Técnico y Manual de Usuario |
| `scripts/` | Preparación, compilación e instalación del kernel (con BTF y eBPF) |
| `img/` | Capturas |
| `Drive/` | `blacklist.json` y archivos de prueba proporcionados por el curso |
