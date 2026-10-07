# microkernel

A small L4-style microkernel for x86_64, written in C. Drivers, file
systems and networking run as isolated user-space servers that talk over
IPC (inter-process communication) and hold capabilities. Goal: a driver
can crash and be restarted without taking the system down.

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture and milestones.

**Status:** M1 — boots via Limine, sets up the GDT (Global Descriptor
Table), TSS (Task State Segment) and IDT (Interrupt Descriptor Table), and
reports CPU exceptions with registers and a backtrace.

## Build and run

Requirements: `gcc`, `binutils`, `make`, `git`, `xorriso`, `qemu-system-x86_64`
(on Ubuntu/WSL2: `sudo apt install build-essential xorriso qemu-system-x86`).

```sh
make run     # build ISO and boot in QEMU, serial output in the terminal
make test    # boot once per test case in scripts/test.sh and check the output
make debug   # QEMU waits for GDB: gdb build/kernel.elf -ex 'target remote :1234'

make run CMDLINE="selftest=pagefault"     # watch a page-fault report
make run CMDLINE="selftest=doublefault"   # watch a double fault caught on its own stack
```

The first build clones the Limine bootloader into `limine/`.

To turn a backtrace address into a source line:

```sh
addr2line -e build/kernel.elf -f -p 0xffffffff800020ec
```