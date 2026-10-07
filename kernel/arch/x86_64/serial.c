#include <arch/cpu.h>
#include <arch/serial.h>

#define COM1 0x3f8

enum {
    UART_DATA = 0, UART_IER = 1, UART_FCR = 2, UART_LCR = 3,
    UART_MCR = 4, UART_LSR = 5,
};
#define LSR_THR_EMPTY 0x20

static int serial_ok;

void serial_init(void)
{
    outb(COM1 + UART_IER, 0x00);   /* no interrupts: we poll */
    outb(COM1 + UART_LCR, 0x80);   /* DLAB on to set the baud divisor */
    outb(COM1 + UART_DATA, 0x01);  /* divisor 1 = 115200 baud */
    outb(COM1 + UART_IER, 0x00);
    outb(COM1 + UART_LCR, 0x03);   /* 8 data bits, no parity, 1 stop */
    outb(COM1 + UART_FCR, 0xc7);   /* FIFOs on, cleared, 14-byte threshold */

    /* Loopback self-test so we don't spin forever on a missing UART. */
    outb(COM1 + UART_MCR, 0x1e);
    outb(COM1 + UART_DATA, 0xae);
    if (inb(COM1 + UART_DATA) != 0xae)
        return;
    outb(COM1 + UART_MCR, 0x0f);   /* normal mode: DTR, RTS, OUT1, OUT2 */
    serial_ok = 1;
}

void serial_putc(char c)
{
    if (!serial_ok)
        return;
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY))
        cpu_relax();
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}