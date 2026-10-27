/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Estructuras compartidas entre el sensor eBPF (kernel) y el daemon (usuario).
 * Se incluye despues de vmlinux.h (BPF) o de <linux/types.h> (usuario).
 */
#ifndef SENSOR_H
#define SENSOR_H

#define SENSOR_MAX_COMPS  8     /* profundidad maxima del directorio vigilado */
#define SENSOR_COMP_LEN   64    /* largo maximo (con NUL) de cada componente  */
#define SENSOR_NAME_LEN   256   /* largo maximo del nombre de archivo         */
#define SENSOR_COMM_LEN   16

/*
 * Valor del mapa "cfg_map". Lo escribe el daemon con bpf_map_update_elem().
 * El directorio vigilado se guarda como lista de componentes de la HOJA a la
 * RAIZ: "/tmp/monitor" -> comp[0]="monitor", comp[1]="tmp", ncomp=2.
 * "/" se representa con ncomp=0. gen=0 significa "sin configurar".
 */
struct sensor_cfg {
	__u32 ncomp;
	__u32 gen;
	char comp[SENSOR_MAX_COMPS][SENSOR_COMP_LEN];
};

/*
 * Evento que viaja por el ring buffer: uno por archivo abierto para escritura
 * dentro del directorio vigilado, emitido cuando el kernel libera el archivo
 * (se cerro el ultimo descriptor), o sea cuando su contenido ya esta completo.
 * pid/tid/uid/comm son los del proceso que lo ABRIO.
 */
struct sensor_event {
	__u32 pid;      /* tgid del proceso */
	__u32 tid;
	__u32 uid;
	__u32 flags;    /* f_flags (el kernel ya quito O_CREAT/O_TRUNC/O_EXCL/O_NOCTTY) */
	__u32 gen;      /* generacion de la config con la que se filtro */
	__u32 pad;
	char comm[SENSOR_COMM_LEN];
	char name[SENSOR_NAME_LEN];   /* solo el nombre; el directorio lo conoce el daemon */
};

#endif /* SENSOR_H */
