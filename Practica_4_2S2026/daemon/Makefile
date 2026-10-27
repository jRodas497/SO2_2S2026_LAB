# Daemon de la Practica 4 (no necesita libbpf, PAM ni OpenSSL).
#   make        compila ./so2_p4
#   make clean
CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -pthread -Isrc -Ithird_party/cjson
LDLIBS  := -lpthread -lm

SRCS := src/main.c src/common.c src/sistema.c src/eventos.c src/http.c
OBJS := $(SRCS:src/%.c=build/%.o) build/cJSON.o
HDRS := $(wildcard src/*.h)

all: so2_p4

so2_p4: $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

# cJSON (terceros, MIT) se compila sin los avisos estrictos del resto
build/cJSON.o: third_party/cjson/cJSON.c | build
	$(CC) -O2 -w -Ithird_party/cjson -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build so2_p4

.PHONY: all clean
