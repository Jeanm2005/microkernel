/* Starting the system from a manifest.
 *
 * The kernel gave us a factory (ROOT_SLOT_FACTORY), every I/O port, the
 * right to claim interrupt lines, and a read-only capability for each boot
 * module (from ROOT_SLOT_MODULES on). A manifest is one of those modules:
 * a text file with one program per line,
 *
 *     # name   image        priority provides uses     devices quota arg
 *     console  console.elf  200      console  -        com1    -     0
 *     hello    hello.elf    100      -        console  -       -     0
 *
 * For each program we create a process from its image and give it a
 * receive capability for the service it provides (slot SLOT_SERVE), a
 * badged send capability for each service it uses (slots SLOT_USES and
 * up), a factory with its quota of objects and pages (SLOT_FACTORY), and,
 * if it has devices, a notification for their interrupts (SLOT_EVENTS)
 * and the devices' capabilities (SLOT_DEVICES and up), which we carve out
 * of our own: just the ports and interrupt line of that device. Nothing
 * else. Then we start everything, tell the console who each badge
 * belongs to, wait for the applications (the programs that provide
 * nothing) to finish, and shut the servers down in reverse order.
 *
 * Our own output goes through the kernel's debug channel until the
 * console is up, and through the console from then until it shuts down. */
#include <stdbool.h>
#include <abi/boot.h>
#include <abi/console.h>
#include <abi/proto.h>
#include <abi/pci.h>
#include <abi/storage.h>
#include <abi/selftest.h>
#include "ipc.h"
#include "root.h"
#include "ulib.h"

#define MAX_PROGRAMS 8
#define MAX_USES     4
#define MAX_DEVICES  2
#define MAX_MODULES  16
#define FIELDS       8

/* Devices we can hand out. Each is a few capabilities we derive from our
 * own (see device_caps()). */
static const char *const device_names[] = { "com1", "pci" };
#define NDEVICES (int)(sizeof device_names / sizeof device_names[0])
#define MAX_DEVICE_CAPS 3

struct program {
    char *name, *image, *provides;   /* provides: NULL if none */
    char *uses[MAX_USES];
    int   nuses;
    char *devs[MAX_DEVICES];
    int   ndevs;
    long  quota_objects, quota_pages;   /* both 0: no factory */
    long  priority, arg;
    long badges[MAX_USES];
    int restarts;
    bool finished;
    long  proc;   /* our capability to its process */
    long  ep;     /* our (unbadged) capability to the endpoint it serves, or 0 */
};

static struct {
    char name[32];
    long slot;
} modules[MAX_MODULES];
static int nmodules;

static struct program progs[MAX_PROGRAMS];
static int nprogs;
static long child_events;
static char manifest[2048];

/* Who holds each badge on the console's endpoint (0 = us). */
static const char *console_names[CONSOLE_MAX_CLIENTS] = { "root" };

static void find_modules(void)
{
    for (long slot = ROOT_SLOT_MODULES; nmodules < MAX_MODULES; slot++) {
        if (sys_module_info(slot, modules[nmodules].name, sizeof modules[0].name) < 0)
            break;   /* first empty slot: no more modules */
        modules[nmodules++].slot = slot;
    }
}

static long module_slot(const char *name)
{
    for (int i = 0; i < nmodules; i++)
        if (strcmp(modules[i].name, name) == 0)
            return modules[i].slot;
    return 0;
}

static struct program *provider(const char *service)
{
    for (int i = 0; i < nprogs; i++)
        if (progs[i].provides && strcmp(progs[i].provides, service) == 0)
            return &progs[i];
    return NULL;
}

static int device_index(const char *name)
{
    for (int i = 0; i < NDEVICES; i++)
        if (strcmp(device_names[i], name) == 0)
            return i;
    return -1;
}

/* Make the capabilities that make up device `name`, in our own table, in
 * the order the driver gets them. Returns how many, or -1. */
static int device_caps(const char *name, long caps[MAX_DEVICE_CAPS])
{
    int n = 0;
    if (strcmp(name, "com1") == 0) {
        caps[n++] = sys_ioport_sub(ROOT_SLOT_FACTORY, ROOT_SLOT_IOPORTS, 0x3f8, 8);
        caps[n++] = sys_irq_get(ROOT_SLOT_FACTORY, ROOT_SLOT_IRQ_CONTROL, 4);
    } else if (strcmp(name, "pci") == 0) {
        /* Configuration space through the legacy port pair 0xcf8/0xcfc,
         * the range where firmware puts devices' I/O registers, and the
         * right to claim the devices' interrupt lines. */
        caps[n++] = sys_ioport_sub(ROOT_SLOT_FACTORY, ROOT_SLOT_IOPORTS, 0xcf8, 8);
        caps[n++] = sys_ioport_sub(ROOT_SLOT_FACTORY, ROOT_SLOT_IOPORTS, 0x1000, 0xf000);
        caps[n++] = ROOT_SLOT_IRQ_CONTROL;
    }
    for (int i = 0; i < n; i++)
        if (caps[i] <= 0)
            return -1;
    return n;
}


/* ---- parsing ----------------------------------------------------------- */

/* Split `s` at characters in `seps`, NUL-terminating each piece. Returns
 * the number of pieces, or max + 1 if there are too many. */
static int split(char *s, const char *seps, char **out, int max)
{
    int n = 0;
    for (;;) {
        while (*s && (s[0] == seps[0] || s[0] == seps[1] || s[0] == seps[2]))
            *s++ = '\0';
        if (!*s)
            return n;
        if (n == max)
            return max + 1;
        out[n++] = s;
        while (*s && s[0] != seps[0] && s[0] != seps[1] && s[0] != seps[2])
            s++;
    }
}

static bool parse_number(const char *s, long *out)
{
    long v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000)
            return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return true;
}

static bool parse_quota(char *s, long *objects, long *pages)
{
    char *part[2];
    if (split(s, "///", part, 2) != 2)
        return false;
    return parse_number(part[0], objects) && parse_number(part[1], pages);
}

static bool parse_line(char *line, int lineno)
{
    char *f[FIELDS];
    int n = split(line, " \t\r", f, FIELDS);
    if (n == 0 || f[0][0] == '#')
        return true;   /* blank or comment */
    if (n != FIELDS) {
        printf("FAILED: manifest line %d: expected %d fields\n", lineno, FIELDS);
        return false;
    }
    if (nprogs == MAX_PROGRAMS) {
        printf("FAILED: manifest line %d: more than %d programs\n", lineno, MAX_PROGRAMS);
        return false;
    }
    struct program *p = &progs[nprogs++];
    p->name = f[0];
    p->image = f[1];
    p->provides = strcmp(f[3], "-") == 0 ? NULL : f[3];
    if (strlen(p->name) >= 24 || !parse_number(f[2], &p->priority) ||
        !parse_number(f[7], &p->arg)) {
        printf("FAILED: manifest line %d: bad name, priority or argument\n", lineno);
        return false;
    }
    if (strcmp(f[4], "-") != 0) {
        p->nuses = split(f[4], ",,,", p->uses, MAX_USES);
        if (p->nuses > MAX_USES) {
            printf("FAILED: manifest line %d: more than %d services used\n", lineno, MAX_USES);
            return false;
        }
    }
    if (strcmp(f[5], "-") != 0) {
        p->ndevs = split(f[5], ",,,", p->devs, MAX_DEVICES);
        if (p->ndevs > MAX_DEVICES) {
            printf("FAILED: manifest line %d: more than %d devices\n", lineno, MAX_DEVICES);
            return false;
        }
    }
    if (strcmp(f[6], "-") != 0 && !parse_quota(f[6], &p->quota_objects, &p->quota_pages)) {
        printf("FAILED: manifest line %d: quota must be objects/pages\n", lineno);
        return false;
    }
    return true;
}

static bool parse_manifest(char *text)
{
    int lineno = 1;
    for (char *line = text; *line; lineno++) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        bool last = !*end;
        *end = '\0';
        if (!parse_line(line, lineno))
            return false;
        if (last)
            break;
        line = end + 1;
    }
    /* Check references only once every line is known. */
    for (int i = 0; i < nprogs; i++) {
        if (!module_slot(progs[i].image)) {
            printf("FAILED: %s: no boot module '%s'\n", progs[i].name, progs[i].image);
            return false;
        }
        for (int k = 0; k < progs[i].nuses; k++) {
            if (!provider(progs[i].uses[k])) {
                printf("FAILED: %s: nobody provides '%s'\n", progs[i].name, progs[i].uses[k]);
                return false;
            }
        }
        /* A device has exactly one driver. */
        for (int k = 0; k < progs[i].ndevs; k++) {
            int d = device_index(progs[i].devs[k]);
            if (d < 0) {
                printf("FAILED: %s: no device '%s'\n", progs[i].name, progs[i].devs[k]);
                return false;
            }
            for (int j = 0; j < i; j++)
                for (int l = 0; l < progs[j].ndevs; l++)
                    if (strcmp(progs[j].devs[l], progs[i].devs[k]) == 0) {
                        printf("FAILED: %s and %s both claim '%s'\n", progs[j].name,
                               progs[i].name, progs[i].devs[k]);
                        return false;
                    }
        }
    }
    return true;
}

/* ---- starting and stopping --------------------------------------------- */

static bool create(struct program *p, long *next_badge)
{
    p->proc = sys_proc_create(ROOT_SLOT_FACTORY, module_slot(p->image), p->priority, p->name,
                              (long)strlen(p->name));
    if (p->proc < 0) {
        printf("FAILED: creating %s: error %ld\n", p->name, p->proc);
        return false;
    }
    if (sys_proc_watch(p->proc, child_events, 1ul << (p - progs))) {
        printf("FAILED: watching %s\n", p->name);
        return false;
    }
    /* The server may receive on its endpoint and accept capabilities. */
    if (p->ep) {
        long r = sys_proc_give(p->proc, p->ep, SLOT_SERVE, CAP_READ | CAP_GRANT, 0);
        if (r) {
            printf("FAILED: giving %s its endpoint: error %ld\n", p->name, r);
            return false;
        }
        printf("%s serves '%s'\n", p->name, p->provides);
    }
    /* Each client may only send, and everything it sends carries a badge
     * that is unique to it, so servers can tell clients apart. */
    for (int k = 0; k < p->nuses; k++) {
        if (!p->badges[k]) p->badges[k] = (*next_badge)++;
        long badge = p->badges[k];
        long r = sys_proc_give(p->proc, provider(p->uses[k])->ep, SLOT_USES + k,
                               CAP_WRITE | CAP_GRANT, badge);
        if (r) {
            printf("FAILED: giving %s access to '%s': error %ld\n", p->name, p->uses[k], r);
            return false;
        }
        printf("%s uses '%s' as client %ld\n", p->name, p->uses[k], badge);
        if (strcmp(p->uses[k], "console") == 0 && badge < CONSOLE_MAX_CLIENTS)
            console_names[badge] = p->name;
    }
    /* Its own factory, carved out of ours. */
    if (p->quota_objects || p->quota_pages) {
        long f = sys_factory_split(ROOT_SLOT_FACTORY, p->quota_objects, p->quota_pages);
        if (f < 0 || sys_proc_give(p->proc, f, SLOT_FACTORY, CAP_ALL, 0) || sys_cap_delete(f)) {
            printf("FAILED: giving %s its quota\n", p->name);
            return false;
        }
        printf("%s gets a quota of %ld objects and %ld pages\n", p->name, p->quota_objects,
               p->quota_pages);
    }
    /* A driver gets a notification of its own for its interrupts, then
     * each device's capabilities. We keep no copy of the notification. */
    if (p->ndevs) {
        long ntfn = sys_obj_create(ROOT_SLOT_FACTORY, OBJ_NOTIFICATION);
        if (ntfn < 0 || sys_proc_give(p->proc, ntfn, SLOT_EVENTS, CAP_ALL, 0) ||
            sys_cap_delete(ntfn)) {
            printf("FAILED: giving %s a notification\n", p->name);
            return false;
        }
    }
    /* Each device's capabilities. We keep our copies: a restarted driver
     * (M9) will need them again. */
    long slot = SLOT_DEVICES;
    for (int k = 0; k < p->ndevs; k++) {
        long caps[MAX_DEVICE_CAPS];
        int n = device_caps(p->devs[k], caps);
        for (int c = 0; c < n; c++)
            if (sys_proc_give(p->proc, caps[c], slot++, CAP_ALL, 0))
                n = -1;
        if (n < 0) {
            printf("FAILED: giving %s device '%s'\n", p->name, p->devs[k]);
            return false;
        }
        printf("%s drives '%s'\n", p->name, p->devs[k]);
    }
    return true;
}

/* Tell the console whose badge is whose, so every line it prints carries
 * the name of the program that wrote it. */
static bool name_console_clients(long ep)
{
    for (long b = 0; b < CONSOLE_MAX_CLIENTS; b++) {
        if (!console_names[b])
            continue;
        struct msg m = { 0 };
        m.w[0] = CONSOLE_SET_NAME | (b << 8);
        size_t n = strlen(console_names[b]);
        memcpy(&m.w[1], console_names[b], n < CONSOLE_NAME_MAX ? n : CONSOLE_NAME_MAX);
        if (sys_call(ep, &m, 0) || m.w[0]) {
            printf("FAILED: naming console client %ld\n", b);
            return false;
        }
    }
    return true;
}

static void shut_down(struct program *p)
{
    /* Our endpoint capability is the only unbadged one, which is how the
     * server knows this request comes from us. */
    struct msg m = { 0 };
    m.w[0] = PROTO_SHUTDOWN;
    long r = sys_call(p->ep, &m, 0);
    long code = -1;
    if (r == 0 && m.w[0] == 0 && sys_proc_wait(p->proc, &code) == 0 && code == 0) {
        printf("%s shut down\n", p->name);
    } else {
        printf("FAILED: shutting down %s (call %ld, reply %ld, exit %ld)\n", p->name, r,
               m.w[0], code);
        failures++;
    }
}

/* Deterministic test plans. Ordinary manifests always restart with arg 0. */
static long disk_test_arg(long mode, int restarts)
{
    switch (mode) {
    case ROOT_MODE_INIT_BOOT_TIMEOUT: return BLK_TEST_STALL_BOOT;
    case ROOT_MODE_INIT_EXHAUST:
        return restarts == DISK_RESTART_LIMIT ? BLK_TEST_STALL_LATE : BLK_TEST_STALL;
    case ROOT_MODE_INIT_TIMEOUT: return restarts ? 0 : BLK_TEST_STALL;
    case ROOT_MODE_INIT_NOISY_TIMEOUT: return restarts ? 0 : BLK_TEST_STALL_NOISY;
    case ROOT_MODE_INIT_RESTART: return restarts ? 0 : BLK_TEST_CRASH;
    case ROOT_MODE_INIT_REPEATED:
        if (restarts == 1) return BLK_TEST_STALL;
        return restarts < DISK_RESTART_LIMIT ? BLK_TEST_CRASH : 0;
    default: return 0;
    }
}

int init_main(long mode)
{
    const char *file = mode == ROOT_MODE_INIT_CRASH ? "crash.manifest"
                     : mode == ROOT_MODE_INIT_INPUT ? "input.manifest"
                     : "system.manifest";
    find_modules();
    long mod = module_slot(file);
    long size = mod ? sys_module_info(mod, 0, 0) : -1;
    if (size < 0 || size >= (long)sizeof manifest) {
        printf("FAILED: manifest '%s' is missing or too large\n", file);
        return 1;
    }
    if (sys_module_read(mod, 0, manifest, size) != size || !parse_manifest(manifest))
        return 1;
    printf("read %s: %d programs\n", file, nprogs);

    /* Deadline and pending-bit semantics on a private notification. */
    long timer_test = sys_obj_create(ROOT_SLOT_FACTORY, OBJ_NOTIFICATION);
    long timer_bits = 99;
    unsigned long start = sys_clock();
    check(timer_test > 0 && sys_wait_until(timer_test, start, &timer_bits) == -ERR_TIMEDOUT &&
          timer_bits == 99, "an expired wait times out without changing output bits");
    start = sys_clock();
    check(sys_wait_until(timer_test, start + 2, &timer_bits) == -ERR_TIMEDOUT &&
          sys_clock() - start >= 2, "a blocked notification wait expires at its deadline");
    check(sys_notify(timer_test, 5) == 0 && sys_wait_until(timer_test, 0, &timer_bits) == 0 &&
          timer_bits == 5 && sys_wait_until(timer_test, 0, &timer_bits) == -ERR_TIMEDOUT,
          "pending notification bits win over expiration and are consumed once");
    long write_timer = sys_cap_copy(timer_test, CAP_WRITE, 0);
    check(sys_wait_until(write_timer, 0, &timer_bits) == -ERR_PERM &&
          sys_wait_until(0, 0, &timer_bits) == -ERR_BADCAP,
          "timed waits check notification capabilities before deadlines");
    sys_cap_delete(write_timer);
    sys_cap_delete(timer_test);

    /* Our own authority has limits too. */
    child_events = sys_obj_create(ROOT_SLOT_FACTORY, OBJ_NOTIFICATION);
    if (child_events < 0) return 1;
    long image = module_slot("root.elf");
    check(sys_proc_create(ROOT_SLOT_FACTORY, image, ROOT_TASK_PRIORITY + 1, "x", 1) == -ERR_PERM,
          "can't create a process above its own priority");
    check(sys_proc_create(ROOT_SLOT_FACTORY, ROOT_SLOT_FACTORY, 100, "x", 1) == -ERR_TYPE,
          "a process image must come from a module capability");
    /* A process that is never started is destroyed with its last
     * capability (the kernel's leak check confirms its memory came back). */
    long unused = sys_proc_create(ROOT_SLOT_FACTORY, image, 1, "unused", 6);
    long code;
    long write_proc = sys_cap_copy(unused, CAP_WRITE, 0);
    check(write_proc > 0 && sys_proc_poll(write_proc, &code) == -ERR_PERM &&
          sys_proc_watch(write_proc, child_events, 1) == -ERR_PERM,
          "observing process exit requires a readable process capability");
    sys_cap_delete(write_proc);
    long ro_events = sys_cap_copy(child_events, CAP_READ, 0);
    check(unused > 0 && sys_proc_watch(unused, child_events, 0) == -ERR_INVAL &&
          sys_proc_watch(unused, ro_events, 1) == -ERR_PERM &&
          sys_proc_watch(unused, child_events, 1) == 0 &&
          sys_proc_watch(unused, child_events, 2) == 0,
          "exit watches validate notification rights and signal bits");
    sys_cap_delete(ro_events);
    check(sys_proc_poll(unused, &code) == -ERR_INVAL,
          "polling a process that was never started is refused");
    check(unused > 0 && sys_proc_wait(unused, &code) == -ERR_INVAL && sys_cap_delete(unused) == 0,
          "a process that was never started can be discarded");

    /* Endpoints first, so any program can use any service, whatever the
     * order of the lines. */
    for (int i = 0; i < nprogs; i++) {
        if (progs[i].provides) {
            progs[i].ep = sys_obj_create(ROOT_SLOT_FACTORY, OBJ_ENDPOINT);
            if (progs[i].ep < 0) {
                printf("FAILED: endpoint for %s: error %ld\n", progs[i].name, progs[i].ep);
                return 1;
            }
        }
    }
    long next_badge = 1;   /* badge 0 means "the root task" */
    for (int i = 0; i < nprogs; i++)
        if (!create(&progs[i], &next_badge))
            return 1;

    if (mode == ROOT_MODE_INIT_RESTART || mode == ROOT_MODE_INIT_TIMEOUT ||
        mode == ROOT_MODE_INIT_REPEATED || mode == ROOT_MODE_INIT_EXHAUST ||
        mode == ROOT_MODE_INIT_BOOT_TIMEOUT || mode == ROOT_MODE_INIT_NOISY_TIMEOUT) {
        struct program *disk = provider("blk");
        if (!disk) return 1;
        disk->arg = disk_test_arg(mode, 0);
    }

    /* Everything we start runs below our priority, so nothing runs until
     * we block waiting below. */
    for (int i = 0; i < nprogs; i++) {
        sys_proc_start(progs[i].proc, progs[i].arg);
        check(sys_proc_poll(progs[i].proc, &code) == -ERR_AGAIN &&
              sys_proc_watch(progs[i].proc, child_events, 1) == -ERR_INVAL,
              "a running child polls as pending and cannot change its watch");
        printf("started %s (priority %ld)\n", progs[i].name, progs[i].priority);
    }

    /* Our first call runs the console (it outranks the applications), and
     * from then on our output goes through it too. */
    struct program *console = provider("console");
    if (console) {
        if (!name_console_clients(console->ep))
            return 1;
        printf_use_console(console->ep);
        printf("output now goes through the console server\n");
    }

    long unavailable_ep = 0;
    int pending = 0;
    for (int i = 0; i < nprogs; i++) if (!progs[i].provides) pending++;
    while (pending) {
        long bits;
        if (unavailable_ep) {
            struct msg request;
            long status = sys_recv(unavailable_ep, &request);
            if (!status) {
                if (request.cap > 0) sys_cap_delete(request.cap);
                long reply[4] = { -ERR_IO, 0, 0, 0 };
                printf("disk service unavailable; refusing request\n");
                if (sys_reply(reply, 0)) return 1;
                continue;
            }
            if (status != IPC_NOTIFIED) return 1;
            bits = request.w[0];
        } else if (sys_wait(child_events, &bits)) return 1;
        for (int i = 0; i < nprogs; i++) {
            struct program *p = &progs[i];
            if (p->finished || !(bits & (1ul << i))) continue;
            long r = sys_proc_poll(p->proc, &code);
            if (r == -ERR_AGAIN) continue;
            if (p->provides) {
                /* This milestone supervises the disk driver. Other server
                 * failures are reported, rather than restarted blindly. */
                bool recoverable = r == -ERR_DEAD ||
                    (r == 0 && (code == BLK_EXIT_TIMEOUT || code == BLK_EXIT_IO));
                if (!recoverable || strcmp(p->provides, "blk")) {
                    printf("FAILED: unexpected server exit: %s\n", p->name);
                    return 1;
                }
                if (r == -ERR_DEAD)
                    printf("observed disk driver fault after process teardown\n");
                else
                    printf("observed disk driver I/O failure after process teardown\n");
                struct program *pc = provider("pci");
                struct msg m = { .w = { PCI_FIND,
                    PCI_VENDOR_VIRTIO << 16 | PCI_DEVICE_VIRTIO_BLK } };
                if (!pc || sys_call(pc->ep, &m, 0) || m.w[0] < 0) return 1;
                m = (struct msg){ .w = { PCI_RECOVER, m.w[0] } };
                if (sys_call(pc->ep, &m, 0) || m.w[0]) return 1;
                printf("stopped disk DMA and released its old buffers\n");
                if (sys_cap_delete(p->proc)) return 1;
                if (p->restarts == DISK_RESTART_LIMIT) {
                    printf("disk restart budget exhausted; stopping the service\n");
                    p->finished = true;
                    failures++;
                    /* Root now drains the stable endpoint with terminal
                     * errors, alongside child-exit notifications. */
                    unavailable_ep = p->ep;
                    if (sys_ntfn_bind(child_events)) return 1;
                    continue;
                }
                p->restarts++;
                p->arg = disk_test_arg(mode, p->restarts);
                if (!create(p, &next_badge) || sys_proc_start(p->proc, p->arg)) return 1;
                printf("restarted blk with the same endpoint and client badges\n");
            } else {
                p->finished = true;
                pending--;
                if (r == 0) {
                    printf("%s exited with code %ld\n", p->name, code);
                    if (code) failures++;
                } else if (r == -ERR_DEAD) {
                    printf("%s was killed\n", p->name);
                    if (mode != ROOT_MODE_INIT_CRASH || strcmp(p->name, "crasher")) failures++;
                } else return 1;
            }
        }
    }
    if (mode == ROOT_MODE_INIT_RESTART)
        check(provider("blk")->restarts == 1, "disk driver restarted exactly once");

    if (mode == ROOT_MODE_INIT_REPEATED)
        check(provider("blk")->restarts == DISK_RESTART_LIMIT,
              "disk survived three successive recoveries");
    if (mode == ROOT_MODE_INIT_TIMEOUT || mode == ROOT_MODE_INIT_NOISY_TIMEOUT)
        check(provider("blk")->restarts == 1, "disk timeout recovered with one restart");
    if (mode == ROOT_MODE_INIT_EXHAUST || mode == ROOT_MODE_INIT_BOOT_TIMEOUT)
        check(provider("blk")->restarts == DISK_RESTART_LIMIT && provider("blk")->finished,
              "restart exhaustion stopped DMA without starting another driver");

    printf_use_console(0);   /* the console is about to go away */
    for (int i = nprogs - 1; i >= 0; i--)
        if (progs[i].provides && !progs[i].finished)
            shut_down(&progs[i]);

    printf("system finished, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}