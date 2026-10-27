#!/usr/bin/env bash
# Crea un usuario del sistema con useradd y lo agrega al grupo del rol.
#   sudo ./crear_usuario.sh <usuario> <admin|common>
# El daemon lee los grupos en cada login, no hace falta reiniciarlo.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Ejecuta con sudo"; exit 1; }
[ $# -eq 2 ] || { echo "Uso: $0 <usuario> <admin|common>"; exit 1; }

user="$1"; rol="$2"
case "$rol" in
  admin)  grupo=admin_user ;;
  common) grupo=common_user ;;
  *) echo "El rol debe ser 'admin' o 'common'"; exit 1 ;;
esac
getent group "$grupo" >/dev/null || { echo "Falta el grupo $grupo: corre instalar_sistema.sh"; exit 1; }

if id "$user" >/dev/null 2>&1; then
  usermod -aG "$grupo" "$user"
  echo "El usuario $user ya existia: agregado al grupo $grupo"
else
  useradd -m -s /bin/bash -G "$grupo" "$user"
  echo "Usuario $user creado en el grupo $grupo. Define su contraseña:"
  passwd "$user"
fi
id "$user"
