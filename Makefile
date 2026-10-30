# Top-level build for the kernel. Targets:
#   make          build build/kernel.elf
#   make iso      build a bootable hybrid BIOS/UEFI ISO (build/os.iso)
#   make run      boot the ISO in QEMU, serial on stdio
#   make debug    same, but QEMU waits for GDB on :1234
#   make test     boot once per test case in scripts/test.sh, check output
#   make clean
#
# Built: the kernel (kernel/) and the user programs (user/<name>/, one ELF
# each). The bootloader loads the programs and the manifests (user/*.manifest)
# as modules; the root task starts the rest. include/abi/ holds what the
# kernel and the programs share.
#
# Pass a kernel command line with CMDLINE, e.g.
#   make run CMDLINE="selftest=pagefault"
#
# Needs: gcc, binutils, make, xorriso, qemu-system-x86_64, git.

CC      := gcc
LD      := ld
QEMU    := qemu-system-x86_64
BUILD   := build
CMDLINE ?=
ISO     ?= $(BUILD)/os.iso

# Freestanding x86_64 kernel flags.
#  -mcmodel=kernel     kernel lives in the top 2 GiB (0xffffffff80000000)
#  -mno-red-zone       interrupts would clobber the SysV red zone
#  -mgeneral-regs-only no SSE/AVX in the kernel, so we never save FPU state on traps
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
           -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-lto -fno-pie -fno-pic -fno-omit-frame-pointer \
           -m64 -march=x86-64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
           -Ikernel/include -Iinclude -MMD -MP
ASFLAGS := -m64 -g -Ikernel/include -Iinclude -MMD -MP
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           -T kernel/linker.ld

SRCS := $(shell find kernel -name '*.c' -o -name '*.S')
OBJS := $(patsubst %,$(BUILD)/%.o,$(basename $(SRCS)))

# User programs: ordinary static executables at 0x400000. They avoid SSE
# for now because the kernel doesn't save FPU/SSE registers on a switch yet.
USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffreestanding \
               -fno-stack-protector -fno-pie -fno-pic -fno-omit-frame-pointer \
               -m64 -march=x86-64 -mgeneral-regs-only \
               -Iinclude -Iuser/lib -MMD -MP
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T user/linker.ld
ULIB_SRCS := $(wildcard user/lib/*.c user/lib/*.S)
ULIB_OBJS := $(patsubst %,$(BUILD)/%.o,$(basename $(ULIB_SRCS)))

# Programs: user/<name>/*.c, linked with user/lib into build/user/<name>.elf.
USER_PROGS := root console hello pci blk fs cat net
USER_ELFS  := $(USER_PROGS:%=$(BUILD)/user/%.elf)
MANIFESTS  := $(wildcard user/*.manifest)

define user_prog
$(1)_OBJS := $$(patsubst %,$$(BUILD)/%.o,$$(basename $$(wildcard user/$(1)/*.c)))
$$(BUILD)/user/$(1).elf: $$(ULIB_OBJS) $$($(1)_OBJS) user/linker.ld
	$$(LD) $$(USER_LDFLAGS) $$(ULIB_OBJS) $$($(1)_OBJS) -o $$@
endef
$(foreach p,$(USER_PROGS),$(eval $(call user_prog,$(p))))
USER_OBJS := $(foreach p,$(USER_PROGS),$($(p)_OBJS))

# The disk: a virtio block device in "transitional" mode, so it keeps the
# legacy virtio interface (registers in I/O ports) next to the modern one.
DISK      := $(BUILD)/disk.img
QEMUFLAGS := -M q35 -m 256M -cpu qemu64 -smp 1 -no-reboot \
             -serial stdio -display none \
             -drive file=$(DISK),if=none,id=disk0,format=raw \
             -device virtio-blk-pci,drive=disk0,disable-legacy=off

.PHONY: all iso run debug test net-demo clean FORCE
# The user_prog rules above come first, so name the default goal explicitly.
.DEFAULT_GOAL := all

all: $(BUILD)/kernel.elf $(USER_ELFS)

$(BUILD)/kernel.elf: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# User objects use their own flags. Make picks these rules over the ones
# above because their pattern stem is shorter (more specific).
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -g -MMD -MP -c $< -o $@

# Limine binaries are fetched, not vendored.
LIMINE_BRANCH := v9.x-binary
limine/limine:
	[ -d limine ] || git clone --branch=$(LIMINE_BRANCH) --depth=1 \
	    https://github.com/limine-bootloader/limine.git limine
	$(MAKE) -C limine

iso: $(ISO)

# Always rebuilt (FORCE) because CMDLINE may differ from the last build;
# it only takes a moment.
$(ISO): $(BUILD)/kernel.elf $(USER_ELFS) $(MANIFESTS) limine.conf limine/limine FORCE
	rm -rf $@.root
	mkdir -p $@.root/boot/limine $@.root/EFI/BOOT
	cp $(BUILD)/kernel.elf $(USER_ELFS) $(MANIFESTS) $@.root/boot/
	sed 's|@CMDLINE@|$(CMDLINE)|' limine.conf > $@.root/boot/limine/limine.conf
	cp limine/limine-bios.sys limine/limine-bios-cd.bin \
	   limine/limine-uefi-cd.bin $@.root/boot/limine/
	cp limine/BOOTX64.EFI $@.root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $@.root -o $@ 2>/dev/null
	./limine/limine bios-install $@ 2>/dev/null

# A reproducible tiny filesystem image, rebuilt when its contents change.
$(DISK): scripts/mkdisk.py user/disk/hello.txt
	python3 scripts/mkdisk.py user/disk/hello.txt $@

run: iso $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO)

debug: iso $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) -s -S

test: all limine/limine $(DISK)
	@QEMU="$(QEMU)" QEMUFLAGS="$(QEMUFLAGS)" BUILD="$(BUILD)" sh scripts/test.sh
	$(MAKE) net-demo

net-demo: all limine/limine $(DISK)
	$(MAKE) -s iso ISO=$(BUILD)/net.iso CMDLINE=selftest=net-ping
	python3 scripts/net-test.py --qemu $(QEMU) --iso $(BUILD)/net.iso --disk $(DISK) --log $(BUILD)/net-demo.log

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(ULIB_OBJS:.o=.d) $(USER_OBJS:.o=.d)