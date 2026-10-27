#!/usr/bin/env bash
# Extrae Linux limpio y aplica encima los 3 archivos del kernel (P2+P3+P4)
# que estan en ProyectoUnico_2S2026/Kernel/.
set -euo pipefail
source "$(dirname "$0")/env.sh"

[ -f "$PU_TARBALL" ] || { echo "Falta $PU_TARBALL"; exit 1; }
[ ! -d "$PU_KERNEL_DIR" ] || { echo "Ya existe $PU_KERNEL_DIR"; exit 1; }
for f in kernel/sys.c include/linux/syscalls.h arch/x86/entry/syscalls/syscall_64.tbl; do
  [ -f "$PU_BASE/Kernel/$f" ] || { echo "Falta $PU_BASE/Kernel/$f"; exit 1; }
done

mkdir -p "$PU_WORK" "$PU_BASE"/{logs,docs,img,Dashboard,ProgramaIntermedio/ebpf}
tar -xf "$PU_TARBALL" -C "$PU_WORK"
cd "$PU_KERNEL_DIR"
[ "$(make kernelversion)" = "6.12.69" ] || { echo "Version inesperada"; exit 1; }

echo "== Numeros 548-553 en el arbol limpio (deben estar libres)"
if grep -nE '^(548|549|550|551|552|553)[[:space:]]' arch/x86/entry/syscalls/syscall_64.tbl; then
  echo "Ocupados. Abortando."; exit 2
fi
echo "Libres."

echo "== Aplicando archivos del kernel (originales en $PU_WORK/originales)"
mkdir -p "$PU_WORK/originales"
for f in kernel/sys.c include/linux/syscalls.h arch/x86/entry/syscalls/syscall_64.tbl; do
  mkdir -p "$PU_WORK/originales/$(dirname "$f")"
  cp "$f" "$PU_WORK/originales/$f"
  cp "$PU_BASE/Kernel/$f" "$f"
  printf '%-45s +%s -%s lineas\n' "$f" \
    "$(diff "$PU_WORK/originales/$f" "$f" | grep -c '^>' || true)" \
    "$(diff "$PU_WORK/originales/$f" "$f" | grep -c '^<' || true)"
done

echo "== Syscalls registradas"
grep -nE '^(548|549|550|551|552|553)[[:space:]]' arch/x86/entry/syscalls/syscall_64.tbl
grep -nE 'sys_(get_process_info|get_processes_state|ipc_channel_send|ipc_channel_receive|start_log_watch|stop_log_watch)' include/linux/syscalls.h
