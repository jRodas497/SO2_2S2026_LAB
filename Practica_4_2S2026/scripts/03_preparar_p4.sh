#!/usr/bin/env bash
set -euo pipefail
BASE="$HOME/SO2/Practica_4_2S2026"
WORK="$BASE/workspace"
TAR="$HOME/SO2/recursos/linux-6.12.69.tar.xz"
[ -f "$TAR" ] || { echo "Falta $TAR"; exit 1; }

mkdir -p "$WORK" "$BASE/Programa_Intermedio" "$BASE/dashboard" "$BASE/docs" "$BASE/img" "$BASE/logs" "$BASE/linux 6.12.69"
[ ! -d "$WORK/linux-6.12.69" ] || { echo "Ya existe el arbol del kernel"; exit 1; }

tar -xf "$TAR" -C "$WORK"
cd "$WORK/linux-6.12.69"
make kernelversion

if grep -nE '^(548|549|550|551|552|553)[[:space:]]' arch/x86/entry/syscalls/syscall_64.tbl; then
  echo "Alguno de 548-553 esta ocupado"; exit 2
else
  echo "Syscalls 548-553 libres."
fi
