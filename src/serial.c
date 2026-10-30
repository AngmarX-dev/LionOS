#include <stdint.h>
#include "io.h"
#include "serial.h"

/* 16550-compatible COM1. Polling keeps this independent of PIC/APIC IRQ setup. */
#define UART_DATA(base)       ((uint16_t)((base) + 0u))
#define UART_IER(base)        ((uint16_t)((base) + 1u))
#define UART_FIFO(base)       ((uint16_t)((base) + 2u))
#define UART_LINE(base)       ((uint16_t)((base) + 3u))
#define UART_MODEM(base)      ((uint16_t)((base) + 4u))
#define UART_STATUS(base)     ((uint16_t)((base) + 5u))
#define UART_DLL(base)        ((uint16_t)((base) + 0u))
#define UART_DLM(base)        ((uint16_t)((base) + 1u))

#define UART_LSR_DATA_READY   0x01u
#define UART_LSR_OVERRUN      0x02u
#define UART_LSR_PARITY       0x04u
#define UART_LSR_FRAMING      0x08u
#define UART_LSR_BREAK        0x10u
#define UART_LSR_TX_EMPTY     0x20u
#define UART_POLL_LIMIT       100000u
#define UART_LOOPBACK_TEST    0xAEu

static volatile uint8_t uart_ready;
static volatile uint8_t previous_was_cr;
static volatile uint32_t rx_bytes;
static volatile uint32_t tx_bytes;
static volatile uint32_t io_errors;

static void uart_raw_putc(uint8_t value) {
    if (!uart_ready) return;
    for (uint32_t i = 0u; i < UART_POLL_LIMIT; ++i) {
        if (inb(UART_STATUS(SERIAL_COM1_BASE)) & UART_LSR_TX_EMPTY) {
            outb(UART_DATA(SERIAL_COM1_BASE), value);
            ++tx_bytes;
            return;
        }
    }
    ++io_errors;
}

int serial_init(void) {
    uart_ready = 0u;
    previous_was_cr = 0u;
    rx_bytes = 0u;
    tx_bytes = 0u;
    io_errors = 0u;

    /* Disable UART interrupts before touching divisor and line-control registers. */
    outb(UART_IER(SERIAL_COM1_BASE), 0x00u);
    outb(UART_LINE(SERIAL_COM1_BASE), 0x80u);
    outb(UART_DLL(SERIAL_COM1_BASE), 0x01u); /* 115200 baud from 1.8432 MHz clock */
    outb(UART_DLM(SERIAL_COM1_BASE), 0x00u);
    outb(UART_LINE(SERIAL_COM1_BASE), 0x03u); /* 8 data bits, no parity, 1 stop bit */
    outb(UART_FIFO(SERIAL_COM1_BASE), 0xC7u);

    /*
     * Verify a real 16550-compatible UART using its internal loopback.
     * An absent legacy COM1 normally reads as 0xFF and will fail this test.
     * Do not enable IRQs; the driver is deliberately polled.
     */
    outb(UART_MODEM(SERIAL_COM1_BASE), 0x1Eu);
    outb(UART_DATA(SERIAL_COM1_BASE), UART_LOOPBACK_TEST);

    uint8_t received = 0u;
    uint8_t got_byte = 0u;
    for (uint32_t i = 0u; i < UART_POLL_LIMIT; ++i) {
        uint8_t status = inb(UART_STATUS(SERIAL_COM1_BASE));
        if (status == 0xFFu) break;
        if (status & UART_LSR_DATA_READY) {
            received = inb(UART_DATA(SERIAL_COM1_BASE));
            got_byte = 1u;
            break;
        }
    }

    /* Leave the UART in normal operation, with DTR/RTS and OUT2 asserted. */
    outb(UART_MODEM(SERIAL_COM1_BASE), 0x0Bu);
    if (!got_byte || received != UART_LOOPBACK_TEST) {
        uart_ready = 0u;
        return -1;
    }

    uart_ready = 1u;
    return 0;
}

int serial_available(void) {
    return uart_ready != 0u;
}

void serial_putc(char c) {
    if (!uart_ready) return;
    if (c == '\n') uart_raw_putc('\r');
    uart_raw_putc((uint8_t)c);
}

int serial_getchar(void) {
    if (!uart_ready) return -1;

    uint8_t status = inb(UART_STATUS(SERIAL_COM1_BASE));
    if (status == 0xFFu) {
        uart_ready = 0u;
        return -1;
    }
    if (!(status & UART_LSR_DATA_READY)) return -1;

    uint8_t value = inb(UART_DATA(SERIAL_COM1_BASE));
    if (status & (UART_LSR_OVERRUN | UART_LSR_PARITY |
                  UART_LSR_FRAMING | UART_LSR_BREAK)) {
        ++io_errors;
        return -1;
    }

    ++rx_bytes;
    if (value == '\r') {
        previous_was_cr = 1u;
        return '\n';
    }
    if (value == '\n') {
        if (previous_was_cr) {
            previous_was_cr = 0u;
            return -1;
        }
        return '\n';
    }

    previous_was_cr = 0u;
    if (value == 0x7Fu) return '\b';
    return (int)value;
}

uint32_t serial_rx_count(void) { return rx_bytes; }
uint32_t serial_tx_count(void) { return tx_bytes; }
uint32_t serial_error_count(void) { return io_errors; }
