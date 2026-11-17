#include <stdint.h>
#include "tty.h"
#include "spinlock.h"
#include "console.h"
#include "process.h"
#include "signal.h"

struct tty_console_state {
    struct spinlock lock;
    struct spinlock input_lock;
    struct spinlock output_lock;
    uint32_t owner_sid;
    uint32_t foreground_pgid;
    uint8_t input[TTY_INPUT_QUEUE_CAPACITY];
    uint32_t input_head;
    uint32_t input_tail;
    uint32_t input_count;
    uint8_t edit[TTY_EDIT_LINE_CAPACITY];
    uint32_t edit_length;
};

static struct tty_console_state console_tty;
static uint8_t tty_read_wait_channel;

static void tty_reset_buffers_locked(void) {
    console_tty.input_head = 0u;
    console_tty.input_tail = 0u;
    console_tty.input_count = 0u;
    console_tty.edit_length = 0u;
}

void tty_init(void) {
    spinlock_init(&console_tty.lock);
    spinlock_init(&console_tty.input_lock);
    spinlock_init(&console_tty.output_lock);
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    console_tty.owner_sid = 0u;
    console_tty.foreground_pgid = 0u;
    tty_reset_buffers_locked();
    spinlock_irqrestore_release(&console_tty.lock, flags);
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
}

int32_t tty_claim_session(uint32_t sid, uint32_t leader_pid) {
    if (sid == 0u || leader_pid == 0u || sid != leader_pid)
        return TTY_ERROR;

    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    if (console_tty.owner_sid != 0u && console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.lock, flags);
        spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
        return TTY_ERROR;
    }

    if (console_tty.owner_sid == 0u) {
        console_tty.owner_sid = sid;
        console_tty.foreground_pgid = sid;
        tty_reset_buffers_locked();
    }
    spinlock_irqrestore_release(&console_tty.lock, flags);
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
    return 0;
}

int32_t tty_release_session(uint32_t sid) {
    if (sid == 0u)
        return TTY_ERROR;

    uint32_t output_flags = spinlock_irqsave_acquire(&console_tty.output_lock);
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.lock, flags);
        spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
        spinlock_irqrestore_release(&console_tty.output_lock, output_flags);
        return TTY_ERROR;
    }

    console_tty.owner_sid = 0u;
    console_tty.foreground_pgid = 0u;
    tty_reset_buffers_locked();
    spinlock_irqrestore_release(&console_tty.lock, flags);
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
    spinlock_irqrestore_release(&console_tty.output_lock, output_flags);
    process_wakeup((uintptr_t)&tty_read_wait_channel);
    return 0;
}

uint32_t tty_session_owner(void) {
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    uint32_t sid = console_tty.owner_sid;
    spinlock_irqrestore_release(&console_tty.lock, flags);
    return sid;
}

int32_t tty_get_foreground_pgid(uint32_t sid) {
    if (sid == 0u)
        return TTY_ERROR;

    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    int32_t result = console_tty.owner_sid == sid
        ? (int32_t)console_tty.foreground_pgid : TTY_ERROR;
    spinlock_irqrestore_release(&console_tty.lock, flags);
    return result;
}

int32_t tty_set_foreground_pgid(uint32_t sid, uint32_t pgid) {
    if (sid == 0u || pgid == 0u)
        return TTY_ERROR;

    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.lock, flags);
        return TTY_ERROR;
    }
    console_tty.foreground_pgid = pgid;
    spinlock_irqrestore_release(&console_tty.lock, flags);
    return 0;
}

int32_t tty_input_byte(uint8_t byte) {
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid == 0u) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }

    if (byte == 0x03u) {
        uint32_t sid = console_tty.owner_sid;
        console_tty.edit_length = 0u;
        uint32_t state_flags = spinlock_irqsave_acquire(&console_tty.lock);
        uint32_t pgid = console_tty.owner_sid == sid
            ? console_tty.foreground_pgid : 0u;
        spinlock_irqrestore_release(&console_tty.lock, state_flags);
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return process_tty_signal_foreground(sid, pgid, LIONOS_SIG_INT) == 0
            ? TTY_INPUT_INTERRUPT : TTY_ERROR;
    }

    if (byte == 0x08u || byte == 0x7Fu) {
        if (console_tty.edit_length != 0u)
            --console_tty.edit_length;
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_INPUT_NONE;
    }

    if (byte == '\r')
        byte = '\n';

    if (byte == '\n') {
        uint32_t required = console_tty.edit_length + 1u;
        uint32_t free_space = TTY_INPUT_QUEUE_CAPACITY - console_tty.input_count;
        if (required > free_space) {
            spinlock_irqrestore_release(&console_tty.input_lock, flags);
            return TTY_ERROR;
        }

        for (uint32_t i = 0u; i < console_tty.edit_length; ++i) {
            console_tty.input[console_tty.input_tail] = console_tty.edit[i];
            console_tty.input_tail =
                (console_tty.input_tail + 1u) % TTY_INPUT_QUEUE_CAPACITY;
        }
        console_tty.input[console_tty.input_tail] = '\n';
        console_tty.input_tail =
            (console_tty.input_tail + 1u) % TTY_INPUT_QUEUE_CAPACITY;
        console_tty.input_count += required;
        console_tty.edit_length = 0u;
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        process_wakeup((uintptr_t)&tty_read_wait_channel);
        return TTY_INPUT_LINE_READY;
    }

    if (byte < 0x20u && byte != '\t') {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_INPUT_NONE;
    }

    if (console_tty.edit_length >= TTY_EDIT_LINE_CAPACITY - 1u) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }

    console_tty.edit[console_tty.edit_length++] = byte;
    spinlock_irqrestore_release(&console_tty.input_lock, flags);
    return TTY_INPUT_NONE;
}

int32_t tty_read(uint32_t sid, uint8_t *destination, uint32_t capacity) {
    if (sid == 0u || !destination || capacity == 0u)
        return TTY_ERROR;

    uint32_t flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }
    if (console_tty.input_count == 0u) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_READ_EMPTY;
    }

    uint32_t copied = 0u;
    while (copied < capacity && console_tty.input_count != 0u) {
        destination[copied++] = console_tty.input[console_tty.input_head];
        console_tty.input_head =
            (console_tty.input_head + 1u) % TTY_INPUT_QUEUE_CAPACITY;
        --console_tty.input_count;
    }
    spinlock_irqrestore_release(&console_tty.input_lock, flags);
    return (int32_t)copied;
}

int32_t tty_read_blocking(uint32_t sid, uint8_t *destination, uint32_t capacity) {
    if (sid == 0u || !destination || capacity == 0u)
        return TTY_ERROR;

    uint32_t flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }

    if (console_tty.input_count == 0u) {
        int32_t blocked = process_sleep_on(
            (uintptr_t)&tty_read_wait_channel, &console_tty.input_lock, flags);
        return blocked == PROCESS_WAIT_BLOCKED ? TTY_READ_BLOCKED : TTY_ERROR;
    }

    uint32_t copied = 0u;
    while (copied < capacity && console_tty.input_count != 0u) {
        destination[copied++] = console_tty.input[console_tty.input_head];
        console_tty.input_head =
            (console_tty.input_head + 1u) % TTY_INPUT_QUEUE_CAPACITY;
        --console_tty.input_count;
    }
    spinlock_irqrestore_release(&console_tty.input_lock, flags);
    return (int32_t)copied;
}

int32_t tty_write(uint32_t sid, const uint8_t *source, uint32_t length) {
    if (sid == 0u || (!source && length != 0u) || length > 4096u)
        return TTY_ERROR;
    uint32_t output_flags = spinlock_irqsave_acquire(&console_tty.output_lock);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    int owns_tty = console_tty.owner_sid == sid;
    spinlock_irqrestore_release(&console_tty.lock, flags);
    if (!owns_tty) {
        spinlock_irqrestore_release(&console_tty.output_lock, output_flags);
        return TTY_ERROR;
    }
    if (length != 0u)
        console_write_n((const char *)source, length);
    spinlock_irqrestore_release(&console_tty.output_lock, output_flags);
    return (int32_t)length;
}
