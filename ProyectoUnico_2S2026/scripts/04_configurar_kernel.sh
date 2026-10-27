#!/usr/bin/env bash
# Prepara .config con BTF + eBPF (requisito del proyecto).
#
# USE_LOCALMOD=1 (por defecto) reduce los modulos a los que la VM tiene cargados
# ahora mismo (make localmodconfig). Es lo que hace viable compilar con BTF en
# ~17 GB libres. Con USE_LOCALMOD=0 se usa la config completa de Ubuntu.
set -euo pipefail
source "$(dirname "$0")/env.sh"
USE_LOCALMOD="${USE_LOCALMOD:-1}"

[ -d "$PU_KERNEL_DIR" ] || { echo "Falta $PU_KERNEL_DIR (corre 03 primero)"; exit 1; }
[ -f "$PU_CONFIG_BASE" ] || { echo "Falta $PU_CONFIG_BASE"; exit 1; }
command -v pahole >/dev/null || { echo "Falta pahole (paquete dwarves). Corre 02."; exit 1; }

cd "$PU_KERNEL_DIR"
cp "$PU_CONFIG_BASE" .config
./scripts/config --set-str LOCALVERSION "$PU_LOCALVERSION"
./scripts/config --disable LOCALVERSION_AUTO
make olddefconfig

if [ "$USE_LOCALMOD" = "1" ]; then
  echo "== localmodconfig con los modulos cargados ahora"
  lsmod > "$PU_WORK/lsmod.txt"
  make LSMOD="$PU_WORK/lsmod.txt" localmodconfig < <(yes '') >/dev/null
fi

# Informacion de depuracion (necesaria para BTF)
./scripts/config --disable DEBUG_INFO_NONE
./scripts/config --enable  DEBUG_INFO
./scripts/config --enable  DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
./scripts/config --enable  DEBUG_INFO_BTF
./scripts/config --disable DEBUG_INFO_BTF_MODULES   # menos tamano; el sensor no lo necesita
# eBPF y trazado
for o in BPF BPF_SYSCALL BPF_EVENTS BPF_JIT FTRACE FTRACE_SYSCALLS TRACEPOINTS KPROBES KPROBE_EVENTS PERF_EVENTS DEBUG_FS; do
  ./scripts/config --enable "$o"
done
./scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
./scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
make olddefconfig

echo; echo "== Verificacion (todo debe ser =y)"
fallo=0
for o in BPF BPF_SYSCALL BPF_EVENTS DEBUG_INFO_BTF FTRACE_SYSCALLS TRACEPOINTS; do
  l=$(grep -E "^CONFIG_$o=|^# CONFIG_$o is not set" .config || echo "CONFIG_$o (ausente)")
  echo "$l"; [ "$l" = "CONFIG_$o=y" ] || fallo=1
done
grep -E '^CONFIG_LOCALVERSION=|LOCALVERSION_AUTO' .config
echo "kernelrelease: $(make -s kernelrelease)"
[ "$(make -s kernelrelease)" = "$PU_RELEASE" ] || fallo=1
[ $fallo -eq 0 ] || { echo "ERROR: la configuracion no quedo como se requiere."; exit 1; }

mkdir -p "$HOME/SO2/recursos/configs"
cp .config "$HOME/SO2/recursos/configs/config-6.12.69-final-base"
echo "Config guardada en ~/SO2/recursos/configs/config-6.12.69-final-base"
echo "Modulos a compilar: $(grep -c '=m$' .config)"
