#!/usr/bin/env bash
# OPCIONAL, para mas adelante: elimina el kernel P4 INSTALADO (/boot, /lib/modules).
# Usa el nombre EXACTO (nunca globs: 6.12.69-202200389-so2* coincide con todos).
# Solo corre si NO estas arrancado con el kernel P4 ni con el kernel final.
#
# Uso:   ./99_eliminar_kernel_p4.sh              (simulacion)
#        ./99_eliminar_kernel_p4.sh --ejecutar
set -euo pipefail
source "$(dirname "$0")/env.sh"

ACTUAL="$(uname -r)"
[ "$ACTUAL" != "$P4_RELEASE" ] || { echo "Estas ejecutando el kernel P4. Arranca otro y reintenta."; exit 1; }
[ "$ACTUAL" != "$PU_RELEASE" ] || { echo "Arrancaste con el kernel final: confirma que funciona; conserva un kernel generico de respaldo."; }
ls /boot/vmlinuz-*generic >/dev/null 2>&1 || { echo "No hay kernel generico de Ubuntu en /boot. Abortando."; exit 1; }

R="$P4_RELEASE"
LISTA=(/boot/vmlinuz-$R /boot/initrd.img-$R /boot/System.map-$R /boot/config-$R /lib/modules/$R /var/lib/initramfs-tools/$R)
echo "Kernel actual: $ACTUAL"; echo "Se eliminaria:"
for x in "${LISTA[@]}"; do [ -e "$x" ] && echo "  $x" || echo "  ($x no existe)"; done
du -shc /lib/modules/$R 2>/dev/null | tail -1

[ "${1:-}" = "--ejecutar" ] || { echo "SIMULACION. Usa --ejecutar para borrar."; exit 0; }
for x in "${LISTA[@]}"; do [ -e "$x" ] && sudo rm -rf -- "$x"; done
sudo update-grub
df -h / | tail -1
