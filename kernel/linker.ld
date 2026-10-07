/* Kernel image layout. Limine loads us at the top 2 GiB of the address
 * space (higher half), already in 64-bit long mode with paging on. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

/* Separate PT_LOAD segments so Limine can map each with the right
 * permissions: text = R+X, rodata = R, data/bss = R+W. */
 PHDRS
 {
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
 }

 SECTIONS
 {
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : { __text_start = .; *(.text .text.*) __text_end = .; } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : { *(.data .data.*) } :data

    /* .bss last so it doesn't take up space in the file. */
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
 }