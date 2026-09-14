#include <stdint.h>
#include "ramfs.h"

#define RAMFS_MAX_FILES 16u
#define RAMFS_NAME_MAX 16u
#define RAMFS_DATA_MAX 256u

struct ramfs_file { char name[RAMFS_NAME_MAX]; char data[RAMFS_DATA_MAX]; uint32_t size; };
static struct ramfs_file files[RAMFS_MAX_FILES];
static uint32_t count;

/* Tiny valid ELF32/i386 user program: prints "Hi!" and exits through the syscall ABI. */
static const uint8_t hello_elf[] = {
    0x7F,0x45,0x4C,0x46,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00,0x03,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x34,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x34,0x00,0x20,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x54,0x00,0x00,0x00,
    0x00,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x3A,0x00,0x00,0x00,0x3A,0x00,0x00,0x00,
    0x05,0x00,0x00,0x00,0x00,0x10,0x00,0x00,
    0xB8,0x01,0x00,0x00,0x00,0xBB,0x48,0x00,0x00,0x00,0xCD,0x80,
    0xB8,0x01,0x00,0x00,0x00,0xBB,0x69,0x00,0x00,0x00,0xCD,0x80,
    0xB8,0x01,0x00,0x00,0x00,0xBB,0x21,0x00,0x00,0x00,0xCD,0x80,
    0xB8,0x01,0x00,0x00,0x00,0xBB,0x0A,0x00,0x00,0x00,0xCD,0x80,
    0xB8,0x04,0x00,0x00,0x00,0xCD,0x80,0xF4,0xEB,0xFC
};

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static struct ramfs_file *find(const char *name) {
    for (uint32_t i = 0; i < count; ++i) if (streq(files[i].name, name)) return &files[i];
    return 0;
}

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    while (src[i] && i < RAMFS_NAME_MAX - 1u) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

void ramfs_init(void) {
    count = 0;
    ramfs_write("readme.txt", "Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n", 61u);
    ramfs_write("version", "LionOS 0.6\n", 12u);
    ramfs_write("motd", "The LionOS kernel is alive.\n", 28u);
    ramfs_write("hello.elf", (const char *)hello_elf, sizeof(hello_elf));
}

uint32_t ramfs_count(void) { return count; }
const char *ramfs_name(uint32_t index) { return index < count ? files[index].name : 0; }
const char *ramfs_data(const char *name) { struct ramfs_file *f = find(name); return f ? f->data : 0; }
uint32_t ramfs_size(const char *name) { struct ramfs_file *f = find(name); return f ? f->size : 0; }
int ramfs_exists(const char *name) { return find(name) != 0; }

int ramfs_write(const char *name, const char *data, uint32_t size) {
    if (!name || !*name || !data) return -1;
    struct ramfs_file *f = find(name);
    if (!f) {
        if (count >= RAMFS_MAX_FILES) return -1;
        f = &files[count++];
        copy_name(f->name, name);
    }
    if (size > RAMFS_DATA_MAX) size = RAMFS_DATA_MAX;
    for (uint32_t i = 0; i < size; ++i) f->data[i] = data[i];
    f->size = size;
    return 0;
}

int ramfs_remove(const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (streq(files[i].name, name)) {
            for (uint32_t j = i + 1u; j < count; ++j) files[j - 1u] = files[j];
            --count;
            return 0;
        }
    }
    return -1;
}
