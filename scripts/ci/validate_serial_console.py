#!/usr/bin/env python3
"""Static contract checks for the LAVA-capable LionOS serial shell."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


serial = read("src/serial.c")
header = read("include/serial.h")
console = read("src/console.c")
kernel = read("src/kernel.c")
shell = read("src/shell.c")
job = read("hardware/lava/lionos-job-template.yaml")
lava_docs = read("hardware/lava/README.md")

checks = {
    "COM1 I/O base": "0x03F8u" in header,
    "COM2 fallback base": "0x02F8u" in header,
    "115200 baud": "115200u" in header,
    "16550 internal loopback test": "UART_LOOPBACK_TEST" in serial and "0xAEu" in serial,
    "COM1 then COM2 detection": "SERIAL_COM1_BASE" in serial and "SERIAL_COM2_BASE" in serial,
    "8N1 UART configuration": "0x03u); /* 8 data bits, no parity, 1 stop bit */" in serial,
    "UART interrupts disabled": "UART_IER(SERIAL_COM1_BASE), 0x00u" in serial,
    "bounded UART polling": "UART_POLL_LIMIT" in serial,
    "CR/LF normalization": "previous_was_cr" in serial and "return '\\n';" in serial,
    "console output mirrored": "serial_putc(c);" in console,
    "serial shell input": "serial_getchar()" in shell,
    "keyboard input preserved": "keyboard_poll()" in shell and "keyboard_getchar()" in shell,
    "UART initialized before console": kernel.index("serial_init()") < kernel.index("console_init()"),
    "serial-equipped target enters shell": "if (serial_available())" in kernel and kernel.index("if (serial_available())") < kernel.index("gui_run();"),
    "serial diagnostic command exposed": 'eq(cmd,"serial")' in shell and "Serial console diagnostics" in shell and "serial_port_name()" in shell,
    "LAVA serial test command": "command: serial" in job and "serial-diagnostics" in job,
    "LAVA shell prompt": 'lion:/ >' in job,
    "non-destructive removable-media policy": "internal storage" in lava_docs and "removable" in lava_docs,
}

failed = [name for name, passed in checks.items() if not passed]
for name, passed in checks.items():
    print(f"[{'PASS' if passed else 'FAIL'}] {name}")
if failed:
    raise SystemExit(f"{len(failed)} serial-console contract check(s) failed")
print(f"Serial-console source contract: {len(checks)} checks passed")
print("Physical UART cable, LAVA dispatcher, and target boot: NOT TESTED")
