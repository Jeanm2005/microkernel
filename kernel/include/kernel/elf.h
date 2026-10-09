#pragma once
#include <stddef.h>
#include <stdint.h>

/* Load a static ELF64 x86_64 executable into the user half of address
 * space `root`: one fresh, zeroed frame per page, file bytes copied in,
 * mapped user-accessible with the segment's permissions. Rejects anything
 * malformed, outside the user half, or both writable and executable.
 *
 * Returns 0 and sets *entry, or a negative error; on error some pages may
 * already be mapped, so the caller destroys the address space. */
int elf_load(uint64_t root, const void *image, size_t size, uint64_t *entry);

/* Human-readable reason for an elf_load() error code. */
const char *elf_strerror(int err);