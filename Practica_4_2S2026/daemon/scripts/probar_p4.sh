#!/usr/bin/env bash
# Prueba de extremo a extremo del daemon de la Practica 4 (debe estar corriendo).
#   ./scripts/probar_p4.sh [URL]      (por defecto http://127.0.0.1:8080)
# Prueba el patron event-driven: agrega una linea con la palabra clave al log vigilado
# y comprueba que el evento llega SIN refrescar nada.
set -u
BASE="${1:-http://127.0.0.1:8080}"
LOG="${LOG_VIGILADO:-/var/log/so2_app.log}"
PALABRA="${PALABRA_CLAVE:-ERROR}"
BODY=$(mktemp); SSE=$(mktemp); PID_SSE=""
trap 'rm -f "$BODY" "$SSE"; [ -n "$PID_SSE" ] && kill "$PID_SSE" 2>/dev/null' EXIT
PASS=0; FAIL=0
check() { if [ "$2" = "$3" ]; then PASS=$((PASS+1)); printf 'PASS  %s\n' "$1"; else FAIL=$((FAIL+1)); printf 'FAIL  %s  (esperado: %s, obtenido: %s)\n' "$1" "$2" "$3"; fi; }
js() { python3 -c "import json; d=json.load(open('$BODY')); print($1)" 2>/dev/null; }

echo "== API"
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/ping");  check "ping" 200 "$code"
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/datos"); check "/api/datos responde" 200 "$code"
for k in memoria_usada memoria_libre memoria_cache swap_usada procesos eventos_seguridad; do
  check "campo '$k' presente" True "$(js "'$k' in d")"
done
check "memoria total > 0" True "$(js "d['memoria_total'] > 0")"
check "usada + libre + cache = total" True "$(js "abs(d['memoria_usada']+d['memoria_libre']+d['memoria_cache']-d['memoria_total']) < 4096")"
check "tabla de procesos no vacia" True "$(js "len(d['procesos']) > 0")"
check "cada proceso tiene nombre, pid y estado" True "$(js "all({'nombre','pid','estado'} <= set(p) for p in d['procesos'])")"
echo "      origen de la tabla: $(js "d['fuente_procesos']")"

echo "== Consulta por PID (sys_get_process_info)"
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/proceso?pid=$$"); check "consulta del PID $$" 200 "$code"
check "nombre devuelto" bash "$(js "d.get('nombre')")"
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/proceso?pid=999999"); check "PID inexistente -> 400" 400 "$code"
code=$(curl -s -o "$BODY" -w '%{http_code}' "$BASE/api/proceso?pid=abc");    check "PID invalido -> 400" 400 "$code"

echo "== Evento de seguridad (log_watch -> IPC -> daemon -> SSE)"
curl -sN "$BASE/api/stream" > "$SSE" & PID_SSE=$!
sleep 1
MARCA="prueba-p4-$(date +%s)"
echo "$PALABRA $MARCA linea de prueba" >> "$LOG" 2>/dev/null || { echo "No se pudo escribir en $LOG"; }
llego=no
for _ in $(seq 1 60); do
  if grep -q "$MARCA" "$SSE"; then llego=si; break; fi
  sleep 0.1
done
check "el evento llego por SSE (sin refrescar) en <= 6 s" si "$llego"
grep -q '^event: evento' "$SSE" && ev=si || ev=no
check "llego como evento 'evento' (no como sondeo)" si "$ev"
curl -s -o "$BODY" "$BASE/api/datos"
check "queda en eventos_seguridad" True "$(js "any('$MARCA' in e['contenido'] for e in d['eventos_seguridad'])")"
check "tiene archivo, palabra, contenido y timestamp" True "$(js "all({'archivo_origen','palabra_detectada','contenido','timestamp'} <= set(e) for e in d['eventos_seguridad'])")"

echo; echo "Resultado: $PASS correctas, $FAIL fallidas"
[ $FAIL -eq 0 ]
