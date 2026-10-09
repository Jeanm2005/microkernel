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

# run_case NAME CMDLINE EXPECTED_STATUS EXPECTED_TEXT...
run_case() {
    name=$1 cmdline=$2 want_status=$3
    shift 3
    iso="$OUT/$name.iso" log="$OUT/$name.log"

    make -s iso ISO="$iso" CMDLINE="$cmdline" >/dev/null || { echo "FAIL $name (ISO build)"; failed=1; return; }
    # shellcheck disable=SC2086  # QEMUFLAGS is meant to be split into words
    timeout 20 $QEMU $QEMUFLAGS -cdrom "$iso" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 >"$log" 2>&1
    status=$?

    ok=1
    [ "$status" -eq "$want_status" ] || { echo "  exit status $status, expected $want_status"; ok=0; }
    for text in "$@"; do
        grep -qF -- "$text" "$log" || { echo "  missing output: $text"; ok=0; }
    done

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

run_case unknown-selftest "selftest=nonsense" 3 \
    "unknown selftest 'nonsense'"

exit $failed