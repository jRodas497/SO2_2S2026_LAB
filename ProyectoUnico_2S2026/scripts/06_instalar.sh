#!/usr/bin/env bash
# Instala el kernel nuevo. NO reinicia: hazlo tu despues de revisar la salida.
# INSTALL_MOD_STRIP=1 quita la informacion de depuracion de los modulos
# instalados (si no, /lib/modules ocuparia varios GB).
set -euo pipefail
source "$(dirname "$0")/env.sh"

cd "$PU_KERNEL_DIR"
[ -f arch/x86/boot/bzImage ] || { echo "No hay bzImage. Compila primero (05)."; exit 1; }
[ "$(make -s kernelrelease)" = "$PU_RELEASE" ] || { echo "kernelrelease inesperado"; exit 1; }

sudo make INSTALL_MOD_STRIP=1 modules_install
sudo make install
sudo update-grub

echo; echo "== Verificacion"
ls -ld "/lib/modules/$PU_RELEASE"
ls -lh /boot | grep "$PU_RELEASE"
sudo grep -n "$PU_RELEASE" /boot/grub/grub.cfg | head -3
df -h / | tail -1
echo "Listo. Reinicia con: sudo reboot   y luego verifica: uname -r  (debe ser $PU_RELEASE)"
