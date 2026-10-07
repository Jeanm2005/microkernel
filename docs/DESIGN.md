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
                  headers used only inside arch/ (gdt.h, idt.h, trap.h) live here
  core/           portable kernel logic (main, panic, cmdline, pmm, slab,
                  kstack, sched, self-tests; later ipc/cap)
  lib/            freestanding libc pieces (string, kprintf)
  include/arch/   interface core/ uses to reach arch code
  include/kernel/ core interfaces
  linker.ld
scripts/test.sh   boot-and-check test runner (`make test`)
docs/DESIGN.md
limine.conf
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
  set per thread on context switch once user mode exists (M4).

## Testing

`make test` runs `scripts/test.sh`, which boots the kernel in QEMU once per
case with a different kernel command line (`CMDLINE`, passed through
`limine.conf`) and checks the serial log and QEMU's exit status. QEMU's
`isa-debug-exit` device turns `qemu_exit(code)` into exit status
`code*2+1`: 1 = normal finish, 3 = panic, 124 = timeout (hang or reboot).
`panic()` calls `qemu_exit(1)` so failing cases end immediately.

Destructive tests are selected with `selftest=<name>` (memory tests in
`core/selftest.c`, scheduler tests in `core/selftest_sched.c`, CPU tests in
`arch/x86_64/selftest.c`). Every boot also
runs the non-destructive ones: an `int3` round trip, and PMM, paging (incl.
switching to a second address space) and slab checks from `kmain`, then
from the init thread the scheduler checks: priority order, sleep length,
preemption of two threads that never yield, and leak-free thread exit.

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

## IPC

- **Endpoints**: synchronous rendezvous. Sender blocks until a receiver is
  ready (and vice versa).
- **Messages**: a small fixed payload passed in registers (target: 6 words)
  plus optional capability transfer. Bulk data goes through shared-memory
  regions set up with frame capabilities.
- **Calls**: `send`, `recv`, `call` (send + wait for reply), `reply_recv`
  (reply to last caller + wait for next; the server loop's single syscall),
  `notify`/`wait` for notifications.
- **Reply capability**: `call` gives the server a one-shot reply cap, so a
  server can only answer the client that called it.
- **Notifications**: a word of bits, OR-ed on signal. Used for IRQs and
  async events. Never blocks the signaller.

## Capabilities

- Every process has a capability table (CSpace): a flat array of slots to
  start; a two-level table if we need more than a few hundred slots.
- A slot holds `{object pointer, type, rights}`. Rights: read, write, grant.
- Object types: thread, address space, endpoint, notification, frame,
  IRQ, I/O-port range, cap table.
- Operations: copy (with equal or fewer rights), move, delete, revoke
  (deletes all caps derived from this one via a derivation tree).
- Simplification vs seL4: no "untyped memory" retyping at first. The kernel
  allocates objects from its own pools, and per-process quotas stop abuse.
- x86-specific: **I/O-port capabilities** are enforced through the TSS I/O
  permission bitmap, switched on context switch for threads that hold one.

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

## Syscall ABI (target)

Entered with `syscall`; number in `rax`, args in `rdi, rsi, rdx, r10, r8, r9`.

| # | Call | Purpose |
|---|---|---|
| 0 | `send(ep, msg)` | blocking send |
| 1 | `recv(ep) -> msg` | blocking receive |
| 2 | `call(ep, msg) -> msg` | send + wait for reply |
| 3 | `reply_recv(ep, msg) -> msg` | server loop |
| 4 | `notify(ntfn, bits)` | signal |
| 5 | `wait(ntfn) -> bits` | wait for signal |
| 6 | `cap_copy(src, dst, rights)` | derive cap |
| 7 | `cap_delete(slot)` / `cap_revoke(slot)` | |
| 8 | `map(as, frame, vaddr, perms)` / `unmap` | |
| 9 | `thread_create(...)`, `thread_start`, `yield` | |

Debug-only `debug_putc` exists until the console server works.

## Milestones

| | Milestone | Done when |
|---|---|---|
| **M0** | Boot via Limine, serial output, memory map | kernel boots in QEMU and prints the memory map ✅ |
| **M1** | GDT/TSS, IDT, exception handlers | a deliberate #PF prints a register dump; a #DF on a broken stack is caught via IST ✅ |
| **M2** | Frame allocator, page tables, kernel slab heap | kernel runs on its own PML4 (top-level page table) with W^X; second address space works ✅ |
| **M3** | LAPIC timer, kernel threads, scheduler | two kernel threads that never yield share the CPU; stack overflow caught by guard page ✅ |
| M4 | Ring 3, `syscall`/`sysret`, root task loaded | user program calls `debug_putc` |
| M5 | Endpoints, notifications, capability tables | ping-pong between two user threads over IPC |
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