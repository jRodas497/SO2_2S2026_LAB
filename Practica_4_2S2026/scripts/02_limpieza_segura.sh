#!/usr/bin/env bash
set -euo pipefail
RUNNING="$(uname -r)"
case "$RUNNING" in
  6.12.69-202200389-so2*)
    echo "ERROR: estas ejecutando un kernel personalizado anterior."
    echo "Reinicia con el kernel generico de Ubuntu y vuelve a ejecutar."
    exit 1;;
esac

echo "Se borraran P1, P2, P3, respaldos y kernels personalizados antiguos."
echo "Se conservara ~/SO2/recursos/"
read -r -p "Escribe BORRAR para continuar: " X
[ "$X" = "BORRAR" ] || { echo "Cancelado"; exit 0; }

rm -rf "$HOME/SO2/Practica_1_2S2026" \
       "$HOME/SO2/Practica_2_2S2026" \
       "$HOME/SO2/Practica_3_2S2026" \
       "$HOME/SO2/respaldos"

sudo rm -f /boot/vmlinuz-6.12.69-202200389-so2* \
            /boot/initrd.img-6.12.69-202200389-so2* \
            /boot/System.map-6.12.69-202200389-so2* \
            /boot/config-6.12.69-202200389-so2*
sudo rm -rf /lib/modules/6.12.69-202200389-so2*
sudo update-grub

df -h /
du -sh "$HOME"/SO2/* 2>/dev/null | sort -h || true
