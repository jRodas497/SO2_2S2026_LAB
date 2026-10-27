#!/usr/bin/env bash
set -u
echo "=== KERNEL ==="; uname -r
echo "=== DISCO ==="; df -h /
echo "=== SO2 ==="; du -sh "$HOME"/SO2/* 2>/dev/null | sort -h || true
echo "=== MODULOS ==="; du -sh /lib/modules/* 2>/dev/null | sort -h || true
echo "=== BOOT ==="; ls -lh /boot | grep -E '6\.12\.69|7\.0\.0|vmlinuz|initrd|System.map|config-' || true
echo "=== RECURSOS ==="; ls -lh "$HOME/SO2/recursos" 2>/dev/null || true
