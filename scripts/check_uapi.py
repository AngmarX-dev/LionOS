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
    "if(!usercopy_validate_write(arg1,sizeof(uint32_t)))return SYSCALL_ERR;{int32_t result=process_waitpid",
    "case LIONOS_SYS_THREAD_JOIN:if(!has_cap(PROCESS_CAP_PROCESS)||!usercopy_validate_write(arg1,sizeof(uint32_t)))",
    "case LIONOS_SYS_PIPE_READ:if(!has_cap(PROCESS_CAP_IPC)||arg2==0||arg2>IPC_MESSAGE_MAX||!usercopy_validate_write(arg1,arg2))",
    "case LIONOS_SYS_READDIR:if(!has_cap(PROCESS_CAP_FS))return SYSCALL_ERR;{char path[VFS_PATH_MAX];",
    "!usercopy_validate_write(arg1,sizeof(uint32_t))||!usercopy_validate_write(arg2,sizeof(*user_entry))",
)
for fragment in required_write_checks:
    if fragment not in dispatcher:
        fail("writable user-buffer validation missing or changed: " + fragment)

print("UAPI check: PASS (" + str(len(definitions)) +
      " unique syscall numbers; ABI " + version.group(1) +
      "; every registered syscall has a dispatcher case)")
