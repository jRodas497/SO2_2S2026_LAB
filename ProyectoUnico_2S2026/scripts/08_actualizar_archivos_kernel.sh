#!/usr/bin/env bash
# Copia al arbol de compilacion los 3 archivos de Kernel/ (tras editarlos en
# Fedora y subirlos a la VM). Despues: ./05_compilar.sh, ./06_instalar.sh, reboot.
set -euo pipefail
source "$(dirname "$0")/env.sh"

[ -d "$PU_KERNEL_DIR" ] || { echo "Falta $PU_KERNEL_DIR"; exit 1; }
for f in kernel/sys.c include/linux/syscalls.h arch/x86/entry/syscalls/syscall_64.tbl; do
  if cmp -s "$PU_BASE/Kernel/$f" "$PU_KERNEL_DIR/$f"; then
    echo "sin cambios  $f"
  else
    cp "$PU_BASE/Kernel/$f" "$PU_KERNEL_DIR/$f"
    echo "ACTUALIZADO  $f"
  fi
done
echo
grep -nE 'SYSCALL_DEFINE3\(get_processes_state' "$PU_KERNEL_DIR/kernel/sys.c" \
  && echo "OK: sys.c tiene la version nueva de get_processes_state" \
  || { echo "ERROR: sys.c no tiene la version nueva"; exit 1; }
echo "Nota: cambiar syscalls.h recompila casi todo el kernel (varios minutos)."
