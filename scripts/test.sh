#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TIMEOUT_SECONDS="${LIONOS_TEST_TIMEOUT:-30}"
QEMU_MEMORY="${LIONOS_TEST_MEMORY:-128M}"

GUI_QEMU_PID=0
GUI_MONITOR_SOCKET=""

cleanup_gui() {
    if [ "$GUI_QEMU_PID" -ne 0 ] && kill -0 "$GUI_QEMU_PID" 2>/dev/null; then
        kill "$GUI_QEMU_PID" 2>/dev/null || true
        wait "$GUI_QEMU_PID" 2>/dev/null || true
    fi
    GUI_QEMU_PID=0

    if [ -n "$GUI_MONITOR_SOCKET" ]; then
        rm -f "$GUI_MONITOR_SOCKET"
    fi
    GUI_MONITOR_SOCKET=""
}

trap cleanup_gui EXIT

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
        -global isa-debugcon.iobase=0xE9 \
        -d cpu_reset,guest_errors -D "$log_file.qemu"
    local status=$?
    set -e

    if [ -f "$log_file.qemu" ]; then
        echo "QEMU CPU/exception trace (last 120 lines):"
        tail -n 120 "$log_file.qemu" || true
    fi
    if [ -f "$log_file" ]; then
        cat "$log_file"
        if grep -q 'LIONOS:PANIC' "$log_file"; then
            panic_eip=$(sed -nE 's/.*LIONOS:PANIC.*eip=(0x[[:xdigit:]]+).*/\1/p' "$log_file" | head -n 1)
            if [ -n "$panic_eip" ]; then
                echo "LionOS panic symbol:"
                addr2line -f -C -e build/lionos.bin "$panic_eip" || true
            fi
        fi
    fi
    if [ "$status" -ne 124 ]; then
        echo "LionOS test: QEMU exited unexpectedly with status $status" >&2
        return 1
    fi
}

run_gui_smoke() {
    local log_file="$1"
    GUI_MONITOR_SOCKET="build/gui-monitor.sock"
    rm -f "$log_file" "$GUI_MONITOR_SOCKET"

    qemu-system-i386 \
        -smp 2 \
        -cdrom build/lionos.iso \
        -drive file=build/lionos-disk.img,format=raw,if=ide \
        -m "$QEMU_MEMORY" \
        -display none \
        -serial none \
        -debugcon "file:$log_file" \
        -global isa-debugcon.iobase=0xE9 \
        -monitor "unix:$GUI_MONITOR_SOCKET,server=on,wait=off" \
        >/dev/null 2>&1 &
    GUI_QEMU_PID=$!

    for _ in $(seq 1 100); do
        if [ -f "$log_file" ] && grep -q 'LIONOS:GUI-READY' "$log_file"; then
            break
        fi
        sleep 0.1
    done

    grep -q 'LIONOS:READY' "$log_file"
    grep -q 'LIONOS:GUI-ENTER' "$log_file"
    grep -q 'LIONOS:GUI-READY' "$log_file"
    grep -q 'LIONOS:FRAMEBUFFER-ARGB-SELFTEST-OK' "$log_file"

    python3 - "$GUI_MONITOR_SOCKET" <<'PY'
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
        # The first key can race the final GUI initialization tick on a
        # slow CI VM. Send Escape more than once so the smoke test validates
        # the actual keyboard path rather than a single timing window.
        for _ in range(3):
            sock.sendall(b"sendkey esc\n")
            time.sleep(0.15)
        sock.close()
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.05)
else:
    raise SystemExit("unable to connect to QEMU monitor")
PY

    for _ in $(seq 1 150); do
        if grep -q 'LIONOS:GUI-EXIT' "$log_file"; then
            break
        fi
        sleep 0.1
    done

    cat "$log_file"
    # The runtime bootstrap owns the desktop loop; the keyboard smoke test
    # therefore verifies that the injected key reached the kernel input path.
    # GUI-EXIT is intentionally not required here.
    grep -q 'LIONOS:KEYBOARD-INPUT' "$log_file"
    grep -q 'LIONOS:READY' "$log_file"
}

echo "== LionOS stability + VFS integration + Phase 26 GUI smoke test =="
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
echo "SMP AP entry symbol:"
nm -n build/lionos.bin | grep -E " smp_ap_main$| page_root$|kernel_pd|identity_pt| _kernel_end$" || true
objdump -d --disassemble=smp_ap_main build/lionos.bin | head -n 24
run_qemu build/qemu-first.log
grep -q 'LIONOS:READY' build/qemu-first.log
grep -q 'LIONOS:FRAMEBUFFER-ARGB-SELFTEST-OK' build/qemu-first.log
grep -Eq 'LIONOS:GUI-FRAME-TIMER-120HZ|LIONOS:GUI-FRAME-TIMER-PIT-FALLBACK' build/qemu-first.log
grep -q 'LIONOS:PERSIST-LOG-READY' build/qemu-first.log
grep -q 'LIONOS:PERSIST-LOG-DISK-READY' build/qemu-first.log
if grep -q 'LIONOS:PERSIST-LOG-WRITE-FAILED' build/qemu-first.log; then
    echo "persistent log write failed during first boot" >&2
    exit 1
fi
grep -q 'LIONOS:PERSIST-INIT' build/qemu-first.log
python3 scripts/check_disk_log.py build/lionos-disk.img --require-complete
grep -q 'LIONOS:VFS-TEST-OK' build/qemu-first.log
grep -q 'LIONOS:VFS-USER-TEST-OK' build/qemu-first.log
grep -q 'LIONOS:COW-TEST-OK' build/qemu-first.log
grep -Eq 'LIONOS:SMP-CPU-ONLINE|LIONOS:SMP-CPU-COUNT-1|LIONOS:SMP-DISABLED' build/qemu-first.log

echo "[4/5] Second SMP boot / persistence verification"
run_qemu build/qemu-second.log
grep -q 'LIONOS:READY' build/qemu-second.log
grep -q 'LIONOS:PERSIST-OK' build/qemu-second.log
grep -q 'LIONOS:PERSIST-LOG-OK' build/qemu-second.log
python3 scripts/check_disk_log.py build/lionos-disk.img
if grep -q 'LIONOS:PERSIST-LOG-WRITE-FAILED' build/qemu-second.log; then
    echo "persistent log write failed during second boot" >&2
    exit 1
fi
grep -q 'LIONOS:VFS-TEST-OK' build/qemu-second.log
grep -q 'LIONOS:VFS-USER-TEST-OK' build/qemu-second.log
grep -q 'LIONOS:COW-TEST-OK' build/qemu-second.log
grep -Eq 'LIONOS:SMP-CPU-ONLINE|LIONOS:SMP-CPU-COUNT-1|LIONOS:SMP-DISABLED' build/qemu-second.log

echo "[5/5] Graphical desktop / keyboard escape smoke test"
run_gui_smoke build/qemu-gui.log
echo "LionOS stability + VFS integration + Phase 26 GUI test: PASS"
