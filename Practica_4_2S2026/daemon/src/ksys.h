/*
 * Interfaz con las syscalls del kernel de la Practica 4 (548-553).
 *
 * Nota de compatibilidad: en el kernel de la P4 sys_get_processes_state recibe
 * UN argumento y devuelve solo contadores (total/running/sleeping/zombie). El
 * daemon la invoca con tres argumentos: el kernel de la P4 ignora los dos
 * ultimos (cuenta queda en 0 y el daemon arma la lista por otra via); un
 * kernel mas nuevo, que si devuelve la lista por proceso, la rellena.
 */
#ifndef KSYS_H
#define KSYS_H

#include <stdint.h>

#define SYS_GET_PROCESS_INFO    548
#define SYS_GET_PROCESSES_STATE 549
#define SYS_IPC_CHANNEL_SEND    550
#define SYS_IPC_CHANNEL_RECEIVE 551
#define SYS_START_LOG_WATCH     552
#define SYS_STOP_LOG_WATCH      553

#define KS_COMM_LEN   16
#define KS_MAX_PROCS  4096
#define KS_IPC_MSGLEN 256    /* el buffer de ipc_channel_receive debe tener >= 256 */

struct ks_process_info {
	char name[KS_COMM_LEN];
	int pid;
	uint64_t runtime_ns;
	unsigned long memory_kb;
};

struct ks_process_entry {
	int pid;
	char state;
	char name[KS_COMM_LEN];
	unsigned long rss_kb;
};

struct ks_processes_state {
	unsigned long total;
	unsigned long running;
	unsigned long sleeping;
	unsigned long zombie;
	unsigned long count;     /* lo escribe solo el kernel nuevo; en el de la P4 queda en 0 */
};

#endif
