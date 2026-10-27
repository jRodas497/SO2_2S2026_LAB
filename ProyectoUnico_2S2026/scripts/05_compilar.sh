#!/usr/bin/env bash
# Compila el kernel. JOBS por defecto = 2 (con 8 GiB de RAM). Ejecutar dentro de
# tmux/screen para que no se corte si se cierra la terminal.
set -euo pipefail
set -o pipefail
source "$(dirname "$0")/env.sh"
JOBS="${JOBS:-2}"
LOG="$PU_BASE/logs/compilacion-kernel.log"

cd "$PU_KERNEL_DIR"
[ "$(make -s kernelrelease)" = "$PU_RELEASE" ] || { echo "kernelrelease inesperado: $(make -s kernelrelease)"; exit 1; }
disp_gb=$(df --output=avail -BG / | tail -1 | tr -dc '0-9')
ram_mb=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
echo "Disco libre: ${disp_gb} GB | RAM disponible: ${ram_mb} MB | JOBS=$JOBS"
[ "$disp_gb" -ge 10 ] || { echo "Menos de 10 GB libres. Abortando."; exit 1; }

mkdir -p "$PU_BASE/logs"
echo "== Paso 1: make prepare + kernel/sys.o"
make prepare
make -j1 kernel/sys.o

echo "== Paso 2: compilacion completa (log: $LOG)"
time make -j"$JOBS" 2>&1 | tee "$LOG"

echo; echo "== Errores/warnings"
grep -inE 'error:|fatal:|make(\[[0-9]+\])?: \*\*\*' "$LOG" || echo "sin errores"
ls -lh arch/x86/boot/bzImage vmlinux System.map
df -h / | tail -1
