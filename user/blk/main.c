/* Legacy virtio-blk server: one DMA request at a time, completed by IRQ. */
#include <stdint.h>
#include <abi/boot.h>
#include <abi/pci.h>
#include <abi/storage.h>
#include "io.h"
#include "ipc.h"
#include "syscall.h"
#include "ulib.h"

#define CONSOLE  SLOT_USES
#define PCI      (SLOT_USES + 1)
#define PCI2     (SLOT_USES + 2)   /* a second badge: looks like another client */

#define BUF      USER_MAP_BASE
#define BUF2     (USER_MAP_BASE + 0x100000)

/* Legacy virtio PCI registers, as offsets from BAR 0. */
#define VIRTIO_HOST_FEATURES  0x00
#define VIRTIO_STATUS         0x12
#define VIRTIO_BLK_CAPACITY   0x14   /* 64-bit, in 512-byte sectors */

#define STATUS_ACKNOWLEDGE    1      /* we've seen the device */
#define STATUS_DRIVER         2      /* and we know how to drive it */

static void check_memory(void)
{
    long frame = sys_frame_alloc(SLOT_FACTORY, 2);
    check(frame > 0, "allocated a 2-page frame from its own quota");
    check(sys_frame_alloc(SLOT_FACTORY, 64) == -ERR_NOMEM, "can't allocate beyond its page quota");

    long pages = 0;
    long phys = sys_frame_info(frame, &pages);
    check(phys > 0 && (phys & 0xfff) == 0 && pages == 2,
          "the frame's physical address is known (what a device needs for DMA)");

    check(sys_map(frame, 0xffff800000000000ul, 1) == -ERR_INVAL, "can't map into the kernel half");
    check(sys_map(frame, 0x400000, 1) == -ERR_INVAL, "can't map over its own program");
    check(sys_map(frame, BUF, 1) == 0, "mapped the frame, writable");
    volatile uint64_t *p = (volatile uint64_t *)BUF;
    for (int i = 0; i < 1024; i++)
        p[i] = 0x1234567800000000ull + (uint64_t)i;
    check(p[0] == 0x1234567800000000ull && p[1023] == 0x12345678000003ffull,
          "it reads back what was written");
    check(sys_map(frame, BUF, 1) == -ERR_EXIST, "can't map over an existing mapping");

    long ro = sys_cap_copy(frame, CAP_READ, 0);
    check(sys_map(ro, BUF2, 1) == -ERR_PERM,
          "a read-only frame capability can't be mapped writable");
    check(sys_map(ro, BUF2, 0) == 0 && ((volatile uint64_t *)BUF2)[1023] == p[1023],
          "mapped read-only at a second address, it shows the same memory");

    check(sys_cap_revoke(frame) == -ERR_BUSY,
          "revocation refuses an active descendant frame mapping");
    check(sys_unmap(BUF2) == 0 && sys_unmap(BUF) == 0 && sys_unmap(BUF) == -ERR_INVAL,
          "unmapped both");
    check(sys_cap_revoke(frame) == 0 && sys_frame_info(ro, &pages) == -ERR_REVOKED,
          "frame descendants can be revoked after unmapping");
    sys_cap_delete(ro);
    sys_cap_delete(frame);
}

/* One request to the PCI server through endpoint `ep`; a capability that
 * comes back lands in *cap. */
static long pci_on(long ep, long op, long a1, long a2, long *out1, long *cap)
{
    struct msg m = { 0 };
    m.w[0] = op;
    m.w[1] = a1;
    m.w[2] = a2;
    long r = sys_call(ep, &m, 0);
    if (r)
        return r;
    if (out1)
        *out1 = m.w[1];
    if (cap)
        *cap = m.cap;
    return m.w[0];
}

static long pci(long op, long a1, long a2, long *out1, long *cap)
{
    return pci_on(PCI, op, a1, a2, out1, cap);
}

/* Queue layout follows the legacy PCI transport: used ring page-aligned.
 * x86 DMA is coherent; full barriers order publication and completion. */
struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct request { uint32_t type, reserved; uint64_t sector; };
static volatile struct desc *desc;
static volatile uint16_t *avail, *used;
static volatile struct request *request;
static volatile uint8_t *data, *status;
static uint64_t dma_phys;
static uint16_t qsize, submitted, completed, base;
static long events, disk_irq, device_addr;
static long test_mode;
static uint64_t capacity;

static void barrier(void) { __sync_synchronize(); }

static int queue_init(void)
{
    outl(base + 4, 0);   /* negotiate no optional features */
    outw(base + 14, 0);  /* request queue */
    qsize = inw(base + 12);
    if (qsize < 3 || qsize > 256 || (qsize & (qsize - 1)))
        return -1;
    unsigned used_offset = (16u * qsize + 6u + 2u * qsize + 4095) & ~4095u;
    unsigned bytes = used_offset + 6u + 8u * qsize;
    unsigned request_offset = (bytes + 4095) & ~4095u;
    unsigned pages = (request_offset + 16 + 512 + 1 + 4095) / 4096;
    long frame = sys_frame_alloc(SLOT_FACTORY, pages), n;
    long phys = frame > 0 ? sys_frame_info(frame, &n) : -1;
    if (phys <= 0 || (uint64_t)phys >> 44 || sys_map(frame, BUF, 1))
        return -1;
    dma_phys = (uint64_t)phys;
    desc = (void *)BUF;
    avail = (void *)(BUF + 16u * qsize);
    used = (void *)(BUF + used_offset);
    request = (void *)(BUF + request_offset);
    data = (void *)(BUF + request_offset + 16);
    status = data + 512;
    desc[0] = (struct desc){ dma_phys + request_offset, 16, 1, 1 };
    desc[1] = (struct desc){ dma_phys + request_offset + 16, 512, 3, 2 };
    desc[2] = (struct desc){ dma_phys + request_offset + 528, 1, 2, 0 };
    /* The PCI server keeps a copy even if this process faults. */
    struct msg pin = { .w = { PCI_PIN_DMA, device_addr } };
    if (sys_call(PCI, &pin, frame) || pin.w[0]) return -1;
    barrier();
    outl(base + 8, (uint32_t)(dma_phys >> 12));
    outb(base + VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | 4);
    return 0;
}

static int transfer(uint64_t sector, int write)
{
    request->type = write ? 1 : 0;
    request->reserved = 0;
    request->sector = sector;
    *status = 0xff;
    desc[1].flags = write ? 1 : 3;
    avail[2 + submitted % qsize] = 0;
    barrier();
    avail[1] = ++submitted;
    barrier();
    unsigned long deadline = sys_clock() + DISK_REQUEST_TICKS;
    int stall = !write && (((test_mode == BLK_TEST_STALL || test_mode == BLK_TEST_STALL_NOISY) &&
                            sector == 1) ||
                           (test_mode == BLK_TEST_STALL_LATE && sector == 2) ||
                           (test_mode == BLK_TEST_STALL_BOOT && sector == capacity - 1));
    if (stall)
        printf("simulating a disk request with no completion\n");
    else
        outw(base + 16, 0);
    if (test_mode == BLK_TEST_SPIN && !write && sector == 1) {
        printf("spinning without syscalls after DMA submission\n");
        for (;;) __asm__ volatile("pause");
    }
    if (test_mode == BLK_TEST_CRASH && !write && sector == 1) {
        printf("crashing during a filesystem request after DMA submission\n");
        *(volatile int *)0 = 1;
    }
    /* Always consume the notification and deassert INTx before unmasking.
     * A pending notification closes the completion-before-wait race. */
    for (;;) {
        long bits;
        if (stall && test_mode == BLK_TEST_STALL_NOISY) sys_notify(events, 2);
        /* wait_until lets pending bits win. Enforce the request deadline
         * independently so a flood of unrelated IRQs cannot postpone it. */
        long r = sys_clock() >= deadline ? -ERR_TIMEDOUT :
            sys_wait_until(events, deadline, &bits);
        if (r == -ERR_TIMEDOUT) {
            printf("disk request timed out; exiting for supervisor recovery\n");
            /* Never reuse DMA buffers after an ambiguous completion.
             * PCI keeps them pinned until root resets the device. */
            sys_exit(BLK_EXIT_TIMEOUT);
        }
        if (r) sys_exit(BLK_EXIT_IO);
        (void)inb(base + 19);
        if (sys_irq_ack(disk_irq)) sys_exit(BLK_EXIT_IO);
        barrier();
        /* DRIVER_OK may schedule queue processing before the suppressed
         * kick. Ignore such a racing completion only in the stall test,
         * so the initial read cannot reach the scratch-write self-test. */
        if (!stall && used[1] != completed)
            break;
    }
    volatile uint32_t *entry = (void *)&used[2 + 4u * (completed % qsize)];
    if (used[1] != (uint16_t)(completed + 1) || entry[0] != 0 || *status != 0)
        sys_exit(BLK_EXIT_IO);
    completed++;
    return 0;
}

static int serve(uint64_t sectors)
{
    struct msg m;
    if (sys_recv(SLOT_SERVE, &m))
        return 1;
    for (;;) {
        long reply[4] = { -ERR_INVAL, 0, 0, 0 };
        int stop = 0;
        switch (PROTO_OP(m.w[0])) {
        case PROTO_SHUTDOWN:
            reply[0] = m.badge ? -ERR_PERM : 0;
            stop = !m.badge;
            break;
        case BLK_SIZE: reply[0] = (long)sectors; break;
        case BLK_READ:
            if ((uint64_t)m.w[1] < sectors && m.w[2] >= 0 && m.w[2] < 512 &&
                m.w[3] > 0 && m.w[3] <= STORAGE_CHUNK && m.w[3] <= 512 - m.w[2]) {
                if (transfer((uint64_t)m.w[1], 0))
                    reply[0] = -ERR_DEAD;
                else {
                    reply[0] = m.w[3];
                    for (long i = 0; i < m.w[3]; i++)
                        ((uint8_t *)&reply[1])[i] = data[m.w[2] + i];
                }
            }
            break;
        }
        if (m.cap > 0) sys_cap_delete(m.cap);
        if (stop) {
            outb(base + VIRTIO_STATUS, 0); /* stop DMA before process teardown */
            sys_reply(reply, 0);
            return failures ? 1 : 0;
        }
        if (sys_reply_recv(SLOT_SERVE, reply, 0, &m))
            return 1;
    }
}

int main(long arg)
{
    test_mode = arg;
    printf_use_console(CONSOLE);
    check_memory();

    long class;
    long addr = pci(PCI_FIND, PCI_VENDOR_VIRTIO << 16 | PCI_DEVICE_VIRTIO_BLK, 0, &class, 0);
    if (addr < 0) {
        printf("FAILED: no virtio block device (%ld)\n", addr);
        return 1;
    }
    device_addr = addr;
    printf("found the virtio block device at %02lx:%02lx.%lx\n", PCI_BUS(addr), PCI_DEV(addr),
           PCI_FN(addr));
    check(pci(PCI_BAR, addr, 0, 0, 0) == -ERR_PERM,
          "the PCI server hands out a device's ports only after it is claimed");
    check(pci(PCI_CLAIM, addr, 0, 0, 0) == 0, "claimed the device");
    check(pci(PCI_RECOVER, addr, 0, 0, 0) == -ERR_PERM,
          "only the supervisor may reset a claimed device");
    check(pci_on(PCI2, PCI_CLAIM, addr, 0, 0, 0) == -ERR_EXIST &&
              pci_on(PCI2, PCI_BAR, addr, 0, 0, 0) == -ERR_PERM,
          "another client can neither claim it nor get its ports");

    long ports, nports, port_cap = 0, irq_cap = 0;
    ports = pci(PCI_BAR, addr, 0, &nports, &port_cap);
    long line = pci(PCI_IRQ, addr, 0, 0, &irq_cap);
    check(ports > 0 && port_cap > 0, "received the device's I/O ports as a capability");
    check(line > 0 && irq_cap > 0, "received the device's interrupt line as a capability");
    printf("registers at ports 0x%lx-0x%lx, IRQ %ld\n", ports, ports + nports - 1, line);
    if (ports <= 0 || sys_ioport_enable(port_cap))
        return 1;

    /* Reset, then announce ourselves, as the virtio spec's start-up
     * sequence begins. The size is in the device-specific registers. */
    outb((uint16_t)(ports + VIRTIO_STATUS), 0);
    outb((uint16_t)(ports + VIRTIO_STATUS), STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    uint32_t features = inl((uint16_t)(ports + VIRTIO_HOST_FEATURES));
    uint64_t sectors = inl((uint16_t)(ports + VIRTIO_BLK_CAPACITY)) |
                       (uint64_t)inl((uint16_t)(ports + VIRTIO_BLK_CAPACITY + 4)) << 32;
    printf("disk: %lu sectors of 512 bytes (%lu KiB), features 0x%08x\n", sectors, sectors / 2,
           features);
    capacity = sectors;
    check(sectors > 0, "read the disk's size from the device itself");


    /* The interrupt line will signal a notification made from our quota. */
    long ntfn = sys_obj_create(SLOT_FACTORY, OBJ_NOTIFICATION);
    check(ntfn > 0 && sys_irq_bind(irq_cap, ntfn, 1) == 0,
          "bound the disk's interrupt to a notification from its own quota");

    long ro_irq = sys_cap_copy(irq_cap, CAP_READ, 0);
    check(ro_irq > 0 && sys_irq_unbind(ro_irq) == -ERR_PERM,
          "a read-only IRQ capability cannot disconnect the driver");
    sys_cap_delete(ro_irq);
    base = (uint16_t)ports;
    events = ntfn;
    disk_irq = irq_cap;
    if (failures || queue_init()) {
        outb(base + VIRTIO_STATUS, 0);
        return 1;
    }
    /* Last sector is scratch space, outside the tiny filesystem. Restore
     * it after testing writes so repeated boots see identical contents. */
    uint8_t saved[512];
    int ok = transfer(sectors - 1, 0) == 0;
    if (ok) {
        for (int i = 0; i < 512; i++) { saved[i] = data[i]; data[i] = (uint8_t)i; }
        ok = transfer(sectors - 1, 1) == 0 && transfer(sectors - 1, 0) == 0;
        for (int i = 0; i < 512; i++) {
            if (data[i] != (uint8_t)i) ok = 0;
            data[i] = saved[i];
        }
        if (transfer(sectors - 1, 1) || transfer(sectors - 1, 0)) ok = 0;
        for (int i = 0; i < 512; i++) if (data[i] != saved[i]) ok = 0;
    }
    check(ok, "DMA read/write round trip completed through IRQ notifications");
    printf("done, %d failure(s)\n", failures);
    if (failures) { outb(base + VIRTIO_STATUS, 0); return 1; }
    return serve(sectors);
}