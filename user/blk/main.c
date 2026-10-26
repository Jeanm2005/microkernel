/* First step of the virtio-blk (virtual block device) driver: everything
 * up to the point where it can talk to the disk.
 *
 * It checks the memory capabilities it will need for DMA buffers (a frame
 * from its own quota, mapped into its address space, with a physical
 * address to give the device), then asks the PCI server for the disk:
 * claims it, receives its I/O ports and interrupt line as capabilities,
 * and reads the disk's size from the device's own registers. Reading and
 * writing blocks (the virtqueue) is the next step.
 *
 * The device is QEMU's virtio-blk in "transitional" mode, which still
 * offers the legacy virtio interface: registers in an I/O-port BAR (base
 * address register) instead of memory-mapped ones. */
#include <stdint.h>
#include <abi/boot.h>
#include <abi/pci.h>
#include "io.h"
#include "ipc.h"
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

    check(sys_unmap(BUF2) == 0 && sys_unmap(BUF) == 0 && sys_unmap(BUF) == -ERR_INVAL,
          "unmapped both");
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

int main(void)
{
    printf_use_console(CONSOLE);
    check_memory();

    long class;
    long addr = pci(PCI_FIND, PCI_VENDOR_VIRTIO << 16 | PCI_DEVICE_VIRTIO_BLK, 0, &class, 0);
    if (addr < 0) {
        printf("FAILED: no virtio block device (%ld)\n", addr);
        return 1;
    }
    printf("found the virtio block device at %02lx:%02lx.%lx\n", PCI_BUS(addr), PCI_DEV(addr),
           PCI_FN(addr));
    check(pci(PCI_BAR, addr, 0, 0, 0) == -ERR_PERM,
          "the PCI server hands out a device's ports only after it is claimed");
    check(pci(PCI_CLAIM, addr, 0, 0, 0) == 0, "claimed the device");
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
    check(sectors > 0, "read the disk's size from the device itself");
    outb((uint16_t)(ports + VIRTIO_STATUS), 0);   /* reset: leave it as we found it */

    /* The interrupt line will signal a notification made from our quota. */
    long ntfn = sys_obj_create(SLOT_FACTORY, OBJ_NOTIFICATION);
    check(ntfn > 0 && sys_irq_bind(irq_cap, ntfn, 1) == 0,
          "bound the disk's interrupt to a notification from its own quota");

    printf("done, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}