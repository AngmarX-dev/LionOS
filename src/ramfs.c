#include <stdint.h>
#include "ramfs.h"

#define RAMFS_MAX_FILES 32u
#define RAMFS_NAME_MAX 64u
#define RAMFS_DATA_MAX 4096u
#define RAMFS_EMBEDDED_COUNT 8u

struct ramfs_file {
    char name[RAMFS_NAME_MAX];
    char data[RAMFS_DATA_MAX];
    uint32_t size;
};

struct ramfs_embedded_file {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};

extern const uint8_t _binary_hello_elf_start[], _binary_hello_elf_end[];
extern const uint8_t _binary_process_test_elf_start[], _binary_process_test_elf_end[];
extern const uint8_t _binary_ipc_test_elf_start[], _binary_ipc_test_elf_end[];
extern const uint8_t _binary_pipe_test_elf_start[], _binary_pipe_test_elf_end[];
extern const uint8_t _binary_signal_test_elf_start[], _binary_signal_test_elf_end[];
extern const uint8_t _binary_net_test_elf_start[], _binary_net_test_elf_end[];
extern const uint8_t _binary_vfs_test_elf_start[], _binary_vfs_test_elf_end[];
extern const uint8_t _binary_cow_test_elf_start[], _binary_cow_test_elf_end[];

static const struct ramfs_embedded_file embedded_files[RAMFS_EMBEDDED_COUNT] = {
    { "hello.elf", _binary_hello_elf_start, _binary_hello_elf_end },
    { "process_test.elf", _binary_process_test_elf_start, _binary_process_test_elf_end },
    { "ipc_test.elf", _binary_ipc_test_elf_start, _binary_ipc_test_elf_end },
    { "pipe_test.elf", _binary_pipe_test_elf_start, _binary_pipe_test_elf_end },
    { "signal_test.elf", _binary_signal_test_elf_start, _binary_signal_test_elf_end },
    { "net_test.elf", _binary_net_test_elf_start, _binary_net_test_elf_end },
    { "vfs_test.elf", _binary_vfs_test_elf_start, _binary_vfs_test_elf_end },
    { "cow_test.elf", _binary_cow_test_elf_start, _binary_cow_test_elf_end }
};

static struct ramfs_file files[RAMFS_MAX_FILES];
static uint32_t count;

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static const struct ramfs_embedded_file *find_embedded(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < RAMFS_EMBEDDED_COUNT; ++i)
        if (streq(name, embedded_files[i].name)) return &embedded_files[i];
    return 0;
}

static struct ramfs_file *find(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (streq(files[i].name, name)) return &files[i];
    return 0;
}

static void copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    while (src[i] && i < RAMFS_NAME_MAX - 1u) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

void ramfs_init(void) {
    count = 0;
    (void)ramfs_write("readme.txt",
        "Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n", 61u);
    (void)ramfs_write("version", "LionOS 1.0.0\n", 13u);
    (void)ramfs_write("motd", "The LionOS kernel is alive.\n", 28u);
}

uint32_t ramfs_count(void) {
    return count + RAMFS_EMBEDDED_COUNT;
}

const char *ramfs_name(uint32_t index) {
    if (index < count) return files[index].name;
    index -= count;
    if (index < RAMFS_EMBEDDED_COUNT) return embedded_files[index].name;
    return 0;
}

const char *ramfs_data(const char *name) {
    const struct ramfs_embedded_file *embedded = find_embedded(name);
    if (embedded) return (const char *)embedded->start;
    struct ramfs_file *file = find(name);
    return file ? file->data : 0;
}

uint32_t ramfs_size(const char *name) {
    const struct ramfs_embedded_file *embedded = find_embedded(name);
    if (embedded) return (uint32_t)(embedded->end - embedded->start);
    struct ramfs_file *file = find(name);
    return file ? file->size : 0u;
}

int ramfs_exists(const char *name) {
    return find_embedded(name) != 0 || find(name) != 0;
}

int ramfs_write(const char *name, const char *data, uint32_t size) {
    if (!name || !*name || !data || find_embedded(name)) return -1;

    struct ramfs_file *file = find(name);
    if (!file) {
        if (count >= RAMFS_MAX_FILES - RAMFS_EMBEDDED_COUNT) return -1;
        file = &files[count++];
        copy_name(file->name, name);
    }

    if (size > RAMFS_DATA_MAX) size = RAMFS_DATA_MAX;
    for (uint32_t i = 0; i < size; ++i) file->data[i] = data[i];
    file->size = size;
    return 0;
}

int ramfs_remove(const char *name) {
    if (find_embedded(name)) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (!streq(files[i].name, name)) continue;
        for (uint32_t j = i + 1u; j < count; ++j)
            files[j - 1u] = files[j];
        --count;
        return 0;
    }
    return -1;
}
