#!/bin/sh
# Debug the sensor firmware on the emulator with a real arm-none-eabi-gdb:
# break main, continue, stepi, info registers, x/4wx 0x20000000.
#
#   tools/gdb_firmware.sh [path/to/stm32-baremetal-sensor-node]
#
# Needs build/cortex-m-emu, the firmware's build/firmware.{elf,bin} and
# arm-none-eabi-gdb. Prints GDB's output; register values depend on the
# firmware build.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
fw=${1:-"$root/../stm32-baremetal-sensor-node"}
log=$(mktemp)
trap 'rm -f "$log"; kill "$emu" 2>/dev/null || true' EXIT

"$root/build/cortex-m-emu" --gdb 0 "$fw/build/firmware.bin" >/dev/null 2>"$log" &
emu=$!
port=
for _ in 1 2 3 4 5 6 7 8 9 10; do
    port=$(sed -n 's/^gdb: listening on 127\.0\.0\.1:\([0-9]*\)$/\1/p' "$log")
    [ -n "$port" ] && break
    sleep 0.2
done
[ -n "$port" ] || { cat "$log" >&2; exit 1; }

arm-none-eabi-gdb -nx -batch \
    -ex 'set pagination off' \
    -ex 'set confirm off' \
    -ex "target remote 127.0.0.1:$port" \
    -ex 'break main' \
    -ex 'continue' \
    -ex 'stepi' \
    -ex 'info registers' \
    -ex 'x/4wx 0x20000000' \
    -ex 'kill' \
    "$fw/build/firmware.elf"
wait "$emu"
