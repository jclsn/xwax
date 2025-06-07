
#include <errno.h>
#include <stdio.h>

#include "timecoder_mk2.h"
#include "lfsr.h"

/* 
 * Initializes the structs for the decimated LFSR
 */

void sub_lfsr_init(struct sub_lfsr *lfsr, size_t idx_max, slot_no_t start_slot)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    lfsr->slot = start_slot;
    lfsr->idx_max = idx_max;
    lfsr->idx = 0;
    lfsr->state = 0;
}


/* 
 * Initializes the struct for the actual LFSR 
 */

void mk2_lfsr_init(struct mk2_lfsr *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    sub_lfsr_init(&lfsr->sub_lfsr[0], 2, 0);
    sub_lfsr_init(&lfsr->sub_lfsr[1], 1, 1);

    lfsr->current_lfsr = 0;
}

/* 
 * Advances the actual LFSR
 */

void mk2_lfsr_fwd(struct mk2_lfsr *lfsr, bits_t taps, bits_t bits)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    struct sub_lfsr *current = &lfsr->sub_lfsr[lfsr->current_lfsr];

    current->idx++;

    if (current->idx > current->idx_max) {
        fwd(current->state, taps, bits);
        current->idx = 0;
        lfsr->current_lfsr = !lfsr->current_lfsr;
    }
}

/* 
 * Reverses the actual LFSR
 */

void mk2_lfsr_rev(struct mk2_lfsr *lfsr, bits_t taps, bits_t bits)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return;
    }

    struct sub_lfsr *current = &lfsr->sub_lfsr[lfsr->current_lfsr];
    struct sub_lfsr *other = &lfsr->sub_lfsr[!lfsr->current_lfsr];

    if (current->idx == 0) {
        lfsr->current_lfsr = !lfsr->current_lfsr;
        other->idx = other->idx_max;
        rev(other->state, taps, bits);
    } else {
        current->idx--;
    }
}

/* 
 * Computes the slot of the actual 110-bit LFSR
 */

slot_no_t mk2_compute_actual_slot(struct mk2_lfsr *lfsr)
{
    if (!lfsr) {
        errno = EINVAL;
        perror(__func__);
        return -1;
    }

    static const slot_no_t multiplier = 5; // Base multiplication by five to reconstruct slot
    static const slot_no_t offset = 3; // The secondary LFSR has a fixed offset of 3

    if (!lfsr->current_lfsr)
        return (lfsr->sub_lfsr[0].slot * multiplier) + lfsr->sub_lfsr[0].idx;
    else
        return ((lfsr->sub_lfsr[1].slot - 1) * multiplier) + offset + lfsr->sub_lfsr[1].idx;
}
