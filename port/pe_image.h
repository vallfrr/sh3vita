#pragma once
#include <stddef.h>
#include <stdint.h>

/* PE32 loader for a bounded SH3 guest arena. Addresses are guest offsets. */
typedef struct {
    uint32_t image_base, image_size, entry, heap_base;
} Sh3PeLayout;

/* Input bytes and arena must not overlap. On any failure, layout is untouched.
 * Validate the whole image before modifying the arena. Returns 0 on success. */
int sh3_pe_load(const uint8_t *file, size_t file_size, uint8_t *arena,
                size_t arena_size, Sh3PeLayout *layout);
