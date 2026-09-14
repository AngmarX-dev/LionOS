#include <stdint.h>
#include "ramfs.h"

#define RAMFS_MAX_FILES 16u
#define RAMFS_NAME_MAX 16u
#define RAMFS_DATA_MAX 256u

struct ramfs_file { char name[RAMFS_NAME_MAX]; char data[RAMFS_DATA_MAX]; uint32_t size; };
static struct ramfs_file files[RAMFS_MAX_FILES];
static uint32_t count;

extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_process_test_elf_start[];
extern const uint8_t _binary_process_test_elf_end[];

static int streq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static struct ramfs_file *find(const char *name) { for (uint32_t i = 0; i < count; ++i) if (streq(files[i].name, name)) return &files[i]; return 0; }
static void copy_name(char *dst, const char *src) {
    uint32_t i = 0; while (src[i] && i < RAMFS_NAME_MAX - 1u) { dst[i] = src[i]; ++i; } dst[i] = 0;
}

void ramfs_init(void) {
    count = 0;
    ramfs_write("readme.txt", "Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n", 61u);
    ramfs_write("version", "LionOS 0.7\n", 12u);
    ramfs_write("motd", "The LionOS kernel is alive.\n", 28u);
}

uint32_t ramfs_count(void) { return count + 2u; }
const char *ramfs_name(uint32_t index) {
    if (index < count) return files[index].name;
    if (index == count) return "hello.elf";
    if (index == count + 1u) return "process_test.elf";
    return 0;
}
const char *ramfs_data(const char *name) {
    if (streq(name, "hello.elf")) return (const char *)_binary_hello_elf_start;
    if (streq(name, "process_test.elf")) return (const char *)_binary_process_test_elf_start;
    struct ramfs_file *f = find(name); return f ? f->data : 0;
}
uint32_t ramfs_size(const char *name) {
    if (streq(name, "hello.elf")) return (uint32_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    if (streq(name, "process_test.elf")) return (uint32_t)(_binary_process_test_elf_end - _binary_process_test_elf_start);
    struct ramfs_file *f = find(name); return f ? f->size : 0;
}
int ramfs_exists(const char *name) { return streq(name, "hello.elf") || streq(name, "process_test.elf") || find(name) != 0; }

int ramfs_write(const char *name, const char *data, uint32_t size) {
    if (!name || !*name || !data || streq(name, "hello.elf") || streq(name, "process_test.elf")) return -1;
    struct ramfs_file *f = find(name);
    if (!f) { if (count >= RAMFS_MAX_FILES - 2u) return -1; f = &files[count++]; copy_name(f->name, name); }
    if (size > RAMFS_DATA_MAX) size = RAMFS_DATA_MAX;
    for (uint32_t i = 0; i < size; ++i) f->data[i] = data[i];
    f->size = size; return 0;
}

int ramfs_remove(const char *name) {
    if (streq(name, "hello.elf") || streq(name, "process_test.elf")) return -1;
    for (uint32_t i = 0; i < count; ++i) if (streq(files[i].name, name)) {
        for (uint32_t j = i + 1u; j < count; ++j) files[j - 1u] = files[j];
        --count; return 0;
    }
    return -1;
}
