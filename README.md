# microkernel

A small L4-style microkernel for x86_64, written in C. Drivers, file
systems and networking run as isolated user-space servers that talk over
IPC (inter-process communication) and hold capabilities. Goal: a driver
can crash and be restarted without taking the system down.

See [docs/DESIGN.md](docs/DESIGN.md) for the architecture and milestones.

**Status:** M0–M10 demo acceptance tests complete. Networking now includes persistent UDP/TCP echo services alongside ARP and IPv4/ICMP.

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
- M9: the root task watches child exits and restarts a faulting disk
  driver up to three times, preserving its service endpoint and client badges; the PCI
  server pins DMA memory until reset, and the filesystem retries its read;
  disk requests have one-second deadlines, with explicit I/O errors after
  the restart budget is exhausted

- M10: a user-space virtio-net driver and minimal IP stack answer ARP and
  ICMP echo at `10.0.2.15`; the host validates 260 pings, malformed packet
  rejection, virtqueue reuse and complete memory reclamation.

## Build and run

Requirements: `python3`, `gcc`, `binutils`, `make`, `git`, `xorriso`, `qemu-system-x86_64`
(on Ubuntu/WSL2: `sudo apt install build-essential xorriso qemu-system-x86`).

```sh
make run     # build ISO and boot in QEMU, serial output in the terminal (Ctrl+C to quit)
make test    # boot regression cases, then run the host-to-guest network test
make net-demo # ARP, ping, UDP and TCP packet tests over a local socket pair
make net-serve # persistent guest: TCP localhost:2222 and UDP localhost:2223
make net-sockets-test # native host TCP/UDP clients against the persistent guest
make guest-sockets-test # separate guest application, plus blocked-server restart/reconnect
make debug   # QEMU waits for GDB: gdb build/kernel.elf -ex 'target remote :1234'

make run CMDLINE="selftest=pagefault"     # watch a page-fault report
make run CMDLINE="selftest=doublefault"   # watch a double fault caught on its own stack
make run CMDLINE="selftest=write-text"    # writing to kernel code faults (W^X)
make run CMDLINE="selftest=stack-overflow" # a thread overflows into its guard page
make run CMDLINE="selftest=user-pagefault" # a user process crashes; the kernel survives
make run CMDLINE="selftest=ipc-server-dies" # a server dies mid-call; its client gets -ERR_DEAD
make run CMDLINE="selftest=app-crash"       # one application crashes; the system carries on
make run CMDLINE="selftest=disk-timeout"   # a missing disk completion triggers recovery
make run CMDLINE="selftest=driver-repeated" # fault, timeout, fault, then a successful read
make run CMDLINE="selftest=disk-boot-timeout" # a disk unavailable from the first request
make run CMDLINE="selftest=driver-exhausted" # repeated timeouts stop the service cleanly
make run CMDLINE="selftest=driver-restart" # disk driver faults after submitting DMA; root restarts it
make run CMDLINE="selftest=console-input"   # type a line and press Enter: it arrives by IRQ 4
```

The first build clones the Limine bootloader into `limine/`.

To turn a backtrace address into a source line:

```sh
addr2line -e build/kernel.elf -f -p 0xffffffff800020ec
```
The network demo uses `net.manifest`, separate from the storage demo. Its
static address is `10.0.2.15` and MAC is `52:54:00:12:34:56`. `make net-demo`
uses a Python ICMP peer on QEMU's Ethernet socket backend; it needs no
Internet connection, TAP interface, or privileged host `ping` socket. See
[docs/DESIGN.md](docs/DESIGN.md#networking-m10) for the supported protocols
and current limits.

## Functional networking and hardening

`make net-serve` keeps the guest running and forwards **TCP port 2222** and
**UDP port 2223** on host loopback to the guest's echo port 7. For example,
in another terminal:

```sh
python3 -c 'import socket; s=socket.create_connection(("127.0.0.1",2222)); s.sendall(b"hello guest\n"); print(s.recv(1024).decode(),end=""); s.close()'
python3 -c 'import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(2); s.sendto(b"hello UDP",("127.0.0.1",2223)); print(s.recv(1024)); s.close()'
```

TCP supports passive connections, bounded concurrent state, binary streams,
retransmission, duplicate detection, sequence wrap and passive FIN close.
UDP preserves datagram boundaries and validates lengths and checksums.
Port 7 remains a built-in echo service. Guest applications can now use the
separate IPC socket API described below; it is not a POSIX socket API.
The stack deliberately uses one outstanding TCP segment per connection;
active connects and a full TCP congestion-control
implementation remain future work.

The supervisor now configures a two-second CPU watchdog for the disk and
network drivers. A driver spinning in user space without syscalls is killed
at a safe user boundary; disk recovery resets DMA before restarting it.
The CPU watchdog does not time blocked IPC. Absolute-deadline calls and
receives, safe supervisor stops, and endpoint revocation now provide separate
blocked-IPC cancellation.
`make test` includes sanitizer checks of truncated/mutated packets, 21 boot
cases (including `selftest=driver-watchdog`), packet-level loss/reorder tests,
and native host TCP/UDP socket tests. LeakSanitizer needs an environment
that permits process inspection (run outside a debugger/restrictive sandbox).
See [the design](docs/DESIGN.md) for current limits and follow-up priorities.

## Guest sockets and service recovery

The guest-facing protocol lives in [include/abi/net.h](include/abi/net.h),
with helpers in [user/lib/net.h](user/lib/net.h). It supports UDP bind,
receive and reply, and passive TCP bind/listen/accept/read/write/close.
`user/netsock/main.c` is a separate application with console and network
endpoint capabilities only. Its test receives UDP on guest port 9000 and
TCP on 9001 and sends the data back through IPC. Operations are nonblocking;
messages carry up to 16 payload bytes. Handles belong to a client badge
and server incarnation, so neither another client nor a replacement server
can accidentally reuse an old handle.

`make guest-sockets-test` runs the application normally, then repeats after
a deliberately blocked network server. The client times out its call,
uses an explicitly granted test-admin process capability to stop the server,
waits for the new incarnation, and reopens its sockets. Root resets the NIC
before releasing DMA buffers, revokes old delegated grants, and restarts the
server on its stable endpoint. TCP connections do not survive restart;
applications reconnect explicitly. `make net-serve` now uses a persistent,
supervised server (`serve.manifest`) with the same bounded recovery policy.

Capabilities now have derivation history. `cap_revoke` invalidates descendants
while retaining the caller's capability; cross-process blocked IPC is canceled
with `-ERR_REVOKED`. Revoked slots stay occupied and deletable until owners
clean them up, preventing silent slot reuse. Revocation returns `-ERR_BUSY`
if affected descendants still authorize mappings, enabled ports or bindings:
stop/unmap/unbind first. It does not itself reset hardware or revoke DMA.
See [docs/DESIGN.md](docs/DESIGN.md) for semantics, bounds and remaining work.
