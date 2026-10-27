#!/usr/bin/env bash
# Libera espacio eliminando SOLO el arbol de compilacion de la Practica 4
# (~/SO2/Practica_4_2S2026/workspace). NO toca /boot, /lib/modules, GRUB ni
# ~/SO2/recursos. El kernel P4 instalado queda como respaldo de arranque.
#
# Uso:   ./01_limpieza_p4.sh              (simulacion, no borra nada)
#        ./01_limpieza_p4.sh --ejecutar   (borra)
set -euo pipefail
source "$(dirname "$0")/env.sh"

W="$P4_BASE/workspace"
K="$W/linux-6.12.69"
EJECUTAR=0; [ "${1:-}" = "--ejecutar" ] && EJECUTAR=1

# Hashes de los archivos P4 que ya estan entregados en Git/GitLab.
declare -A ESPERADO=(
  ["kernel/sys.c"]="d9c0d9c323fd3cd76c8e04911f533ff0bdc45d00885e7760acb0409cef6ae1ee"
  ["include/linux/syscalls.h"]="413fd295c6d475ab4fc5a706e6215fb1dccf7778452a3bfe079ba5181de8ebcd"
  ["arch/x86/entry/syscalls/syscall_64.tbl"]="486fd22955fe1e12691967990be85fa372e0f535954ce4e8e2fc0b248ad05861"
)

[ -d "$W" ] || { echo "No existe $W: nada que limpiar."; exit 0; }
case "$(realpath "$W")" in "$HOME"/SO2/Practica_4_2S2026/workspace) ;; *) echo "Ruta inesperada, abortando"; exit 1;; esac

echo "Kernel en ejecucion: $(uname -r)"
echo "Tamano a liberar:    $(du -sh "$W" | cut -f1)"
df -h / | tail -1

echo; echo "== Verificando que lo de la VM coincide con lo entregado en Git"
if [ -d "$K" ]; then
  for f in "${!ESPERADO[@]}"; do
    h=$(sha256sum "$K/$f" | cut -d' ' -f1)
    if [ "$h" = "${ESPERADO[$f]}" ]; then echo "  OK   $f"
    else echo "  DIFIERE $f  (la VM tiene cambios que no estan en Git). Abortando."; exit 2; fi
  done
else
  echo "  (no hay arbol del kernel, se omite la verificacion)"
fi

echo; echo "== Respaldo de la .config con la que se compilo P4"
mkdir -p "$HOME/SO2/recursos/configs"
if [ -f "$K/.config" ]; then
  DEST="$HOME/SO2/recursos/configs/config-6.12.69-p4-compilada"
  [ -f "$DEST" ] && echo "  ya existe $DEST" || { cp "$K/.config" "$DEST"; echo "  guardada $DEST"; }
fi
[ -f "$PU_CONFIG_BASE" ] && echo "  config base presente: $PU_CONFIG_BASE" || { echo "  FALTA $PU_CONFIG_BASE. Abortando."; exit 3; }
[ -f "$PU_TARBALL" ] && echo "  tarball presente:      $PU_TARBALL" || { echo "  FALTA $PU_TARBALL. Abortando."; exit 3; }

echo
if [ $EJECUTAR -eq 0 ]; then
  echo "SIMULACION: se borraria $W. Para borrar de verdad: $0 --ejecutar"
  exit 0
fi
rm -rf -- "$W"
echo "Borrado $W"; df -h / | tail -1
