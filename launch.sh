#!/bin/bash
set -e

WIN_IMAGE=$(wslpath -w "$(readlink -f kiwiOS.iso)")
WIN_DISK=$(wslpath -w "$(readlink -f disk.img)")

cd "/mnt/c/Program Files/qemu/"

./qemu-system-x86_64.exe \
  -M q35 \
  -serial stdio \
  -device ich9-ahci,id=ahci0 \
  -drive id=d0,file="$WIN_DISK",if=none,format=raw \
  -device ide-hd,drive=d0,bus=ahci0.0 \
  -cdrom "$WIN_IMAGE" \
  -boot order=d
