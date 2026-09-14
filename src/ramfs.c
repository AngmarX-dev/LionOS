#include <stdint.h>
#include "ramfs.h"

#define RAMFS_MAX_FILES 16u
#define RAMFS_NAME_MAX 16u
#define RAMFS_DATA_MAX 256u

struct ramfs_file { char name[RAMFS_NAME_MAX]; char data[RAMFS_DATA_MAX]; uint32_t size; };
static struct ramfs_file files[RAMFS_MAX_FILES];
static uint32_t count;

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void ramfs_init(void) {
    count = 2;
    const char *n0 = "readme.txt"; const char *d0 = "Welcome to LionOS.\nBuilt as an experimental 32-bit x86 OS.\n";
    const char *n1 = "version"; const char *d1 = "LionOS 0.2\n";
    for (uint32_t i = 0; i < RAMFS_NAME_MAX - 1u && n0[i]; ++i) files[0].name[i] = n0[i];
    for (uint32_t i = 0; i < RAMFS_DATA_MAX && d0[i]; ++i) files[0].data[i] = d0[i];
    files[0].size = 61u;
    for (uint32_t i = 0; i < RAMFS_NAME_MAX - 1u && n1[i]; ++i) files[1].name[i] = n1[i];
    for (uint32_t i = 0; i < RAMFS_DATA_MAX && d1[i]; ++i) files[1].data[i] = d1[i];
    files[1].size = 12u;
}

static struct ramfs_file *find(const char *name) {
    for (uint32_t i = 0; i < count; ++i) if (streq(files[i].name, name)) return &files[i];
    return 0;
}

uint32_t ramfs_count(void) { return count; }
const char *ramfs_name(uint32_t index) { return index < count ? files[index].name : 0; }
const char *ramfs_data(const char *name) { struct ramfs_file *f = find(name); return f ? f->data : 0; }
uint32_t ramfs_size(const char *name) { struct ramfs_file *f = find(name); return f ? f->size : 0; }

int ramfs_write(const char *name, const char *data, uint32_t size) {
    struct ramfs_file *f = find(name);
    if (!f) {
        if (count >= RAMFS_MAX_FILES) return -1;
        f = &files[count++];
        uint32_t i = 0; while (name[i] && i < RAMFS_NAME_MAX - 1u) { f->name[i] = name[i]; ++i; }
        f->name[i] = 0;
    }
    if (size > RAMFS_DATA_MAX) size = RAMFS_DATA_MAX;
    for (uint32_t i = 0; i < size; ++i) f->data[i] = data[i];
    f->size = size;
    return 0;
}
