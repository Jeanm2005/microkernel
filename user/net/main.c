/* Legacy virtio-net driver and ping stack, entirely in ring 3.
 * One pinned frame contains two queues and all packet buffers. */
#include <abi/boot.h>
#include <abi/pci.h>
#include "io.h"
#include "ipc.h"
#include "ulib.h"
#include "stack.h"
#define PCI (SLOT_USES + 1)
#define DMA USER_MAP_BASE
#define RX_BUFFERS 16
#define BUFFER_SIZE 2048
#define HEADER 10
#define QUEUE_BYTES 12288
#define BUFFERS_OFFSET (2 * QUEUE_BYTES)
#define DMA_PAGES ((BUFFERS_OFFSET + (RX_BUFFERS + 1) * BUFFER_SIZE + 4095) / 4096)
struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct queue {
    volatile struct desc *desc;
    volatile uint16_t *avail, *used;
    uint16_t size, submitted, completed;
};
static struct queue rx, tx;
static uint16_t base;
static uint64_t phys;
static long events, irq;
static uint8_t mac[6];
static uint8_t reply[NET_FRAME_MAX];
static void barrier(void) { __sync_synchronize(); }
static long pci(long op, long addr, long *cap)
{
    struct msg m = { .w = { op, addr } };
    long r = sys_call(PCI, &m, 0);
    if (cap) *cap = m.cap;
    return r ? r : m.w[0];
}
static int queue_init(struct queue *q, unsigned index)
{
    outw(base + 14, index);
    q->size = inw(base + 12);
    if (q->size < 2 * RX_BUFFERS || q->size > 256 || (q->size & (q->size - 1))) return -1;
    uintptr_t start = DMA + index * QUEUE_BYTES;
    unsigned used = (16u * q->size + 6 + 2u * q->size + 4095) & ~4095u;
    if (used + 6 + 8u * q->size > QUEUE_BYTES) return -1;
    q->desc = (void *)start;
    q->avail = (void *)(start + 16u * q->size);
    q->used = (void *)(start + used);
    outl(base + 8, (uint32_t)((phys + index * QUEUE_BYTES) >> 12));
    return 0;
}
static uint8_t *buffer(unsigned index)
{
    return (void *)(DMA + BUFFERS_OFFSET + index * BUFFER_SIZE);
}
static void offer_rx(unsigned id)
{
    rx.avail[2 + rx.submitted % rx.size] = (uint16_t)(id * 2);
    barrier(); rx.avail[1] = ++rx.submitted; barrier();
}
static long wait_irq(unsigned long deadline)
{
    long bits;
    long r = sys_wait_until(events, deadline, &bits);
    if (r) return r;
    (void)inb(base + 19);
    return sys_irq_ack(irq);
}
static int send_packet(size_t len)
{
    uint8_t *b = buffer(RX_BUFFERS);
    memset(b, 0, HEADER);
    memcpy(b + HEADER, reply, len);
    tx.desc[1].len = (uint32_t)len;
    tx.avail[2 + tx.submitted % tx.size] = 0;
    barrier(); tx.avail[1] = ++tx.submitted; barrier();
    outw(base + 16, 1);
    unsigned long deadline = sys_clock() + CLOCK_HZ;
    for (;;) {
        barrier();
        if (tx.used[1] != tx.completed) break;
        if (sys_clock() >= deadline || wait_irq(deadline)) return -1;
    }
    volatile uint32_t *used = (void *)&tx.used[2 + 4u * (tx.completed % tx.size)];
    if (tx.used[1] != (uint16_t)(tx.completed + 1) || used[0] != 0) return -1;
    tx.completed++;
    return 0;
}
int main(long count)
{
    printf_use_console(SLOT_USES);
    long addr = pci(PCI_FIND, PCI_VENDOR_VIRTIO << 16 | PCI_DEVICE_VIRTIO_NET, 0);
    if (count < 0 || count > 1000 || addr < 0 || pci(PCI_CLAIM, addr, 0)) return 1;
    long port_cap, irq_cap;
    long ports = pci(PCI_BAR, addr, &port_cap);
    long line = pci(PCI_IRQ, addr, &irq_cap);
    if (ports <= 0 || line <= 0 || sys_ioport_enable(port_cap)) return 1;
    base = (uint16_t)ports;
    outb(base + 18, 0);
    outb(base + 18, 3);
    /* MAC only; no checksum/GSO/merged-buffer offloads. */
    if (!(inl(base) & (1u << 5))) goto fail;
    outl(base + 4, 1u << 5);
    for (int i = 0; i < 6; i++) mac[i] = inb(base + 20 + i);
    long frame = sys_frame_alloc(SLOT_FACTORY, DMA_PAGES), pages;
    long p = frame > 0 ? sys_frame_info(frame, &pages) : -1;
    if (p <= 0 || (uint64_t)p >> 44 || sys_map(frame, DMA, 1)) goto fail;
    phys = (uint64_t)p;
    /* Keep DMA memory alive if a fault occurs after queue activation. */
    struct msg pin = { .w = { PCI_PIN_DMA, addr } };
    if (sys_call(PCI, &pin, frame) || pin.w[0]) goto fail;
    if (queue_init(&rx, 0) || queue_init(&tx, 1)) goto fail;
    for (unsigned i = 0; i < RX_BUFFERS; i++) {
        uint64_t b = phys + BUFFERS_OFFSET + i * BUFFER_SIZE;
        /* Legacy layout requires a separate descriptor for the header. */
        rx.desc[2*i] = (struct desc){ b, HEADER, 3, (uint16_t)(2*i + 1) };
        rx.desc[2*i+1] = (struct desc){ b + HEADER, BUFFER_SIZE - HEADER, 2, 0 };
        offer_rx(i);
    }
    uint64_t b = phys + BUFFERS_OFFSET + RX_BUFFERS * BUFFER_SIZE;
    tx.desc[0] = (struct desc){ b, HEADER, 1, 1 };
    tx.desc[1] = (struct desc){ b + HEADER, 0, 0, 0 };
    events = sys_obj_create(SLOT_FACTORY, OBJ_NOTIFICATION);
    irq = irq_cap;
    if (events <= 0 || sys_irq_bind(irq, events, 1)) goto fail;
    barrier(); outb(base + 18, 7); outw(base + 16, 0);
    printf("ready: 10.0.2.15, MAC %02x:%02x:%02x:%02x:%02x:%02x, IRQ %ld\n",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],line);
    net_reset();
    unsigned long deadline = sys_clock() + 30 * CLOCK_HZ;
    unsigned received = 0, dropped = 0;
    long echoes = 0;
    while (!count || echoes < count) {
        unsigned long now = sys_clock();
        if (count && now >= deadline) goto fail;
        size_t timer_reply = net_poll(reply, now);
        if (timer_reply && send_packet(timer_reply)) goto fail;
        barrier();
        if (rx.used[1] == rx.completed) {
            long r = wait_irq(sys_clock() + CLOCK_HZ / 10);
            if (r && r != -ERR_TIMEDOUT) goto fail;
            continue;
        }
        if ((uint16_t)(rx.used[1] - rx.completed) > RX_BUFFERS) goto fail;
        volatile uint32_t *u = (void *)&rx.used[2 + 4u * (rx.completed % rx.size)];
        unsigned id = u[0], len = u[1];
        if ((id & 1) || id >= 2 * RX_BUFFERS || len < HEADER || len > BUFFER_SIZE) goto fail;
        int echo;
        uint8_t *buf = buffer(id / 2);
        /* No receive offload features were negotiated. */
        size_t n = buf[0] || buf[1] ? 0 : net_reply(buf + HEADER, len - HEADER, reply, mac, &echo, now);
        received++;
        if (n) {
            if (send_packet(n)) goto fail;
            if (echo) echoes++;
        } else dropped++;
        rx.completed++;
        offer_rx(id / 2);
        outw(base + 16, 0);
    }
    outb(base + 18, 0);
    printf("ping ok: %ld echo replies, %u frames, %u dropped\n", echoes, received, dropped);
    return 0;
fail:
    outb(base + 18, 0); /* stop DMA before teardown; PCI also retains the frame */
    printf("FAILED: network initialization, completion or receive deadline\n");
    return 1;
}
