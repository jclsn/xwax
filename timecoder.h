/*
 * Copyright (C) 2024 Mark Hills <mark@xwax.org>
 *
 * This file is part of "xwax".
 *
 * "xwax" is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 3 as
 * published by the Free Software Foundation.
 *
 * "xwax" is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef TIMECODER_H
#define TIMECODER_H

#include <stdbool.h>

#include "filters.h"
#include "lut.h"
#include "pitch.h"
#include "delayline.h"
#include "types.h"

#define TIMECODER_CHANNELS 2

typedef unsigned int bits_t;

struct timecode_def {
    const char *name, *desc;
    int bits, /* number of bits in string */
        resolution, /* wave cycles per second */
        flags;
    bits_t seed, seed2, /* LFSR value at timecode zero */
        taps; /* central LFSR taps, excluding end taps */
    unsigned int length, /* in cycles */
        safe; /* last 'safe' timecode number (for auto disconnect) */
    bool lookup; /* true if lut has been generated */
    struct lut lut, lut2;
};

struct timecoder_channel_mk2 {
    int rms, rms_deriv; /* RMS values for the signal and its derivative */
    signed int deriv, deriv_scaled; /* Derivative and its scaled version */

    struct delayline delayline; /* needed for the Traktor MK2 demodulation */
    struct ema_filter ema_filter;
    struct differentiator differentiator;
    struct root_mean_square rms_filter, rms_deriv_filter;
};

struct timecoder_channel {
    bool positive, /* wave is in positive part of cycle */
	swapped; /* wave recently swapped polarity */
    signed int zero;
    unsigned int crossing_ticker; /* samples since we last crossed zero */

    struct timecoder_channel_mk2 mk2;
};

struct mk2_subcode {
    u128 window;
    bits_t bitstream;
    bits_t timecode;
    bits_t bit;

    unsigned int valid_counter;
    signed int avg_reading;
    signed int avg_slope;
    bool recent_bit_flip;

    struct delayline readings;
    struct ema_filter ema_reading;
    struct ema_filter ema_slope;
};

struct timecode_mk2 {
    struct mk2_subcode upper_subcode, lower_subcode;

    slot_no_t lfsr1_slot;
    slot_no_t lfsr2_slot;

    unsigned int lfsr1_idx;
    unsigned int lfsr2_idx;

    bool is_lfsr1;
};

struct timecoder {
    struct timecode_def *def;
    double speed;

    /* Precomputed values */

    double dt, zero_alpha;
    int sample_rate;
    signed int threshold;

    /* Pitch information */

    bool forwards;
    struct timecoder_channel primary, secondary;
    struct pitch pitch;

    /* Numerical timecode */

    signed int ref_level;
    bits_t bitstream, /* actual bits from the record */
        timecode; /* corrected timecode */
    unsigned int valid_counter, /* number of successful error checks */
        timecode_ticker; /* samples since valid timecode was read */
    double dB; /* Decibels to detect phono level */

    /* Feedback */

    unsigned char *mon; /* x-y array */
    int mon_size, mon_counter;

    double gain_compensation; /* Scaling factor for the derivative */

    /* MK2 quirks */
    struct timecode_mk2 mk2;
};

struct timecode_def* timecoder_find_definition(const char *name);
void timecoder_free_lookup(void);

void timecoder_init(struct timecoder *tc, struct timecode_def *def,
                    double speed, unsigned int sample_rate, bool phono);
void timecoder_clear(struct timecoder *tc);

int timecoder_monitor_init(struct timecoder *tc, int size);
void timecoder_monitor_clear(struct timecoder *tc);

void timecoder_cycle_definition(struct timecoder *tc);
void timecoder_submit(struct timecoder *tc, signed short *pcm, size_t npcm);
signed int timecoder_get_position(struct timecoder *tc, double *when);

/*
 * The timecode definition currently in use by this decoder
 */

static inline struct timecode_def* timecoder_get_definition(struct timecoder *tc)
{
    return tc->def;
}

/*
 * Return the pitch relative to reference playback speed
 */

static inline double timecoder_get_pitch(struct timecoder *tc)
{
    return pitch_current(&tc->pitch) / tc->speed;
}

/*
 * The last 'safe' timecode value on the record. Beyond this value, we
 * probably want to ignore the timecode values, as we will hit the
 * label of the record.
 */

static inline unsigned int timecoder_get_safe(struct timecoder *tc)
{
    return tc->def->safe;
}

/*
 * The resolution of the timecode. This is the number of bits per
 * second at reference playback speed
 */

static inline double timecoder_get_resolution(struct timecoder *tc)
{
    return tc->def->resolution * tc->speed;
}

/*
 * The number of revolutions per second of the timecode vinyl,
 * used only for visual display
 */

static inline double timecoder_revs_per_sec(struct timecoder *tc)
{
    return (33.0 + 1.0 / 3) * tc->speed / 60;
}


/*
 * Computes the actual timecode slot from LFSR1 or LFSR2 of the Traktor MK2 timecode
 */

static inline slot_no_t mk2_compute_actual_slot(struct timecoder *tc)
{
    static const slot_no_t lfsr1_multiplier = 5;
    static const slot_no_t lfsr2_offset = 3;

    if (tc->mk2.is_lfsr1)
        return (tc->mk2.lfsr1_slot * lfsr1_multiplier) + tc->mk2.lfsr1_idx;
    else
        return ((tc->mk2.lfsr2_slot - 1) * lfsr1_multiplier) + lfsr2_offset +
               tc->mk2.lfsr2_idx;
}

/*
 * Reset the indexes for the primary and secondary LFSR in case of bit errors
 */

static inline void mk2_reset_indexes(struct timecoder *tc)
{
    if (tc->forwards) {
        tc->mk2.lfsr1_idx = 0;
        tc->mk2.lfsr2_idx = 0;
    } else {
        tc->mk2.lfsr1_idx = 3;
        tc->mk2.lfsr2_idx = 2;
    }
}

/* 
 * Decimates the 110-bit LFSR to a 22-bit LFSR by taking only every fifth value into account
 */

static inline bits_t mk2_decimate(u128 window)
{
    static const size_t decimation_factor = 5;
    static const size_t resulting_bits = 22;

    bits_t decimated = 0;
    bits_t shifted = 0;

    for (size_t i = 0; i < resulting_bits; i++) {
        shifted = u128_and(window, U128_ONE).low << i;
        decimated |= shifted;
        u128_rshift(window, decimation_factor);
    }
    
    return decimated;
}

/* 
 * Appends the new bit to the 110-bit window
 */

static inline void mk2_window_append(u128 *window, const u128 bit)
{
    *window = u128_lshift(*window, 1);
    *window = u128_and(*window, bit);
}

/* 
 * Prepends the new bit to the 110-bit window
 */

static inline void mk2_window_prepend(u128 *window, const u128 bit, const unsigned int bits)
{
    u128 mask = u128_lshift(bit, bits);
    *window = u128_lshift(*window, 1);
    *window = u128_rshift(*window, 1);
    *window = u128_or(*window, mask);
}

#endif
