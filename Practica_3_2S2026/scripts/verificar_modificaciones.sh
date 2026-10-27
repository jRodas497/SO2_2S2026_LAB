#!/usr/bin/env bash
set -euo pipefail

KERNEL_DIR="${HOME}/SO2/Practica_3_2S2026/workspace/linux-6.12.69"
cd "$KERNEL_DIR"

echo "=== KERNEL ==="
make kernelversion

echo
echo "=== SYSCALLS 548-551 ==="
grep -nE '^(548|549|550|551)[[:space:]]' arch/x86/entry/syscalls/syscall_64.tbl

echo
echo "=== IMPLEMENTACIONES ==="
grep -nE 'SYSCALL_DEFINE[0-9]\((ipc_channel|start_log_watch|stop_log_watch)' kernel/sys.c

echo
echo "=== PROTOTIPOS ==="
grep -nE 'sys_(ipc_channel|start_log_watch|stop_log_watch)' include/linux/syscalls.h

echo
echo "=== IPC: copy_from_user / copy_to_user ==="
grep -nE 'copy_from_user|copy_to_user' kernel/sys.c | tail -n 20

echo
echo "=== IPC: wait queue ==="
grep -nE 'wait_event_interruptible|wake_up_interruptible' kernel/sys.c

echo
echo "=== SINCRONIZACION ==="
grep -nE 'DEFINE_MUTEX|mutex_lock|mutex_unlock' kernel/sys.c | tail -n 25

echo
echo "=== KTHREAD ==="
grep -nE 'kthread_run|kthread_should_stop|kthread_stop' kernel/sys.c

echo
echo "=== ACCESO A LOGS ==="
grep -nE 'filp_open|kernel_read|kernel_write' kernel/sys.c
