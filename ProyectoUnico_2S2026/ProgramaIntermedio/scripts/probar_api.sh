#!/usr/bin/env bash
# Prueba de extremo a extremo del backend con curl (el daemon debe estar corriendo).
# Pide un usuario administrador y uno comun (creados con crear_usuario.sh).
#   ./probar_api.sh
set -u
BASE="${BASE:-http://127.0.0.1:8080}"
HERE="$(cd "$(dirname "$0")" && pwd)"
MUESTRAS="$HERE/../../Drive/Archivos de Prueba"
MON=/tmp/monitor
ADM=$(mktemp); COM=$(mktemp); BODY=$(mktemp)
trap 'rm -f "$ADM" "$COM" "$BODY"' EXIT
PASS=0; FAIL=0

read -rp "Usuario administrador: " AU; read -rsp "Contraseña: " AP; echo
read -rp "Usuario comun:         " CU; read -rsp "Contraseña: " CP; echo

# req JAR METODO RUTA [JSON]   -> CODE y $BODY
req() {
  local jar=$1 m=$2 p=$3 d=${4:-}
  if [ -n "$d" ]; then
    CODE=$(curl -s -o "$BODY" -w '%{http_code}' -b "$jar" -c "$jar" -X "$m" -H 'Content-Type: application/json' -d "$d" "$BASE$p")
  else
    CODE=$(curl -s -o "$BODY" -w '%{http_code}' -b "$jar" -c "$jar" -X "$m" "$BASE$p")
  fi
}
js() { python3 -c "import json,sys; d=json.load(open('$BODY')); print($1)" 2>/dev/null; }
check() { # descripcion esperado obtenido
  if [ "$2" = "$3" ]; then PASS=$((PASS+1)); printf 'PASS  %s\n' "$1"
  else FAIL=$((FAIL+1)); printf 'FAIL  %s  (esperado: %s, obtenido: %s)\n' "$1" "$2" "$3"; fi
}

echo "== Sesion y roles"
req "$ADM" GET /api/ping;                     check "ping responde 200" 200 "$CODE"
req "$ADM" GET /api/state;                    check "estado sin sesion -> 401" 401 "$CODE"
req "$ADM" POST /api/login "{\"usuario\":\"$AU\",\"password\":\"incorrecta-xyz\"}"
check "login con contraseña incorrecta -> 401" 401 "$CODE"
req "$ADM" POST /api/login "{\"usuario\":\"$AU\",\"password\":\"$AP\"}"
check "login administrador -> 200" 200 "$CODE"; check "rol administrador" admin "$(js "d.get('rol')")"
req "$COM" POST /api/login "{\"usuario\":\"$CU\",\"password\":\"$CP\"}"
check "login usuario comun -> 200" 200 "$CODE"; check "rol comun" common "$(js "d.get('rol')")"
req "$ADM" GET /api/state
check "estado: monitoreo activo" True "$(js "d['flags']['monitoreo']")"
check "estado: top de procesos <= 20 y no vacio" True "$(js "0 < len(d['procesos']['top']) <= 20")"
check "estado: memoria total > 0" True "$(js "d['metricas']['mem_total_kb'] > 0")"

echo "== Permisos aplicados en el daemon (usuario comun)"
req "$COM" GET /api/state;                         check "comun ve el estado" 200 "$CODE"
req "$COM" POST /api/kill '{"pid":99999}';         check "comun NO puede matar" 403 "$CODE"
req "$COM" POST /api/filescan '{"activo":true}';   check "comun NO activa escaneo" 403 "$CODE"
req "$COM" POST /api/path '{"ruta":"/tmp"}';       check "comun NO configura ruta" 403 "$CODE"
req "$COM" POST /api/blacklist '{}';               check "comun NO sube blacklist" 403 "$CODE"
req "$COM" GET /api/quarantine;                    check "comun NO ve cuarentena" 403 "$CODE"
req "$COM" GET '/api/process?pid=1';               check "comun NO consulta PID" 403 "$CODE"
req "$COM" POST /api/logwatch/stop '{"id":1}';     check "comun NO finaliza logwatch" 403 "$CODE"

echo "== Escaneo de archivos (eBPF) y cuarentena"
rm -f "$MON"/* 2>/dev/null
req "$ADM" POST /api/path "{\"ruta\":\"$MON\"}";   check "configurar ruta" 200 "$CODE"
req "$ADM" POST /api/path '{"ruta":"relativa"}';   check "ruta relativa rechazada" 400 "$CODE"
req "$ADM" POST /api/filescan '{"activo":true}';   check "activar escaneo (attach eBPF)" 200 "$CODE"
cp "$MUESTRAS"/*.txt "$MON"/
echo "archivo inocente" > "$MON/benigno.txt"
mkdir -p "$MON/sub"; echo "x" > "$MON/sub/ignorado.txt"
sleep 3
req "$ADM" GET /api/state
check "6 amenazas detectadas" 6 "$(js "len(d['amenazas'])")"
check "7 archivos analizados (6 + benigno; sub/ ignorado)" 7 "$(js "len(d['archivos'])")"
check "benigno.txt es benigno" benigno "$(js "[a['estado'] for a in d['archivos'] if a['archivo'].endswith('/benigno.txt')][0]")"
check "6 en cuarentena automatica" 6 "$(js "len([a for a in d['archivos'] if a['estado']=='cuarentena'])")"
check "permisos 0000 en muestra" 0 "$(stat -c %a "$MON/Simulated.Worm.Spreader.txt")"
check "alerta HIGH hash malicioso" True "$(js "any(a['severidad']=='HIGH' and 'malicioso' in a['titulo'] for a in d['alertas'])")"
check "alerta HIGH cuarentena" True "$(js "any(a['severidad']=='HIGH' and 'cuarentena' in a['titulo'] for a in d['alertas'])")"
check "alerta LOW archivo modificado" True "$(js "any(a['severidad']=='LOW' and 'modificado' in a['titulo'] for a in d['alertas'])")"
req "$ADM" GET /api/quarantine;                    check "admin ve cuarentena" 6 "$(js "len(d)")"
req "$ADM" POST /api/quarantine "{\"ruta\":\"$MON/benigno.txt\"}"; check "cuarentena manual" 200 "$CODE"
check "permisos 0000 tras cuarentena manual" 0 "$(stat -c %a "$MON/benigno.txt")"
req "$ADM" POST /api/quarantine '{"ruta":"/etc/passwd"}'
check "NO se permite poner /etc/passwd en cuarentena" 400 "$CODE"
check "/etc/passwd intacto (644)" 644 "$(stat -c %a /etc/passwd)"

echo "== Blacklist"
req "$ADM" POST /api/blacklist '{"X":{"hash":"corto"}}';  check "blacklist invalida -> 400" 400 "$CODE"
req "$ADM" POST /api/blacklist '{"Prueba.Nueva":{"hash":"'"$(printf 'a%.0s' {1..64})"'","descripcion":"d"}}'
check "blacklist valida -> 200" 200 "$CODE"; check "1 firma cargada" 1 "$(js "d.get('firmas')")"
# La subida valida se PERSISTE en config/blacklist.json; la oficial se restaura desde Drive/.
req "$ADM" POST /api/blacklist "$(cat "$HERE/../../Drive/blacklist.json")"; check "blacklist oficial restaurada" 6 "$(js "d.get('firmas')")"

echo "== Procesos"
req "$ADM" GET "/api/process?pid=$$";              check "info de proceso (sys_get_process_info)" 200 "$CODE"
check "nombre del proceso" bash "$(js "d.get('nombre')")"
req "$ADM" POST /api/kill '{"pid":1}';             check "NO se mata PID 1" 400 "$CODE"
DPID=$(pgrep -x so2monitor | head -1)
req "$ADM" POST /api/kill "{\"pid\":${DPID:-0}}";  check "NO se mata el propio daemon" 400 "$CODE"
req "$ADM" POST /api/kill '{"pid":2}';             check "NO se mata un hilo del kernel (kthreadd)" 400 "$CODE"
sleep 300 & VICT=$!
req "$ADM" POST /api/kill "{\"pid\":$VICT}";       check "kill de un proceso normal" 200 "$CODE"
sleep 0.5; kill -0 $VICT 2>/dev/null && R=vivo || R=muerto; check "el proceso murio" muerto "$R"

echo "== Logwatch + canal IPC (hilo bloqueado, sin polling)"
: > /var/log/so2_app.log     # un monitoreo nuevo lee el archivo desde el inicio: partir de cero
req "$ADM" POST /api/logwatch '{"central":"/var/log/so2_central.log","palabra":"CLAVEPRUEBA","rutas":["/var/log/so2_app.log"]}'
check "crear logwatch" 200 "$CODE"; ID=$(js "d.get('id')")
echo "linea con CLAVEPRUEBA de la prueba" >> /var/log/so2_app.log
sleep 3
req "$ADM" GET /api/state
check "alerta MEDIUM por palabra clave" True "$(js "any(a['severidad']=='MEDIUM' and a['tipo']=='logs' for a in d['alertas'])")"
check "logwatch listado" True "$(js "any(l['id']==$ID for l in d['logwatches'])")"
req "$ADM" POST /api/logwatch/stop "{\"id\":$ID}";  check "finalizar logwatch" 200 "$CODE"

echo "== Interruptores"
req "$ADM" POST /api/monitoring '{"activo":false}'; check "desactivar monitoreo" 200 "$CODE"
req "$ADM" POST /api/monitoring '{"activo":true}';  check "activar monitoreo" 200 "$CODE"
req "$ADM" GET /api/state
check "alerta MEDIUM monitoreo desactivado" True "$(js "any(a['titulo'].startswith('Monitoreo del sistema desactivado') for a in d['alertas'])")"
check "alerta LOW monitoreo activado" True "$(js "any(a['titulo'].startswith('Monitoreo del sistema activado') for a in d['alertas'])")"
check "alerta LOW inicio de sesion" True "$(js "any(a['titulo'].startswith('Inicio de sesión exitoso') for a in d['alertas'])")"
check "alerta MEDIUM inicio fallido" True "$(js "any(a['titulo'].startswith('Inicio de sesión fallido') for a in d['alertas'])")"
req "$ADM" POST /api/filescan '{"activo":false}';   check "desactivar escaneo (detach eBPF)" 200 "$CODE"

echo "== Logout"
req "$ADM" POST /api/logout;                        check "logout" 200 "$CODE"
req "$ADM" GET /api/state;                          check "sesion invalida tras logout" 401 "$CODE"

echo; echo "Resultado: $PASS correctas, $FAIL fallidas"
[ $FAIL -eq 0 ]
