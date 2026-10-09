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
6. **Interrupt routing**: IOAPIC (I/O APIC) or MSI → notification to the
   driver thread that holds the IRQ (interrupt request line) capability.

Not in the kernel: device drivers (except the early debug serial and the
timer the scheduler needs), file systems, networking, ELF (executable file
format) loading of anything but the root task, process-management policy,
POSIX (the Unix API standard).

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
                  self-tests)
  lib/            freestanding libc pieces (string, kprintf)
  include/arch/   interface core/ uses to reach arch code
  include/kernel/ core interfaces
  linker.ld
include/abi/      what the kernel and user programs share: syscall numbers,
                  rights, error codes, test protocol (no kernel internals)
user/
  lib/            user runtime: crt0 (_start), syscall and IPC wrappers, printf
  root/           the root task (also runs the IPC test server and client)
  linker.ld       user programs load at 0x400000
scripts/test.sh   boot-and-check test runner (`make test`)
docs/DESIGN.md
limine.conf       also loads user/root.elf as a boot module
```

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
  frame-pointer backtrace, then panics. Of vectors 32–255, only the timer
  (0x30) and spurious interrupts are expected (see M3); any other one
  panics. From M9, faults in user threads go to the supervisor instead.
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
in `core/selftest_user.c`, IPC tests in `core/selftest_ipc.c`, CPU tests
in `arch/x86_64/selftest.c`). The user and IPC tests are the exception:
the kernel must survive them, so they end with status 1.

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
  came back.

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
  stacks, at the start of the init thread. A buddy allocator comes later
  if a driver needs physically contiguous buffers.
- **Kernel objects**: slab caches, one per object type (thread, endpoint,
  capability table). One slab = one 4 KiB page with a header at the start,
  so freeing finds the slab by rounding down. Free objects carry a magic
  word, which catches double frees. Empty slabs go straight back to the
  PMM. There is no general-purpose `kmalloc`, so user requests can never
  make the kernel allocate odd-sized blocks.
- **User memory**: granted as frame capabilities and mapped with `map()`.
  No `mmap` or demand paging in the kernel; a user-space pager can add that.
- Device memory (LAPIC, IOAPIC, HPET) is not in the HHDM; whoever needs it
  maps it uncached (`PAGE_UNCACHED`).

## Threads and scheduling (M3)

- **Tick**: the LAPIC timer in periodic mode at 100 Hz (`SCHED_HZ`),
  calibrated at boot against PIT (programmable interval timer) channel 2,
  polled through port 0x61 so no interrupt is needed for calibration. The
  legacy 8259 PIC is remapped to vectors 0x20–0x2f and fully masked; its
  spurious interrupts are ignored. Timer = vector 0x30, LAPIC spurious =
  0xff.
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
  it. Types so far: endpoint, notification, factory.
- **Factories** replace seL4's untyped memory with something simpler:
  creating a kernel object requires a factory capability, and each factory
  has a budget. Freeing an object refunds its factory (which stays alive
  until all its objects are gone). So there is no ambient authority to
  consume kernel memory: a process can only create as many objects as the
  factory it was given allows. The tests give a factory to the server only.
- **Not yet**: revocation (deleting every capability derived from one).
  It needs a derivation tree per object; it comes with supervision in M9,
  where the supervisor must be able to take capabilities back.
- x86-specific (later): I/O-port capabilities enforced through the TSS
  I/O permission bitmap.

## Drivers and modules

- A "module" is a user-space server binary.
- The root task is the first and only process the kernel loads (from a
  Limine module). It reads a manifest, starts each server, and hands it
  exactly the capabilities it needs (e.g. the console server gets the COM1
  I/O-port cap and IRQ 4).
- **Supervision**: the root task holds a notification bound to each child's
  fault handler. On a fault it tears the server down, restarts it, and
  re-hands its caps. Clients see an IPC error and reconnect.
- **Interfaces**: each server's message protocol is a versioned header in
  `proto/` shared by server and clients. No IDL until hand-written protocols
  hurt.
- **PCI**: a user-space PCI server owns ECAM, enumerates devices and hands
  out per-device config/BAR/MSI caps to drivers.

## Syscall ABI

Entered with `syscall`; number in `rax`, args in `rdi, rsi, rdx, r10, r8, r9`,
result in `rax` (negative = `-ERR_*`); `rcx` and `r11` are clobbered. IPC
calls also return the message in the argument registers. Numbers, rights,
object types and error codes live in `include/abi/syscall.h`.

| # | Call | Notes |
|---|---|---|
| 0 | `debug_write(buf, len) -> len` | debug only; goes away once the console server exists |
| 1 | `exit(code)` | |
| 2 | `yield()` | |
| 3 | `send(ep, w0..w3, cap)` | needs WRITE (GRANT to send a cap) |
| 4 | `recv(ep) -> msg` | needs READ |
| 5 | `call(ep, w0..w3, cap) -> reply` | needs WRITE |
| 6 | `reply(w0..w3, cap)` | to the last caller received |
| 7 | `reply_recv(ep, w0..w3, cap) -> msg` | the server loop |
| 8 | `notify(ntfn, bits)` | needs WRITE |
| 9 | `wait(ntfn) -> bits` | needs READ |
| 10 | `cap_copy(slot, rights, badge) -> slot` | rights must be a subset |
| 11 | `cap_delete(slot)` | |
| 12 | `obj_create(factory, type) -> slot` | endpoint or notification; uses budget |
| later | `map`, `unmap`, `thread_create`, `process_create`, `cap_revoke` | M6/M9 |

## Milestones

| | Milestone | Done when |
|---|---|---|
| **M0** | Boot via Limine, serial output, memory map | kernel boots in QEMU and prints the memory map ✅ |
| **M1** | GDT/TSS, IDT, exception handlers | a deliberate #PF prints a register dump; a #DF on a broken stack is caught via IST ✅ |
| **M2** | Frame allocator, page tables, kernel slab heap | kernel runs on its own PML4 (top-level page table) with W^X; second address space works ✅ |
| **M3** | LAPIC timer, kernel threads, scheduler | two kernel threads that never yield share the CPU; stack overflow caught by guard page ✅ |
| **M4** | Ring 3, `syscall`, root task loaded from an ELF module | root task prints via a syscall; preempted in ring 3 and resumed; a faulting process is killed and the kernel survives ✅ |
| **M5** | Endpoints, notifications, capability tables | two processes talk only through capabilities they were given: 1000 call/reply round trips on the fast path, a capability passed in a reply, a cross-process notification; a dying server returns -ERR_DEAD to its caller ✅ |
| M6 | Root task spawns servers from a manifest | console server running in ring 3 |
| M7 | User-space serial console driver (IRQ via notification) | kernel stops printing directly |
| M8 | PCI server + virtio-blk/AHCI driver + tiny FS server | `cat` a file from disk |
| M9 | Fault supervision + restart | kill the disk driver, system survives |
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