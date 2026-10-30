# Design

A small L4-style microkernel for x86_64, written in C. The kernel does the
minimum that *must* run in ring 0; everything else (drivers, file system,
network stack, process management policy) runs as isolated user-space
servers that talk over IPC (inter-process communication) and hold
capabilities (unforgeable tokens that grant access to one kernel object).

The property we are building toward and will demo: **a driver can crash,
be restarted by a supervisor, and the rest of the system keeps running.**

## Decisions

| Decision | Choice | Why |
|---|---|---|
| Kernel model | Microkernel, user-space servers | Real fault isolation; restartable drivers |
| Language | C (gnu11), minimal assembly | Matches OS-course material, smallest toolchain |
| Architecture | x86_64 only (arch code isolated) | Most relevant ISA; QEMU q35 as reference machine |
| Bootloader | Limine (base revision 3) | Enters 64-bit long mode and maps the kernel in the higher half for us; gives the memory map and HHDM (higher-half direct map). Skips the 32→64-bit trampoline that teaches little |
| Reference machine | QEMU `q35`, 256 MiB, serial on COM1 | Modern chipset: PCIe (PCI Express) with ECAM (memory-mapped config space), AHCI (SATA disk controller), MSI (message-signaled interrupts) |
| Kernel address | `0xffffffff80000000` (`-mcmodel=kernel`) | Standard higher-half layout |
| Physical memory access | Limine HHDM (`phys + 0xffff8000_00000000`) | Kernel can touch any frame without temporary mappings |
| FPU/SIMD (floating-point unit / vector instructions) in kernel | Forbidden (`-mgeneral-regs-only`) | No FPU state to save on kernel entry; lazy/eager save only for user threads |

## What lives in the kernel

1. **Traps**: GDT (Global Descriptor Table), TSS (Task State Segment),
   IDT (Interrupt Descriptor Table), exception handlers, `syscall`/`sysret`
   entry.
2. **Memory**: physical frame allocator, 4-level page tables per address
   space, kernel heap for kernel objects.
3. **Threads & scheduling**: thread control blocks, context switch,
   preemptive fixed-priority round-robin driven by the LAPIC (local
   Advanced Programmable Interrupt Controller) timer.
4. **IPC**: synchronous endpoints + asynchronous notifications.
5. **Capabilities**: per-process capability table; every kernel object is
   reached only through a capability.
6. **Interrupt routing**: a device interrupt → notification to the driver
   thread that holds the IRQ (interrupt request line) capability. Legacy
   lines go through the 8259 PIC today (PCI devices' legacy INTx lines
   too); the IOAPIC (I/O APIC) and MSI come later.

Not in the kernel: device drivers (except the timer the scheduler needs and
a write-only serial log for the kernel's own messages), file systems,
networking, process-management policy, POSIX (the Unix API standard). One
exception for now: ELF (executable file format) loading stays in the
kernel until user space can map memory (see Starting the system).

```
 ┌──────────────────────── ring 3 ───────────────────────────────┐
 │ root task/supervisor │ console │ pci │ blk │ fs │ net │ apps   │
 └──────────▲────────────────────────────────────────────▲───────┘
            │        syscalls: IPC + capability ops       │
 ┌──────────┴────────────────────────────────────────────┴───────┐
 │ core/: sched · ipc · cap · vm · irq-routing     (portable C)   │
 ├────────────────────────────────────────────────────────────────┤
 │ arch/x86_64/: boot (Limine) · gdt/idt/tss · paging · lapic ·   │
 │               context switch · syscall entry                   │
 └────────────────────────────────────────────────────────────────┘
```

## Source layout

```
kernel/
  arch/x86_64/    everything x86-specific, incl. the only Limine-aware file;
                  headers used only inside arch/ (gdt.h, idt.h, trap.h,
                  lapic.h) live here
  core/           portable kernel logic (main, panic, cmdline, pmm, slab,
                  kstack, sched, process, elf, syscall, object, cap, ipc,
                  spawn, irq, memory, self-tests)
  lib/            freestanding libc pieces (string, kprintf)
  include/arch/   interface core/ uses to reach arch code (incl. irq.h and
                  ioport.h: interrupt lines and I/O-port permissions)
  include/kernel/ core interfaces
  linker.ld
include/abi/      what the kernel and user programs share: syscall numbers,
                  rights, error codes, initial capability slots (boot.h),
                  server protocols (proto.h, console.h, pci.h), test
                  protocol (no kernel internals)
user/
  lib/            user runtime: crt0 (_start), syscall and IPC wrappers,
                  printf/snprintf, console client, port I/O (io.h)
  root/           the root task: starts the system from a manifest (init.c);
                  also runs the kernel's user-mode and IPC tests
  console/        the console server: the COM1 serial driver
  pci/            the PCI server
  blk/            the virtio-blk server (legacy PCI virtqueue and DMA)
  fs/             read-only single-file filesystem
  cat/            file reader using only the filesystem and console
  disk/           source file for the generated disk image
  hello/          a console client
  *.manifest      which programs the root task starts, and with what
  linker.ld       user programs load at 0x400000
scripts/test.sh   boot-and-check test runner (`make test`)
docs/DESIGN.md
limine.conf       loads the user programs and manifests as boot modules
```

`make` also creates `build/disk.img`, the disk QEMU attaches as a virtio
block device.

Rule: `core/` never includes `<limine.h>` or uses inline asm; it goes through
`include/arch/`. Boot data (memory map, command line) is copied into
`struct boot_info` once, so the rest of the kernel is bootloader-neutral.

## Traps (M1)

- **GDT order** is fixed by `syscall`/`sysret`: null, kernel code `0x08`,
  kernel data `0x10`, user data `0x18`, user code `0x20`, TSS `0x28`
  (16 bytes). `sysret` derives user SS and CS from one base, so user data
  must sit directly before user code.
- **IST (Interrupt Stack Table)**: double fault, NMI (non-maskable
  interrupt) and machine check each run on their own 16 KiB stack from the
  TSS. A fault caused by a broken stack (overflow, bad RSP) therefore still
  gets reported instead of escalating to a triple fault, which silently
  resets the machine. `selftest=doublefault` proves this.
- **Entry stubs**: `isr.S` generates 256 stubs. Each pushes a dummy error
  code where the CPU doesn't, then the vector number, so every trap reaches
  `trap_dispatch()` with the same `struct trap_frame` layout.
- **Policy today**: `#BP` (breakpoint) prints and resumes; any other
  exception prints the cause, registers, control registers and a
  frame-pointer backtrace, then panics; in user mode it kills the process
  instead (M4). Of vectors 32–255, only the timer (0x30), the PIC's device
  lines (0x20–0x2f, M7) and spurious interrupts are expected; any other one
  panics. Since M9 the supervisor also receives a notification after user-process
  teardown; the report remains on the kernel log.
- **RSP0**, the kernel stack the CPU loads when a ring-3 thread traps, is
  set in the TSS on every context switch (M3/M4).

## Testing

`make test` runs `scripts/test.sh`, which boots the kernel in QEMU once per
case with a different kernel command line (`CMDLINE`, passed through
`limine.conf`) and checks the serial log and QEMU's exit status. QEMU's
`isa-debug-exit` device turns `qemu_exit(code)` into exit status
`code*2+1`: 1 = normal finish, 3 = panic, 124 = timeout (hang or reboot).
`panic()` calls `qemu_exit(1)` so failing cases end immediately.

Destructive tests are selected with `selftest=<name>` (memory tests in
`core/selftest.c`, scheduler tests in `core/selftest_sched.c`, user tests
in `core/selftest_user.c`, IPC tests in `core/selftest_ipc.c`, system
tests in `core/selftest_system.c`, CPU tests in `arch/x86_64/selftest.c`).
The user, IPC and system tests are the exception: the kernel must survive
them, so they end with status 1.

Every boot also runs the non-destructive tests:
- from `kmain`: an `int3` round trip, and PMM, paging (including switching
  to a second address space) and slab checks;
- from the init thread: the scheduler checks (priority order, sleep
  length, preemption of two threads that never yield, leak-free thread
  exit);
- the root task, which checks its own privilege level and syscall error
  handling, then computes for 10^9 cycles without syscalls while a kernel
  thread competes for the CPU; its exit code and a leak check on its
  memory are verified;
- the IPC test (see IPC below): a server and a client process, checking
  both exit codes, the number of direct switches, and that every frame
  came back;
- the system itself (see Starting the system below): the root task runs
  `system.manifest`, and the kernel checks it exited cleanly and that
  every frame came back. A named system test (`app-crash`,
  `console-input`) runs its own manifest instead.

`console-input` needs typed input: the script runs QEMU with a named pipe
as its stdin and writes a line into it once the guest prints that it is
waiting. Text sent earlier could arrive before the kernel sets up the
UART, which discards it.

## Memory (M2)

- **Virtual layout** (per address space):
  - `0x0000_0000_0000_0000 – 0x0000_7fff_ffff_ffff` user half (empty in
    the kernel's own address space, so a stray low pointer always faults)
  - `0xffff_8000_0000_0000 – …` HHDM: direct map of all RAM-like regions
    (usable, bootloader-reclaimable, kernel, ACPI tables), read/write, NX
  - `0xffff_ffff_8000_0000 – …` kernel image
- **Kernel image permissions** follow W^X (write xor execute): `.text` is
  read + execute, `.rodata` read-only, `.data/.bss` read/write + NX. The
  linker script page-aligns each section so they can be mapped separately.
  `selftest=write-text` and `selftest=exec-data` prove both directions.
- **Shared kernel half**: `paging_init()` creates all 256 kernel-half
  PDPTs (page directory pointer tables) up front, costing 1 MiB. The
  kernel's PML4 entries then never change, so a new address space just
  copies them, and nothing has to be synced later. Kernel-half pages are
  marked global (CR4.PGE) so a CR3 switch doesn't flush them from the TLB
  (translation lookaside buffer).
- **Page sizes**: 2 MiB pages where physical and virtual addresses are both
  aligned (most of the HHDM), 4 KiB elsewhere. 1 GiB pages aren't used.
- **Frames**: PMM (physical memory manager) is a bitmap with one bit per
  4 KiB frame and a next-fit search. Frame 0 is never handed out, so 0 means
  "no memory". Bootloader-reclaimable memory (Limine's page tables, stack
  and boot data) is handed to the PMM once the kernel runs on its own
  stacks, at the start of the init thread. Physically contiguous runs
  (DMA buffers, since M8) are found by a linear scan of the bitmap; a buddy
  allocator would make that faster if it ever matters.
- **Allocator locking**: the PMM and the slab caches run with interrupts
  off. System calls are preemptible, so before M8 two threads could in
  principle be inside an allocator at once; nothing triggered it, but
  M8's user-driven allocations made it worth closing.
- **Kernel objects**: slab caches, one per object type (thread, endpoint,
  capability table). One slab = one 4 KiB page with a header at the start,
  so freeing finds the slab by rounding down. Free objects carry a magic
  word, which catches double frees. Empty slabs go straight back to the
  PMM. There is no general-purpose `kmalloc`, so user requests can never
  make the kernel allocate odd-sized blocks.
- **User memory** (since M8): granted as frame capabilities and mapped
  with `map` (see Memory and PCI below). No `mmap` or demand paging in the
  kernel; a user-space pager can add that.
- Device memory (LAPIC, IOAPIC, HPET) is not in the HHDM; whoever needs it
  maps it uncached (`PAGE_UNCACHED`).

## Threads and scheduling (M3)

- **Tick**: the LAPIC timer in periodic mode at 100 Hz (`SCHED_HZ`),
  calibrated at boot against PIT (programmable interval timer) channel 2,
  polled through port 0x61 so no interrupt is needed for calibration. The
  legacy 8259 PIC is remapped to vectors 0x20–0x2f and fully masked (until
  M7 unmasks lines for drivers); its spurious interrupts are ignored.
  Timer = vector 0x30, LAPIC spurious = 0xff.
- **Threads**: `struct thread` comes from a slab cache. Each thread has a
  16 KiB kernel stack in a dedicated region (PML4 slot 510), one 32 KiB
  slot per stack: 16 KiB unmapped guard below 16 KiB of stack. An overflow
  hits the guard, the CPU can't push the page-fault frame there, and the
  double fault is reported on its IST stack (`selftest=stack-overflow`).
- **Context switch** (`switch.S`) saves only the callee-saved registers on
  the old stack and swaps RSP. A new thread's stack is pre-built so the
  first switch "returns" into a trampoline that calls `thread_entry()`.
- **Policy**: 256 fixed priorities (255 highest). The highest-priority
  ready thread runs; equal priorities round-robin in 2-tick (20 ms)
  timeslices. A thread that becomes ready with higher priority than the
  running one (created, or woken by the timer) preempts it immediately.
  Run queues are one FIFO per priority plus a 256-bit bitmap, so picking
  the next thread is constant time. The idle thread (priority 0, never
  queued) runs `sti; hlt`.
- **Where preemption happens**: only at the end of an interrupt, in
  `sched_preempt_if_needed()`. The interrupted thread's trap frame stays on
  its own kernel stack, so when it's picked again it simply returns from
  the handler and `iretq`s. Voluntary switches happen in `thread_yield`,
  `thread_sleep` and `thread_exit`.
- **Locking**: one CPU, so disabling interrupts (`irq_save`/`irq_restore`)
  is the lock. Every switch happens with interrupts off; each thread gets
  its own interrupt state back when it resumes.
- **Exit**: a dead thread can't free the stack it's running on, so the next
  thread frees it right after the switch (`finish_switch`).
- **Per-CPU data**: `struct cpu` (current, idle, need_resched, counters) is
  reached through the GS base via `this_cpu()`, so adding CPUs later means
  one `struct cpu` each plus real locks, not a rewrite.
- **Boot handoff**: `sched_start()` switches from Limine's stack to the
  `init` thread and never returns. Init then hands Limine's leftover memory
  (its page tables, stack and boot data, about 1 MiB here) to the PMM.
- Sleeping threads sit on one unsorted list scanned each tick. Fine for
  tens of threads; a sorted list or timer wheel can come later.

## User mode (M4)

- **Processes**: a process is an address space plus (for now) one thread.
  `thread->proc` tells the scheduler which page-table root to load; kernel
  threads run on whatever is loaded, since the kernel half is the same
  everywhere, which avoids needless TLB flushes.
- **Loading**: the root task is an ordinary static ELF that Limine loads as
  a boot module. The kernel's ELF loader treats it as untrusted: every
  offset and size is bounds-checked, segments must lie in the user half
  (above the never-mapped null page), and a segment that is both writable
  and executable is refused, so W^X holds for user code too. Each page gets
  a fresh zeroed frame. The user stack is 16 KiB just below the top of the
  user half, with unmapped pages on both sides.
- **Entering ring 3**: `arch_enter_user()` builds an `iretq` frame (user CS
  0x23, SS 0x1b, interrupts on), zeroes every register but the argument so
  no kernel values leak, and does `swapgs`.
- **GS discipline**: in the kernel, GS base points at `struct cpu`; in user
  mode it is the user's own (0). Every entry from ring 3 (interrupt stubs
  check the saved CS; `syscall` always comes from ring 3) does `swapgs`,
  and every return to ring 3 swaps back.
- **System calls**: `syscall` → `syscall_entry` switches to the thread's
  kernel stack (kept in `struct cpu` next to TSS.RSP0), builds the same
  `struct trap_frame` an interrupt would, enables interrupts (the kernel is
  preemptible inside syscalls) and calls the portable `syscall_handle()`.
  The return path uses `iretq`, not `sysretq`: slightly slower, but it
  works for any frame and avoids the `sysretq` non-canonical-RIP bug that
  faults in ring 0 on Intel CPUs.
- **User pointers**: never dereferenced before `paging_user_range_ok()`
  confirms every page is present, user-accessible and (if needed)
  writable; otherwise the call returns `-ERR_FAULT`. Safe without locks
  while each process has one thread.
- **Faults**: an exception from ring 3 prints the usual report (minus the
  backtrace: user frame pointers are untrusted), kills only that process
  and frees its memory; the kernel carries on. Tests: `user-pagefault`,
  `user-privileged` (`cli` → #GP), `user-kernel-read`.
- **Teardown**: a process's memory is freed when its thread is reaped, i.e.
  after the scheduler has switched to another address space; if the next
  thread is a kernel thread, the scheduler loads the kernel's root first so
  the dying root is never the active one.
- **FPU/SSE**: the kernel doesn't save FPU/SSE registers on a switch yet,
  so user programs are compiled with `-mgeneral-regs-only` too. Saving them
  (`fxsave`/`xsave`) is needed before running arbitrary compiled code.
- Waiting for a process to exit blocks on a wait queue (since M5).

## IPC (M5)

- **Endpoints** are synchronous rendezvous points: a sender waits until a
  receiver takes its message and vice versa. Each endpoint has one queue
  of waiting senders and one of waiting receivers; at most one is
  non-empty.
- **Messages** are 4 words in registers, the sender's badge, and at most
  one capability. Message words are copied once, straight into the
  receiver's syscall results; there are no kernel message buffers.
- **Calls**: `send`, `recv`, `call` (send, then wait for the reply),
  `reply`, and `reply_recv` (reply, then wait for the next message: a
  server's whole main loop is one syscall).
- **Replies** go to an implicit per-thread reply slot: receiving a `call`
  records the caller as `reply_to`, so a server can only answer the client
  that called it, exactly once. This is the "one-shot reply capability"
  without a table slot.
- **Fast path**: when a `call` finds a server already waiting, the kernel
  blocks the caller and switches straight to the server
  (`sched_block_and_switch`), skipping the run queue; `reply_recv`
  switches straight back. The test counts these: ~2000 direct switches
  for 1000 round trips. (Raw cycle counts under QEMU's emulator say
  nothing about real hardware, so they're printed but not judged.)
- **Death**: a server that dies or calls `recv` without replying makes its
  pending caller's `call` return `-ERR_DEAD` instead of hanging forever
  (`selftest=ipc-server-dies`). Clients queued on the endpoint stay queued:
  whoever receives on it next (a restarted server, in M9) serves them.
- **Notifications**: a word of bits. `notify` ORs in bits and the cap's
  badge and never blocks; `wait` returns and clears the word, blocking if
  it is zero. Meant for interrupts and events.
- **Bound notifications** (since M7): a thread can bind one notification
  to itself (`ntfn_bind`, one per thread and per notification). If it is
  signalled while the thread is blocked in `recv` or `reply_recv`, the
  call returns `IPC_NOTIFIED` with the bits instead of a message; pending
  bits are taken before waiting senders, so clients can't starve a
  driver's interrupts. One server thread can therefore serve clients and
  its device at once, as in seL4.
- **Locking**: every IPC path runs with interrupts off, which on one CPU
  makes each operation atomic.
- Blocked threads hold a reference to the object they wait on, so an
  endpoint or notification can never be freed while anyone is queued.
- `process_wait` in the kernel now blocks on a wait queue instead of
  polling once per tick.

## Capabilities (M5)

- **Capability tables**: each process has a flat table of 64 slots
  (`struct cspace`), embedded in `struct process`. A handle is a slot
  number; slot 0 is never used. A slot holds `{object, rights, badge}`.
- **Rights**: READ (recv/wait), WRITE (send/call/notify), GRANT
  (capabilities may travel in messages over this endpoint cap; for a
  reply, the caller's cap must have GRANT, so a client decides whether it
  accepts capabilities back).
- **Operations**: `cap_copy` derives a copy with a subset of the rights,
  and can stamp a badge on an unbadged cap ("mint"), never re-badge one;
  `cap_delete` drops one. Sending a cap in a message copies it into the
  receiver's table; the sender keeps its own.
- **Badges** let a server tell clients apart: every message sent through a
  badged endpoint cap carries that badge, and the receiver sees it.
- **Objects** (`struct kobj`) are reference-counted: each capability and
  each thread blocked on the object holds one; the last `kobj_put` frees
  it. Types so far: endpoint, notification, factory, module and process
  (since M6), I/O-port range and IRQ line (since M7), frame and IRQ
  control (since M8).
- **Factories** replace seL4's untyped memory with something simpler:
  creating a kernel object requires a factory capability, and each factory
  has a budget. Freeing an object refunds its factory (which stays alive
  until all its objects are gone). So there is no ambient authority to
  consume kernel memory: a process can only create as many objects as the
  factory it was given allows. The tests give a factory to the server only.
  Since M8 a factory also has a page budget for frames, and can be split:
  `factory_split` carves a smaller factory out of a bigger one, and the
  budget goes back when the smaller one is freed.
- **Not yet**: revocation (deleting every capability derived from one).
  It needs a derivation tree per object; it comes with supervision in M9,
  where the supervisor must be able to take capabilities back.

## Starting the system (M6)

- **What the kernel hands over**: the root task is the only process the
  kernel creates by itself. It starts at priority 250 with a factory
  (slot 1: 64 objects and 512 pages), every I/O port (slot 2), the IRQ
  control capability (slot 3) and one read-only capability per boot
  module (slots 4 and up, in load order). That is all the authority in the
  system; everything else is derived from it (`include/abi/boot.h`).
- **Manifest**: a text file loaded as a boot module, one program per line:
  name, image (a module), priority, the service it provides (or `-`), the
  services it uses (comma-separated, or `-`), the devices it drives (or
  `-`; since M7), its quota as `objects/pages` (or `-`; since M8), and an
  argument for `main`. The root task reads it with `module_read`.
- **What each program gets**: a server gets a receive capability
  (READ|GRANT) for its own endpoint in slot 1; a client gets a send
  capability (WRITE|GRANT) for each service it uses, in slots 2, 3, ... in
  the order listed, each with a badge unique to that client; a program
  with a quota gets its own factory in slot 7; a driver gets a
  notification in slot 8 and its devices' capabilities from slot 9.
  Nothing else. `hello` checks every one of its 63 slots to prove it. A
  device can be given to only one program. Device capabilities are carved
  out of the root task's own: `com1` is ports 0x3f8-0x3ff plus IRQ 4;
  `pci` is ports 0xcf8-0xcff, ports 0x1000-0xffff and IRQ control.
- **Order**: the root task first creates an endpoint per service, then the
  processes (handing out capabilities before any of them runs), then
  starts them all. Everything runs below the root task's priority, so
  nothing starts running until the root task blocks.
- **Process objects**: `proc_create` returns a process capability. WRITE
  lets the holder give the process capabilities (`proc_give`, which uses
  the same derivation rules as `cap_copy`, into a chosen slot) and start
  it; READ lets it wait for the exit code (`proc_wait`, or `-ERR_DEAD` if
  the process was killed). A process can't be created above its creator's
  priority. If the last capability to a process that was never started is
  dropped, the process is destroyed.
- **The root task's own capability is unbadged**: every client's cap is
  badged (>= 1), so a request with badge 0 can only come from the root
  task. Servers accept `PROTO_SHUTDOWN` only from badge 0. The root task
  shuts servers down in reverse order once every application has exited.
- **Console**: see Devices (M7).
- **Crash isolation**: `selftest=app-crash` runs `crash.manifest`, in which
  one application crashes (since M7, by writing to the serial port it
  doesn't own). The root task sees `-ERR_DEAD` from `proc_wait`, the other
  application and the console carry on, and shutdown proceeds normally.
- **Known compromises**:
  - The kernel still loads ELF images (`proc_create` takes a module
    capability), because user space can't map memory into another address
    space yet. In seL4 the root task does this itself from untyped memory
    and frame capabilities; here that waits for `map`/`unmap`.
  - A process costs one unit of factory budget, the same as an endpoint,
    although it uses far more memory (page tables, stack, image). Object
    budgets count objects, not bytes; only frames are charged by the page.
  - Servers are started by the root task, not restarted: supervision is
    M9.

## Devices (M7)

- **I/O ports**: an I/O-port capability names a range of ports.
  `ioport_enable` sets those bits in the process's permission bitmap; on
  every context switch the scheduler copies that bitmap into the TSS I/O
  permission bitmap, and the CPU checks every `in`/`out` in ring 3 against
  it. A refused port is a #GP and the process is killed
  (`selftest=app-crash` does exactly that). The permission stays with the
  process for its lifetime: deleting the capability doesn't revoke it
  (revocation is M9). Since M8 the bitmap covers all 65536 ports (8 KiB,
  allocated only for processes that enable a port); each process records
  which bytes of it are in use, so a switch rewrites only those (one byte
  for COM1, 16 for a virtio disk) instead of 8 KiB. `ioport_sub` makes a
  capability for part of a range, which is how the root task and the PCI
  server hand out exactly one device's ports.
- **Interrupt lines** are claimed from the IRQ control capability
  (`irq_get`, since M8), once each: while any copy of the resulting IRQ
  capability exists, nobody else can claim that line. Lines 0 (the old
  PIT timer) and 2 (the PIC cascade) can't be claimed.
- **Interrupts**: an IRQ capability names one line. `irq_bind` connects
  it to a notification and unmasks the line. When it fires, the kernel
  masks it, acknowledges the controller and signals the notification;
  it never touches the device. The driver handles the device, then
  `irq_ack` unmasks the line. A line with no binding stays masked.
- **Interrupt controller**: the legacy 8259 PIC, reached through the
  LAPIC's LINT0 in "virtual wire" mode (ExtINT). That's enough for the
  ISA lines (COM1 = IRQ 4) and for PCI devices' legacy INTx lines, which
  the firmware routes to PIC lines (the disk gets IRQ 11), and it needs no
  ACPI tables. The IOAPIC (found through ACPI's MADT) and MSI replace it
  later.
- **Console server = serial driver**: the root task gives it `com1` (the
  ports 0x3f8-0x3ff and IRQ 4). It enables receive interrupts, binds its
  notification to itself and waits in `recv` for clients and interrupts
  alike. Output is written by polling the transmitter (a few microseconds
  per byte at 115200 baud; instant under QEMU): an interrupt-driven
  transmit queue only pays off for far more output than a console
  produces. Input lines are echoed as `[console] input: ...`.
- **Naming**: the root task registers a name for each badge
  (`CONSOLE_SET_NAME`, accepted from badge 0 only), so every line the
  console prints starts with `[program]`, just like the kernel's debug
  output did.
- **Who can print**: `debug_write` is now refused (`-ERR_PERM`) to every
  process the root task starts; they can only print through the console.
  `hello` checks this. The root task and the kernel's self-test processes
  (started by the kernel itself) keep it. The root task uses it until the
  console is up and again after shutting it down, and prints through the
  console in between.
- **The kernel still writes its own log to COM1**: boot messages, fault
  reports and panics must work with no user space at all. The kernel
  never reads the port or enables its interrupts. Kernel lines and
  console lines can interleave between lines (not within one in
  practice, since the console writes a line in one go at priority 200).
  Moving the kernel log to a second port (COM2) would separate them.
- Input typed before the kernel initializes the UART is lost (the
  initialization clears its buffers).

## Memory and PCI (M8, first part)

- **Frames**: `frame_alloc` makes a frame of 1-64 physically contiguous,
  zeroed pages, charged to a factory's page budget. `map` puts it at a
  page-aligned address in the caller's own address space (between 4 GiB
  and 0x7f00_0000_0000, so it can't collide with the program or the
  stack), writable only if the capability has WRITE, never executable.
  `unmap` undoes it. Each mapping holds a reference to the frame, so the
  memory lives until the last mapping and the last capability are gone. A
  process can have 16 mappings; they're undone when it dies, before its
  own pages are freed. A frame capability can be sent to another process,
  which maps the same memory: shared memory.
- **Physical addresses**: `frame_info` tells the holder where the frame is
  in physical memory, because a device doing DMA (direct memory access)
  needs that. Without an IOMMU (I/O memory management unit) a driver that
  can program DMA can reach any memory anyway, so it is trusted with the
  whole machine whatever the kernel does; an IOMMU is a non-goal for now.
- **Quotas**: the manifest's quota column gives a program its own factory
  (`factory_split` of the root task's), so a driver's DMA buffers and
  notifications come out of its own budget, not everyone's.
- **PCI server**: owns configuration space (through the legacy port pair
  0xcf8/0xcfc), the I/O range 0x1000-0xffff where firmware puts devices'
  registers, and IRQ control. At start it scans all 256 buses and prints
  every function. A driver finds its device (`PCI_FIND` by vendor and
  device ID), claims it (`PCI_CLAIM`, one client per device; turns on I/O
  decoding and bus mastering), then gets each resource as a capability in
  a reply: `PCI_BAR` returns a capability for exactly that BAR's ports,
  `PCI_IRQ` one for the device's interrupt line. A second client gets
  `-ERR_EXIST` / `-ERR_PERM`; `blk` checks both, using a second badge.
- **The disk**: QEMU attaches a 1 MiB raw image as a virtio-blk device in
  transitional mode (`disable-legacy=off`), so it offers the legacy virtio
  interface (registers in an I/O BAR) next to the modern one. `blk`
  claims it, gets ports 0xc000-0xc07f and IRQ 11 (as routed by the
  firmware), enables the ports, and reads the capacity register: 2048
  sectors. M8b adds the virtqueue and filesystem described below.
- **Known compromises**:
  - Configuration access uses the legacy ports, which reach the first 256
    bytes of each function's configuration space. ECAM (the
    memory-mapped mechanism, 4 KiB per function) needs ACPI's MCFG table
    and device-memory mapping; nothing needs it yet.
  - Only I/O-port BARs are handed out. Memory-mapped BARs (which modern
    virtio and most real devices use) need device-memory capabilities
    with uncached mappings.
  - Legacy interrupts only (INTx, routed by the firmware to a PIC line).
    MSI needs the IOAPIC/LAPIC path and is deferred with it.
  - Device recovery is implemented only for the legacy virtio block
    device. Other devices keep their claims until the PCI server exits.

## Disk I/O and tiny filesystem (M8b)

The block server negotiates no optional virtio features and sets up legacy
queue 0 in a contiguous frame from its page quota. The used ring starts
on a 4 KiB boundary. A three-descriptor chain contains a request header,
a 512-byte buffer, and device-written status. One request is outstanding
at a time; memory barriers order publication and completion. An IRQ wakes
`wait` on the driver's notification; reading the ISR deasserts INTx before
`irq_ack` unmasks the PIC line. The device is reset before normal shutdown
so it stops accessing the frame before process teardown.

A boot self-test reads the last sector, writes a pattern, reads it back,
and restores the original contents. Clients get a read-only block protocol:
24-byte slices of a sector, with capacity and bounds checks. No arbitrary
physical addresses or client pointers are accepted. See
`include/abi/storage.h` for both storage protocols. Queue layout follows the
[OASIS virtio legacy transport specification](https://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html).

`scripts/mkdisk.py` builds a 1 MiB image from `user/disk/hello.txt`.
Sector 0 holds a versioned magic, file size and bounded filename; file data
starts at sector 1, with the last sector reserved for the DMA test. The
filesystem validates this metadata and serves stat and byte reads, splitting
requests that cross sector boundaries. It has only console and block
capabilities. The `cat` process has only console and filesystem capabilities.
The root task stops the filesystem before the block server and PCI server.

This is deliberately a single-file, read-only filesystem with no directory
or write API. The driver supports QEMU's legacy port interface, queues of
3..256 descriptors (power of two), and coherent x86 DMA. Device timeouts,
read-only disks and hot unplug remain future work; normal operation requires
a responsive writable disk. M9 adds pinned-buffer teardown during recovery.

## Drivers and modules

- A "module" is a user-space program image (or a manifest) loaded by the
  bootloader. The root task starts the programs (see above).
- **Supervision**: the root task watches every child's exit through a
  notification. For a disk-driver fault it resets the device, restarts the
  process and re-hands its capabilities. The filesystem retries the call
  using its existing endpoint capability (details below).
- **Interfaces**: each server's message protocol is a versioned header in
  `include/abi/` shared by server and clients (`proto.h` for the common
  rules, `console.h`, ...). No IDL until hand-written protocols hurt.
- **PCI**: a user-space PCI server enumerates devices and hands out
  per-device port and IRQ capabilities to drivers (since M8, see above).

## Fault supervision and disk-driver restart (M9)

`proc_watch(proc, notification, bits)` registers a notification before a
process is started. It needs READ on the process and WRITE on the
notification. The kernel holds a reference until teardown and signals after
address-space and capability cleanup, for both ordinary exits and faults.
`proc_poll` returns `-ERR_AGAIN` while running, `-ERR_DEAD` after a fault,
or the normal exit code. Notifications may coalesce; status stays readable.
A never-started process can still be discarded without producing an exit.

Root now waits on a shared child-exit notification, using one bit per
manifest entry. It polls signalled children, counts completed applications,
and allows one automatic restart of a faulted block server. Other unexpected
server exits are errors. The restarted driver gets the same service endpoint
and the same client badges, with a fresh process, quota and event notification.
This keeps queued calls reachable without redistributing endpoint capabilities.
The filesystem retries a transport `-ERR_DEAD` once; it remains the same process.

DMA memory must outlive a faulting driver. Before enabling its virtqueue the
driver transfers a frame capability to the PCI server using `PCI_PIN_DMA`.
The PCI server keeps that frame alive even after process teardown. Only root
can call `PCI_RECOVER`, and does so after observing the old process's death.
For the supported legacy virtio block device, recovery disables PCI bus
mastering, resets device status and confirms reset, masks/unbinds the IRQ,
then drops the pinned frame and releases the claim. The replacement can
claim the device again. PCI command writes use 16-bit accesses to avoid
writing back the adjacent write-one-to-clear status register.

`selftest=driver-restart` injects a null-pointer fault after submitting a
file-sector DMA request. The filesystem's in-flight call receives `-ERR_DEAD`,
root recovers the disk, and the retry queues until the replacement receives.
`cat` finishes the multi-sector file. Both the ordinary boot and restart
test verify that all frames return after shutdown, including pinned buffers
and the old IRQ notification. Permission tests cover process observation,
IRQ unbinding and supervisor-only device recovery.

Limits: this is bounded restart of this disk driver, not general capability
revocation or arbitrary server supervision. PCI and the supervisor are trusted;
recovery must not run while the old driver is alive. No IOMMU or protection
against a malicious bus-mastering driver is provided. A device that never
completes I/O still needs a timeout/watchdog policy.

## Syscall ABI

Entered with `syscall`; number in `rax`, args in `rdi, rsi, rdx, r10, r8, r9`,
result in `rax` (negative = `-ERR_*`); `rcx` and `r11` are clobbered. IPC
calls also return the message in the argument registers. Numbers, rights,
object types and error codes live in `include/abi/syscall.h`.

| # | Call | Notes |
|---|---|---|
| 0 | `debug_write(buf, len) -> len` | debug only; refused to processes the root task starts |
| 1 | `exit(code)` | |
| 2 | `yield()` | |
| 3 | `send(ep, w0..w3, cap)` | needs WRITE (GRANT to send a cap) |
| 4 | `recv(ep) -> msg` | needs READ; `IPC_NOTIFIED` + bits if the bound notification fired |
| 5 | `call(ep, w0..w3, cap) -> reply` | needs WRITE |
| 6 | `reply(w0..w3, cap)` | to the last caller received |
| 7 | `reply_recv(ep, w0..w3, cap) -> msg` | the server loop |
| 8 | `notify(ntfn, bits)` | needs WRITE |
| 9 | `wait(ntfn) -> bits` | needs READ |
| 10 | `cap_copy(slot, rights, badge) -> slot` | rights must be a subset |
| 11 | `cap_delete(slot)` | |
| 12 | `obj_create(factory, type) -> slot` | endpoint or notification; uses budget |
| 13 | `module_info(mod, name_buf, len) -> size` | needs READ; copies the name out |
| 14 | `module_read(mod, offset, buf, len) -> bytes` | needs READ |
| 15 | `proc_create(factory, mod, prio, name, len) -> slot` | ELF from a module; prio <= caller's; uses budget |
| 16 | `proc_give(proc, slot, dst_slot, rights, badge)` | needs WRITE; cap_copy rules, into another table |
| 17 | `proc_start(proc, arg)` | needs WRITE; once |
| 18 | `proc_wait(proc) -> code` | needs READ; `-ERR_DEAD` if killed |
| 19 | `ioport_enable(ioport)` | needs WRITE; lasts for the process's lifetime |
| 20 | `irq_bind(irq, ntfn, bits)` | needs WRITE on both; unmasks the line |
| 21 | `irq_ack(irq)` | needs WRITE; unmasks the line again |
| 22 | `ntfn_bind(ntfn)` | needs READ; recv can now return `IPC_NOTIFIED` |
| 23 | `frame_alloc(factory, pages) -> slot` | 1-64 contiguous zeroed pages; uses page budget |
| 24 | `frame_info(frame) -> phys` | needs READ; pages in rdi |
| 25 | `map(frame, vaddr, writable)` | needs READ (and WRITE if writable); own address space |
| 26 | `unmap(vaddr)` | undoes the `map` that started at vaddr |
| 27 | `factory_split(factory, objects, pages) -> slot` | a quota carved out of a factory |
| 28 | `ioport_sub(factory, ioport, base, count) -> slot` | part of a port range |
| 29 | `irq_get(factory, irq_control, line) -> slot` | each line claimed once |
| 30 | `proc_watch(proc, ntfn, bits)` | register exit notification before start; READ process, WRITE notification |
| 31 | `proc_poll(proc) -> code` | nonblocking status; `-ERR_AGAIN` while running, `-ERR_DEAD` after a fault |
| 32 | `irq_unbind(irq)` | WRITE; mask and release the old notification |
| later | `thread_create`, `cap_revoke` | future work |

## Milestones

| | Milestone | Done when |
|---|---|---|
| **M0** | Boot via Limine, serial output, memory map | kernel boots in QEMU and prints the memory map ✅ |
| **M1** | GDT/TSS, IDT, exception handlers | a deliberate #PF prints a register dump; a #DF on a broken stack is caught via IST ✅ |
| **M2** | Frame allocator, page tables, kernel slab heap | kernel runs on its own PML4 (top-level page table) with W^X; second address space works ✅ |
| **M3** | LAPIC timer, kernel threads, scheduler | two kernel threads that never yield share the CPU; stack overflow caught by guard page ✅ |
| **M4** | Ring 3, `syscall`, root task loaded from an ELF module | root task prints via a syscall; preempted in ring 3 and resumed; a faulting process is killed and the kernel survives ✅ |
| **M5** | Endpoints, notifications, capability tables | two processes talk only through capabilities they were given: 1000 call/reply round trips on the fast path, a capability passed in a reply, a cross-process notification; a dying server returns -ERR_DEAD to its caller ✅ |
| **M6** | Root task spawns servers from a manifest | the root task starts a console server and a client from `system.manifest`, each holding only the capabilities its line grants; one crashing application leaves the rest running ✅ |
| **M7** | User-space serial console driver (IRQ via notification) | the console server drives COM1 with its own I/O ports and IRQ 4; programs the root task starts can only print through it; a line typed on the serial port reaches it by interrupt; a program touching the port without permission is killed ✅ |
| **M8a** | Memory capabilities + PCI server | a driver gets a frame from its own quota and maps it; the PCI server hands it exactly its disk's ports and IRQ; it reads the disk's size from the device ✅ |
| **M8b** | virtio-blk driver + tiny FS server | `cat` prints `hello.txt` from disk; DMA read/write, IRQ completion and leak checks pass ✅ |
| **M9** | Fault supervision + restart | disk driver faults after DMA submission; root resets and restarts it, filesystem retries, `cat` finishes, no frames leak ✅ |
| M10 | virtio-net + user-space TCP/IP stack (stretch) | ping the guest |

#PF = page fault, #DF = double fault.

## Non-goals (for now)

POSIX, SMP, graphics, swapping, ACPI beyond what's needed to find the
IOAPIC/HPET, UEFI runtime services, security hardening beyond the
capability model (KASLR, SMAP/SMEP come later and are cheap to add).

## Prior art to read

seL4 (capabilities, derivation trees), the L4 family (IPC fast path),
Zircon (handles, ports), Minix 3 (driver restart / reincarnation server),
xv6 (clean small-kernel code). This project borrows freely; its purpose is
learning and a working demo, not novelty.