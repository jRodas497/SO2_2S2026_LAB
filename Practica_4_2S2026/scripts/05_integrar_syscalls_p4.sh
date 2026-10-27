#!/usr/bin/env bash
set -euo pipefail

BASE="$HOME/SO2/Practica_4_2S2026"
K="$BASE/workspace/linux-6.12.69"

P2="$BASE/referencias/Practica_2_2S2026/linux 6.12.69"
P3="$BASE/referencias/Practica_3_2S2026/linux 6.12.69"

ORIG="$BASE/backups_originales"

SYS="$K/kernel/sys.c"
SYSH="$K/include/linux/syscalls.h"
TBL="$K/arch/x86/entry/syscalls/syscall_64.tbl"

echo "=================================================="
echo " INTEGRACION P2 + P3 -> PRACTICA 4"
echo "=================================================="

for f in \
    "$ORIG/sys.c.original" \
    "$ORIG/syscalls.h.original" \
    "$ORIG/syscall_64.tbl.original" \
    "$P2/kernel/sys.c" \
    "$P3/kernel/sys.c"
do
    if [ ! -f "$f" ]; then
        echo "ERROR: falta $f"
        exit 1
    fi
done

echo
echo "Restaurando archivos limpios del kernel..."

cp "$ORIG/sys.c.original" "$SYS"
cp "$ORIG/syscalls.h.original" "$SYSH"
cp "$ORIG/syscall_64.tbl.original" "$TBL"

python3 - "$SYS" "$SYSH" "$TBL" \
    "$P2/kernel/sys.c" \
    "$P3/kernel/sys.c" <<'PY'
from pathlib import Path
import sys

sys_c = Path(sys.argv[1])
sys_h = Path(sys.argv[2])
tbl   = Path(sys.argv[3])

p2_c = Path(sys.argv[4])
p3_c = Path(sys.argv[5])


def extract_block(text, begin, end):
    try:
        start = text.index(begin)
        finish = text.index(end, start) + len(end)
    except ValueError:
        raise SystemExit(
            f"ERROR: no se encontró bloque {begin} ... {end}"
        )

    return text[start:finish]


# ============================================================
# kernel/sys.c
# ============================================================

base_sys = sys_c.read_text()
p2_text = p2_c.read_text()
p3_text = p3_c.read_text()

p2_block = extract_block(
    p2_text,
    "/* SO2_P2_BEGIN */",
    "/* SO2_P2_END */"
)

p3_block = extract_block(
    p3_text,
    "/* SO2_P3_BEGIN */",
    "/* SO2_P3_END */"
)

required_includes = [
    "#include <linux/pid.h>",
    "#include <linux/sched/signal.h>",
    "#include <linux/timekeeping.h>",

    "#include <linux/atomic.h>",
    "#include <linux/delay.h>",
    "#include <linux/err.h>",
    "#include <linux/fcntl.h>",
    "#include <linux/kthread.h>",
    "#include <linux/list.h>",
    "#include <linux/mutex.h>",
    "#include <linux/slab.h>",
    "#include <linux/string.h>",
    "#include <linux/wait.h>",
]

lines = base_sys.splitlines(keepends=True)

# Mantener SPDX como primera línea.
insert_pos = 1 if lines and "SPDX-License-Identifier" in lines[0] else 0

missing = [
    inc for inc in required_includes
    if inc not in base_sys
]

if missing:
    include_text = "".join(x + "\n" for x in missing) + "\n"
    lines.insert(insert_pos, include_text)

merged_sys = "".join(lines).rstrip()

merged_sys += "\n\n"
merged_sys += "/* =============================================== */\n"
merged_sys += "/* SO2 PRACTICA 4 - SYSCALLS DE PRACTICA 2       */\n"
merged_sys += "/* =============================================== */\n\n"
merged_sys += p2_block
merged_sys += "\n\n"
merged_sys += "/* =============================================== */\n"
merged_sys += "/* SO2 PRACTICA 4 - SYSCALLS DE PRACTICA 3       */\n"
merged_sys += "/* =============================================== */\n\n"
merged_sys += p3_block
merged_sys += "\n"

sys_c.write_text(merged_sys)


# ============================================================
# include/linux/syscalls.h
# ============================================================

base_h = sys_h.read_text()

proto_block = r'''
/* SO2_P4_SYSCALLS_BEGIN */

struct so2_process_info;
struct so2_processes_state;

asmlinkage long sys_get_process_info(
    pid_t pid,
    struct so2_process_info __user *user_info);

asmlinkage long sys_get_processes_state(
    struct so2_processes_state __user *user_state);

asmlinkage long sys_ipc_channel_send(
    const char __user *mensaje);

asmlinkage long sys_ipc_channel_receive(
    char __user *buffer);

asmlinkage long sys_start_log_watch(
    const char __user * const __user *rutas,
    int cantidad,
    const char __user *ruta_log_central,
    const char __user *palabra_clave);

asmlinkage long sys_stop_log_watch(
    int id_monitoreo);

/* SO2_P4_SYSCALLS_END */
'''

last_endif = base_h.rfind("#endif")

if last_endif == -1:
    raise SystemExit("ERROR: no se encontró #endif en syscalls.h")

merged_h = (
    base_h[:last_endif].rstrip()
    + "\n\n"
    + proto_block.strip()
    + "\n\n"
    + base_h[last_endif:]
)

sys_h.write_text(merged_h)


# ============================================================
# syscall_64.tbl
# ============================================================

base_tbl = tbl.read_text().rstrip()

entries = r'''
548	common	get_process_info		sys_get_process_info
549	common	get_processes_state		sys_get_processes_state
550	common	ipc_channel_send		sys_ipc_channel_send
551	common	ipc_channel_receive		sys_ipc_channel_receive
552	common	start_log_watch			sys_start_log_watch
553	common	stop_log_watch			sys_stop_log_watch
'''

tbl.write_text(base_tbl + "\n" + entries.lstrip())


print("Integración completada.")
PY


echo
echo "=================================================="
echo " VERIFICACION"
echo "=================================================="

echo
echo "=== Syscalls 548-553 ==="
grep -nE '^(548|549|550|551|552|553)[[:space:]]' "$TBL"

echo
echo "=== Bloques integrados ==="
grep -nE \
'SO2_P2_BEGIN|SO2_P2_END|SO2_P3_BEGIN|SO2_P3_END' \
"$SYS"

echo
echo "=== Implementaciones ==="
grep -nE \
'SYSCALL_DEFINE[0-9]\((get_process_info|get_processes_state|ipc_channel_send|ipc_channel_receive|start_log_watch|stop_log_watch)' \
"$SYS"

echo
echo "=== Prototipos ==="
grep -nE \
'sys_get_process_info|sys_get_processes_state|sys_ipc_channel_send|sys_ipc_channel_receive|sys_start_log_watch|sys_stop_log_watch' \
"$SYSH"

echo
echo "INTEGRACION BASE P4 LISTA."
echo "Todavia falta adaptar log_watch -> IPC."
