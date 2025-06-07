#ifndef LFSR_MK2_H

#define LFSR_MK2_H

#include "types.h"
#include <stdbool.h>
#include <sys/types.h>

typedef unsigned int slot_no_t;
typedef unsigned int bits_t;

struct sub_lfsr {
    slot_no_t slot;
    ssize_t idx;
    ssize_t idx_max;
    bits_t timecode;
};

struct mk2_timecode {
    struct sub_lfsr lfsr[2];
    int current; // Index of the LFSR currently in use
};

void mk2_lfsr_init(struct mk2_timecode *lfsr);
void mk2_lfsr_fwd(struct mk2_timecode *lfsr, bits_t taps, bits_t bits);
void mk2_lfsr_rev(struct mk2_timecode *lfsr, bits_t taps, bits_t bits);
void mk2_lfsr_reset(struct mk2_timecode *lfsr);
bits_t mk2_lfsr_decimate(u128 window);

void mk2_window_fwd(u128 *window, const bits_t b);
void mk2_window_rev(u128 *window, const bits_t b);

slot_no_t mk2_compute_actual_slot(struct mk2_timecode *lfsr);

#endif /* end of include guard LFSR_MK2_H */

