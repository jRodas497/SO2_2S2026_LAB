// SPDX-License-Identifier: GPL-2.0
/*
 * Sensor eBPF de archivos.
 *
 * Unica logica permitida en el kernel: filtro por ruta. Los metadatos de cada
 * archivo abierto para escritura dentro del directorio configurado (sin
 * subdirectorios) viajan al daemon por un ring buffer. Hash, severidad y
 * cuarentena los decide el daemon.
 *
 * Funcionamiento:
 *   1. sys_exit_openat (y open/creat/openat2): con el fd devuelto se obtiene el
 *      struct file, se resuelve su ruta real recorriendo los dentries y, si cae
 *      directamente en el directorio configurado y se abrio para escritura, se
 *      recuerda en el mapa "watched" usando el puntero al struct file como clave.
 *   2. kprobe security_file_free: el kernel libera el struct file cuando se cerro
 *      el ULTIMO descriptor que lo referenciaba (dup, fork y redirecciones de
 *      bash incluidos). En ese momento se emite el evento: el archivo ya esta
 *      completo y el daemon puede calcular su hash.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include "sensor.h"

char LICENSE[] SEC("license") = "GPL";

#define O_ACCMODE_MASK 3        /* O_WRONLY=1, O_RDWR=2 */
#define MAX_FD         (1 << 20)

/* offsetof(struct mount, mnt) resuelto con CO-RE */
#define MNT_OFF ((long)__builtin_preserve_field_info(((struct mount *)0)->mnt, BPF_FIELD_BYTE_OFFSET))

struct watch_val {
	__u32 pid;
	__u32 tid;
	__u32 uid;
	__u32 flags;
	__u32 gen;
	char comm[SENSOR_COMM_LEN];
	char name[SENSOR_NAME_LEN];
};

/* Configuracion (mapa cfg_map): directorio vigilado (la actualiza el daemon). */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct sensor_cfg);
} cfg_map SEC(".maps");

/* PID del daemon: sus propios eventos se descartan (evita el bucle infinito). */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} daemon_pid SEC(".maps");

/* Archivos abiertos para escritura dentro del directorio vigilado. Clave: struct file *. */
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 4096);
	__type(key, __u64);
	__type(value, struct watch_val);
} watched SEC(".maps");

/* Buffer de trabajo (el stack de eBPF solo tiene 512 bytes). */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct watch_val);
} scratch SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} events SEC(".maps");

/* Si *d es la raiz de un montaje que no es el raiz, salta al punto de montaje. */
static __always_inline void normalize(struct dentry **d, struct mount **m)
{
#pragma unroll
	for (int i = 0; i < 3; i++) {
		struct vfsmount *v = (struct vfsmount *)((void *)*m + MNT_OFF);
		struct dentry *root = BPF_CORE_READ(v, mnt_root);
		struct mount *parent = BPF_CORE_READ(*m, mnt_parent);

		if (*d != root || parent == *m)
			return;
		*d = BPF_CORE_READ(*m, mnt_mountpoint);
		*m = parent;
	}
}

/* Sube un nivel. Devuelve 0 si ya estabamos en la raiz real. */
static __always_inline int up(struct dentry **d, struct mount **m)
{
	struct dentry *p = BPF_CORE_READ(*d, d_parent);

	if (p == *d)
		return 0;
	*d = p;
	normalize(d, m);
	return 1;
}

static __always_inline int is_real_root(struct dentry *d, struct mount *m)
{
	return BPF_CORE_READ(d, d_parent) == d && BPF_CORE_READ(m, mnt_parent) == m;
}

static __always_inline int streq(const char *a, const char *b)
{
#pragma unroll
	for (int i = 0; i < SENSOR_COMP_LEN; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

/*
 * 1 si el archivo "f" esta directamente dentro del directorio de cfg.
 * Copia el nombre del archivo en name_out.
 */
static __always_inline int path_matches(struct file *f, const struct sensor_cfg *cfg,
					char *name_out)
{
	struct dentry *d = BPF_CORE_READ(f, f_path.dentry);
	struct vfsmount *v = BPF_CORE_READ(f, f_path.mnt);
	struct mount *m = (struct mount *)((void *)v - MNT_OFF);
	const unsigned char *np;
	char buf[SENSOR_COMP_LEN];
	__u32 n = cfg->ncomp;

	if (n > SENSOR_MAX_COMPS)
		return 0;

	np = BPF_CORE_READ(d, d_name.name);
	if (bpf_probe_read_kernel_str(name_out, SENSOR_NAME_LEN, np) <= 0)
		return 0;

	for (int i = 0; i < SENSOR_MAX_COMPS; i++) {
		if (i >= n)
			break;
		if (!up(&d, &m))
			return 0;
		np = BPF_CORE_READ(d, d_name.name);
		__builtin_memset(buf, 0, sizeof(buf));
		if (bpf_probe_read_kernel_str(buf, sizeof(buf), np) <= 0)
			return 0;
		if (!streq(buf, cfg->comp[i]))
			return 0;
	}

	/* Lo que queda encima del ultimo componente debe ser la raiz "/" */
	if (!up(&d, &m))
		return 0;
	return is_real_root(d, m);
}

static __always_inline int on_open_exit(long fd)
{
	__u32 zero = 0;
	__u64 id = bpf_get_current_pid_tgid();
	__u32 tgid = id >> 32;
	__u32 *dpid;
	struct sensor_cfg *cfg;
	struct watch_val *val;
	struct task_struct *task;
	struct fdtable *fdt;
	struct file **fda;
	struct file *f = NULL;
	__u64 key;
	unsigned int fl;

	if (fd < 0 || fd >= MAX_FD)
		return 0;

	dpid = bpf_map_lookup_elem(&daemon_pid, &zero);
	if (dpid && *dpid == tgid)
		return 0;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg || cfg->gen == 0)
		return 0;

	task = (struct task_struct *)bpf_get_current_task();
	fdt = BPF_CORE_READ(task, files, fdt);
	fda = BPF_CORE_READ(fdt, fd);
	if (!fda)
		return 0;
	bpf_probe_read_kernel(&f, sizeof(f), &fda[fd]);
	if (!f)
		return 0;

	fl = BPF_CORE_READ(f, f_flags);
	if (!(fl & O_ACCMODE_MASK))     /* abierto solo para lectura: no interesa */
		return 0;

	val = bpf_map_lookup_elem(&scratch, &zero);
	if (!val)
		return 0;
	if (!path_matches(f, cfg, val->name))
		return 0;

	val->pid = tgid;
	val->tid = (__u32)id;
	val->uid = (__u32)bpf_get_current_uid_gid();
	val->flags = fl;
	val->gen = cfg->gen;
	bpf_get_current_comm(val->comm, sizeof(val->comm));

	key = (__u64)f;
	bpf_map_update_elem(&watched, &key, val, BPF_ANY);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int handle_openat_exit(struct trace_event_raw_sys_exit *ctx)
{
	return on_open_exit(ctx->ret);
}

SEC("tracepoint/syscalls/sys_exit_openat2")
int handle_openat2_exit(struct trace_event_raw_sys_exit *ctx)
{
	return on_open_exit(ctx->ret);
}

SEC("tracepoint/syscalls/sys_exit_open")
int handle_open_exit(struct trace_event_raw_sys_exit *ctx)
{
	return on_open_exit(ctx->ret);
}

SEC("tracepoint/syscalls/sys_exit_creat")
int handle_creat_exit(struct trace_event_raw_sys_exit *ctx)
{
	return on_open_exit(ctx->ret);
}

/* El kernel libera el struct file: se cerro el ultimo descriptor. */
SEC("kprobe/security_file_free")
int BPF_KPROBE(handle_file_free, struct file *file)
{
	__u64 key = (__u64)file;
	struct watch_val *v = bpf_map_lookup_elem(&watched, &key);
	struct sensor_event *e;

	if (!v)
		return 0;

	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (e) {
		e->pid = v->pid;
		e->tid = v->tid;
		e->uid = v->uid;
		e->flags = v->flags;
		e->gen = v->gen;
		e->pad = 0;
		__builtin_memcpy(e->comm, v->comm, sizeof(e->comm));
		__builtin_memcpy(e->name, v->name, sizeof(e->name));
		bpf_ringbuf_submit(e, 0);
	}
	bpf_map_delete_elem(&watched, &key);
	return 0;
}
