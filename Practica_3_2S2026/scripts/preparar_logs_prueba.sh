#!/usr/bin/env bash
set -euo pipefail

BASE="${HOME}/SO2/Practica_3_2S2026/pruebas_logs"
mkdir -p "$BASE"

printf 'INFO inicio de app1\n' > "$BASE/app1.log"
printf 'INFO inicio de app2\n' > "$BASE/app2.log"
: > "$BASE/central.log"

echo "Logs de prueba preparados:"
ls -lh "$BASE"
echo
echo "Rutas absolutas:"
realpath "$BASE/app1.log"
realpath "$BASE/app2.log"
realpath "$BASE/central.log"
