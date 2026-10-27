#!/usr/bin/env bash
# Verifica el kernel final: BTF, soporte eBPF, tracepoints, ring buffer y syscalls
# 548-553. Genera vmlinux.h para el sensor eBPF. No modifica el sistema.
set -uo pipefail
source "$(dirname "$0")/env.sh"

BIN="$PU_BASE/workspace/bin"
EBPF="$PU_BASE/ProgramaIntermedio/ebpf"
mkdir -p "$BIN" "$EBPF"

echo "== Kernel"; uname -r
[ "$(uname -r)" = "$PU_RELEASE" ] && echo "OK release" || echo "ATENCION: no es $PU_RELEASE"
[ -e /sys/kernel/btf/vmlinux ] && echo "OK BTF" || echo "FALTA BTF"

# bpftool real (el wrapper de /usr/sbin no corre con kernels personalizados)
BPFTOOL=$(ls -1 /usr/lib/linux-tools/*/bpftool 2>/dev/null | sort -V | tail -1)
echo; echo "== bpftool: ${BPFTOOL:-NO ENCONTRADO}"
[ -n "$BPFTOOL" ] || exit 1
sudo "$BPFTOOL" version | head -1

echo; echo "== Soporte eBPF del kernel"
sudo "$BPFTOOL" feature probe kernel 2>/dev/null | grep -E 'bpf\(\) syscall|map_type ringbuf|program_type tracepoint|program_type kprobe|helper bpf_ringbuf_output' | head -8

echo; echo "== Tracepoints necesarios"
for t in sys_enter_openat sys_enter_write sys_enter_close; do
  sudo test -d /sys/kernel/tracing/events/syscalls/$t && echo "OK   $t" || echo "FALTA $t"
done

echo; echo "== vmlinux.h"
sudo "$BPFTOOL" btf dump file /sys/kernel/btf/vmlinux format c > "$EBPF/vmlinux.h" \
  && echo "Generado $EBPF/vmlinux.h ($(wc -l < "$EBPF/vmlinux.h") lineas)"

echo; echo "== Compilando programas de prueba de las syscalls"
cd "$PU_BASE/ProgramaIntermedio/pruebas" || exit 1
for p in prueba ipc_send ipc_receive start_log_watch stop_log_watch; do
  gcc -Wall -Wextra "$p.c" -o "$BIN/$p" && echo "OK $p"
done

echo; echo "== Syscalls 548/549 (P2)"
timeout 20 "$BIN/prueba" </dev/null | head -40

echo; echo "== dmesg"
sudo dmesg | grep -E 'SO2' | tail -n 15
echo
echo "Binarios en $BIN. Prueba IPC manual:"
echo "  Terminal 1: $BIN/ipc_receive      (debe bloquearse)"
echo "  Terminal 2: $BIN/ipc_send 'Hola'  (el receptor despierta)"
