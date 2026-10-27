#!/usr/bin/env bash
set -euo pipefail

PRACTICA_DIR="${HOME}/SO2/Practica_3_2S2026"
KERNEL_DIR="${PRACTICA_DIR}/workspace/linux-6.12.69"
DELIVERY_KERNEL="${PRACTICA_DIR}/linux 6.12.69"
PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SYS_SEND=548
SYS_RECEIVE=549
SYS_START=550
SYS_STOP=551

if [ ! -d "$KERNEL_DIR" ]; then
    echo "ERROR: no existe $KERNEL_DIR" >&2
    exit 1
fi

cd "$KERNEL_DIR"

for f in kernel/sys.c include/linux/syscalls.h arch/x86/entry/syscalls/syscall_64.tbl; do
    if [ ! -f "$f" ]; then
        echo "ERROR: falta $KERNEL_DIR/$f" >&2
        exit 1
    fi
done

if grep -q "SO2_P3_BEGIN" kernel/sys.c; then
    echo "ERROR: la implementacion de Practica 3 ya fue aplicada." >&2
    exit 1
fi

if grep -qE "^(${SYS_SEND}|${SYS_RECEIVE}|${SYS_START}|${SYS_STOP})[[:space:]]" \
    arch/x86/entry/syscalls/syscall_64.tbl; then
    echo "ERROR: una de las syscalls 548-551 ya esta ocupada." >&2
    grep -nE "^(${SYS_SEND}|${SYS_RECEIVE}|${SYS_START}|${SYS_STOP})[[:space:]]" \
        arch/x86/entry/syscalls/syscall_64.tbl || true
    exit 1
fi

mkdir -p "${PRACTICA_DIR}/backups_originales"
cp kernel/sys.c "${PRACTICA_DIR}/backups_originales/sys.c.original"
cp include/linux/syscalls.h "${PRACTICA_DIR}/backups_originales/syscalls.h.original"
cp arch/x86/entry/syscalls/syscall_64.tbl \
   "${PRACTICA_DIR}/backups_originales/syscall_64.tbl.original"

python3 - "$KERNEL_DIR" <<'PY'
from pathlib import Path
import sys

kernel = Path(sys.argv[1])

# -------------------------------------------------------------------
# kernel/sys.c
# -------------------------------------------------------------------
sys_c = kernel / "kernel/sys.c"
text = sys_c.read_text()

includes = [
    "#include <linux/atomic.h>",
    "#include <linux/delay.h>",
    "#include <linux/err.h>",
    "#include <linux/fcntl.h>",
    "#include <linux/fs.h>",
    "#include <linux/kthread.h>",
    "#include <linux/list.h>",
    "#include <linux/mutex.h>",
    "#include <linux/slab.h>",
    "#include <linux/string.h>",
    "#include <linux/uaccess.h>",
    "#include <linux/wait.h>",
]

lines = text.splitlines()
insert_at = 1 if lines and lines[0].startswith("// SPDX-License-Identifier") else 0
new_includes = [inc for inc in includes if inc not in text]

if new_includes:
    lines[insert_at:insert_at] = new_includes + [""]
    text = "\n".join(lines) + "\n"

impl = r'''
/* SO2_P3_BEGIN */

#define SO2_IPC_QUEUE_SIZE       16
#define SO2_IPC_MSG_SIZE         256

#define SO2_MAX_LOG_FILES        8
#define SO2_MAX_PATH             256
#define SO2_MAX_KEYWORD          64
#define SO2_LOG_READ_CHUNK       1024
#define SO2_LOG_LINE_MAX         1024
#define SO2_LOG_POLL_MS          1000

/* ================================================================
 * 1. Canal IPC bloqueante
 * ================================================================ */

static char so2_ipc_queue[SO2_IPC_QUEUE_SIZE][SO2_IPC_MSG_SIZE];
static size_t so2_ipc_lengths[SO2_IPC_QUEUE_SIZE];
static unsigned int so2_ipc_head;
static unsigned int so2_ipc_tail;
static unsigned int so2_ipc_count;

static DEFINE_MUTEX(so2_ipc_mutex);
static DECLARE_WAIT_QUEUE_HEAD(so2_ipc_wait_queue);

SYSCALL_DEFINE1(ipc_channel_send, const char __user *, mensaje)
{
	char local[SO2_IPC_MSG_SIZE] = { 0 };
	long user_len;

	if (!mensaje)
		return -EINVAL;

	/*
	 * strnlen_user() incluye el byte NUL cuando lo encuentra.
	 * Si devuelve un valor mayor al maximo, el mensaje no cabe.
	 */
	user_len = strnlen_user(mensaje, SO2_IPC_MSG_SIZE);

	if (user_len == 0)
		return -EFAULT;

	if (user_len > SO2_IPC_MSG_SIZE)
		return -EMSGSIZE;

	if (copy_from_user(local, mensaje, user_len))
		return -EFAULT;

	if (mutex_lock_interruptible(&so2_ipc_mutex))
		return -ERESTARTSYS;

	if (so2_ipc_count == SO2_IPC_QUEUE_SIZE) {
		mutex_unlock(&so2_ipc_mutex);
		return -ENOSPC;
	}

	memcpy(so2_ipc_queue[so2_ipc_tail], local, user_len);
	so2_ipc_lengths[so2_ipc_tail] = user_len;

	so2_ipc_tail = (so2_ipc_tail + 1) % SO2_IPC_QUEUE_SIZE;
	so2_ipc_count++;

	pr_info("SO2 P3: ipc_send queued=%u message=\"%s\"\n",
		so2_ipc_count, local);

	mutex_unlock(&so2_ipc_mutex);

	/* Despierta a un receptor que este bloqueado. */
	wake_up_interruptible(&so2_ipc_wait_queue);

	return 0;
}

SYSCALL_DEFINE1(ipc_channel_receive, char __user *, buffer)
{
	size_t len;
	int ret;

	if (!buffer)
		return -EINVAL;

	for (;;) {
		/*
		 * No hay polling: el proceso duerme hasta que la condicion
		 * se vuelve verdadera o recibe una senal.
		 */
		ret = wait_event_interruptible(
			so2_ipc_wait_queue,
			READ_ONCE(so2_ipc_count) > 0
		);

		if (ret)
			return ret;

		if (mutex_lock_interruptible(&so2_ipc_mutex))
			return -ERESTARTSYS;

		/*
		 * Otro receptor pudo consumir el elemento antes de que
		 * obtuvieramos el mutex. En ese caso volvemos a dormir.
		 */
		if (so2_ipc_count == 0) {
			mutex_unlock(&so2_ipc_mutex);
			continue;
		}

		len = so2_ipc_lengths[so2_ipc_head];

		/*
		 * Solo retiramos el mensaje despues de copiarlo con exito.
		 * El buffer de usuario debe tener al menos 256 bytes.
		 */
		if (copy_to_user(buffer, so2_ipc_queue[so2_ipc_head], len)) {
			mutex_unlock(&so2_ipc_mutex);
			return -EFAULT;
		}

		pr_info("SO2 P3: ipc_receive queued_before=%u message=\"%s\"\n",
			so2_ipc_count, so2_ipc_queue[so2_ipc_head]);

		memset(so2_ipc_queue[so2_ipc_head], 0, SO2_IPC_MSG_SIZE);
		so2_ipc_lengths[so2_ipc_head] = 0;
		so2_ipc_head = (so2_ipc_head + 1) % SO2_IPC_QUEUE_SIZE;
		so2_ipc_count--;

		mutex_unlock(&so2_ipc_mutex);

		/* Retorna la cantidad de bytes utiles, sin contar NUL. */
		return len > 0 ? (long)(len - 1) : 0;
	}
}

/* ================================================================
 * 2. Monitoreo de logs con kthread
 * ================================================================ */

struct so2_log_watch {
	int id;
	int cantidad;
	char rutas[SO2_MAX_LOG_FILES][SO2_MAX_PATH];
	loff_t posiciones[SO2_MAX_LOG_FILES];

	/* Conserva una linea incompleta entre lecturas periodicas. */
	char parcial[SO2_MAX_LOG_FILES][SO2_LOG_LINE_MAX];
	size_t parcial_len[SO2_MAX_LOG_FILES];

	char ruta_central[SO2_MAX_PATH];
	char palabra[SO2_MAX_KEYWORD];

	struct task_struct *thread;
	struct list_head list;
};

static LIST_HEAD(so2_log_watch_list);
static DEFINE_MUTEX(so2_log_watch_mutex);
static atomic_t so2_log_watch_next_id = ATOMIC_INIT(0);

static int so2_copy_user_string(char *dst, size_t dst_size,
				const char __user *src)
{
	long len;

	if (!src || dst_size == 0)
		return -EINVAL;

	len = strnlen_user(src, dst_size);

	if (len == 0)
		return -EFAULT;

	if (len > dst_size)
		return -ENAMETOOLONG;

	if (copy_from_user(dst, src, len))
		return -EFAULT;

	dst[dst_size - 1] = '\0';
	return 0;
}

static void so2_log_write_match(struct so2_log_watch *watch,
				int file_index,
				const char *line)
{
	struct file *central;
	char entry[SO2_LOG_LINE_MAX + SO2_MAX_PATH +
		   SO2_MAX_KEYWORD + 96];
	loff_t pos = 0;
	int len;

	central = filp_open(watch->ruta_central,
			    O_WRONLY | O_CREAT | O_APPEND,
			    0644);

	if (IS_ERR(central)) {
		pr_err("SO2 P3: watch=%d no pudo abrir log central %s: %ld\n",
		       watch->id, watch->ruta_central, PTR_ERR(central));
		return;
	}

	len = scnprintf(entry, sizeof(entry),
			"[watch=%d] archivo=%s palabra=%s linea=%s\n",
			watch->id,
			watch->rutas[file_index],
			watch->palabra,
			line);

	if (kernel_write(central, entry, len, &pos) < 0)
		pr_err("SO2 P3: watch=%d fallo al escribir log central\n",
		       watch->id);
	else
		pr_info("SO2 P3: watch=%d match file=%s keyword=%s line=\"%s\"\n",
			watch->id,
			watch->rutas[file_index],
			watch->palabra,
			line);

	filp_close(central, NULL);
}

static void so2_log_process_bytes(struct so2_log_watch *watch,
				  int file_index,
				  const char *data,
				  size_t data_len)
{
	size_t i;

	for (i = 0; i < data_len; ++i) {
		char c = data[i];
		size_t *plen = &watch->parcial_len[file_index];
		char *line = watch->parcial[file_index];

		if (c == '\n') {
			line[*plen] = '\0';

			if (strstr(line, watch->palabra))
				so2_log_write_match(watch, file_index, line);

			*plen = 0;
			line[0] = '\0';
			continue;
		}

		if (*plen < SO2_LOG_LINE_MAX - 1) {
			line[*plen] = c;
			(*plen)++;
			line[*plen] = '\0';
		} else {
			/* Linea demasiado larga: procesa el fragmento acumulado. */
			line[*plen] = '\0';

			if (strstr(line, watch->palabra))
				so2_log_write_match(watch, file_index, line);

			*plen = 0;
			line[0] = '\0';
		}
	}
}

static void so2_log_scan_file(struct so2_log_watch *watch, int file_index)
{
	struct file *source;
	char buffer[SO2_LOG_READ_CHUNK];
	ssize_t nread;

	source = filp_open(watch->rutas[file_index], O_RDONLY, 0);

	if (IS_ERR(source)) {
		/* El archivo puede aparecer despues; se intentara de nuevo. */
		return;
	}

	for (;;) {
		nread = kernel_read(source,
				    buffer,
				    sizeof(buffer),
				    &watch->posiciones[file_index]);

		if (nread <= 0)
			break;

		so2_log_process_bytes(watch,
				      file_index,
				      buffer,
				      (size_t)nread);

		if (kthread_should_stop())
			break;
	}

	filp_close(source, NULL);
}

static int so2_log_watch_thread(void *data)
{
	struct so2_log_watch *watch = data;
	int i;

	pr_info("SO2 P3: log_watch id=%d thread started\n", watch->id);

	while (!kthread_should_stop()) {
		for (i = 0; i < watch->cantidad; ++i) {
			so2_log_scan_file(watch, i);

			if (kthread_should_stop())
				break;
		}

		if (!kthread_should_stop())
			msleep_interruptible(SO2_LOG_POLL_MS);
	}

	pr_info("SO2 P3: log_watch id=%d thread stopped\n", watch->id);
	return 0;
}

SYSCALL_DEFINE4(start_log_watch,
		const char __user * __user *, rutas,
		int, cantidad,
		const char __user *, ruta_log_central,
		const char __user *, palabra_clave)
{
	struct so2_log_watch *watch;
	struct task_struct *thread;
	int i;
	int ret;

	if (!rutas || !ruta_log_central || !palabra_clave)
		return -EINVAL;

	if (cantidad <= 0 || cantidad > SO2_MAX_LOG_FILES)
		return -EINVAL;

	watch = kzalloc(sizeof(*watch), GFP_KERNEL);
	if (!watch)
		return -ENOMEM;

	for (i = 0; i < cantidad; ++i) {
		const char __user *user_path = NULL;

		if (copy_from_user(&user_path,
				   &rutas[i],
				   sizeof(user_path))) {
			ret = -EFAULT;
			goto error_free;
		}

		ret = so2_copy_user_string(watch->rutas[i],
					   sizeof(watch->rutas[i]),
					   user_path);
		if (ret)
			goto error_free;
	}

	ret = so2_copy_user_string(watch->ruta_central,
				   sizeof(watch->ruta_central),
				   ruta_log_central);
	if (ret)
		goto error_free;

	ret = so2_copy_user_string(watch->palabra,
				   sizeof(watch->palabra),
				   palabra_clave);
	if (ret)
		goto error_free;

	watch->cantidad = cantidad;
	watch->id = atomic_inc_return(&so2_log_watch_next_id);
	INIT_LIST_HEAD(&watch->list);

	thread = kthread_run(so2_log_watch_thread,
			     watch,
			     "so2_log_watch/%d",
			     watch->id);

	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		goto error_free;
	}

	watch->thread = thread;

	mutex_lock(&so2_log_watch_mutex);
	list_add_tail(&watch->list, &so2_log_watch_list);
	mutex_unlock(&so2_log_watch_mutex);

	pr_info("SO2 P3: start_log_watch id=%d files=%d central=%s keyword=%s\n",
		watch->id,
		watch->cantidad,
		watch->ruta_central,
		watch->palabra);

	return watch->id;

error_free:
	kfree(watch);
	return ret;
}

SYSCALL_DEFINE1(stop_log_watch, int, id_monitoreo)
{
	struct so2_log_watch *watch;
	struct so2_log_watch *tmp;
	struct so2_log_watch *found = NULL;
	int ret;

	if (id_monitoreo <= 0)
		return -EINVAL;

	mutex_lock(&so2_log_watch_mutex);

	list_for_each_entry_safe(watch, tmp,
				 &so2_log_watch_list, list) {
		if (watch->id == id_monitoreo) {
			list_del_init(&watch->list);
			found = watch;
			break;
		}
	}

	mutex_unlock(&so2_log_watch_mutex);

	if (!found)
		return -ENOENT;

	ret = kthread_stop(found->thread);

	pr_info("SO2 P3: stop_log_watch id=%d result=%d\n",
		found->id, ret);

	kfree(found);
	return ret < 0 ? ret : 0;
}

/* SO2_P3_END */
'''

if not text.endswith("\n"):
    text += "\n"
text += impl
sys_c.write_text(text)

# -------------------------------------------------------------------
# include/linux/syscalls.h
# -------------------------------------------------------------------
syscalls_h = kernel / "include/linux/syscalls.h"
text = syscalls_h.read_text()

block = r'''
/* SO2_P3_SYSCALLS_BEGIN */
asmlinkage long sys_ipc_channel_send(const char __user *mensaje);
asmlinkage long sys_ipc_channel_receive(char __user *buffer);
asmlinkage long sys_start_log_watch(
	const char __user * const __user *rutas,
	int cantidad,
	const char __user *ruta_log_central,
	const char __user *palabra_clave);
asmlinkage long sys_stop_log_watch(int id_monitoreo);
/* SO2_P3_SYSCALLS_END */

'''

pos = text.rfind("#endif")
if pos == -1:
    raise SystemExit("No se encontro #endif final en syscalls.h")

text = text[:pos] + block + text[pos:]
syscalls_h.write_text(text)

# -------------------------------------------------------------------
# syscall_64.tbl
# -------------------------------------------------------------------
table = kernel / "arch/x86/entry/syscalls/syscall_64.tbl"
text = table.read_text()

if not text.endswith("\n"):
    text += "\n"

text += (
    "548\tcommon\tipc_channel_send\t\tsys_ipc_channel_send\n"
    "549\tcommon\tipc_channel_receive\t\tsys_ipc_channel_receive\n"
    "550\tcommon\tstart_log_watch\t\t\tsys_start_log_watch\n"
    "551\tcommon\tstop_log_watch\t\t\tsys_stop_log_watch\n"
)

table.write_text(text)
PY

mkdir -p "${DELIVERY_KERNEL}/kernel"
mkdir -p "${DELIVERY_KERNEL}/include/linux"
mkdir -p "${DELIVERY_KERNEL}/arch/x86/entry/syscalls"
mkdir -p "${PRACTICA_DIR}/Programa_Intermedio"

cp kernel/sys.c "${DELIVERY_KERNEL}/kernel/sys.c"
cp include/linux/syscalls.h "${DELIVERY_KERNEL}/include/linux/syscalls.h"
cp arch/x86/entry/syscalls/syscall_64.tbl \
   "${DELIVERY_KERNEL}/arch/x86/entry/syscalls/syscall_64.tbl"

cp "${PACKAGE_DIR}/Programa_Intermedio/"*.c \
   "${PRACTICA_DIR}/Programa_Intermedio/"

echo
echo "============================================================"
echo " PRACTICA 3 APLICADA"
echo "============================================================"
echo
echo "Syscalls:"
grep -nE '^(548|549|550|551)[[:space:]]' \
    arch/x86/entry/syscalls/syscall_64.tbl

echo
echo "Implementaciones:"
grep -nE 'SYSCALL_DEFINE[0-9]\((ipc_channel|start_log_watch|stop_log_watch)' \
    kernel/sys.c

echo
echo "Archivos de usuario:"
find "${PRACTICA_DIR}/Programa_Intermedio" -maxdepth 1 -type f -name '*.c' | sort

echo
echo "Archivos del kernel para entrega:"
find "$DELIVERY_KERNEL" -type f | sort
