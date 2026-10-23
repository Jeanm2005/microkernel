/* The console server: the user-space driver for the COM1 serial port.
 *
 * It owns the UART (universal asynchronous receiver-transmitter): the
 * root task gave it COM1's I/O ports and interrupt line, and nobody else
 * in user space can touch them. Clients send it text through badged
 * capabilities; it prints each client's text a line at a time, prefixed
 * with the name the root task registered for that badge. Typed input
 * arrives by interrupt: IRQ 4 signals our notification, which is bound to
 * this thread, so the same recv that waits for clients also wakes up for
 * the device.
 *
 * Output is written by polling the transmitter (a few microseconds per
 * byte at 115200 baud on real hardware, instant under QEMU). An
 * interrupt-driven transmit queue would only pay off for much more
 * output than a debug console produces. */
#include <stdbool.h>
#include <stdint.h>
#include <abi/boot.h>
#include <abi/console.h>
#include "io.h"
#include "ipc.h"
#include "ulib.h"

/* Our capabilities, as the manifest line "devices: com1" lays them out. */
#define COM1_PORTS  SLOT_DEVICES
#define COM1_IRQ    (SLOT_DEVICES + 1)
#define IRQ_BIT     1   /* the bit IRQ 4 sets in our notification */

/* 16550 UART registers, as offsets from the base port. */
#define COM1        0x3f8
#define UART_DATA   0
#define UART_IER    1          /* interrupt enable */
#define UART_LSR    5          /* line status */
#define IER_RX      0x01       /* interrupt when a byte arrives */
#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY  0x20

#define LINE_MAX 120

static struct {
    char   name[CONSOLE_NAME_MAX + 1];
    char   text[LINE_MAX + 1];
    size_t len;
} clients[CONSOLE_MAX_CLIENTS];

static char     input[LINE_MAX + 1];
static size_t   input_len;
static uint64_t input_lines, interrupts, lines_out;

/* ---- the device ---------------------------------------------------------- */

static void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY))
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* A line of our own. */
__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uart_puts("[console] ");
    uart_puts(buf);
    uart_putc('\n');
}

/* IRQ 4 fired: take every byte the UART has, then let the next interrupt
 * through. */
static void handle_interrupt(void)
{
    interrupts++;
    while (inb(COM1 + UART_LSR) & LSR_DATA_READY) {
        char c = (char)inb(COM1 + UART_DATA);
        if (c == '\r' || c == '\n') {
            if (input_len) {
                input[input_len] = '\0';
                say("input: %s", input);
                input_lines++;
                input_len = 0;
            }
        } else if (input_len < LINE_MAX) {
            input[input_len++] = c;
        }
    }
    sys_irq_ack(COM1_IRQ);
}

/* ---- the protocol -------------------------------------------------------- */

static void flush(long badge)
{
    if (!clients[badge].len)
        return;
    char prefix[40];
    if (clients[badge].name[0])
        snprintf(prefix, sizeof prefix, "[%s] ", clients[badge].name);
    else
        snprintf(prefix, sizeof prefix, "[client %ld] ", badge);
    clients[badge].text[clients[badge].len] = '\0';
    uart_puts(prefix);
    uart_puts(clients[badge].text);
    uart_putc('\n');
    clients[badge].len = 0;
    lines_out++;
}

static long write(long badge, long w0, const long *words)
{
    long len = (w0 >> 8) & 0xff;
    if (len > CONSOLE_WRITE_MAX)
        return -ERR_INVAL;
    if (badge < 0 || badge >= CONSOLE_MAX_CLIENTS)
        return -ERR_INVAL;   /* more clients than we have line buffers for */
    const char *bytes = (const char *)words;
    for (long i = 0; i < len; i++) {
        if (bytes[i] == '\n') {
            flush(badge);
            continue;
        }
        clients[badge].text[clients[badge].len++] = bytes[i];
        if (clients[badge].len == LINE_MAX)
            flush(badge);   /* an over-long line is split, not dropped */
    }
    return len;
}

static long set_name(long badge, long w0, const long *words)
{
    long target = (w0 >> 8) & 0xff;
    if (badge != 0)
        return -ERR_PERM;   /* only the root task names clients */
    if (target >= CONSOLE_MAX_CLIENTS)
        return -ERR_INVAL;
    memcpy(clients[target].name, words, CONSOLE_NAME_MAX);
    clients[target].name[CONSOLE_NAME_MAX] = '\0';
    return 0;
}

int main(void)
{
    /* Take over the device. Without output we can't report a failure
     * here, so the exit code says which step failed. */
    if (sys_ioport_enable(COM1_PORTS))
        return 2;
    if (sys_ntfn_bind(SLOT_EVENTS))
        return 3;
    if (sys_irq_bind(COM1_IRQ, SLOT_EVENTS, IRQ_BIT))
        return 4;
    outb(COM1 + UART_IER, IER_RX);
    say("ready: driving COM1 from user space, input by IRQ 4");

    struct msg m;
    long reply[4];
    bool replying = false;
    for (;;) {
        long r = replying ? sys_reply_recv(SLOT_SERVE, reply, 0, &m)
                          : sys_recv(SLOT_SERVE, &m);
        replying = false;
        if (r == IPC_NOTIFIED) {
            if (m.w[0] & IRQ_BIT)
                handle_interrupt();
            continue;
        }
        if (r) {
            say("FAILED: receive returned %ld", r);
            return 1;
        }

        memset(reply, 0, sizeof reply);
        switch (PROTO_OP(m.w[0])) {
        case CONSOLE_WRITE:
            reply[0] = write(m.badge, m.w[0], &m.w[1]);
            break;
        case CONSOLE_SET_NAME:
            reply[0] = set_name(m.badge, m.w[0], &m.w[1]);
            break;
        case CONSOLE_STATS:
            reply[0] = (long)input_lines;
            reply[1] = (long)interrupts;
            break;

        case PROTO_SHUTDOWN:
            if (m.badge != 0) {   /* only the root task holds badge 0 */
                reply[0] = -ERR_PERM;
                break;
            }
            for (long b = 0; b < CONSOLE_MAX_CLIENTS; b++)
                flush(b);
            outb(COM1 + UART_IER, 0);
            say("shutting down (%lu lines written, %lu interrupts, %lu input lines)",
                lines_out, interrupts, input_lines);
            sys_reply(reply, 0);
            return 0;

        default:
            reply[0] = -ERR_NOSYS;
        }
        replying = true;
    }
}
