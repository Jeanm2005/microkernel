/* PCI (Peripheral Component Interconnect) server protocol.
 *
 * The PCI server owns the configuration ports and every device's I/O
 * ports and interrupt lines. A driver finds its device, claims it (only
 * one client can), and then asks for the device's resources one at a
 * time; each comes back as a capability in the reply, covering exactly
 * that device's registers or that device's interrupt line. */
#pragma once
#include <abi/proto.h>

/* A device's address: bus, device (slot) and function. */
#define PCI_ADDR(bus, dev, fn) (((bus) << 8) | ((dev) << 3) | (fn))
#define PCI_BUS(a)             (((a) >> 8) & 0xff)
#define PCI_DEV(a)             (((a) >> 3) & 0x1f)
#define PCI_FN(a)              ((a) & 0x7)

#define PCI_FIND 1 /* w1 = vendor << 16 | device, w2 = n (0 = first match)
                          -> w0 = address, w1 = class << 8 | subclass; -ERR_INVAL if none */
#define PCI_CLAIM 2  /* w1 = address -> 0, or -ERR_EXIST if another client has it.
                          Turns on the device's I/O decoding and bus mastering (DMA). */
#define PCI_BAR 3  /* w1 = address, w2 = BAR (base address register) 0-5
                          -> w0 = first port, w1 = number of ports, cap = those ports.
                          Claimant only; I/O-port BARs only for now. */
#define PCI_IRQ 4  /* w1 = address -> w0 = line, cap = the IRQ. Claimant only. */

/* PCI vendor and device IDs we know about. */
#define PCI_VENDOR_VIRTIO 0x1af4
#define PCI_DEVICE_VIRTIO_BLK 0x1001 /* "transitional" (legacy-capable) block device */
/* Restart support for the legacy virtio block device. The PCI server pins
 * one DMA frame before DRIVER_OK. Only root may reset/release the device,
 * after its old process has exited; this is not capability revocation. */
#define PCI_PIN_DMA 5 /* w1=device, cap=frame; claimant only, one frame */
#define PCI_RECOVER 6 /* w1=device; badge 0 only. Stop DMA, unbind IRQ, drop frame, release claim. */
