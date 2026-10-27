#!/usr/bin/env bash
set -euo pipefail

BASE="$HOME/SO2/Practica_4_2S2026"
K="$BASE/workspace/linux-6.12.69"
SYS="$K/kernel/sys.c"
BACKUP="$BASE/backups_integracion_base"

mkdir -p "$BACKUP"

if [ ! -f "$BACKUP/sys.c.antes_logwatch_ipc" ]; then
    cp "$SYS" "$BACKUP/sys.c.antes_logwatch_ipc"
fi

python3 - "$SYS" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

if "/* SO2_P4_IPC_INTERNAL_BEGIN */" in text:
    raise SystemExit("ERROR: la integracion log_watch -> IPC ya fue aplicada.")

# ============================================================
# 1. Reemplazar ipc_channel_send por helper interno + syscall
# ============================================================

send_start = text.index(
    "SYSCALL_DEFINE1(ipc_channel_send, const char __user *, mensaje)"
)

receive_start = text.index(
    "SYSCALL_DEFINE1(ipc_channel_receive, char __user *, buffer)",
    send_start
)

new_send = r'''/* SO2_P4_IPC_INTERNAL_BEGIN */

/*
 * Inserta en la cola IPC un mensaje que ya se encuentra
 * en espacio de kernel.
 *
 * Esta funcion permite reutilizar la misma cola tanto desde
 * sys_ipc_channel_send() como desde el kthread de log_watch,
 * sin utilizar copy_from_user() sobre memoria del kernel.
 */
static int so2_ipc_enqueue_kernel(const char *mensaje, size_t len)
{
	int ret = 0;

	if (!mensaje || len == 0)
		return -EINVAL;

	if (len > SO2_IPC_MSG_SIZE)
		return -EMSGSIZE;

	/*
	 * Todos los mensajes almacenados en la cola deben incluir
	 * terminador NUL.
	 */
	if (mensaje[len - 1] != '\0')
		return -EINVAL;

	mutex_lock(&so2_ipc_mutex);

	if (so2_ipc_count == SO2_IPC_QUEUE_SIZE) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	memset(so2_ipc_queue[so2_ipc_tail], 0, SO2_IPC_MSG_SIZE);

	memcpy(so2_ipc_queue[so2_ipc_tail],
	       mensaje,
	       len);

	so2_ipc_lengths[so2_ipc_tail] = len;

	so2_ipc_tail =
		(so2_ipc_tail + 1) % SO2_IPC_QUEUE_SIZE;

	so2_ipc_count++;

	pr_info("SO2 P4: ipc_enqueue queued=%u message=\"%s\"\n",
		so2_ipc_count,
		mensaje);

out_unlock:
	mutex_unlock(&so2_ipc_mutex);

	if (!ret)
		wake_up_interruptible(&so2_ipc_wait_queue);

	return ret;
}


/*
 * Entrada desde espacio de usuario.
 * copy_from_user() sigue realizandose aqui.
 */
SYSCALL_DEFINE1(ipc_channel_send, const char __user *, mensaje)
{
	char local[SO2_IPC_MSG_SIZE] = { 0 };
	long user_len;
	int ret;

	if (!mensaje)
		return -EINVAL;

	user_len = strnlen_user(
		mensaje,
		SO2_IPC_MSG_SIZE
	);

	if (user_len == 0)
		return -EFAULT;

	if (user_len > SO2_IPC_MSG_SIZE)
		return -EMSGSIZE;

	if (copy_from_user(
		    local,
		    mensaje,
		    user_len))
		return -EFAULT;

	ret = so2_ipc_enqueue_kernel(
		local,
		(size_t)user_len
	);

	if (ret)
		return ret;

	pr_info("SO2 P4: ipc_send desde usuario message=\"%s\"\n",
		local);

	return 0;
}

/* SO2_P4_IPC_INTERNAL_END */

'''

text = (
    text[:send_start]
    + new_send
    + text[receive_start:]
)

# ============================================================
# 2. Conectar so2_log_write_match() con la cola IPC
# ============================================================

fn_start = text.index(
    "static void so2_log_write_match"
)

fn_end = text.index(
    "static void so2_log_process_bytes",
    fn_start
)

fn = text[fn_start:fn_end]

old_decl = '''{
\tstruct file *central;
\tchar *entry;
\tsize_t entry_size;
\tloff_t pos = 0;
\tint len;
'''

new_decl = '''{
\tstruct file *central;
\tchar *entry;
\tchar event_msg[SO2_IPC_MSG_SIZE];
\tsize_t entry_size;
\tloff_t pos = 0;
\tint len;
\tint event_len;
\tint ipc_ret;

\t/*
\t * PRACTICA 4:
\t * Una coincidencia de log_watch produce inmediatamente
\t * una notificacion mediante el mismo canal IPC.
\t *
\t * El daemon permanece bloqueado en ipc_channel_receive()
\t * y despierta solamente cuando aparece un evento real.
\t */
\tevent_len = scnprintf(
\t\tevent_msg,
\t\tsizeof(event_msg),
\t\t"SECURITY|%d|%s|%s|%s",
\t\twatch->id,
\t\twatch->rutas[file_index],
\t\twatch->palabra,
\t\tline
\t);

\tipc_ret = so2_ipc_enqueue_kernel(
\t\tevent_msg,
\t\t(size_t)event_len + 1
\t);

\tif (ipc_ret) {
\t\tpr_warn(
\t\t\t"SO2 P4: watch=%d no pudo notificar evento IPC: %d\\n",
\t\t\twatch->id,
\t\t\tipc_ret
\t\t);
\t} else {
\t\tpr_info(
\t\t\t"SO2 P4: watch=%d evento de seguridad notificado via IPC\\n",
\t\t\twatch->id
\t\t);
\t}
'''

if old_decl not in fn:
    raise SystemExit(
        "ERROR: no se encontro el bloque esperado "
        "de so2_log_write_match()."
    )

fn = fn.replace(old_decl, new_decl, 1)

text = text[:fn_start] + fn + text[fn_end:]

path.write_text(text)

print("Conexion log_watch -> IPC aplicada.")
PY

echo
echo "=================================================="
echo " VERIFICACION P4 EVENT-DRIVEN"
echo "=================================================="

echo
echo "=== Helper interno IPC ==="
grep -n \
"so2_ipc_enqueue_kernel" \
"$SYS"

echo
echo "=== Integracion log_watch ==="
grep -nE \
'SECURITY\||evento de seguridad notificado|no pudo notificar evento IPC' \
"$SYS"

echo
echo "=== Syscalls existentes ==="
grep -nE \
'SYSCALL_DEFINE[0-9]\((get_process_info|get_processes_state|ipc_channel_send|ipc_channel_receive|start_log_watch|stop_log_watch)' \
"$SYS"

echo
echo "P4 log_watch -> IPC aplicado."

