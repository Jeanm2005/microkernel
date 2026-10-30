# microkernel

A small L4-style microkernel for x86_64, written in C. Drivers, file
systems and networking run as isolated user-space servers that talk over
IPC (inter-process communication) and hold capabilities. Goal: a driver
can crash and be restarted without taking the system down.

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture and milestones.

**Status:** M8 of 10 done.

- M0: boots via Limine on QEMU, serial console, memory map
- M1: own GDT (Global Descriptor Table), TSS (Task State Segment) and IDT
  (Interrupt Descriptor Table); CPU exceptions reported with registers and
  a backtrace; double faults caught on a separate stack
- M2: physical frame allocator, the kernel's own 4-level page tables with
  W^X (write xor execute) section permissions, separate address spaces,
  slab caches for kernel objects
- M3: LAPIC (local APIC) timer, kernel threads with guard-paged stacks,
  context switching, preemptive priority scheduler
- M4: user mode: ELF loader, `syscall` entry, a root task loaded as a boot
  module; a faulting user process is killed and the kernel keeps running
- M5: capability tables, synchronous IPC endpoints with a direct-switch
  fast path, notifications, capability transfer, and object creation
  limited by factory capabilities
- M6: the root task reads a manifest and starts a console server and its
  clients, handing each exactly the capabilities its manifest line lists
- M7: the console server is a user-space serial driver: it holds COM1's
  I/O ports and IRQ 4 as capabilities, gets interrupts as notifications,
  and is the only way the programs it serves can print
- M8: memory capabilities (frames a process maps into its
  own address space, with per-program quotas) and a user-space PCI server
  that hands each driver only its own device's ports and interrupt line;
  a virtio disk driver performs DMA reads and writes with IRQ completion;
  a tiny read-only filesystem server and `cat` client print `hello.txt`
  from disk

## Build and run

Requirements: `python3`, `gcc`, `binutils`, `make`, `git`, `xorriso`, `qemu-system-x86_64`
(on Ubuntu/WSL2: `sudo apt install build-essential xorriso qemu-system-x86`).

```sh
make run     # build ISO and boot in QEMU, serial output in the terminal (Ctrl+C to quit)
make test    # boot once per test case in scripts/test.sh and check the output
make debug   # QEMU waits for GDB: gdb build/kernel.elf -ex 'target remote :1234'

make run CMDLINE="selftest=pagefault"     # watch a page-fault report
make run CMDLINE="selftest=doublefault"   # watch a double fault caught on its own stack
make run CMDLINE="selftest=write-text"    # writing to kernel code faults (W^X)
make run CMDLINE="selftest=stack-overflow" # a thread overflows into its guard page
make run CMDLINE="selftest=user-pagefault" # a user process crashes; the kernel survives
make run CMDLINE="selftest=ipc-server-dies" # a server dies mid-call; its client gets -ERR_DEAD
make run CMDLINE="selftest=app-crash"       # one application crashes; the system carries on
make run CMDLINE="selftest=console-input"   # type a line and press Enter: it arrives by IRQ 4
```

The first build clones the Limine bootloader into `limine/`.

To turn a backtrace address into a source line:

```sh
addr2line -e build/kernel.elf -f -p 0xffffffff800020ec
```