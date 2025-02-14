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
#include <stddef.h>
#include <stdlib.h>

#include "lut.h"
#include "pitch.h"
#include "delayline.h"

#define TIMECODER_CHANNELS 2

struct lfsr {
        /* LFSR states */
        bits_t current;
        bits_t next;
        bits_t last;

        /* LFSR definition */
        bits_t seed;
        bits_t seed2;
        bits_t taps;
        bits_t bits;
};

struct timecode_def {
    const char *name, *desc;
    int bits, /* number of bits in string */
        resolution, /* wave cycles per second */
        flags;
    bits_t seed, /* LFSR value at timecode zero */
        taps; /* central LFSR taps, excluding end taps */
    unsigned int length, /* in cycles */
        safe; /* last 'safe' timecode number (for auto disconnect) */
    bool lookup; /* true if lut has been generated */
    struct lut lut;
    struct lfsr lfsr1, lfsr2;
};

struct timecoder_channel {
    bool positive, /* wave is in positive part of cycle */
	swapped; /* wave recently swapped polarity */
    signed int zero;
    unsigned int crossing_ticker; /* samples since we last crossed zero */

    int ref_level;

    /* For MK2 demodulation */
    struct delayline delayline;
    struct delayline envelope_heights;

    unsigned int avg_envelope_height, offset_threshold;
    int last_upper_reading, last_lower_reading;
    int jump_upper, jump_lower;
    int lower_reading, upper_reading;
    int deriv, deriv_old;
    int ema, ema_old;
    int ema2, ema_old2;
    int upper_slope, lower_slope;
};

struct timecoder {
    struct timecode_def *def;
    double speed;

    /* Precomputed values */

    double dt, zero_alpha;
    signed int threshold;

    /* Pitch information */

    bool forwards;
    struct timecoder_channel primary, secondary;
    struct pitch pitch;

    /* Numerical timecode */

    signed int ref_level;
    bits_t bitstream, error_mask, /* actual bits from the record */
        timecode; /* corrected timecode */
    unsigned int valid_counter, /* number of successful error checks */
        timecode_ticker; /* samples since valid timecode was read */

    /* Feedback */

    unsigned char *mon; /* x-y array */
    int mon_size, mon_counter;

    /* Last reading level to compare the current to using the MK_OFFSET_FACTOR */
    bits_t lower_bit, upper_bit;

    int reading_type;

    bits_t upper_bitstream, lower_bitstream, upper_bitstream2, lower_bitstream2;
    bits_t upper_timecode, lower_timecode, upper_timecode2, lower_timecode2;
    unsigned int upper_valid_counter, lower_valid_counter, upper_valid_counter2, lower_valid_counter2;
    bool lower_just_flipped, upper_just_flipped;
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

static inline unsigned int envelope_height(signed int lower_reading, signed int upper_reading)
{
    unsigned int envelope = 0;

    if (upper_reading > 0 && lower_reading < 0)
        envelope =  (abs(upper_reading) + abs(lower_reading));
    if (upper_reading > 0 && lower_reading > 0)
        envelope =  (abs(upper_reading) - abs(lower_reading));

    return envelope;
}

#endif
