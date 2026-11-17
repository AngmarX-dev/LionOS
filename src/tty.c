#include <stdint.h>
#include "tty.h"
#include "spinlock.h"
#include "console.h"
#include "process.h"
#include "signal.h"

#define TTY_ECHO_QUEUE_CAPACITY 512u

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
    uint8_t echo[TTY_ECHO_QUEUE_CAPACITY];
    uint32_t echo_count;
    uint32_t attributes;
    uint32_t vmin;
};

static struct tty_console_state console_tty;
static uint8_t tty_read_wait_channel;

static void tty_queue_echo_locked(const uint8_t *bytes, uint32_t length) {
    if ((console_tty.attributes & TTY_ATTR_ECHO) == 0u || !bytes ||
        length > TTY_ECHO_QUEUE_CAPACITY - console_tty.echo_count)
        return;
    for (uint32_t i = 0u; i < length; ++i)
        console_tty.echo[console_tty.echo_count++] = bytes[i];
}

void tty_flush_echo(uint32_t sid) {
    uint8_t pending[TTY_ECHO_QUEUE_CAPACITY];
    uint32_t count = 0u;
    if (!sid)
        return;

    uint32_t output_flags = spinlock_irqsave_acquire(&console_tty.output_lock);
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid == sid) {
        count = console_tty.echo_count;
        for (uint32_t i = 0u; i < count; ++i)
            pending[i] = console_tty.echo[i];
        console_tty.echo_count = 0u;
    }
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);

    uint32_t state_flags = spinlock_irqsave_acquire(&console_tty.lock);
    int owns_tty = console_tty.owner_sid == sid;
    spinlock_irqrestore_release(&console_tty.lock, state_flags);
    if (owns_tty && count != 0u)
        console_write_n((const char *)pending, count);
    spinlock_irqrestore_release(&console_tty.output_lock, output_flags);
}

static void tty_reset_buffers_locked(void) {
    console_tty.input_head = 0u;
    console_tty.input_tail = 0u;
    console_tty.input_count = 0u;
    console_tty.edit_length = 0u;
    console_tty.echo_count = 0u;
}

void tty_init(void) {
    spinlock_init(&console_tty.lock);
    spinlock_init(&console_tty.input_lock);
    spinlock_init(&console_tty.output_lock);
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.lock);
    console_tty.owner_sid = 0u;
    console_tty.foreground_pgid = 0u;
    console_tty.attributes = TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON;
    console_tty.vmin = 1u;
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
        console_tty.attributes = TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON;
        console_tty.vmin = 1u;
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
    console_tty.attributes = TTY_ATTR_ECHO | TTY_ATTR_ISIG | TTY_ATTR_ICANON;
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

    uint32_t sid = console_tty.owner_sid;
    if (byte == 0x03u && (console_tty.attributes & TTY_ATTR_ISIG) != 0u) {
        static const uint8_t interrupt_echo[] = {'^', 'C', '\n'};
        console_tty.edit_length = 0u;
        tty_queue_echo_locked(interrupt_echo, sizeof(interrupt_echo));
        uint32_t state_flags = spinlock_irqsave_acquire(&console_tty.lock);
        uint32_t pgid = console_tty.owner_sid == sid
            ? console_tty.foreground_pgid : 0u;
        spinlock_irqrestore_release(&console_tty.lock, state_flags);
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return process_tty_signal_foreground(sid, pgid, LIONOS_SIG_INT) == 0
            ? TTY_INPUT_INTERRUPT : TTY_ERROR;
    }

    if ((console_tty.attributes & TTY_ATTR_ICANON) == 0u) {
        if (console_tty.input_count >= TTY_INPUT_QUEUE_CAPACITY) {
            spinlock_irqrestore_release(&console_tty.input_lock, flags);
            return TTY_ERROR;
        }
        console_tty.input[console_tty.input_tail] = byte;
        console_tty.input_tail =
            (console_tty.input_tail + 1u) % TTY_INPUT_QUEUE_CAPACITY;
        ++console_tty.input_count;
        tty_queue_echo_locked(&byte, 1u);
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        process_wakeup((uintptr_t)&tty_read_wait_channel);
        return TTY_INPUT_DATA_READY;
    }

    if (byte == 0x08u || byte == 0x7Fu) {
        if (console_tty.edit_length != 0u) {
            --console_tty.edit_length;
            static const uint8_t erase_echo[] = {'\b', ' ', '\b'};
            tty_queue_echo_locked(erase_echo, sizeof(erase_echo));
        }
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
        tty_queue_echo_locked((const uint8_t *)"\n", 1u);
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        process_wakeup((uintptr_t)&tty_read_wait_channel);
        return TTY_INPUT_LINE_READY;
    }

    if (byte < 0x20u && byte != '\t' && byte != 0x03u) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_INPUT_NONE;
    }

    if (console_tty.edit_length >= TTY_EDIT_LINE_CAPACITY - 1u) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }

    console_tty.edit[console_tty.edit_length++] = byte;
    tty_queue_echo_locked(&byte, 1u);
    spinlock_irqrestore_release(&console_tty.input_lock, flags);
    return TTY_INPUT_NONE;
}

int32_t tty_read(uint32_t sid, uint8_t *destination, uint32_t capacity) {
    if (sid == 0u || !destination || capacity == 0u)
        return TTY_ERROR;

    tty_flush_echo(sid);
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

    tty_flush_echo(sid);
    uint32_t flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.input_lock, flags);
        return TTY_ERROR;
    }

    uint32_t required = 1u;
    if ((console_tty.attributes & TTY_ATTR_ICANON) == 0u) {
        required = console_tty.vmin;
        if (required > capacity)
            required = capacity;
    }
    if (console_tty.input_count < required) {
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

int32_t tty_get_attributes(uint32_t sid, uint32_t *flags, uint32_t *vmin) {
    if (sid == 0u || !flags || !vmin)
        return TTY_ERROR;
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid != sid) {
        spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
        return TTY_ERROR;
    }
    *flags = console_tty.attributes;
    *vmin = console_tty.vmin;
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
    return 0;
}

int32_t tty_set_attributes(uint32_t sid, uint32_t flags, uint32_t vmin) {
    if (sid == 0u || (flags & ~TTY_ATTR_SUPPORTED) != 0u || vmin > TTY_VMIN_MAX)
        return TTY_ERROR;
    uint32_t input_flags = spinlock_irqsave_acquire(&console_tty.input_lock);
    if (console_tty.owner_sid != sid ||
        (((flags ^ console_tty.attributes) & TTY_ATTR_ICANON) != 0u &&
         console_tty.edit_length != 0u)) {
        spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
        return TTY_ERROR;
    }
    console_tty.attributes = flags;
    console_tty.vmin = vmin;
    if ((flags & TTY_ATTR_ECHO) == 0u)
        console_tty.echo_count = 0u;
    spinlock_irqrestore_release(&console_tty.input_lock, input_flags);
    return 0;
}
