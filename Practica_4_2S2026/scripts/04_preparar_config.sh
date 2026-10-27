#!/usr/bin/env bash
set -euo pipefail
K="$HOME/SO2/Practica_4_2S2026/workspace/linux-6.12.69"
CFG="$HOME/SO2/recursos/configs/config-6.12.69-p4-base"
[ -d "$K" ] || { echo "Falta $K"; exit 1; }
[ -f "$CFG" ] || { echo "Falta $CFG"; exit 1; }

cd "$K"
cp "$CFG" .config
./scripts/config --set-str LOCALVERSION "-202200389-so2-p4"
./scripts/config --disable LOCALVERSION_AUTO
./scripts/config --enable DEBUG_INFO_NONE
./scripts/config --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
./scripts/config --disable DEBUG_INFO_DWARF4
./scripts/config --disable DEBUG_INFO_DWARF5
./scripts/config --disable DEBUG_INFO_BTF
./scripts/config --disable DEBUG_INFO_BTF_MODULES
./scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
./scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
make olddefconfig
make kernelrelease
echo "Configuracion preparada. Todavia no se ha compilado."
