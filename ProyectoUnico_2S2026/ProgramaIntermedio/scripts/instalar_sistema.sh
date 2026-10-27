#!/usr/bin/env bash
# Prepara el sistema para el daemon (ejecutar UNA vez, con sudo):
#   - grupos admin_user y common_user
#   - servicio PAM "so2-monitor" (/etc/pam.d/so2-monitor)
#   - directorio vigilado y logs de ejemplo para log_watch
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Ejecuta con sudo: sudo $0"; exit 1; }

for g in admin_user common_user; do
  getent group "$g" >/dev/null || { groupadd "$g"; echo "grupo creado: $g"; }
done

cat > /etc/pam.d/so2-monitor <<'PAM'
# Servicio PAM del dashboard so2monitor: autentica con las cuentas del sistema.
@include common-auth
@include common-account
PAM
chmod 644 /etc/pam.d/so2-monitor
echo "servicio PAM instalado: /etc/pam.d/so2-monitor"

mkdir -p /tmp/monitor
chmod 1777 /tmp/monitor
touch /var/log/so2_app.log /var/log/so2_central.log
chmod 666 /var/log/so2_app.log
chmod 644 /var/log/so2_central.log
echo "directorio vigilado: /tmp/monitor"
echo "logs de ejemplo:     /var/log/so2_app.log (vigilado)  /var/log/so2_central.log (central)"
echo
echo "Crea usuarios con:  sudo $(dirname "$0")/crear_usuario.sh <usuario> <admin|common>"
