#!/usr/bin/env python3
"""Check the syscall registry against the native dispatcher."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
FILES = (ROOT / "include/uapi.h", ROOT / "include/vfs_uapi.h")
DEFINE = re.compile(r"^\s*#define\s+(LIONOS_SYS_[A-Z0-9_]+)\s+([0-9]+)u?\s*$", re.M)
ALIAS = re.compile(r"^\s*#define\s+(SYS_[A-Z0-9_]+)\s+(LIONOS_SYS_[A-Z0-9_]+)\s*$", re.M)
CASE = re.compile(r"\bcase\s+([A-Z][A-Z0-9_]*)\s*:")
VERSION = re.compile(r"^\s*#define\s+LIONOS_SYSCALL_ABI\s+([0-9]+)u?\s*$", re.M)
QUERY = re.compile(r"case\s+SYS_ABI_VERSION\s*:\s*return\s+([0-9]+)u?\s*;", re.S)

def fail(message):
    print("UAPI check: FAIL: " + message, file=sys.stderr)
    raise SystemExit(1)

definitions = {}
for path in FILES:
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        fail("cannot read " + str(path) + ": " + str(exc))
    for name, raw in DEFINE.findall(source):
        if name in definitions:
            fail("duplicate symbol " + name)
        definitions[name] = int(raw, 10)

if not definitions:
    fail("no syscall definitions found")

numbers = {}
for name, number in definitions.items():
    if number in numbers:
        fail("number " + str(number) + " is shared by " + numbers[number] + " and " + name)
    numbers[number] = name

header = (ROOT / "include/syscall.h").read_text(encoding="utf-8")
dispatcher = (ROOT / "src/syscall.c").read_text(encoding="utf-8")
aliases = dict(ALIAS.findall(header))
cases = set(CASE.findall(dispatcher))
missing = []
for name in definitions:
    target = next((alias for alias, symbol in aliases.items() if symbol == name), name)
    if target not in cases:
        missing.append(name + " (case " + target + ")")
if missing:
    fail("missing dispatcher cases: " + ", ".join(sorted(missing)))

uapi = (ROOT / "include/uapi.h").read_text(encoding="utf-8")
version = VERSION.search(uapi)
query = QUERY.search(dispatcher)
if not version or not query:
    fail("ABI version declaration or syscall 0 version response is missing")
if int(version.group(1)) != int(query.group(1)):
    fail("header ABI version and syscall 0 response disagree")

# Guard the user-pointer access direction for syscalls that write into user memory.
required_write_checks = (
    "case SYS_READ:if(!has_cap(PROCESS_CAP_CONSOLE))return SYSCALL_ERR;",
    "case LIONOS_SYS_FREAD:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;",
    "case LIONOS_SYS_FWRITE:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;",
    "case LIONOS_SYS_IPC_RECV_BLOCKING:if(!has_cap(PROCESS_CAP_IPC))return SYSCALL_ERR;",
    "case LIONOS_SYS_PIPE_READ:if(!has_cap(PROCESS_CAP_IPC)",
    "case LIONOS_SYS_NET_RECV:if(!has_cap(PROCESS_CAP_NET))return SYSCALL_ERR;",
    "case LIONOS_SYS_READDIR:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;",
    "case LIONOS_SYS_THREAD_CREATE:if(!has_cap(PROCESS_CAP_PROCESS)||!usercopy_validate_read(arg0,1u))",
)

for fragment in required_write_checks:
    if fragment not in dispatcher:
        fail("writable user-buffer validation missing or changed: " + fragment)

# User pointers must be copied through the centralized boundary helpers.
if re.search(r"\([^)]*\*\)\s*\(uintptr_t\)\s*arg[0-4]", dispatcher):
    fail("raw syscall argument is cast directly to a pointer")

for forbidden in (
    "(const char*)(uintptr_t)arg0",
    "(const void*)(uintptr_t)arg1",
    "(void*)(uintptr_t)arg1",
    "(uint32_t*)(uintptr_t)arg0",
    "(uint32_t*)(uintptr_t)arg1",
):
    if forbidden in dispatcher:
        fail("direct user-pointer dereference remains: " + forbidden)

for required in ("usercopy_copy_from(", "usercopy_copy_to(", "syscall_copy_in(", "syscall_copy_out("):
    if required not in dispatcher:
        fail("centralized usercopy operation missing: " + required)

print("UAPI check: PASS (" + str(len(definitions)) +
      " unique syscall numbers; ABI " + version.group(1) +
      "; every registered syscall has a dispatcher case)")
