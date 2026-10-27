#!/usr/bin/env bash
# Instala lo necesario para compilar el kernel con BTF y construir eBPF/daemon.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
  build-essential bc bison flex libssl-dev libelf-dev libncurses-dev \
  dwarves cpio rsync git zlib1g-dev pkg-config curl xz-utils ca-certificates \
  clang llvm libbpf-dev libpam0g-dev

# bpftool: en Ubuntu puede venir como paquete propio o dentro de linux-tools.
sudo apt-get install -y bpftool || sudo apt-get install -y linux-tools-common linux-tools-generic || true

echo; echo "== Verificacion"
for t in gcc make clang llvm-strip pahole bison flex; do printf '%-10s' "$t"; command -v "$t" || echo "FALTA"; done
pahole --version
clang --version | head -1
pkg-config --modversion libbpf openssl
[ -f /usr/include/security/pam_appl.h ] && echo "pam OK" || echo "pam FALTA"
if command -v bpftool >/dev/null; then
  bpftool version 2>&1 | head -1 || echo "bpftool es un wrapper que no corre en este kernel"
fi
ls /usr/lib/linux-tools/*/bpftool 2>/dev/null || true
