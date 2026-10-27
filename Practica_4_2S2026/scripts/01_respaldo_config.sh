#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$HOME/SO2/recursos/configs"
TAR="$HOME/SO2/recursos/linux-6.12.69.tar.xz"
[ -f "$TAR" ] || { echo "Falta $TAR"; exit 1; }

RUNNING="$(uname -r)"
DEST="$HOME/SO2/recursos/configs/config-6.12.69-p4-base"

if [ -r "/boot/config-$RUNNING" ]; then
  cp "/boot/config-$RUNNING" "$DEST"
elif [ -f "$HOME/SO2/respaldos/Practica2/config-6.12.69-p2" ]; then
  cp "$HOME/SO2/respaldos/Practica2/config-6.12.69-p2" "$DEST"
else
  echo "No se encontro una configuracion funcional"; exit 1
fi

ls -lh "$TAR" "$DEST"
