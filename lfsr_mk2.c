#include "types.h"
#include "types.h"
#include <errno.h>
#include <stdio.h>

#include "lfsr.h"
#include "lfsr_mk2.h"

/* 
 * Initializes the structs for the decimated LFSR
 */

static void sub_lfsr_init(struct sub_lfsr *lfsr, size_t idx_max, slot_no_t start_slot)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    lfsr->slot = start_slot;
    lfsr->idx_max = idx_max;
    lfsr->idx = 0;
    lfsr->timecode = 0;
}

void sub_lfsr_print(struct sub_lfsr *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    printf("slot: %d, idx: %zu, idx_max: %zu\n", lfsr->slot, lfsr->idx, lfsr->idx_max);
}

/* 
 * Initializes the struct for the actual LFSR 
 */

void mk2_lfsr_init(struct mk2_timecode *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    sub_lfsr_init(&lfsr->lfsr[0], 2, 0);
    sub_lfsr_init(&lfsr->lfsr[1], 1, 1);

    lfsr->current_lfsr = 0;
}

/*
 * Reset the indexes for the primary and secondary LFSR in case of bit errors
 */

void mk2_lfsr_reset(struct mk2_timecode *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    lfsr->lfsr[0].idx = 0;
    lfsr->lfsr[1].idx = 0;
}

void mk2_lfsr_print(struct mk2_timecode *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    struct sub_lfsr *current = &lfsr->lfsr[lfsr->current_lfsr];
    printf("sub_lfsr[%d]\n\t", lfsr->current_lfsr);
    sub_lfsr_print(current);
}

/* 
 * Advances the actual LFSR
 */

void mk2_lfsr_fwd(struct mk2_timecode *lfsr, bits_t taps, bits_t bits)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    struct sub_lfsr *current = &lfsr->lfsr[lfsr->current_lfsr];
    struct sub_lfsr *other = &lfsr->lfsr[!lfsr->current_lfsr];

    current->idx++;

    if (current->idx > current->idx_max) {
        fwd(other->timecode, taps, bits);
        other->idx = 0;
        lfsr->current_lfsr = !lfsr->current_lfsr;
    }
}

/* 
 * Reverses the actual LFSR
 */

void mk2_lfsr_rev(struct mk2_timecode *lfsr, bits_t taps, bits_t bits)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    struct sub_lfsr *current = &lfsr->lfsr[lfsr->current_lfsr];
    struct sub_lfsr *other = &lfsr->lfsr[!lfsr->current_lfsr];

    if (current->idx == 0) {
        lfsr->current_lfsr = !lfsr->current_lfsr;
        other->idx = other->idx_max;
        rev(other->timecode, taps, bits);
    } else {
        current->idx--;
    }
}

/* 
 * Decimates the 110-bit LFSR to a 22-bit LFSR by taking only every fifth value into account
 */

bits_t mk2_lfsr_decimate(u128 window)
{
    static const size_t decimation_factor = 5;
    static const size_t resulting_bits = 22;

    bits_t decimated = 0;
    bits_t shifted = 0;

    for (size_t i = 0; i < resulting_bits; i++) {
        shifted = u128_and(window, U128_ONE).low << i;
        decimated |= shifted;
        window = u128_rshift(window, decimation_factor);
    }

    return decimated;
}

/* 
 * Computes the slot of the actual 110-bit LFSR
 */

slot_no_t mk2_compute_actual_slot(struct mk2_timecode *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return -1;
    }

    static const slot_no_t multiplier = 5; // Base multiplication by five to reconstruct slot
    static const slot_no_t offset = 3; // The secondary LFSR has a fixed offset of 3

    if (!lfsr->current_lfsr)
        return (lfsr->lfsr[0].slot * multiplier) + lfsr->lfsr[0].idx;
    else
        return ((lfsr->lfsr[1].slot - 1) * multiplier) + offset + lfsr->lfsr[1].idx;
}

/* 
 * Appends the new bit to the 110-bit window
 */

void mk2_window_fwd(u128 *window, const bits_t b)
{
    if (!window) {
        errno = EINVAL;
        perror(__func__);
        return;
    }
    
    static const unsigned int mk2_bits = 110;
    u128 bit = U128(0x0, b);

    *window = u128_add(u128_rshift(*window, 1), u128_lshift(bit, mk2_bits - 1));
}

/* 
 * Prepends the new bit to the 110-bit window
 */

void mk2_window_rev(u128 *window, const bits_t b)
{
    if (!window) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    static const unsigned int mk2_bits = 110;
    u128 bit = U128(0x0, b);

    u128 mask = u128_sub(u128_lshift(U128_ONE, mk2_bits), U128_ONE);
    *window = u128_add(u128_and(u128_lshift(*window, 1), mask), bit);
}

void print_bits(unsigned int num, int bit_count) {

    bit_count %= 32;

    // Start from the most significant bit and go to the least significant bit
    for (int i = bit_count; i > 0; --i) {
        unsigned int bit = (num >> i) & 0x1;
        printf("%u", bit);
    }
    printf("\n"); // New line after printing all bits
}
