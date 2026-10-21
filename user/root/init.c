/* Starting the system from a manifest.
 *
 * The kernel gave us a factory (ROOT_SLOT_FACTORY) and a read-only
 * capability for each boot module (from ROOT_SLOT_MODULES on). A manifest
 * is one of those modules: a text file with one program per line,
 *
 *     # name    image         priority  provides  uses       arg
 *     console   console.elf   200       console   -          0
 *     hello     hello.elf     100       -         console    0
 *
 * For each program we create a process from its image, give it a receive
 * capability for the service it provides (slot SLOT_SERVE) and a badged
 * send capability for each service it uses (slots SLOT_USES and up), and
 * nothing else. Then we start everything, wait for the applications (the
 * programs that provide nothing) to finish, and shut the servers down in
 * reverse order. */
#include <stdbool.h>
#include <abi/boot.h>
#include <abi/proto.h>
#include <abi/selftest.h>
#include "ipc.h"
#include "root.h"
#include "ulib.h"

#define MAX_PROGRAMS 8
#define MAX_USES     4
#define MAX_MODULES  16

struct program {
    char *name, *image, *provides;   /* provides: NULL if none */
    char *uses[MAX_USES];
    int   nuses;
    long  priority, arg;
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
static char manifest[2048];

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

static bool parse_line(char *line, int lineno)
{
    char *f[6];
    int n = split(line, " \t\r", f, 6);
    if (n == 0 || f[0][0] == '#')
        return true;   /* blank or comment */
    if (n != 6) {
        printf("FAILED: manifest line %d: expected 6 fields\n", lineno);
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
        !parse_number(f[5], &p->arg)) {
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
        long badge = (*next_badge)++;
        long r = sys_proc_give(p->proc, provider(p->uses[k])->ep, SLOT_USES + k,
                               CAP_WRITE | CAP_GRANT, badge);
        if (r) {
            printf("FAILED: giving %s access to '%s': error %ld\n", p->name, p->uses[k], r);
            return false;
        }
        printf("%s uses '%s' as client %ld\n", p->name, p->uses[k], badge);
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

int init_main(long mode)
{
    const char *file = mode == ROOT_MODE_INIT ? "system.manifest" : "crash.manifest";
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

    /* Our own authority has limits too. */
    long image = module_slot("root.elf");
    check(sys_proc_create(ROOT_SLOT_FACTORY, image, ROOT_TASK_PRIORITY + 1, "x", 1) == -ERR_PERM,
          "can't create a process above its own priority");
    check(sys_proc_create(ROOT_SLOT_FACTORY, ROOT_SLOT_FACTORY, 100, "x", 1) == -ERR_TYPE,
          "a process image must come from a module capability");
    /* A process that is never started is destroyed with its last
     * capability (the kernel's leak check confirms its memory came back). */
    long unused = sys_proc_create(ROOT_SLOT_FACTORY, image, 1, "unused", 6);
    long code;
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

    /* Everything we start runs below our priority, so nothing runs until
     * we block waiting below. */
    for (int i = 0; i < nprogs; i++) {
        sys_proc_start(progs[i].proc, progs[i].arg);
        printf("started %s (priority %ld)\n", progs[i].name, progs[i].priority);
    }

    for (int i = 0; i < nprogs; i++) {
        struct program *p = &progs[i];
        if (p->provides)
            continue;
        long r = sys_proc_wait(p->proc, &code);
        if (r == 0)
            printf("%s exited with code %ld\n", p->name, code);
        else if (r == -ERR_DEAD)
            printf("%s was killed\n", p->name);
        else
            printf("FAILED: waiting for %s: error %ld\n", p->name, r);
    }

    for (int i = nprogs - 1; i >= 0; i--)
        if (progs[i].provides)
            shut_down(&progs[i]);

    printf("system finished, %d failure(s)\n", failures);
    return failures ? 1 : 0;
}