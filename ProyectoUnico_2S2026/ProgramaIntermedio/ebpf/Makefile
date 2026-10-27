CLANG   ?= clang
CC      ?= gcc
# bpftool real (el de /usr/sbin es un wrapper que falla con kernels personalizados)
BPFTOOL ?= $(shell ls -1 /usr/lib/linux-tools/*/bpftool 2>/dev/null | sort -V | tail -1)

BPF_CFLAGS  := -g -O2 -target bpf -D__TARGET_ARCH_x86 -I.
USER_CFLAGS := -O2 -Wall -Wextra -I.
LIBS        := -lbpf -lelf -lz

all: sensor.skel.h test_sensor

# vmlinux.h se genera con el BTF del kernel que esta corriendo.
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

sensor.bpf.o: sensor.bpf.c sensor.h vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

sensor.skel.h: sensor.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

test_sensor: test_sensor.c sensor.skel.h sensor.h
	$(CC) $(USER_CFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f sensor.bpf.o sensor.skel.h test_sensor

.PHONY: all clean
