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

run_case unknown-selftest "selftest=nonsense" 3 \
    "unknown selftest 'nonsense'"

exit $failed