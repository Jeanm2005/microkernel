/* The PCI (Peripheral Component Interconnect) server.
 *
 * It owns PCI configuration space, the I/O-port range where firmware puts
 * devices' registers, and the right to claim interrupt lines. At start it
 * walks every bus, device and function and prints what it finds. After
 * that, drivers ask it for their device: one client may claim a device,
 * and only that client gets the device's resources, each as a capability
 * covering exactly that device's ports or interrupt line. No driver ever
 * holds more than its own device.
 *
 * Configuration space is reached through the legacy port pair 0xcf8
 * (address) / 0xcfc (data), which every PC chipset still supports and
 * which covers the first 256 bytes of each device's configuration. The
 * memory-mapped ECAM (enhanced configuration access mechanism) would give
 * the full 4 KiB, but needs the address from ACPI's MCFG table and
 * memory-mapped device access; nothing here needs it yet. */
#include <stdbool.h>
#include <stdint.h>
#include <abi/boot.h>
#include <abi/pci.h>
#include "io.h"
#include "ipc.h"
#include "ulib.h"

#define CONSOLE      SLOT_USES
#define CFG_PORTS    SLOT_DEVICES          /* 0xcf8-0xcff */
#define IO_WINDOW    (SLOT_DEVICES + 1)    /* 0x1000-0xffff */
#define IRQ_CONTROL  (SLOT_DEVICES + 2)

#define CFG_ADDRESS  0xcf8
#define CFG_DATA     0xcfc

/* Configuration space registers (the standard type-0 header). */
#define CFG_VENDOR      0x00
#define CFG_COMMAND     0x04
#define CFG_CLASS       0x08   /* revision, prog-if, subclass, class */
#define CFG_HEADER      0x0e
#define CFG_BAR0        0x10
#define CFG_IRQ_LINE    0x3c

#define CMD_IO_SPACE    0x0001
#define CMD_BUS_MASTER  0x0004

#define MAX_DEVICES 32

static struct device {
    uint16_t addr, vendor, device;
    uint8_t  class, subclass;
    long     owner;        /* badge + 1 of the client that claimed it, 0 = nobody */
    long     irq_cap;      /* our IRQ capability, once made (a line is claimed once) */
} devs[MAX_DEVICES];
static int ndevs;

/* ---- configuration space ------------------------------------------------ */

static uint32_t cfg_read32(uint16_t addr, uint8_t reg)
{
    outl(CFG_ADDRESS, 0x80000000u | ((uint32_t)addr << 8) | (reg & 0xfc));
    return inl(CFG_DATA);
}

static void cfg_write32(uint16_t addr, uint8_t reg, uint32_t val)
{
    outl(CFG_ADDRESS, 0x80000000u | ((uint32_t)addr << 8) | (reg & 0xfc));
    outl(CFG_DATA, val);
}

static uint16_t cfg_read16(uint16_t addr, uint8_t reg)
{
    return (uint16_t)(cfg_read32(addr, reg) >> ((reg & 2) * 8));
}

static uint8_t cfg_read8(uint16_t addr, uint8_t reg)
{
    return (uint8_t)(cfg_read32(addr, reg) >> ((reg & 3) * 8));
}

static const char *class_name(uint8_t class, uint8_t subclass)
{
    switch (class) {
    case 0x01: return subclass == 0x06 ? "SATA controller" : "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x06: return subclass == 0x00 ? "host bridge" : subclass == 0x01 ? "ISA bridge"
                                                                          : "bridge";
    case 0x0c: return subclass == 0x05 ? "SMBus controller" : "serial bus controller";
    default:   return "device";
    }
}

/* Find every function on every bus. Bridges to other buses don't need
 * following: scanning all 256 bus numbers finds what's behind them. */
static void scan(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint16_t addr = (uint16_t)PCI_ADDR(bus, dev, fn);
                uint16_t vendor = cfg_read16(addr, CFG_VENDOR);
                if (vendor == 0xffff) {
                    if (fn == 0)
                        break;   /* no device in this slot */
                    continue;
                }
                if (ndevs < MAX_DEVICES) {
                    uint32_t cls = cfg_read32(addr, CFG_CLASS);
                    struct device *d = &devs[ndevs++];
                    d->addr = addr;
                    d->vendor = vendor;
                    d->device = cfg_read16(addr, CFG_VENDOR + 2);
                    d->class = (uint8_t)(cls >> 24);
                    d->subclass = (uint8_t)(cls >> 16);
                    printf("%02x:%02x.%x %04x:%04x %s\n", bus, dev, fn, d->vendor, d->device,
                           d->vendor == PCI_VENDOR_VIRTIO && d->device == PCI_DEVICE_VIRTIO_BLK
                               ? "virtio block device"
                               : class_name(d->class, d->subclass));
                }
                /* Functions 1-7 only exist on multi-function devices. */
                if (fn == 0 && !(cfg_read8(addr, CFG_HEADER) & 0x80))
                    break;
            }
        }
    }
}

static bool lookup_irq_cap(long slot)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].irq_cap == slot)
            return true;
    return false;
}

static struct device *lookup(long addr)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].addr == addr)
            return &devs[i];
    return NULL;
}

/* ---- requests ------------------------------------------------------------- */

static long find(long id, long n, long *class_out)
{
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].vendor == ((id >> 16) & 0xffff) && devs[i].device == (id & 0xffff) &&
            n-- == 0) {
            *class_out = (long)devs[i].class << 8 | devs[i].subclass;
            return devs[i].addr;
        }
    }
    return -ERR_INVAL;
}

static long claim(struct device *d, long badge)
{
    if (d->owner && d->owner != badge + 1)
        return -ERR_EXIST;
    d->owner = badge + 1;
    uint32_t cmd = cfg_read32(d->addr, CFG_COMMAND);
    cfg_write32(d->addr, CFG_COMMAND, cmd | CMD_IO_SPACE | CMD_BUS_MASTER);
    return 0;
}

/* BAR = base address register. Writing all ones and reading it back
 * tells how big the region is; then the original value goes back. */
static long bar(struct device *d, long n, long reply[4], long *cap)
{
    if (n < 0 || n > 5)
        return -ERR_INVAL;
    uint8_t reg = (uint8_t)(CFG_BAR0 + 4 * n);
    uint32_t orig = cfg_read32(d->addr, reg);
    if (!(orig & 1))
        return -ERR_NOSYS;   /* a memory-mapped BAR: not supported yet */
    cfg_write32(d->addr, reg, 0xffffffff);
    uint32_t mask = cfg_read32(d->addr, reg);
    cfg_write32(d->addr, reg, orig);
    long base = orig & ~3u;
    long size = (long)((~(mask & ~3u) + 1) & 0xffff);
    if (base == 0 || size == 0)
        return -ERR_INVAL;   /* not set up by the firmware */
    *cap = sys_ioport_sub(SLOT_FACTORY, IO_WINDOW, base, size);
    if (*cap < 0)
        return *cap;
    reply[1] = size;
    return base;
}

static long irq(struct device *d, long *cap)
{
    long line = cfg_read8(d->addr, CFG_IRQ_LINE);
    if (line == 0 || line >= 16)
        return -ERR_INVAL;   /* no legacy interrupt routed to it */
    if (!d->irq_cap) {
        d->irq_cap = sys_irq_get(SLOT_FACTORY, IRQ_CONTROL, line);
        if (d->irq_cap < 0) {
            long err = d->irq_cap;
            d->irq_cap = 0;
            return err;
        }
    }
    *cap = d->irq_cap;
    return line;
}

int main(void)
{
    printf_use_console(CONSOLE);
    if (sys_ioport_enable(CFG_PORTS)) {
        printf("FAILED: no access to the configuration ports\n");
        return 1;
    }
    scan();
    printf("%d functions found\n", ndevs);

    struct msg m;
    long reply[4];
    long send_cap = 0;   /* a capability that goes out with the reply */
    bool replying = false;
    for (;;) {
        long r = replying ? sys_reply_recv(SLOT_SERVE, reply, send_cap, &m)
                          : sys_recv(SLOT_SERVE, &m);
        /* The client has its own copy now. IRQ capabilities stay with us:
         * a line can be claimed only once. */
        if (send_cap && !lookup_irq_cap(send_cap))
            sys_cap_delete(send_cap);
        send_cap = 0;
        replying = false;
        if (r == IPC_NOTIFIED)
            continue;
        if (r) {
            printf("FAILED: receive returned %ld\n", r);
            return 1;
        }

        memset(reply, 0, sizeof reply);
        struct device *d = lookup(m.w[1]);
        long op = PROTO_OP(m.w[0]);
        bool needs_claim = op == PCI_BAR || op == PCI_IRQ;
        if ((op == PCI_CLAIM || needs_claim) && !d) {
            reply[0] = -ERR_INVAL;
        } else if (needs_claim && d->owner != m.badge + 1) {
            reply[0] = -ERR_PERM;   /* claim it first */
        } else {
            switch (op) {
            case PCI_FIND:
                reply[0] = find(m.w[1], m.w[2], &reply[1]);
                break;
            case PCI_CLAIM:
                reply[0] = claim(d, m.badge);
                break;
            case PCI_BAR:
                reply[0] = bar(d, m.w[2], reply, &send_cap);
                break;
            case PCI_IRQ:
                reply[0] = irq(d, &send_cap);
                break;
            case PROTO_SHUTDOWN:
                if (m.badge != 0) {
                    reply[0] = -ERR_PERM;
                    break;
                }
                sys_reply(reply, 0);
                return 0;
            default:
                reply[0] = -ERR_NOSYS;
            }
        }
        if (reply[0] < 0)
            send_cap = 0;
        replying = true;
    }
}