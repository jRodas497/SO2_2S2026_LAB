# Daemon so2monitor + sensor eBPF.
#   make        compila el sensor eBPF (ebpf/) y el daemon
#   make clean
CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -pthread -Isrc -Ithird_party/cjson -Iebpf
LDLIBS  := -lbpf -lelf -lz -lcrypto -lpam -lpthread -lm

SRCS := src/main.c src/common.c src/alerts.c src/metrics.c src/blacklist.c \
        src/fswatch.c src/logwatch.c src/auth.c src/http.c
OBJS := $(SRCS:src/%.c=build/%.o) build/cJSON.o
HDRS := $(wildcard src/*.h) ebpf/sensor.h

all: skel so2monitor

so2monitor: $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

# El esqueleto del sensor lo genera ebpf/Makefile (clang + bpftool); es incremental.
skel:
	$(MAKE) -C ebpf sensor.skel.h

$(OBJS): | skel

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build/fswatch.o: ebpf/sensor.skel.h

# cJSON (terceros, MIT) se compila sin los avisos estrictos del resto
build/cJSON.o: third_party/cjson/cJSON.c | build
	$(CC) -O2 -w -Ithird_party/cjson -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build so2monitor
	$(MAKE) -C ebpf clean

.PHONY: all skel clean
