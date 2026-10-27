#!/usr/bin/env bash
set -euo pipefail

DIR="${HOME}/SO2/Practica_3_2S2026/Programa_Intermedio"
cd "$DIR"

for src in ipc_send.c ipc_receive.c start_log_watch.c stop_log_watch.c; do
    out="${src%.c}"
    echo "Compilando $src -> $out"
    gcc -Wall -Wextra -O2 "$src" -o "$out"
done

echo
echo "Programas compilados:"
ls -lh ipc_send ipc_receive start_log_watch stop_log_watch
