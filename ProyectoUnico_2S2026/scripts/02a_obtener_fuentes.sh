#!/usr/bin/env bash
# Descarga Linux 6.12.69 de kernel.org, verifica su SHA-256 y guarda la configuracion
# base (la del kernel generico de Ubuntu instalado en esta maquina).
# Ejecutar despues de 02_dependencias.sh (necesita curl).
set -euo pipefail
source "$(dirname "$0")/env.sh"

URL="https://cdn.kernel.org/pub/linux/kernel/v6.x"
ARCHIVO="linux-6.12.69.tar.xz"
mkdir -p "$PU_RECURSOS/configs"

echo "== 1/3 Tarball del kernel"
if [ -f "$PU_TARBALL" ]; then
  echo "Ya existe $PU_TARBALL (se verifica igualmente)"
else
  curl -fL --retry 3 --progress-bar -o "$PU_TARBALL.part" "$URL/$ARCHIVO"
  mv "$PU_TARBALL.part" "$PU_TARBALL"
fi

echo "== 2/3 Verificacion SHA-256 (kernel.org)"
esperado=$(curl -fsS --retry 3 "$URL/sha256sums.asc" | awk -v f="$ARCHIVO" '$2 == f { print $1 }')
[ -n "$esperado" ] || { echo "No se pudo obtener el SHA-256 de $ARCHIVO desde kernel.org"; exit 1; }
real=$(sha256sum "$PU_TARBALL" | cut -d' ' -f1)
if [ "$real" != "$esperado" ]; then
  echo "SHA-256 DISTINTO. Esperado: $esperado"
  echo "                  Obtenido: $real"
  echo "Borra $PU_TARBALL y vuelve a ejecutar este script."
  exit 1
fi
echo "OK  $real"

echo "== 3/3 Configuracion base"
if [ -f "$PU_CONFIG_BASE" ]; then
  echo "Ya existe $PU_CONFIG_BASE"
else
  # Se prefiere la config del kernel generico (no la de un kernel personalizado previo)
  origen="/boot/config-$(uname -r)"
  case "$(uname -r)" in
    *-generic) ;;
    *) ultimo=$(ls -1 /boot/config-*-generic 2>/dev/null | sort -V | tail -1 || true)
       [ -n "$ultimo" ] && origen="$ultimo" ;;
  esac
  [ -f "$origen" ] || { echo "No hay $origen. Copia manualmente una config a $PU_CONFIG_BASE"; exit 1; }
  cp "$origen" "$PU_CONFIG_BASE"
  echo "Copiada $origen -> $PU_CONFIG_BASE"
fi
echo; echo "Listo. Siguiente: ./03_preparar_arbol.sh"
