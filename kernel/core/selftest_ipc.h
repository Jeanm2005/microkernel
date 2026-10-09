/* IPC self-tests. The kernel plays the part the root task will take over
 * in M6: it creates an endpoint, hands a server process a receive
 * capability and a client process a badged send capability, starts both,
 * and checks how they finished. The protocol and the per-step checks live
 * in user/root/ipc_test.c. */
#include <abi/selftest.h>
#include <abi/syscall.h>
#include <kernel/kprintf.h>
#include <kernel/mm.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>

#define IPC_PRIORITY 100

static struct process *make(const struct boot_info *boot, const char *name, uint64_t mode)
{
    const struct boot_module *m = boot_find_module(boot, "root.elf");
    kassert(m);
    struct process *p = process_create(name, phys_to_virt(m->phys), m->size, mode, IPC_PRIORITY);
    if (!p)
        panic("could not create process '%s'", name);
    return p;
}

static void give(struct process *p, struct kobj *o, uint32_t rights, uint64_t badge,
                 int64_t want_slot)
{
    int64_t slot = process_give_cap(p, o, rights, badge);
    if (slot != want_slot)
        panic("cap for '%s' landed in slot %ld, not %ld", p->name, slot, want_slot);
}

/* Run one server/client pair to completion. */
static void run_pair(const struct boot_info *boot, uint64_t server_mode, uint64_t client_mode,
                     struct process **server_out, struct process **client_out)
{
    struct kobj *ep = kobj_create(KOBJ_ENDPOINT, NULL);
    struct kobj *factory = factory_create(8);
    kassert(ep && factory);

    struct process *server = make(boot, "pong", server_mode);
    struct process *client = make(boot, "ping", client_mode);

    /* The server may receive (and accept capabilities); the client may
     * only send, and everything it sends is stamped with its badge. Only
     * the server can create kernel objects. */
    give(server, ep, CAP_READ | CAP_GRANT, 0, TEST_SLOT_EP);
    give(server, factory, CAP_ALL, 0, TEST_SLOT_FACTORY);
    give(client, ep, CAP_WRITE | CAP_GRANT, TEST_CLIENT_BADGE, TEST_SLOT_EP);

    /* From here on the processes' capabilities are the only references:
     * once both are gone, the endpoint and factory must be freed. */
    kobj_put(ep);
    kobj_put(factory);

    process_start(server);
    process_start(client);
    process_wait(client);
    process_wait(server);
    *server_out = server;
    *client_out = client;
}

void ipc_selftest_boot(const struct boot_info *boot)
{
    uint64_t before = pmm_free_frames();
    uint64_t t0 = sched_ticks();
    uint64_t direct0 = sched_direct_switches();

    struct process *server, *client;
    run_pair(boot, ROOT_MODE_IPC_SERVER, ROOT_MODE_IPC_CLIENT, &server, &client);

    if (client->killed || client->exit_code != 0)
        panic("IPC client failed (%s, code %d)",
              client->killed ? client->kill_reason : "exited", client->exit_code);
    if (server->killed || server->exit_code != 0)
        panic("IPC server failed (%s, code %d)",
              server->killed ? server->kill_reason : "exited", server->exit_code);
    process_put(server);
    process_put(client);

    /* Both address spaces, both capability tables, the endpoint, the
     * factory and the notification the server made must all be gone. */
    uint64_t after = pmm_free_frames();
    if (after != before)
        panic("IPC test leaked %ld frames", (long)(before - after));
    /* Each of the ~1000 round trips should hand the CPU over directly
     * twice: client -> server on call, server -> client on reply_recv. */
    uint64_t direct = sched_direct_switches() - direct0;
    if (direct < 2000)
        panic("only %lu direct IPC switches; the fast path isn't being used", direct);
    kprintf("selftest: ipc ok (%lu ticks, %lu direct switches, no frames leaked)\n",
            sched_ticks() - t0, direct);
}

bool ipc_selftest_run(const char *name, const struct boot_info *boot)
{
    if (strcmp(name, "ipc-server-dies") != 0)
        return false;

    struct process *server, *client;
    run_pair(boot, ROOT_MODE_IPC_SERVER_CRASH, ROOT_MODE_IPC_CLIENT_ORPHAN, &server, &client);
    if (!server->killed)
        panic("the server was supposed to crash");
    if (client->killed || client->exit_code != 0)
        panic("client didn't handle its server dying (code %d)", client->exit_code);
    kprintf("selftest: client got -ERR_DEAD when its server died; kernel survived\n");
    process_put(server);
    process_put(client);
    return true;
}