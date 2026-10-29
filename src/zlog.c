#include <stdint.h>
#include "kernel_diag.h"
#include "diskfs.h"
#include "vfs.h"
#include "spinlock.h"
#include "debug.h"

#define KLOG_FILE_PATH "/var/log/lionos.log"
#define KLOG_VFS_READ 1u
#define KLOG_VFS_WRITE 2u
#define KLOG_VFS_APPEND 4u
#define KLOG_LINE_CAPACITY 128u

static struct spinlock persistence_lock;
static volatile uint32_t persistence_ready;
static volatile uint32_t persistence_failure_reported;
static uint8_t persistence_io_buffer[DISKFS_MAX_FILE_SIZE];

static const char *persistent_level_name(enum kernel_log_level level) {
    switch (level) {
        case KLOG_DEBUG: return "DEBUG";
        case KLOG_INFO: return "INFO";
        case KLOG_WARN: return "WARN";
        case KLOG_ERROR: return "ERROR";
        case KLOG_PANIC: return "PANIC";
        default: return "UNKNOWN";
    }
}

static void report_persistence_failure(void) {
    if (__atomic_exchange_n(&persistence_failure_reported, 1u,
                            __ATOMIC_ACQ_REL) == 0u)
        debug_write("LIONOS:PERSIST-LOG-WRITE-FAILED\\n");
}

static uint32_t format_log_line(enum kernel_log_level level,
                                const char *message,
                                char *buffer, uint32_t capacity) {
    uint32_t n = 0u;
    const char *tag = persistent_level_name(level);
    if (!buffer || capacity < 2u) return 0u;
    while (tag[n] && n + 1u < capacity) {
        buffer[n] = tag[n];
        ++n;
    }
    if (n + 2u < capacity) {
        buffer[n++] = ':';
        buffer[n++] = ' ';
    }
    uint32_t i = 0u;
    while (message && message[i] && n + 2u < capacity) {
        buffer[n++] = message[i++];
    }
    if (n + 1u < capacity) buffer[n++] = '\n';
    buffer[n] = 0;
    return n;
}

static int contains_bytes(const uint8_t *buffer, uint32_t length,
                          const char *needle) {
    if (!buffer || !needle || !*needle) return 0;
    uint32_t needle_len = 0u;
    while (needle[needle_len]) ++needle_len;
    if (needle_len > length) return 0;
    for (uint32_t i = 0u; i + needle_len <= length; ++i) {
        uint32_t j = 0u;
        while (j < needle_len && buffer[i + j] == (uint8_t)needle[j]) ++j;
        if (j == needle_len) return 1;
    }
    return 0;
}

static int ensure_log_file(struct vfs_stat *st) {
    if (vfs_stat_path(KLOG_FILE_PATH, st) == 0) return 0;
    int fd = vfs_open(KLOG_FILE_PATH, KLOG_VFS_WRITE | KLOG_VFS_APPEND);
    if (fd < 0) return -1;
    (void)vfs_close(fd);
    return vfs_stat_path(KLOG_FILE_PATH, st);
}

/* Called with persistence_lock held. Keep the file bounded to its backend's limit. */
static int append_log_file_locked(const char *line, uint32_t length) {
    struct vfs_stat st;
    if (!line || !length || ensure_log_file(&st) != 0) return -1;

    uint32_t limit = st.backend == VFS_BACKEND_DISKFS
        ? DISKFS_MAX_FILE_SIZE : 256u;
    if (length > limit) return -1;

    if (st.size + length > limit) {
        int fd = vfs_open(KLOG_FILE_PATH, KLOG_VFS_READ);
        if (fd < 0) return -1;
        int read_count = st.size
            ? vfs_read(fd, persistence_io_buffer, st.size) : 0;
        (void)vfs_close(fd);
        if (read_count < 0) return -1;

        uint32_t keep = limit > 128u ? limit - 128u : limit / 2u;
        uint32_t start = (uint32_t)read_count > keep
            ? (uint32_t)read_count - keep : 0u;
        if (start) {
            while (start < (uint32_t)read_count &&
                   persistence_io_buffer[start] != '\n') ++start;
            if (start < (uint32_t)read_count) ++start;
        }

        if (vfs_remove(KLOG_FILE_PATH) != 0) return -1;
        fd = vfs_open(KLOG_FILE_PATH, KLOG_VFS_WRITE);
        if (fd < 0) return -1;
        uint32_t retained = (uint32_t)read_count - start;
        int written = retained
            ? vfs_write(fd, persistence_io_buffer + start, retained) : 0;
        (void)vfs_close(fd);
        if (written < 0 || (uint32_t)written != retained) return -1;

        if (vfs_stat_path(KLOG_FILE_PATH, &st) != 0) return -1;
        limit = st.backend == VFS_BACKEND_DISKFS
            ? DISKFS_MAX_FILE_SIZE : 256u;
        if (st.size + length > limit) return -1;
    }

    int fd = vfs_open(KLOG_FILE_PATH, KLOG_VFS_WRITE | KLOG_VFS_APPEND);
    if (fd < 0) return -1;
    int written = vfs_write(fd, line, length);
    (void)vfs_close(fd);
    return written == (int)length ? 0 : -1;
}

void kernel_log_persistence_write(enum kernel_log_level level,
                                  const char *message) {
    if (!__atomic_load_n(&persistence_ready, __ATOMIC_ACQUIRE)) return;

    char line[KLOG_LINE_CAPACITY];
    uint32_t length = format_log_line(level, message, line, sizeof(line));
    if (!length) return;

    uint32_t flags = spinlock_irqsave_acquire(&persistence_lock);
    if (__atomic_load_n(&persistence_ready, __ATOMIC_ACQUIRE) &&
        append_log_file_locked(line, length) != 0)
        report_persistence_failure();
    spinlock_irqrestore_release(&persistence_lock, flags);
}

int kernel_log_persistence_init(void) {
    struct vfs_stat st;
    int previous_session_complete = 0;

    spinlock_init(&persistence_lock);
    __atomic_store_n(&persistence_ready, 0u, __ATOMIC_RELEASE);

    if (vfs_stat_path(KLOG_FILE_PATH, &st) == 0 &&
        st.backend == VFS_BACKEND_DISKFS && st.size) {
        int fd = vfs_open(KLOG_FILE_PATH, KLOG_VFS_READ);
        if (fd >= 0) {
            int n = vfs_read(fd, persistence_io_buffer, st.size);
            (void)vfs_close(fd);
            if (n > 0) {
                previous_session_complete = contains_bytes(
                    persistence_io_buffer, (uint32_t)n,
                    "LionOS boot completed; starting desktop");
            }
        }
    }

    if (ensure_log_file(&st) != 0) return -1;

    uint32_t count = kernel_log_entry_count();
    uint32_t flags = spinlock_irqsave_acquire(&persistence_lock);
    __atomic_store_n(&persistence_ready, 1u, __ATOMIC_RELEASE);

    static const char session_line[] = "\n----- LionOS boot session -----\n";
    if (append_log_file_locked(session_line,
                               (uint32_t)(sizeof(session_line) - 1u)) != 0)
        report_persistence_failure();

    for (uint32_t i = 0u; i < count; ++i) {
        enum kernel_log_level entry_level = KLOG_INFO;
        char message[96];
        char line[KLOG_LINE_CAPACITY];
        if (kernel_log_read(i, &entry_level, message, sizeof(message)) != 0)
            continue;
        uint32_t length = format_log_line(entry_level, message,
                                           line, sizeof(line));
        if (length && append_log_file_locked(line, length) != 0)
            report_persistence_failure();
    }
    spinlock_irqrestore_release(&persistence_lock, flags);

    if (previous_session_complete)
        debug_write("LIONOS:PERSIST-LOG-OK\n");
    if (st.backend == VFS_BACKEND_DISKFS)
        debug_write("LIONOS:PERSIST-LOG-DISK-READY\n");
    else
        debug_write("LIONOS:PERSIST-LOG-RAM-ONLY\n");
    debug_write("LIONOS:PERSIST-LOG-READY\n");
    return 0;
}
