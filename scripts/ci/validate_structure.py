#!/usr/bin/env python3
"""Source/binary structural checks for LionOS; never boots or emulates the kernel."""
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
passed = []

def check(name, condition, detail=""):
    if not condition:
        raise AssertionError(f"{name}: {detail or 'invariant failed'}")
    passed.append(name)
    print(f"[PASS] {name}")

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

def macro(text, name):
    m = re.search(r"^#define\s+" + re.escape(name) + r"\s+([^\s/]+)", text, re.M)
    check(f"macro {name} exists", bool(m))
    return m.group(1).rstrip("uUlL")

def value(text, name):
    return int(macro(text, name), 0)

def validate_elf(path, label):
    data = path.read_bytes()
    check(f"{label}: ELF magic", data[:4] == b"\x7fELF")
    check(f"{label}: ELF32 little-endian", len(data) >= 52 and data[4:6] == b"\x01\x01")
    check(f"{label}: i386 machine", struct.unpack_from("<H", data, 18)[0] == 3)
    entry, phoff = struct.unpack_from("<II", data, 24)
    ehsize, phentsize, phnum = struct.unpack_from("<HHH", data, 40)
    check(f"{label}: ELF header dimensions", ehsize == 52 and phentsize >= 32 and phnum > 0)
    check(f"{label}: entry point nonzero", entry != 0)
    check(f"{label}: program-header bounds", phoff + phentsize * phnum <= len(data))
    loads = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        typ, poff, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from("<IIIIIIII", data, off)
        if typ != 1:
            continue
        loads += 1
        check(f"{label}: LOAD {i} file bounds", poff + filesz <= len(data))
        check(f"{label}: LOAD {i} memory size", memsz >= filesz)
        check(f"{label}: LOAD {i} alignment", align in (0, 1) or (align & (align - 1) == 0 and vaddr % align == poff % align))
    check(f"{label}: has LOAD segment", loads > 0)
    return data

kernel = validate_elf(BUILD / "lionos.bin", "kernel")

# Verify the actual Multiboot2 header bytes, checksum, alignment and tag terminator.
mb_magic = 0xE85250D6
header = None
for off in range(0, min(len(kernel), 32768), 8):
    if off + 16 > len(kernel) or struct.unpack_from("<I", kernel, off)[0] != mb_magic:
        continue
    arch, length, checksum = struct.unpack_from("<III", kernel, off + 4)
    if arch == 0 and length >= 24 and off + length <= len(kernel) and (mb_magic + arch + length + checksum) & 0xFFFFFFFF == 0:
        header = (off, length)
        break
check("Multiboot2 header: aligned, in first 32 KiB, checksum valid", header is not None)
if header:
    off, length = header
    pos, terminated = off + 16, False
    while pos + 8 <= off + length:
        typ, flags, size = struct.unpack_from("<HHI", kernel, pos)
        check(f"Multiboot2 tag {typ}: size/bounds", size >= 8 and pos + size <= off + length)
        if typ == 0:
            check("Multiboot2 end tag: exact 8-byte tag", size == 8)
            terminated = True
            break
        pos += (size + 7) & ~7
    check("Multiboot2 tag list terminates", terminated)

# Physical memory manager.
mem = read("src/memory.c")
check("PMM page size is 4 KiB", value(mem, "PAGE_SIZE") == 4096)
max_pages, reserved = value(mem, "MAX_PAGES"), value(mem, "RESERVED_PAGES")
check("PMM bitmap covers page universe", "bitmap[MAX_PAGES / 32u]" in mem and max_pages % 32 == 0)
check("PMM reference counts cover page universe", "page_refs[MAX_PAGES]" in mem)
check("PMM reserves low memory", reserved > 0 and "reserve_range(0,RESERVED_PAGES*PAGE_SIZE)" in mem)
for fn in ("page_alloc_contiguous", "page_retain", "page_free", "memory_free_pages"):
    check(f"PMM function {fn} exists", re.search(r"\b" + fn + r"\s*\(", mem) is not None)
check("PMM critical sections use IRQ-safe lock", mem.count("spinlock_irqsave_acquire(&memory_lock)") >= 4 and mem.count("spinlock_irqrestore_release(&memory_lock,irq)") >= 4)

# PAE paging layout and protection boundaries.
paging = read("src/paging.c")
check("PAE root has four entries", value(paging, "PAE_ROOT_ENTRIES") == 4)
check("PAE directory and table dimensions are 512", value(paging, "PAE_PD_ENTRIES") == 512 and value(paging, "PAE_PT_ENTRIES") == 512)
check("Identity map covers 256 MiB", value(paging, "IDENTITY_PT_COUNT") * 512 * 4096 == 256 * 1024 * 1024)
base, tables = value(paging, "KERNEL_MMIO_BASE"), value(paging, "KERNEL_MMIO_PT_COUNT")
check("MMIO window ends exactly at PD slot boundary", base == 0xF0000000 and ((base >> 21) & 0x1FF) + tables == 512)
check("PAE physical address mask is 52-bit", "0x000FFFFFFFFFF000ULL" in paging)
check("User mapper rejects addresses at/above user limit", "virtual_address >= USER_LIMIT" in paging)
check("COW has refcount, retain and copy paths", all(s in paging for s in ("page_refcount", "page_retain", "dst[i]=src[i]")))

# GDT and TSS.
gdt = read("src/gdt.c")
check("GDT descriptor count is seven", value(gdt, "GDT_ENTRIES") == 7)
check("GDT kernel code/data descriptors are present", "0x9Au,0xCFu" in gdt and "0x92u,0xCFu" in gdt)
check("GDT user selectors match process selectors", "0x2Bu" in gdt and "0x33u" in gdt)
check("GDT TSS descriptor is available 32-bit TSS", "0x89u" in gdt and "GDT_TSS 0x18u" in gdt)

# IDT and assembly ISR tables.
idt, asm = read("src/idt.c"), read("boot/boot.asm")
vectors = [int(x) for x in re.findall(r"^ISR_(?:NOERR|ERR)\s+(\d+)\s*$", asm, re.M)]
expected = list(range(49)) + [128]
check("ISR assembly vector numbers are unique", len(vectors) == len(set(vectors)))
check("ISR assembly covers exception, IRQ and syscall vectors", sorted(vectors) == expected)
declared = {int(x) for x in re.findall(r"ISR_DECL\((\d+)\)", idt)}
check("IDT declarations match assembly ISR symbols", declared == set(expected))
check("IDT installs vectors 0 through 48", "i<49" in idt and "idt_set_gate((uint8_t)i" in idt)
check("IDT syscall vector 128 is DPL3", "idt_set_gate(128,(uint32_t)isr128,0xEE)" in idt)
errors = {int(x) for x in re.findall(r"^ISR_ERR\s+(\d+)\s*$", asm, re.M)}
check("ISR error-code stubs match architectural vectors", errors == {8, 10, 11, 12, 13, 14, 30})

# Syscall ABI, user pointer validation, process and VFS contracts.
uapi, vfsu = read("include/uapi.h"), read("include/vfs_uapi.h")
syscall = read("src/syscall.c")
check("Syscall ABI version matches dispatcher", value(uapi, "LIONOS_SYSCALL_ABI") == 4 and "case SYS_ABI_VERSION:return 4u" in syscall)
numbers = {}
for header in (uapi, vfsu):
    for name, raw in re.findall(r"^#define\s+(LIONOS_SYS_[A-Z0-9_]+)\s+(\d+)u?\s*$", header, re.M):
        numbers.setdefault(int(raw), []).append(name)
dupes = {n:names for n,names in numbers.items() if len(names) > 1}
check("UAPI syscall numeric IDs are unique", not dupes, str(dupes))
check("Syscall user address range is bounded", "USER_MIN 0x10000000u" in syscall and "USER_MAX 0xC0000000u" in syscall)
check("Syscall user copy length is capped", "USER_COPY_MAX 4096u" in syscall)
check("Syscalls enforce process capabilities", "process_has_capability" in syscall and "has_cap(PROCESS_CAP_FS)" in syscall)
proc = read("include/process.h")
check("Process and per-process page limits are nonzero", value(proc, "LIONOS_PROCESS_MAX") > 0 and value(proc, "LIONOS_PROCESS_MAX_USER_PAGES") > 0)
check("Scheduler states and priority levels are declared", all(s in proc for s in ("PROCESS_READY", "PROCESS_RUNNING", "PROCESS_WAITING", "PROCESS_ZOMBIE", "PROCESS_PRIORITY_LEVELS")))
vfs_h, vfs = read("include/vfs.h"), read("src/vfs.c")
check("VFS descriptor limit is 32", value(vfs_h, "VFS_FD_MAX") == 32)
check("VFS normalizes dot path components", "src[start]=='.'" in vfs and "src[start+1]=='.'" in vfs)
check("VFS supports RAMFS and DiskFS backends", "VFS_BACKEND_RAMFS" in vfs and "VFS_BACKEND_DISKFS" in vfs)

# DiskFS driver contract.
disk_h, disk = read("include/diskfs.h"), read("src/diskfs.c")
check("DiskFS file/name limits are fixed and bounded", value(disk_h, "DISKFS_MAX_FILES") == 32 and value(disk_h, "DISKFS_NAME_MAX") == 64)
check("DiskFS 4 KiB file size equals eight sectors", value(disk_h, "DISKFS_MAX_FILE_SIZE") == 4096 and "DISKFS_SECTORS_PER_FILE 8u" in disk)
check("DiskFS metadata and data LBAs are contiguous", all(s in disk for s in ("DISKFS_DIR_LBA 1u", "DISKFS_DIR_SECTORS 5u", "DISKFS_DATA_LBA 6u")))
check("DiskFS checks signature and version before enabling", "sector[6]!='1'" in disk and "DISKFS_VERSION" in disk and "available=1" in disk)
check("DiskFS does not format unknown media on boot", "Never format an unknown ATA device" in disk)

# Linked kernel symbols and every userland ELF.
symbols = subprocess.check_output(["nm", "-n", str(BUILD / "lionos.bin")], text=True)
check("Kernel exports _start", re.search(r"\b_start$", symbols, re.M) is not None)
check("Kernel exports kernel_main", re.search(r"\bkernel_main$", symbols, re.M) is not None)
user_elfs = sorted(BUILD.glob("*.elf"))
check("At least one userland ELF exists", bool(user_elfs))
for elf in user_elfs:
    validate_elf(elf, f"user {elf.name}")

print(f"\nLionOS structural validation: {len(passed)} checks passed.")
