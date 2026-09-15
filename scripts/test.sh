#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TIMEOUT_SECONDS="${LIONOS_TEST_TIMEOUT:-10}"
QEMU_MEMORY="${LIONOS_TEST_MEMORY:-128M}"

run_qemu() {
    local log_file="$1"
    set +e
    rm -f "$log_file"
    timeout "${TIMEOUT_SECONDS}s" qemu-system-i386 \
        -smp 2 \
        -cdrom build/lionos.iso \
        -drive file=build/lionos-disk.img,format=raw,if=ide \
        -m "$QEMU_MEMORY" \
        -display none \
        -serial none \
        -debugcon "file:$log_file" \
        -global isa-debugcon.iobase=0xE9
    local status=$?
    set -e

    if [ -f "$log_file" ]; then cat "$log_file"; fi
    if [ "$status" -ne 124 ]; then
        echo "LionOS test: QEMU exited unexpectedly with status $status" >&2
        return 1
    fi
}

run_gui_smoke() {
    local log_file="$1"
    local monitor_socket="build/gui-monitor.sock"
    local pid
    rm -f "$log_file" "$monitor_socket"

    qemu-system-i386 \
        -smp 2 \
        -cdrom build/lionos.iso \
        -drive file=build/lionos-disk.img,format=raw,if=ide \
        -m "$QEMU_MEMORY" \
        -display none \
        -serial none \
        -debugcon "file:$log_file" \
        -global isa-debugcon.iobase=0xE9 \
        -monitor "unix:$monitor_socket,server=on,wait=off" \
        >/dev/null 2>&1 &
    pid=$!

    cleanup_gui() {
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
        rm -f "$monitor_socket"
    }
    trap cleanup_gui RETURN
    trap cleanup_gui EXIT

    for _ in $(seq 1 100); do
        if [ -f "$log_file" ] \
            && grep -q 'LIONOS:READY' "$log_file" \
            && grep -q 'LIONOS:GUI-ENTER' "$log_file"; then
            break
        fi
        sleep 0.1
    done

    grep -q 'LIONOS:READY' "$log_file"
    grep -q 'LIONOS:GUI-ENTER' "$log_file"
    sleep 0.25

    python3 - "$monitor_socket" <<'PY'
import socket
import sys
import time

path = sys.argv[1]
for _ in range(100):
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(path)
        sock.settimeout(1.0)
        try:
            sock.recv(4096)
        except socket.timeout:
            pass
        for _ in range(3):
            sock.sendall(b"sendkey q\n")
            time.sleep(0.15)
            if _ < 2:
                time.sleep(0.05)
        sock.close()
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.05)
else:
    raise SystemExit("unable to connect to QEMU monitor")
PY

    for _ in $(seq 1 100); do
        if grep -q 'LIONOS:GUI-EXIT' "$log_file"; then break; fi
        sleep 0.1
    done

    cat "$log_file"
    grep -q 'LIONOS:GUI-EXIT' "$log_file"
    grep -q 'LIONOS:READY' "$log_file"
}

echo "== LionOS Phase 23 stability + Phase 26 GUI smoke test =="
echo "[1/5] Building kernel, userspace, ISO, and persistent disk"
make clean
make
make userland
make iso
make disk

echo "[2/5] Validating every userspace ELF"
for elf in build/*.elf; do
    test -s "$elf"
    test "$(stat -c%s "$elf")" -le 16384
    readelf -h "$elf" | grep -q 'Class:[[:space:]]*ELF32'
    readelf -h "$elf" | grep -q 'Machine:[[:space:]]*Intel 80386'
    readelf -l "$elf" | grep -q 'LOAD'
done

echo "[3/5] First SMP boot / filesystem initialization"
run_qemu build/qemu-first.log
grep -q 'LIONOS:READY' build/qemu-first.log
grep -q 'LIONOS:PERSIST-INIT' build/qemu-first.log
grep -q 'LIONOS:SMP-CPU-ONLINE' build/qemu-first.log

echo "[4/5] Second SMP boot / persistence verification"
run_qemu build/qemu-second.log
grep -q 'LIONOS:READY' build/qemu-second.log
grep -q 'LIONOS:PERSIST-OK' build/qemu-second.log
grep -q 'LIONOS:SMP-CPU-ONLINE' build/qemu-second.log

echo "[5/5] Graphical desktop / keyboard escape smoke test"
run_gui_smoke build/qemu-gui.log
echo "LionOS Phase 23 + Phase 26 GUI test: PASS"
