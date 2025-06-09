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

/*
 * IMPORTANT
 *
 * This open source license comes with certain obligations.  In
 * particular, it does not permit the copying of this code into
 * proprietary software. This requires a separate license.
 *
 * If you wish to incorporate timecode functionality into software
 * which is not compatible with this license, contact the author for
 * information.
 *
 */

#include "lfsr_mk2.h"
#include "lfsr_mk2.h"
#include "types.h"
#include "types.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "filters.h"
#include "timecoder.h"

#define ZERO_THRESHOLD (128 << 16)

#define ZERO_RC 0.001 /* time constant for zero/rumble filter */

#define REF_PEAKS_AVG 48 /* in wave cycles */

/* The number of correct bits which come in before the timecode is
 * declared valid. Set this too low, and risk the record skipping
 * around (often to blank areas of track) during scratching */

#define VALID_BITS 24

#define MONITOR_DECAY_EVERY 512 /* in samples */

#define SQ(x) ((x)*(x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))

/* Timecode definitions */

#define SWITCH_PHASE 0x1 /* tone phase difference of 270 (not 90) degrees */
#define SWITCH_PRIMARY 0x2 /* use left channel (not right) as primary */
#define SWITCH_POLARITY 0x4 /* read bit values in negative (not positive) */
#define TRAKTOR_MK2 0x8 /* use for Traktor MK2 timecode*/

static struct timecode_def timecodes[] = {
    {
        .name = "serato_2a",
        .desc = "Serato 2nd Ed., side A",
        .resolution = 1000,
        .bits = 20,
        .seed = 0x59017,
        .taps = 0x361e4,
        .length = 712000,
        .safe = 707000,
    },
    {
        .name = "serato_2b",
        .desc = "Serato 2nd Ed., side B",
        .resolution = 1000,
        .bits = 20,
        .seed = 0x8f3c6,
        .taps = 0x4f0d8, /* reverse of side A */
        .length = 922000,
        .safe = 917000,
    },
    {
        .name = "serato_cd",
        .desc = "Serato CD",
        .resolution = 1000,
        .bits = 20,
        .seed = 0xd8b40,
        .taps = 0x34d54,
        .length = 950000,
        .safe = 940000,
    },
    {
        .name = "traktor_a",
        .desc = "Traktor Scratch, side A",
        .resolution = 2000,
        .flags = SWITCH_PRIMARY | SWITCH_POLARITY | SWITCH_PHASE,
        .bits = 23,
        .seed = 0x134503,
        .taps = 0x041040,
        .length = 1500000,
        .safe = 1480000,
    },
    {
        .name = "traktor_b",
        .desc = "Traktor Scratch, side B",
        .resolution = 2000,
        .flags = SWITCH_PRIMARY | SWITCH_POLARITY | SWITCH_PHASE,
        .bits = 23,
        .seed = 0x32066c,
        .taps = 0x041040, /* same as side A */
        .length = 2110000,
        .safe = 2090000,
    },
    {
        .name = "traktor_mk2_a",
        .desc = "Traktor Scratch MK2, side A",
        .resolution = 2500,
        .flags = TRAKTOR_MK2,
        .bits = 22,
        .seed = 0x12c24,
        .seed2 = 0xce4da,
        .taps = 0x404181,
        .length = 1820000,
        .safe = 1800000,
    },    
    {
        .name = "traktor_mk2_b",
        .desc = "Traktor Scratch MK2, side B",
        .resolution = 2500,
        .flags = TRAKTOR_MK2,
        .bits = 22,
        .seed = 0xe1e72,
        .seed2 = 0xd1356,
        .taps = 0x404181,
        .length = 2570000,
        .safe = 2550000,
    },    
    {
        .name = "traktor_mk2_cd",
        .desc = "Traktor Scratch MK2, CD",
        .resolution = 3000,
        .flags = TRAKTOR_MK2,
        .bits = 22,
        .seed = 0xf5ac,
        .seed2 = 0x2089f3,
        .taps = 0x401181,
        .length = 4500000,
        .safe = 4495000,
    },
    {
        .name = "mixvibes_v2",
        .desc = "MixVibes V2",
        .resolution = 1300,
        .flags = SWITCH_PHASE,
        .bits = 20,
        .seed = 0x22c90,
        .taps = 0x00008,
        .length = 950000,
        .safe = 923000,
    },
    {
        .name = "mixvibes_7inch",
        .desc = "MixVibes 7\"",
        .resolution = 1300,
        .flags = SWITCH_PHASE,
        .bits = 20,
        .seed = 0x22c90,
        .taps = 0x00008,
        .length = 312000,
        .safe = 310000,
    },
    {
        .name = "pioneer_a",
        .desc = "Pioneer RekordBox DVS Control Vinyl, side A",
        .resolution = 1000,
        .flags = SWITCH_POLARITY,
        .bits = 20,
        .seed = 0x78370,
        .taps = 0x7933a,
        .length = 635000,
        .safe = 614000,
    },
    {
        .name = "pioneer_b",
        .desc = "Pioneer RekordBox DVS Control Vinyl, side B",
        .resolution = 1000,
        .flags = SWITCH_POLARITY,
        .bits = 20,
        .seed = 0xf7012,
        .taps = 0x2ef1c,
        .length = 918500,
        .safe = 913000,
    },
};

/*
 * Where necessary, build the lookup table required for this timecode
 *
 * Return: -1 if not enough memory could be allocated, otherwise 0
 */

static int build_lookup(struct timecode_def *def)
{
    unsigned int n;
    bits_t current;

    if (def->lookup)
        return 0;

    fprintf(stderr, "Building LUT for %d bit %dHz timecode (%s)\n",
            def->bits, def->resolution, def->desc);

    if (lut_init(&def->lut[0], def->length) == -1)
	return -1;

    current = def->seed;

    for (n = 0; n < def->length; n++) {
        bits_t next;

        /* timecode must not wrap */
        assert(lut_lookup(&def->lut[0], current) == (unsigned)-1);
        lut_push(&def->lut[0], current);

        /* check symmetry of the lfsr functions */
        next = fwd(current, def->taps, def->bits);
        assert(rev(next, def->taps, def->bits) == current);

        current = next;
    }

    def->lookup = true;

    return 0;
}


static int build_lookup_mk2(struct timecode_def *def)
{
    unsigned int n, length;
    bits_t current, next;
    bits_t current2, next2;

    if (def->lookup)
        return 0;

    fprintf(stderr, "Building LUT for %d bit %dHz timecode (%s)\n",
            def->bits, def->resolution, def->desc);

    length = def->length / 5;
    if (lut_init(&def->lut[0], length) == -1)
        return -1;

    if (lut_init(&def->lut[1], length + 1) == -1)
        return -1;

    def->lut[1].avail++;

    current = def->seed;
    current2 = def->seed2;

    for (n = 0; n < length; n++) {

        /* timecode must not wrap */
        assert(lut_lookup(&def->lut[0], current) == (unsigned)-1);
        lut_push(&def->lut[0], current);
        assert(lut_lookup(&def->lut[1], current2) == (unsigned)-1);
        lut_push(&def->lut[1], current2);

        /* check symmetry of the lfsr functions */
        next = fwd(current, def->taps, def->bits);
        next2 = fwd(current2, def->taps, def->bits);
        assert(rev(next, def->taps, def->bits) == current);
        assert(rev(next2, def->taps, def->bits) == current2);

        current = next;
        current2 = next2;
    }

    def->lookup = true;

    return 0;
}

/*
 * Find a timecode definition by name
 *
 * Return: pointer to timecode definition, or NULL if not available
 */

struct timecode_def* timecoder_find_definition(const char *name)
{
    unsigned int n;

    for (n = 0; n < ARRAY_SIZE(timecodes); n++) {
        struct timecode_def *def = &timecodes[n];

        if (strcmp(def->name, name) != 0)
            continue;

	if (def->flags & TRAKTOR_MK2) {
            if (build_lookup_mk2(def) == -1)
                return NULL;  /* error */
	} else {
            if (build_lookup(def) == -1)
                return NULL;  /* error */
	}

        return def;
    }

    return NULL;  /* not found */
}

/*
 * Free the timecoder lookup tables when they are no longer needed
 */

void timecoder_free_lookup(void) {
    unsigned int n;

    for (n = 0; n < ARRAY_SIZE(timecodes); n++) {
        struct timecode_def *def = &timecodes[n];

        if (def->lookup) {
            lut_clear(&def->lut[0]);
            if (def->flags & TRAKTOR_MK2)
                lut_clear(&def->lut[1]);
        }
    }
}

/*
 * Initialise filter values for the MK2 demodulation
 */

static void mk2_init_channel(struct timecoder_channel *ch)
{
    ch->mk2.deriv_scaled = INT_MAX/2;
    ch->mk2.rms = INT_MAX/2;
    ch->mk2.rms_deriv = 0;

    delayline_init(&ch->mk2.delayline);

    ema_init(&ch->mk2.ema_filter, 3e-1);
    derivative_init(&ch->mk2.differentiator);
    rms_init(&ch->mk2.rms_filter, 1e-3);
    rms_init(&ch->mk2.rms_deriv_filter, 1e-3);
}

/*
 * Initialise filter values for one channel
 */

static void init_channel(struct timecode_def *def, struct timecoder_channel *ch)
{
    ch->positive = false;
    ch->zero = 0;

    if (def->flags & TRAKTOR_MK2)
        mk2_init_channel(ch);
}

/*
 * Initialise a timecode decoder at the given reference speed
 *
 * Return: -1 if the timecoder could not be initialised, otherwise 0
 */

void timecoder_init(struct timecoder *tc, struct timecode_def *def,
                    double speed, unsigned int sample_rate, bool phono)
{
    assert(def != NULL);

    /* A definition contains a lookup table which can be shared
     * across multiple timecoders */

    assert(def->lookup);
    tc->def = def;
    tc->speed = speed;

    tc->dt = 1.0 / sample_rate;
    tc->sample_rate = sample_rate;
    tc->zero_alpha = tc->dt / (ZERO_RC + tc->dt);
    tc->threshold = ZERO_THRESHOLD;
    if (phono)
        tc->threshold >>= 5; /* approx -36dB */

    tc->forwards = 1;
    init_channel(tc->def, &tc->primary);
    init_channel(tc->def, &tc->secondary);
    pitch_init(&tc->pitch, tc->dt);

    tc->ref_level = INT_MAX;
    tc->bitstream = 0;
    tc->timecode = 0;
    tc->valid_counter = 0;
    tc->timecode_ticker = 0;

    tc->upper.decimation_window = U128_ZERO;
    tc->lower.decimation_window = U128_ZERO;
    mk2_lfsr_init(&tc->upper.mk2_timecode);
    mk2_lfsr_init(&tc->lower.mk2_timecode);
    delayline_init(&tc->upper.readings);
    delayline_init(&tc->lower.readings);

    tc->mon = NULL;

}

/*
 * Clear resources associated with a timecode decoder
 */

void timecoder_clear(struct timecoder *tc)
{
    assert(tc->mon == NULL);
}

/*
 * Initialise a raster display of the incoming audio
 *
 * The monitor (otherwise known as 'scope' in the interface) is an x-y
 * display of the post-calibrated incoming audio.
 *
 * Return: -1 if not enough memory could be allocated, otherwise 0
 */

int timecoder_monitor_init(struct timecoder *tc, int size)
{
    assert(tc->mon == NULL);
    tc->mon_size = size;
    tc->mon = malloc(SQ(tc->mon_size));
    if (tc->mon == NULL) {
        perror("malloc");
        return -1;
    }
    memset(tc->mon, 0, SQ(tc->mon_size));
    tc->mon_counter = 0;
    return 0;
}

/*
 * Clear the monitor on the given timecoder
 */

void timecoder_monitor_clear(struct timecoder *tc)
{
    assert(tc->mon != NULL);
    free(tc->mon);
    tc->mon = NULL;
}

/*
 * Update channel information with axis-crossings
 */

static void detect_zero_crossing(struct timecoder_channel *ch,
                                 signed int v, double alpha,
                                 signed int threshold)
{
    ch->crossing_ticker++;

    ch->swapped = false;
    if (v > ch->zero + threshold && !ch->positive) {
        ch->swapped = true;
        ch->positive = true;
        ch->crossing_ticker = 0;
    } else if (v < ch->zero - threshold && ch->positive) {
        ch->swapped = true;
        ch->positive = false;
        ch->crossing_ticker = 0;
    }

    ch->zero += alpha * (v - ch->zero);
}

/*
 * Plot the given sample value in the x-y monitor
 */

static void update_monitor(struct timecoder *tc, signed int x, signed int y)
{
    int px, py, size, ref;

    if (!tc->mon)
        return;

    size = tc->mon_size;
    ref = tc->ref_level;

    /* Decay the pixels already in the montior */

    if (++tc->mon_counter % MONITOR_DECAY_EVERY == 0) {
        int p;

        for (p = 0; p < SQ(size); p++) {
            if (tc->mon[p])
                tc->mon[p] = tc->mon[p] * 7 / 8;
        }
    }

    assert(ref > 0);

    /* ref_level is half the precision of signal level */
    px = size / 2 + (long long)x * size / ref / 8;
    py = size / 2 + (long long)y * size / ref / 8;

    if (px < 0 || px >= size || py < 0 || py >= size)
        return;

    tc->mon[py * size + px] = 0xff; /* white */
}

static inline void detect_bit_flip(int slope[2], int rms, int reading, int avg_reading,
				   bits_t *bit, bool *bit_flipped, bool forwards, bits_t one)
{
    static const double forward_factor = 1.5;
    static const double reverse_factor = 1.75;

    double threshold;

    if (*bit_flipped == false) {
        if (forwards) {
            threshold = rms / forward_factor;
        } else {
            threshold = rms / reverse_factor;
            one = !one;
        }

        if (*bit == !one && slope[0] > threshold && slope[1] > threshold) {
            *bit = one;
            *bit_flipped = true;
        } else if (*bit == one && slope[0] < -threshold && slope[1] < -threshold) {
            *bit = !one;
            *bit_flipped = true;
        }
    } else {
        *bit_flipped = false;
    }
}

static inline bool lfsr_verify(struct timecode_def *def, bits_t *timecode, bits_t *bitstream,
        bits_t bit, bool forwards)
{
    if (forwards) {
        *timecode = fwd(*timecode, def->taps, def->bits);
        *bitstream = (*bitstream >> 1) + (bit << (def->bits - 1));
    } else {
        bits_t mask = (1 << def->bits) - 1;
        *timecode = rev(*timecode, def->taps, def->bits);
        *bitstream = ((*bitstream << 1) & mask) + bit;
    }
    if (*timecode == *bitstream)
        return true;
    else
        return false;
}

static void demodulate_bit(struct timecoder *tc, struct timecoder_mk2 *sc, signed int reading)
{
    int current_slope[2];

    delayline_push(&sc->readings, reading);
    sc->avg_reading = ema(&sc->ema_reading, reading);

    /* Calculate absolute of average slope */
    sc->avg_slope = ema(&sc->ema_slope, abs(reading - *delayline_at(&sc->readings, 1)));

    /* Calculate current and last slope */
    current_slope[0] =  (reading - *delayline_at(&sc->readings, 1));
    current_slope[1] =  (reading - *delayline_at(&sc->readings, 2));

    /* The bits only change when an offset jump occurs. Else the previous bit is taken */
    detect_bit_flip(current_slope, tc->secondary.mk2.rms, reading, sc->avg_reading, &sc->bit,
                    &sc->recent_bit_flip, tc->forwards, !tc->secondary.positive);
}

/* 
 * Process the upper or lower timecode
 */

static void process_timecode(struct timecoder *tc, struct timecoder_mk2 *sc, signed int reading)
{
    demodulate_bit(tc, sc, reading);

    if (tc->forwards) {
        mk2_window_fwd(&sc->decimation_window, sc->bit);
        mk2_lfsr_fwd(&sc->mk2_timecode, tc->def->taps, tc->def->bits);
    } else {
        mk2_window_rev(&sc->decimation_window, sc->bit);
        mk2_lfsr_rev(&sc->mk2_timecode, tc->def->taps, tc->def->bits);
    }

    sc->bitstream = mk2_lfsr_decimate(sc->decimation_window);
    sc->timecode = &sc->mk2_timecode.lfsr[sc->mk2_timecode.current_lfsr].timecode;

    /* u128_print(sc->decimation_window); */
    /* printf("bitstream: %x\n", sc->bitstream); */
    /* printf("timecode: %x\n\n", *sc->timecode); */

    if (*sc->timecode == sc->bitstream) {
        (sc->valid_counter)++;
    } else {
        *sc->timecode = sc->bitstream;
        mk2_lfsr_reset(&sc->mk2_timecode);
        sc->valid_counter = 0;
    }
}

/* 
 * Extracts the MK2 bitstreams from the samples.
 */

static void process_bitstreams(struct timecoder *tc, signed int reading) {

    /*
     * Detect if the offset jumps on upper and lower bitstream. 
     */

    if (tc->secondary.positive)
        process_timecode(tc, &tc->upper, reading);
    else if (!tc->secondary.positive)
        process_timecode(tc, &tc->lower, reading);

    /* 
     * When the signal is flipped, the negative half-cycle is on the positive side and vice versa. 
     * In this case the lower bitstream is used for timecode detection. This is currently done 
     * by probing, which is not optimal, but works for now.
     */

    if (tc->lower.valid_counter > tc->upper.valid_counter) {
        /* printf("actual_slot: %u\n", mk2_compute_actual_slot(&tc->lower.mk2_timecode)); */
        tc->bitstream = tc->lower.bitstream;
        tc->timecode = *tc->lower.timecode;
        tc->current_subcode = &tc->lower;
    } else {
        /* printf("actual_slot: %u\n", mk2_compute_actual_slot(&tc->upper.mk2_timecode)); */
        tc->bitstream = tc->upper.bitstream;
        tc->timecode = *tc->upper.timecode;
        tc->current_subcode = &tc->upper;
    }
    if (tc->timecode == tc->bitstream) {

        tc->valid_counter++;
    } else {
        tc->timecode = tc->bitstream;
        tc->valid_counter = 0;
    }

    /* Take note of the last time we read a valid timecode */

    tc->timecode_ticker = 0;

    tc->ref_level -= tc->ref_level / REF_PEAKS_AVG;
    tc->ref_level += abs((int) (tc->secondary.mk2.rms_deriv * tc->gain_compensation)) / REF_PEAKS_AVG;

    /* printf("upper.valid_counter: %d, lower.valid_counter %d, forwards: %b\n", */ 
    /*        tc->upper.valid_counter, */
    /*        tc->lower.valid_counter, */
    /*        tc->forwards); */
}

/*
 * Extract the bitstream from the sample value
 */

static void process_bitstream(struct timecoder *tc, signed int m)
{
    bits_t b;

    b = m > tc->ref_level;

    /* Add it to the bitstream, and work out what we were expecting
     * (timecode). */

    /* tc->bitstream is always in the order it is physically placed on
     * the vinyl, regardless of the direction. */

    if (tc->forwards) {
	tc->timecode = fwd(tc->timecode, tc->def->taps, tc->def->bits);
	tc->bitstream = (tc->bitstream >> 1)
	    + (b << (tc->def->bits - 1));

    } else {
	bits_t mask;

	mask = ((1 << tc->def->bits) - 1);
	tc->timecode = rev(tc->timecode, tc->def->taps, tc->def->bits);
	tc->bitstream = ((tc->bitstream << 1) & mask) + b;
    }

    if (tc->timecode == tc->bitstream)
	tc->valid_counter++;
    else {
	tc->timecode = tc->bitstream;
	tc->valid_counter = 0;
    }

    /* Take note of the last time we read a valid timecode */

    tc->timecode_ticker = 0;

    /* Adjust the reference level based on this new peak */

    tc->ref_level -= tc->ref_level / REF_PEAKS_AVG;
    tc->ref_level += m / REF_PEAKS_AVG;

    debug("%+6d zero, %+6d (ref %+6d)\t= %d%c (%5d)",
          tc->primary.zero,
          m, tc->ref_level,
	  b, tc->valid_counter == 0 ? 'x' : ' ',
	  tc->valid_counter);
}

/*
 * Computes a scaled derivative for both channels which can be used by xwax for pitch detection 
 */

static void compute_derivative(struct timecoder *tc,
			   signed int primary, signed int secondary)
{
        delayline_push(&tc->primary.mk2.delayline, primary);
        delayline_push(&tc->secondary.mk2.delayline, secondary);

        /* Compute the discrete derivative */
        tc->primary.mk2.deriv = derivative(&tc->primary.mk2.differentiator,
                                           ema(&tc->primary.mk2.ema_filter, primary));
        tc->secondary.mk2.deriv = derivative(&tc->secondary.mk2.differentiator,
                                             ema(&tc->secondary.mk2.ema_filter, secondary));

        /* Compute the smoothed RMS value */
        tc->primary.mk2.rms = rms(&tc->primary.mk2.rms_filter, primary);
        tc->secondary.mk2.rms = rms(&tc->secondary.mk2.rms_filter, secondary);

        /* Compute the smoothed RMS value for the derivative */
        tc->primary.mk2.rms_deriv = rms(&tc->primary.mk2.rms_deriv_filter, tc->primary.mk2.deriv);
        tc->secondary.mk2.rms_deriv = rms(&tc->secondary.mk2.rms_deriv_filter, tc->secondary.mk2.deriv);

        /* Compute the gain compensation for the derivative*/
        tc->gain_compensation = (double) tc->secondary.mk2.rms / tc->secondary.mk2.rms_deriv;
        if (tc->gain_compensation > 30.0) // without this limit pitch becomes too sensitive
            tc->gain_compensation = 30.0;

        tc->dB = 20 * log10((double) tc->secondary.mk2.rms / INT_MAX);

        /* Compute the scaled derivative */
        tc->primary.mk2.deriv_scaled = tc->primary.mk2.deriv * tc->gain_compensation;
        tc->secondary.mk2.deriv_scaled = tc->secondary.mk2.deriv * tc->gain_compensation;
}

/*
 * Process a single sample from the incoming audio
 *
 * The two input signals (primary and secondary) are in the full range
 * of a signed int; ie. 32-bit signed.
 */

static void process_sample(struct timecoder *tc,
			   signed int primary, signed int secondary)
{
        /* Push the samples into the ringbuffer */
    if (tc->def->flags & TRAKTOR_MK2) {
        compute_derivative(tc, primary, secondary);

        detect_zero_crossing(&tc->primary, tc->primary.mk2.deriv_scaled, tc->zero_alpha, tc->threshold);
        detect_zero_crossing(&tc->secondary, tc->secondary.mk2.deriv_scaled, tc->zero_alpha, tc->threshold);
    } else {
        detect_zero_crossing(&tc->primary, primary, tc->zero_alpha, tc->threshold);
        detect_zero_crossing(&tc->secondary, secondary, tc->zero_alpha, tc->threshold);
    }

    /* If an axis has been crossed, use the direction of the crossing
     * to work out the direction of the vinyl */

    if (tc->primary.swapped || tc->secondary.swapped) {
        bool forwards;

        if (tc->primary.swapped) {
            forwards = (tc->primary.positive != tc->secondary.positive);
        } else {
            forwards = (tc->primary.positive == tc->secondary.positive);
        }

        if (tc->def->flags & SWITCH_PHASE)
	    forwards = !forwards;

        if (forwards != tc->forwards) { /* direction has changed */
            tc->forwards = forwards;
            tc->valid_counter = 0;
        }
    }

    /* If any axis has been crossed, register movement using the pitch
     * counters */

    if (!tc->primary.swapped && !tc->secondary.swapped)
	pitch_dt_observation(&tc->pitch, 0.0);
    else {
	double dx;

	dx = 1.0 / tc->def->resolution / 4;
	if (!tc->forwards)
	    dx = -dx;
	pitch_dt_observation(&tc->pitch, dx);
    }


    /* If we have crossed the primary channel in the right polarity,
     * it's time to read off a timecode 0 or 1 value */

    if (tc->def->flags & TRAKTOR_MK2) {
        if (tc->secondary.swapped) {
            int reading = *delayline_at(&tc->secondary.mk2.delayline, 3);
            process_bitstreams(tc, reading);
        }
    } else {
        if (tc->secondary.swapped &&
           tc->primary.positive == ((tc->def->flags & SWITCH_POLARITY) == 0))
        {
            signed int m;

            /* scale to avoid clipping */
            m = abs(primary / 2 - tc->primary.zero / 2);
            process_bitstream(tc, m);
        }
    }

    tc->timecode_ticker++;
}

/*
 * Cycle to the next timecode definition which has a valid lookup
 *
 * Return: pointer to timecode definition
 */

static struct timecode_def* next_definition(struct timecode_def *def)
{
    assert(def != NULL);

    do {
        def++;

        if (def >= timecodes + ARRAY_SIZE(timecodes))
            def = timecodes;

    } while (!def->lookup);

    return def;
}

/*
 * Change the timecode definition to the next available
 */

void timecoder_cycle_definition(struct timecoder *tc)
{
    tc->def = next_definition(tc->def);
    tc->valid_counter = 0;
    tc->timecode_ticker = 0;
}

/*
 * Submit and decode a block of PCM audio data to the timecode decoder
 *
 * PCM data is in the full range of signed short; ie. 16-bit signed.
 */

void timecoder_submit(struct timecoder *tc, signed short *pcm, size_t npcm)
{
    while (npcm--) {
	signed int left, right, primary, secondary;

        left = pcm[0] << 16;
        right = pcm[1] << 16;

        if (tc->def->flags & SWITCH_PRIMARY) {
            primary = left;
            secondary = right;
        } else {
            primary = right;
            secondary = left;
        }

        if (tc->def->flags & TRAKTOR_MK2) {
            /* Push the samples into the ringbuffer */
            delayline_push(&tc->primary.mk2.delayline, primary);
            delayline_push(&tc->secondary.mk2.delayline, secondary);

            process_sample(tc, primary, secondary);

            /* Display the derivative in the monitor. Phono level is indicated by a smaller signal */
            if (tc->dB > -40.0)
                update_monitor(tc, tc->primary.mk2.deriv_scaled << 1, tc->secondary.mk2.deriv_scaled << 1);
            else 
                update_monitor(tc, tc->primary.mk2.deriv_scaled >> 2, tc->secondary.mk2.deriv_scaled >> 2);
        } else {
            process_sample(tc, primary, secondary);
            update_monitor(tc, left, right);
        }

        pcm += TIMECODER_CHANNELS;
    }
}

/*
 * Get the last-known position of the timecode
 *
 * If now data is available or if too few bits have been error
 * checked, then this counts as invalid. The last known position is
 * given along with the time elapsed since the position stamp was
 * read.
 *
 * Return: the known position of the timecode, or -1 if not known
 * Post: if when != NULL, *when is the elapsed time in seconds
 */

signed int timecoder_get_position(struct timecoder *tc, double *when)
{
    signed int r;

    if (tc->valid_counter <= VALID_BITS)
        return -1;

    if (tc->def->flags & TRAKTOR_MK2 && tc->current_subcode->mk2_timecode.current_lfsr == 0) {
        /* tc->current->mk2_timecode.lfsr[tc->current->mk2_timecode.current].slot = lut_lookup(&tc->def->lut[tc->current->mk2_timecode.current], tc->bitstream); */
        /* r = mk2_compute_actual_slot(&tc->current->mk2_timecode); */
        /* printf("r: %d\n", r); */
        /* printf("slot: %d\n", tc->current->mk2_timecode.lfsr[tc->current->mk2_timecode.current].slot); */

        r = lut_lookup(&tc->def->lut[0], tc->bitstream);
    } else {
        r = lut_lookup(&tc->def->lut[0], tc->bitstream);
    }
    if (r == -1)
        return -1;



    if (when)
        *when = tc->timecode_ticker * tc->dt;

    return r;
}
