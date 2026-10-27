#!/usr/bin/env bash
# Variables compartidas por los scripts del Proyecto Unico (se cargan con source).
# La carpeta del proyecto se deduce de donde esta este script, asi que funciona en
# cualquier maquina y en cualquier ruta. Todo se puede sobrescribir con variables
# de entorno (PU_BASE, PU_RECURSOS, PU_CONFIG_BASE).
_pu_aqui="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PU_BASE="${PU_BASE:-$(dirname "$_pu_aqui")}"
export PU_RECURSOS="${PU_RECURSOS:-$HOME/SO2/recursos}"
export PU_WORK="$PU_BASE/workspace"
export PU_KERNEL_DIR="$PU_WORK/linux-6.12.69"
export PU_TARBALL="$PU_RECURSOS/linux-6.12.69.tar.xz"
if [ -z "${PU_CONFIG_BASE:-}" ]; then
  # "config-base" lo crea 02a_obtener_fuentes.sh; el otro nombre es el de la VM original
  if [ -f "$PU_RECURSOS/configs/config-6.12.69-p4-base" ] && [ ! -f "$PU_RECURSOS/configs/config-base" ]; then
    PU_CONFIG_BASE="$PU_RECURSOS/configs/config-6.12.69-p4-base"
  else
    PU_CONFIG_BASE="$PU_RECURSOS/configs/config-base"
  fi
fi
export PU_CONFIG_BASE
export PU_LOCALVERSION="-202200389-so2-final"
export PU_RELEASE="6.12.69${PU_LOCALVERSION}"
export P4_BASE="$HOME/SO2/Practica_4_2S2026"
export P4_RELEASE="6.12.69-202200389-so2-p4"
