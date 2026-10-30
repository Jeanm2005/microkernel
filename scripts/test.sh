#!/bin/sh
# Boots the kernel in QEMU once per test case and checks the serial output.
# Run through `make test`, which sets QEMU, QEMUFLAGS and BUILD.
#
# QEMU's isa-debug-exit device turns the kernel's qemu_exit(code) into
# process exit status (code << 1) | 1:
#   1   = kernel finished normally (qemu_exit(0))
#   3   = kernel panicked          (qemu_exit(1))
#   124 = timed out (kernel hung or triple-faulted and rebooted)

set -u
: "${QEMU:?}" "${QEMUFLAGS:?}" "${BUILD:?}"
OUT="$BUILD/test"
mkdir -p "$OUT"
failed=0
disk_hash=$(sha256sum "$BUILD/disk.img")
input=""         # set before run_case to type this on the serial port ...
input_after=""   # ... once this text has appeared in the output

# run_case NAME CMDLINE EXPECTED_STATUS EXPECTED_TEXT...
run_case() {
    name=$1 cmdline=$2 want_status=$3
    shift 3
    iso="$OUT/$name.iso" log="$OUT/$name.log"

    make -s iso ISO="$iso" CMDLINE="$cmdline" >/dev/null || { echo "FAIL $name (ISO build)"; failed=1; return; }
    # QEMU's serial port reads stdin. Normally that's /dev/null (QEMU must
    # not touch the terminal). With $input set, stdin is a named pipe, and
    # we type into it once $input_after shows up in the log: text sent
    # earlier could arrive before the kernel sets up the UART, which
    # discards it.
    # shellcheck disable=SC2086  # QEMUFLAGS is meant to be split into words
    if [ -n "$input" ]; then
        fifo="$OUT/$name.in"
        rm -f "$fifo" && mkfifo "$fifo"
        : >"$log"
        timeout 20 $QEMU $QEMUFLAGS -cdrom "$iso" \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 <"$fifo" >"$log" 2>&1 &
        pid=$!
        exec 3>"$fifo"   # keep the pipe open until QEMU is done
        tries=0
        while ! grep -qF -- "$input_after" "$log" && [ $tries -lt 190 ]; do
            sleep 0.1
            tries=$((tries + 1))
        done
        printf '%s\n' "$input" >&3
        wait $pid
        status=$?
        exec 3>&-
        rm -f "$fifo"
    else
        timeout 20 $QEMU $QEMUFLAGS -cdrom "$iso" \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 </dev/null >"$log" 2>&1
        status=$?
    fi
    input="" input_after=""

    ok=1
    [ "$status" -eq "$want_status" ] || { echo "  exit status $status, expected $want_status"; ok=0; }
    for text in "$@"; do
        grep -qF -- "$text" "$log" || { echo "  missing output: $text"; ok=0; }
    done

    if [ "$(sha256sum "$BUILD/disk.img")" != "$disk_hash" ]; then
        echo "  disk image changed (DMA scratch data was not restored)"
        ok=0
    fi
    if [ $ok -eq 1 ]; then
        echo "PASS $name"
    else
        echo "FAIL $name (serial log: $log)"
        sed 's/^/  | /' "$log"
        failed=1
    fi
}

run_case boot "" 1 \
    "cpu: GDT, TSS and IDT loaded" \
    "selftest: breakpoint handled and resumed" \
    "paging: kernel page tables active" \
    "selftest: pmm ok" \
    "selftest: running in a second address space" \
    "selftest: paging ok" \
    "selftest: slab ok" \
    "timer: LAPIC at" \
    "sched: running on thread 'init'" \
    "pmm: reclaimed" \
    "selftest: priority ok (order HIiL)" \
    "selftest: sleep ok" \
    "selftest: preemption ok" \
    "selftest: thread reaping ok" \
    "module: root.elf" \
    "[root] hello from user space (mode 0)" \
    "[root] running in ring 3 (cs = 23)" \
    "[root] ok: debug_write(kernel address) returns -ERR_FAULT" \
    "[root] ok: debug_write(unmapped address) returns -ERR_FAULT" \
    "[root] ok: debug_write(too long) returns -ERR_INVAL" \
    "[root] ok: unknown syscall returns -ERR_NOSYS" \
    "[root] ran 10^9 cycles without a system call" \
    "[root] done, 0 failure(s)" \
    "selftest: user mode ok" \
    "selftest: user preemption ok" \
    "[ping] ok: recv on a send-only capability fails with -ERR_PERM" \
    "[ping] ok: call on an empty slot fails with -ERR_BADCAP" \
    "[ping] ok: notify on an endpoint capability fails with -ERR_TYPE" \
    "[ping] ok: a copy can't gain rights the original lacks" \
    "[ping] ok: a badged capability can't be re-badged" \
    "[ping] ok: creating kernel objects needs a factory capability" \
    "[ping] ok: a deleted capability is gone" \
    "[ping] ok: 1000 call/reply round trips" \
    "[pong] created a notification, sending a signal-only copy" \
    "[ping] ok: received a notification capability in a reply" \
    "[ping] ok: the received copy can signal but not wait" \
    "[pong] woke up from wait with bits 0x5" \
    "[ping] ok: server woke up and saw bits 0x5" \
    "[pong] quitting" \
    "[ping] done, 0 failure(s)" \
    "selftest: ipc ok" \
    "[root] read system.manifest: 6 programs" \
    "[root] ok: can't create a process above its own priority" \
    "[root] ok: a process image must come from a module capability" \
    "[root] ok: observing process exit requires a readable process capability" \
    "[root] ok: an expired wait times out without changing output bits" \
    "[root] ok: a blocked notification wait expires at its deadline" \
    "[root] ok: pending notification bits win over expiration and are consumed once" \
    "[root] ok: timed waits check notification capabilities before deadlines" \
    "[root] ok: CPU watchdog configuration requires process write rights and bounded ticks" \
    "[root] ok: exit watches validate notification rights and signal bits" \
    "[root] ok: polling a process that was never started is refused" \
    "[root] ok: a running child polls as pending and cannot change its watch" \
    "[root] ok: a process that was never started can be discarded" \
    "[root] console serves 'console'" \
    "[root] console drives 'com1'" \
    "[root] pci gets a quota of 16 objects and 0 pages" \
    "[root] pci drives 'pci'" \
    "[root] blk gets a quota of 4 objects and 8 pages" \
    "[root] started console (priority 200)" \
    "[console] ready: driving COM1 from user space, input by IRQ 4" \
    "[root] output now goes through the console server" \
    "[pci] 00:03.0 1af4:1001 virtio block device" \
    "[blk] ok: allocated a 2-page frame from its own quota" \
    "[blk] ok: can't allocate beyond its page quota" \
    "[blk] ok: the frame's physical address is known (what a device needs for DMA)" \
    "[blk] ok: can't map into the kernel half" \
    "[blk] ok: can't map over its own program" \
    "[blk] ok: it reads back what was written" \
    "[blk] ok: can't map over an existing mapping" \
    "[blk] ok: a read-only frame capability can't be mapped writable" \
    "[blk] ok: mapped read-only at a second address, it shows the same memory" \
    "[blk] ok: unmapped both" \
    "[blk] ok: the PCI server hands out a device's ports only after it is claimed" \
    "[blk] ok: claimed the device" \
    "[blk] ok: another client can neither claim it nor get its ports" \
    "[blk] ok: received the device's I/O ports as a capability" \
    "[blk] ok: received the device's interrupt line as a capability" \
    "[blk] disk: 2048 sectors of 512 bytes (1024 KiB)" \
    "[blk] ok: bound the disk's interrupt to a notification from its own quota" \
    "[blk] ok: DMA read/write round trip completed through IRQ notifications" \
    "[blk] ok: a read-only IRQ capability cannot disconnect the driver" \
    "[blk] ok: only the supervisor may reset a claimed device" \
    "[blk] done, 0 failure(s)" \
    "[fs] ok: block server rejects reads beyond disk capacity" \
    "[fs] ok: block server rejects chunks crossing a sector boundary" \
    "[fs] ok: block server refuses client shutdown" \
    "[fs] mounted hello.txt" \
    "[cat] Hello from a file on the virtio disk!" \
    "[cat] ok: filesystem rejects invalid reads" \
    "[cat] ok: missing file is refused" \
    "[cat] End of the file: the second sector arrived intact." \
    "[cat] ok: filesystem refuses client shutdown" \
    "[cat] ok: EOF returns zero bytes" \
    "[cat] ok: read the complete file from disk through the filesystem" \
    "[root] cat exited with code 0" \
    "[root] fs shut down" \
    "[root] blk shut down" \
    "[hello] ok: holds only the capabilities its manifest line lists" \
    "[hello] ok: can send to the console but not receive from it" \
    "[hello] ok: printing through the kernel is refused; this line came through the console" \
    "[hello] hello from a client of the console server" \
    "[hello] ok: the console refuses a shutdown request from a client" \
    "[root] hello exited with code 0" \
    "[root] pci shut down" \
    "[console] shutting down" \
    "[root] console shut down" \
    "[root] system finished, 0 failure(s)" \
    "selftest: system ok" \
    "kernel: init complete"

run_case pagefault "selftest=pagefault" 3 \
    "*** PAGE FAULT (#PF)" \
    "cause: write to a non-present page at 0x00000000deadb000 (kernel mode)" \
    "KERNEL PANIC"

run_case doublefault "selftest=doublefault" 3 \
    "*** DOUBLE FAULT (#DF)" \
    "KERNEL PANIC"

run_case write-text "selftest=write-text" 3 \
    "cause: write to a protected page" \
    "KERNEL PANIC"

run_case exec-data "selftest=exec-data" 3 \
    "cause: instruction fetch from a protected page" \
    "KERNEL PANIC"

run_case slab-doublefree "selftest=slab-doublefree" 3 \
    "slab_free(doublefree): double free of"

run_case stack-overflow "selftest=stack-overflow" 3 \
    "*** DOUBLE FAULT (#DF)" \
    "KERNEL PANIC"

# A faulting user process is killed, and the kernel carries on (status 1).
run_case user-pagefault "selftest=user-pagefault" 1 \
    "[root] writing through a null pointer" \
    "cause: write to a non-present page at 0x0000000000000000 (user mode)" \
    "process: killed 'root': PAGE FAULT (#PF)" \
    "selftest: kernel survived a faulting user process" \
    "kernel: init complete"

run_case user-privileged "selftest=user-privileged" 1 \
    "[root] executing cli (ring 0 only)" \
    "*** GENERAL PROTECTION FAULT (#GP)" \
    "process: killed 'root': GENERAL PROTECTION FAULT (#GP)" \
    "kernel: init complete"

run_case user-kernel-read "selftest=user-kernel-read" 1 \
    "[root] reading kernel memory" \
    "cause: read from a protected page at 0xffffffff80000000 (user mode)" \
    "process: killed 'root': PAGE FAULT (#PF)" \
    "kernel: init complete"

# A server dies with a call pending: the caller gets an error, not a hang.
run_case ipc-server-dies "selftest=ipc-server-dies" 1 \
    "[pong] crashing while handling ping 500" \
    "process: killed 'pong': PAGE FAULT (#PF)" \
    "[ping] ok: call returns -ERR_DEAD when the server dies mid-call" \
    "selftest: client got -ERR_DEAD when its server died; kernel survived" \
    "kernel: init complete"

# An application touches the serial port it doesn't own and is killed; the
# console and the other application carry on.
run_case app-crash "selftest=app-crash" 1 \
    "[root] read crash.manifest: 3 programs" \
    "[crasher] about to write to the serial port directly" \
    "process: killed 'crasher': GENERAL PROTECTION FAULT (#GP)" \
    "[root] crasher was killed" \
    "[hello] hello from a client of the console server" \
    "[root] hello exited with code 0" \
    "[root] console shut down" \
    "selftest: an application crashed; the root task and the console carried on" \
    "kernel: init complete"

# A line typed on the serial port reaches the console server as IRQ 4,
# while it is blocked waiting for clients.
input="hello kernel"
input_after="[reader] waiting for a line on the serial port"
run_case console-input "selftest=console-input" 1 \
    "[root] read input.manifest: 2 programs" \
    "[console] input: hello kernel" \
    "[reader] ok: the console received a line of input" \
    "[console] shutting down (" \
    "selftest: the console received input through IRQ 4" \
    "kernel: init complete"

run_case driver-restart "selftest=driver-restart" 1 \
    "[blk] crashing during a filesystem request" \
    "process: killed 'blk': PAGE FAULT (#PF)" \
    "[root] observed disk driver fault after process teardown" \
    "[root] stopped disk DMA and released its old buffers" \
    "[root] restarted blk with the same endpoint and client badges" \
    "[fs] disk driver died; retrying on its replacement" \
    "[cat] End of the file: the second sector arrived intact." \
    "[root] cat exited with code 0" \
    "[root] ok: disk driver restarted exactly once" \
    "[root] blk shut down" \
    "selftest: disk driver restarted; filesystem survived; no frames leaked" \
    "kernel: init complete"

run_case disk-timeout "selftest=disk-timeout" 1 \
    "[blk] simulating a disk request with no completion" \
    "[blk] disk request timed out; exiting for supervisor recovery" \
    "[root] observed disk driver I/O failure after process teardown" \
    "[root] stopped disk DMA and released its old buffers" \
    "[root] ok: disk timeout recovered with one restart" \
    "[cat] End of the file: the second sector arrived intact." \
    "[root] cat exited with code 0" \
    "selftest: disk-timeout completed; no frames leaked" \
    "kernel: init complete"

run_case driver-repeated "selftest=driver-repeated" 1 \
    "[blk] crashing during a filesystem request" \
    "[blk] disk request timed out; exiting for supervisor recovery" \
    "[root] ok: disk survived three successive recoveries" \
    "[cat] End of the file: the second sector arrived intact." \
    "[root] cat exited with code 0" \
    "selftest: driver-repeated completed; no frames leaked" \
    "kernel: init complete"

run_case driver-exhausted "selftest=driver-exhausted" 1 \
    "[root] disk restart budget exhausted; stopping the service" \
    "[root] disk service unavailable; refusing request" \
    "[cat] cannot read hello.txt: error -5" \
    "[root] cat exited with code 1" \
    "[root] ok: restart exhaustion stopped DMA without starting another driver" \
    "[root] fs shut down" \
    "[root] pci shut down" \
    "[root] system finished, 2 failure(s)" \
    "selftest: driver-exhausted completed; no frames leaked" \
    "kernel: init complete"

run_case disk-boot-timeout "selftest=disk-boot-timeout" 1 \
    "[blk] disk request timed out; exiting for supervisor recovery" \
    "[root] disk restart budget exhausted; stopping the service" \
    "[root] disk service unavailable; refusing request" \
    "[cat] cannot open hello.txt: error -5" \
    "[fs] disk unavailable; serving I/O errors" \
    "[root] cat exited with code 1" \
    "[root] fs shut down" \
    "[root] pci shut down" \
    "[root] system finished, 2 failure(s)" \
    "selftest: disk-boot-timeout completed; no frames leaked" \
    "kernel: init complete"

run_case disk-noisy-timeout "selftest=disk-noisy-timeout" 1 \
    "[blk] disk request timed out; exiting for supervisor recovery" \
    "[root] ok: disk timeout recovered with one restart" \
    "[root] cat exited with code 0" \
    "selftest: disk-noisy-timeout completed; no frames leaked" \
    "kernel: init complete"

run_case driver-watchdog "selftest=driver-watchdog" 1 \
    "[blk] spinning without syscalls after DMA submission" \
    "process: killed 'blk': CPU WATCHDOG" \
    "[root] stopped disk DMA and released its old buffers" \
    "[root] ok: disk driver restarted exactly once" \
    "[root] cat exited with code 0" \
    "selftest: disk driver restarted; filesystem survived; no frames leaked" \
    "kernel: init complete"

run_case unknown-selftest "selftest=nonsense" 3 \
    "unknown selftest 'nonsense'"

exit $failed