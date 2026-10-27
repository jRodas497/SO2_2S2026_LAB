#!/usr/bin/env bash
# Solo lectura. No modifica nada.
source "$(dirname "$0")/env.sh"

echo "== Kernel en ejecucion"; uname -r
echo "BTF del kernel actual: $([ -e /sys/kernel/btf/vmlinux ] && echo SI || echo NO)"
echo; echo "== CPU / RAM"; nproc; free -h
echo; echo "== Disco"; df -h /
echo; echo "== ~/SO2"; du -sh ~/SO2/* 2>/dev/null | sort -h
echo; echo "== Arbol de compilacion P4"; du -sh "$P4_BASE/workspace" 2>/dev/null || echo "(no existe)"
echo; echo "== Kernels instalados"; ls -d /lib/modules/* ; du -sh /lib/modules/* 2>/dev/null | sort -h
ls -lh /boot | grep -E 'vmlinuz|initrd|System.map|config' 
echo; echo "== Recursos"
ls -lh "$PU_TARBALL" "$PU_CONFIG_BASE" 2>&1
echo; echo "== Herramientas (necesarias para el proyecto)"
for t in gcc make clang llvm-strip bpftool pahole bison flex bc pkg-config; do
  printf '%-12s' "$t"; command -v "$t" || echo "FALTA"
done
for p in libbpf openssl; do
  printf '%-12s' "$p"; pkg-config --modversion "$p" 2>/dev/null || echo "FALTA"
done
printf '%-12s' "pam.h"; [ -f /usr/include/security/pam_appl.h ] && echo OK || echo FALTA
printf '%-12s' "pahole ver"; pahole --version 2>/dev/null || echo "FALTA"
