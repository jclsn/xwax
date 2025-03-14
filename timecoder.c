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

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "delayline.h"
#include "filters.h"
#include "lut.h"
#include "timecoder.h"
#include "print.h"

/*
 * Uncomment to use the plotting script 
 */

/* #define MK2_PLOT */

#ifdef MK2_PLOT
#include <sys/stat.h>
struct channel {
    int value;
    int upper_reading;
    int lower_reading;
    int avg_upper_reading;
    int avg_lower_reading;
    int deriv;
    int upper_errors;
    int lower_errors;
};

struct mk2_signal {
    struct channel primary;
    struct channel secondary;
    int upper_timecode;
    int lower_timecode;
    int plot_upper_digit;
    int plot_lower_digit;
    int forwards;
};

char filepath[1024];
FILE *fp = NULL;
struct mk2_signal mk2_signal = {};
int nwrites = 0;
int T = 1;
#endif

#define ZERO_THRESHOLD (128 << 16)

#define ZERO_RC 0.001 /* time constant for zero/rumble filter */

#define REF_PEAKS_AVG 48 /* in wave cycles */

/* Factor used by the Traktor MK2 by which the sinusoid is offset during 
 * offset modulation */

#define MK2_OFFSET_FACTOR 3.75
#define FILTER_DELAY 3
#define NO_SLOT ((unsigned)-1)

/* The number of correct bits which come in before the timecode is
 * declared valid. Set this too low, and risk the record skipping
 * around (often to blank areas of track) during scratching */

#define VALID_BITS 24
#define VALID_BITS_TRAKTOR_MK2 114
#define VALID_BITS2_TRAKTOR_MK2 1

#define MONITOR_DECAY_EVERY 512 /* in samples */

#define SQ(x) ((x)*(x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))

/* Timecode definitions */

#define SWITCH_PHASE 0x1 /* tone phase difference of 270 (not 90) degrees */
#define SWITCH_PRIMARY 0x2 /* use left channel (not right) as primary */
#define SWITCH_POLARITY 0x4 /* read bit values in negative (not positive) */
#define OFFSET_MODULATION 0x8 /* Use offset modulation used for Traktor MK2 timecodes */

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
        .flags = OFFSET_MODULATION,
        .bits = 110,
        .seed = UINT128(0xc6007c63e, 0x3fc00c60f8c1f00),
        .taps = UINT128(0x400000000040, 0x0000010800000001),
        .length = 1820000,
        .safe = 1800000,
    },    
    {
        .name = "traktor_mk2_b",
        .desc = "Traktor Scratch MK2, side B",
        .resolution = 2500,
        .flags = OFFSET_MODULATION,
        .bits = 110,
        .seed = UINT128(0x1ff9f00003, 0xe73ff00f9fe0c7c1),
        .taps = UINT128(0x400000000040, 0x0000010800000001),
        .length = 2570000,
        .safe = 2550000,
    },    
    {
        .name = "traktor_mk2_cd",
        .desc = "Traktor Scratch MK2, CD",
        .resolution = 3000,
        .flags = OFFSET_MODULATION,
        .bits = 110,
        .seed = UINT128(0x7ce73, 0xe0e0fff1fc1cf8c1),
        .taps = UINT128(0x400000000000, 0x1000010800000001),
        .length = 4495000,
        .safe = 4500000,
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
    }
};

/*
 * Calculate LFSR bit
 */

static inline bits_t lfsr(bits_t code, bits_t taps)
{
    bits_t taken;
    int xrs;

    taken = code & taps;
    xrs = 0;
    while (taken != 0x0) {
        xrs += taken & 0x1;
        taken >>= 1;
    }

    return xrs & 0x1;
}

/*
 * Linear Feedback Shift Register in the forward direction. New values
 * are generated at the least-significant bit.
 */

static inline bits_t fwd(bits_t current, struct timecode_def *def)
{
    /* New bits are added at the MSB; shift right by one */

    bits_t l;
    l = lfsr(current, def->taps | 0x1);
    return (current >> 0x1) | (l << (def->bits - 0x1));
}

/*
 * Linear Feedback Shift Register in the reverse direction
 */

static inline bits_t rev(bits_t current, struct timecode_def *def)
{
    bits_t l, mask;
    bits_t one = 1;
    bits_t taps_shifted = def->taps >> one;
    bits_t bits_shifted = (one << (def->bits - one));

    /* New bits are added at the LSB; shift left one and mask */

    mask = (one << def->bits) - one;
    l = lfsr(current, taps_shifted | bits_shifted);
    return ((current << one) & mask) | l;
}

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

    if (lut_init(&def->lut, def->length) == -1)
	return -1;

    current = def->seed;
    for (n = 0; n < def->length; n++) {
        bits_t next;

        /* timecode must not wrap */
        assert(lut_lookup(&def->lut, current) == (bits_t)-1);
        lut_push(&def->lut, current);

        next = fwd(current, def);
        assert(rev(next, def) == current);

        current = next;
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

        /* if (!lut_load(def)) */
        /*     return def; */

        if (build_lookup(def) == -1)
            return NULL;  /* error */

        /* if(lut_store(def)) { */
        /*     timecoder_free_lookup(); */
        /*     printf("Couldn't store LUT on disk\n"); */
        /*     return NULL; */
        /* } */

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

        if (def->lookup)
            lut_clear(&def->lut);
    }
}

/*
 * Initialise filter values for one channel
 */

static void init_channel(struct timecoder *tc, struct timecoder_channel *ch)
{
    ch->positive = false;
    ch->zero = 0;

    if (tc->def->flags & OFFSET_MODULATION) {
        delayline_init(&ch->delayline);
        delayline_init(&ch->delayline_deriv);
        ch->ref_level = 0;
    }
    ch->upper_avg_slope = INT_MAX/2;
    ch->lower_avg_slope = INT_MAX/2;
    ch->avg_upper_reading = INT_MAX/2;
    ch->avg_lower_reading = INT_MIN/2;
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
    tc->zero_alpha = tc->dt / (ZERO_RC + tc->dt);
    tc->threshold = ZERO_THRESHOLD;
    if (phono)
        tc->threshold >>= 5; /* approx -36dB */

    tc->forwards = 1;
    init_channel(tc, &tc->primary);
    init_channel(tc, &tc->secondary);
    pitch_init(&tc->pitch, tc->dt);

    tc->ref_level = INT_MAX;
    tc->bitstream = 0;
    tc->timecode = 0;
    tc->valid_counter = 0;
    tc->timecode_ticker = 0;

    tc->upper_valid_counter = 0;
    tc->lower_valid_counter = 0;
    tc->mon = NULL;

    tc->upper_corrected_bits = 0;
    tc->lower_corrected_bits = 0;
    #ifdef MK2_PLOT
    printf("Writing plotting data to file\n");
    char *home = getenv("HOME");
    int len = strlen(home) + strlen("/mk2_samples.out") + 1;
    snprintf(filepath, len + 1, "%s/mk2_samples.out", home);

    fp = fopen(filepath, "w");
    if (!fp) {
            perror("fopen");
            exit(-1);
    }
#endif
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

    /* Decay the pixels already in the monitor */

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

// Print a uint128 value in binary format.
void bits_t_print_binary(bits_t a) {
        for (int i = 109; i >= 0; i--) 
            printf("%u", (unsigned) (a >> i) & 0x1);
    printf("\n");
}

#define FORWARD_FACTOR 2
#define REVERSE_FACTOR 1.35
#define AVG_FACTOR 1.0
#define SAFE_FACTOR 3.0
#define SECOND_FACTOR 1.05
void detect_bit_flip(bool over_mean, float slope[2], float avg_slope, int reading, int avg_reading, bits_t *bit, bool *bit_flipped, bool forwards, bits_t one)
{
    double threshold, threshold2;
    

    if (*bit_flipped == false) {

        if (forwards) {
                threshold = FORWARD_FACTOR * avg_slope;
                threshold2 = (FORWARD_FACTOR * avg_slope) * SECOND_FACTOR;
        } else {
                threshold = REVERSE_FACTOR * avg_slope;
                threshold2 = (REVERSE_FACTOR * avg_slope) * SECOND_FACTOR;
                one = !one;
        }

	/* if (*bit == !one && slope[0] > threshold && slope[1] > threshold * SECOND_FACTOR && slope[0] < SAFE_FACTOR * threshold && reading > avg_reading) { */
	if (*bit == !one && slope[0] > threshold && slope[1] > threshold2) {
		*bit = one;
		*bit_flipped = true;
	/* } else if (*bit == one && slope[0] < -threshold && slope[1] < -threshold * SECOND_FACTOR && slope[0] > -SAFE_FACTOR * threshold && reading < avg_reading) { */
	} else if (*bit == one && slope[0] < -threshold && slope[1] < -threshold2) {
                /* printf("slope0: %f, slope1: %f\n", slope[0], slope[1]); */
		*bit = !one;
		*bit_flipped = true;
	}
    } else {
        *bit_flipped = false;
    }
}

#define CORRECTION_ATTEMPTS 0
void correct_bitstream_fwd(bits_t *bitstream, bits_t bit, int *corrected_bits) 
{
    /* printf("corrected_bits : %d\n", *corrected_bits); */
    if (*corrected_bits < CORRECTION_ATTEMPTS) {
        *bitstream >>= 1; 
        *bitstream <<= 1; 
        *bitstream |= bit;
        (*corrected_bits)++;
        return;
    } else {
        *corrected_bits = 0;
    }
}

void correct_bitstream_rev(bits_t *bitstream, bits_t bit, int *corrected_bits) 
{

    /* printf("corrected_bits : %d\n", *corrected_bits); */
    if (*corrected_bits < CORRECTION_ATTEMPTS) {
        bits_t b = bit << 109;
          
        *bitstream <<= 1; 
        *bitstream >>= 1; 
        *bitstream |= b;
        return;
    } else {
        *corrected_bits = 0;
    }
}


int error_counter = 0;
int reading_counter = 0;
#define UPPER_READING 0
#define LOWER_READING 1
#define MEAN_FACTOR 1.1
static void process_mk2_bitstream(struct timecoder *tc, signed int reading) {

    struct timecoder_channel *primary, *secondary;
    int primary_reading, secondary_reading;
    float current_slope[2], last_slope;
    bits_t one;

        primary = &tc->primary;
        secondary = &tc->secondary;

    /* 
     * Due to the delay of the derivative and moving average filter, the third sample after
     * the current sample has to be taken
     */

    primary_reading = *delayline_at_index(&primary->delayline, FILTER_DELAY);
    secondary_reading = *delayline_at_index(&secondary->delayline, FILTER_DELAY);

    /* 
     * Detect if the offset jumps up or down on primary or secondary channel.
     * Both channels are checked to increase accuracy
     */
    if (primary->swapped && primary->positive)  {
            /* Calculate absolute of lower average slope */
	    tc->primary.lower_avg_slope = ema(abs(primary_reading - tc->primary.last_upper_reading[0]),
					  &tc->primary.upper_avg_slope,
					  0.01);

	    primary->last_lower_reading[1] = primary->last_lower_reading[0];
	    primary->last_lower_reading[0] = primary_reading;

            /* TODO: Also process primary bitstream */

	    return; 
    } else if (primary->swapped && !primary->positive)  {
            /* Calculate absolute of upper average slope */
	    tc->primary.upper_avg_slope = ema(abs(primary_reading - tc->primary.last_upper_reading[0]),
					  &tc->primary.upper_avg_slope,
					  0.01);


            primary->last_upper_reading[1] = primary->last_upper_reading[0];
	    primary->last_upper_reading[0] = primary_reading;


            /* TODO: Also process primary bitstream */

	    return; 
    } else if (secondary->swapped && secondary->positive)  {
            /* Calculate absolute of lower average slope */
	    tc->secondary.lower_avg_slope =
		    ema(abs(secondary_reading - tc->secondary.last_lower_reading[0]),
			&tc->secondary.lower_avg_slope,
			0.01);


            /* Calculate current and last slope */
	    current_slope[0] = (float) (secondary_reading - tc->secondary.last_lower_reading[0]) / INT_MAX;
	    current_slope[1] = (float) (secondary_reading - tc->secondary.last_lower_reading[1]) / INT_MAX;
            last_slope = (float) secondary->lower_avg_slope / INT_MAX;

            tc->secondary.avg_lower_reading = ema(secondary_reading, &tc->secondary.avg_lower_reading, 0.01);

            int mean = MEAN_FACTOR * (tc->secondary.last_upper_reading[0] + abs(tc->secondary.last_lower_reading[0])) / 2;
            secondary->last_lower_reading[1] = secondary->last_lower_reading[0];
            secondary->last_lower_reading[0] = secondary_reading;

            one = 0; // If the signal polarity is flipped

            bool over_mean = reading > -mean;
	    /* The bits only change when an offset jump occurs. Else the previous bit is taken  */
            detect_bit_flip(over_mean, current_slope, last_slope, reading, 
                            tc->secondary.avg_lower_reading*AVG_FACTOR, &tc->lower_bit,
                            &tc->lower_bit_flipped, tc->forwards, one);

            /* printf("%d", (int) ~tc->lower_bit & 1); */
            tc->reading_type = LOWER_READING;

#ifdef MK2_PLOT
            mk2_signal.lower_timecode = tc->lower_bit;
            mk2_signal.plot_lower_digit = 1;
            mk2_signal.secondary.lower_reading = secondary_reading;
#endif
    } else if (secondary->swapped && !secondary->positive)  {
            /* Calculate absolute of upper average slope */
	    tc->secondary.upper_avg_slope =
		    ema(abs(secondary_reading - tc->secondary.last_upper_reading[0]),
			&tc->secondary.upper_avg_slope,
			0.01);

            /* Calculate current and last slope */
	    current_slope[0] = (float) (secondary_reading - tc->secondary.last_upper_reading[0]) / INT_MAX;
	    current_slope[1] = (float) (secondary_reading - tc->secondary.last_upper_reading[1]) / INT_MAX;

            last_slope = (float) secondary->upper_avg_slope / INT_MAX;
            tc->secondary.avg_upper_reading = ema(secondary_reading, &tc->secondary.avg_upper_reading, 0.01);
            int mean = MEAN_FACTOR * (tc->secondary.last_upper_reading[0] + abs(tc->secondary.last_lower_reading[0])) / 2;
            secondary->last_upper_reading[1] = secondary->last_upper_reading[0];
            secondary->last_upper_reading[0] = secondary_reading;
            one = 1; // If the signal polarity is normal

            bool over_mean = reading > mean;
	    /* The bits only change when an offset jump occurs. Else the previous bit is taken  */
            detect_bit_flip(over_mean, current_slope, last_slope, reading, 
                            (int) tc->secondary.avg_upper_reading*AVG_FACTOR, &tc->upper_bit,
                            &tc->upper_bit_flipped, tc->forwards, one);

            tc->reading_type = UPPER_READING;

	    /* Uncomment to print the bitstream to stdout */
            /* printf("%d", (int)tc->upper_bit & 1); */

#ifdef MK2_PLOT
            mk2_signal.secondary.upper_reading = secondary_reading;
            mk2_signal.upper_timecode = tc->upper_bit;
            mk2_signal.plot_upper_digit = 1;
            nwrites++;
#endif
    }

    /* Process the upper and lower codes */
    if (tc->forwards) {
        if (tc->reading_type == UPPER_READING) {
                tc->upper_timecode = fwd(tc->upper_timecode, tc->def);
                tc->upper_bitstream = (tc->upper_bitstream >> (bits_t)1) + (tc->upper_bit << (tc->def->bits - (bits_t)1));
                if (tc->upper_timecode == tc->upper_bitstream) {
                        tc->upper_valid_counter++;
                } else {
                    correct_bitstream_fwd(&tc->upper_bitstream, !tc->upper_bit, &tc->upper_corrected_bits);

                    if (tc->upper_timecode == tc->upper_bitstream) {
                        tc->upper_valid_counter++;
                    } else {


#ifdef MK2_PLOT
            mk2_signal.secondary.upper_errors = 1;
            /* printf("ERROR!\n"); */
#endif
                        tc->upper_timecode = tc->upper_bitstream;
                        tc->upper_valid_counter = 0;
                    }
                }

	} else {
                tc->lower_timecode = fwd(tc->lower_timecode, tc->def);
                tc->lower_bitstream = (tc->lower_bitstream >> (bits_t)1) + (tc->lower_bit << (tc->def->bits - (bits_t)1));
                if (tc->lower_timecode == tc->lower_bitstream) {
                        tc->lower_valid_counter++;
                } else {
                    correct_bitstream_fwd(&tc->lower_bitstream, !tc->lower_bit, &tc->lower_corrected_bits);
                    if (tc->lower_timecode == tc->lower_bitstream) {
                        tc->lower_valid_counter++;
                    } else {

#ifdef MK2_PLOT
            mk2_signal.secondary.lower_errors = 1;
            /* printf("ERROR!\n"); */
#endif
                        tc->lower_timecode = tc->lower_bitstream;
                        tc->lower_valid_counter = 0;
                    }
                }
        }

        if (tc->upper_valid_counter > tc->lower_valid_counter + VALID_BITS2_TRAKTOR_MK2) {
            tc->bitstream = tc->upper_bitstream;
            tc->timecode = tc->upper_timecode;
        } else if (tc->lower_valid_counter > tc->upper_valid_counter + VALID_BITS2_TRAKTOR_MK2) {
            tc->bitstream = tc->lower_bitstream;
            tc->timecode = tc->lower_timecode;
        }

            /* printf("upper_valid_counter = %d, lower_valid_counter = %d\n", tc->upper_valid_counter, tc->lower_valid_counter); */
    } else {

        bits_t mask = (((bits_t)1 << tc->def->bits) - (bits_t)1);

        if (tc->reading_type == UPPER_READING) {
                tc->upper_timecode = rev(tc->upper_timecode, tc->def);
                tc->upper_bitstream = ((tc->upper_bitstream << (bits_t)1) & mask) + tc->upper_bit;
                if (tc->upper_timecode == tc->upper_bitstream) {
                        tc->upper_valid_counter++;
                } else {
                    correct_bitstream_rev(&tc->upper_bitstream, !tc->upper_bit, &tc->upper_corrected_bits);
                    if (tc->upper_timecode == tc->upper_bitstream) {
                        tc->upper_valid_counter++;
                    } else {

#ifdef MK2_PLOT
            mk2_signal.secondary.upper_errors = 1;
            /* printf("ERROR!\n"); */
#endif
                        tc->upper_timecode = tc->upper_bitstream;
                        tc->upper_valid_counter = 0;
                    }
                }
        } else {
                tc->lower_timecode = rev(tc->lower_timecode, tc->def);
                tc->lower_bitstream = ((tc->lower_bitstream << (bits_t)1) & mask) + tc->lower_bit;
                if (tc->lower_timecode == tc->lower_bitstream) {
                        tc->lower_valid_counter++;
                } else {
                    correct_bitstream_rev(&tc->lower_bitstream, !tc->lower_bit, &tc->lower_corrected_bits);
                    if (tc->lower_timecode == tc->lower_bitstream) {
                        tc->lower_valid_counter++;
                    } else {


#ifdef MK2_PLOT
            mk2_signal.secondary.lower_errors = 1;
            /* printf("ERROR!\n"); */
#endif
                        tc->lower_timecode = tc->lower_bitstream;
                        tc->lower_valid_counter = 0;
                        error_counter++;
                    }
                }
        }

        if (tc->upper_valid_counter > tc->lower_valid_counter + VALID_BITS2_TRAKTOR_MK2) {
            tc->bitstream = tc->upper_bitstream;
            tc->timecode = tc->upper_timecode;
        } else if (tc->lower_valid_counter > tc->upper_valid_counter + VALID_BITS2_TRAKTOR_MK2) {
            tc->bitstream = tc->lower_bitstream;
            tc->timecode = tc->lower_timecode;
        }

        /* printf("upper_valid_counter = %d, lower_valid_counter = %d\n", tc->upper_valid_counter, tc->lower_valid_counter); */

    }

    reading_counter++;
    if (tc->timecode == tc->bitstream) {
        tc->valid_counter++;
    } else {
        tc->timecode = tc->bitstream;
        tc->valid_counter = 0;
    }
    /* Take note of the last time we read a valid timecode */

    tc->timecode_ticker = 0;

    signed int m = abs(primary->deriv / 2 - tc->primary.zero / 2);
    tc->ref_level -= tc->ref_level / REF_PEAKS_AVG;
    tc->ref_level += m / REF_PEAKS_AVG;


    /* Inspect demodulation quality */
    /* printf("upper_valid_counter: %d, lower_valid_counter %d, upper_valid_counter2: %d, lower_valid_counter2 %d, forwards: %b\n", */
	   /* tc->upper_valid_counter, */
	   /* tc->lower_valid_counter, */
	   /* tc->upper_valid_counter2, */
	   /* tc->lower_valid_counter2, */
           /* tc->forwards); */

    if(reading_counter == tc->def->resolution * 10) {
        printf("errors: %d\n", error_counter);
        /* exit(0); */
    }

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
	tc->timecode = fwd(tc->timecode, tc->def);
	tc->bitstream = (tc->bitstream >> 1)
	    + (b << (tc->def->bits - 1));

    } else {
	bits_t mask;

	mask = ((1 << tc->def->bits) - 1);
	tc->timecode = rev(tc->timecode, tc->def);
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
 * Process a single sample from the incoming audio
 *
 * The two input signals (primary and secondary) are in the full range
 * of a signed int; ie. 32-bit signed.
 */

int p_old, s_old;
double pitch_old = 0;
static void process_sample(struct timecoder *tc,
			   signed int primary, signed int secondary)
{
    double alpha = 0.3;
    double alpha2 = 0.01;



    if (tc->def->flags & OFFSET_MODULATION) {
        /* primary = ema(primary, &p_old, 0.95); */
        /* secondary = ema(secondary, &s_old, 0.95); */
        tc->primary.ema = ema(primary, &tc->primary.ema_old, alpha);
        tc->secondary.ema = ema(secondary, &tc->secondary.ema_old, alpha);
        tc->primary.ema2 = ema(primary, &tc->primary.ema_old, alpha2);
        tc->secondary.ema2 = ema(secondary, &tc->secondary.ema_old, alpha2);
        tc->primary.deriv = 10 * discrete_derivative(tc->primary.ema, &tc->primary.deriv_old);
        tc->secondary.deriv = 10 *  discrete_derivative(tc->secondary.ema, &tc->secondary.deriv_old);
        delayline_push(&tc->primary.delayline_deriv, tc->primary.deriv);
        delayline_push(&tc->secondary.delayline_deriv, tc->secondary.deriv);
        detect_zero_crossing(&tc->primary, tc->primary.deriv, tc->zero_alpha, tc->threshold);
        detect_zero_crossing(&tc->secondary, tc->secondary.deriv, tc->zero_alpha, tc->threshold);
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

	/* dx = emaf(1.0 / tc->def->resolution / 4, &pitch_old, 0.99); */
	dx = 1.0 / tc->def->resolution / 4;
	if (!tc->forwards)
	    dx = -dx;
	pitch_dt_observation(&tc->pitch, dx);
    }

    /* If we have crossed the primary channel in the right polarity,
     * it's time to read off a timecode 0 or 1 value */

#ifdef MK2_PLOT
        mk2_signal.primary.value = *delayline_at_index(&tc->primary.delayline, FILTER_DELAY);
        mk2_signal.primary.deriv = tc->primary.deriv;
        mk2_signal.primary.upper_reading = 0;
        mk2_signal.primary.lower_reading = 0;
        mk2_signal.primary.avg_upper_reading = tc->primary.avg_upper_reading;
        mk2_signal.primary.avg_lower_reading = tc->primary.avg_lower_reading;
        mk2_signal.primary.upper_errors = 0;
        mk2_signal.primary.lower_errors = 0;

        mk2_signal.secondary.value = *delayline_at_index(&tc->secondary.delayline, FILTER_DELAY);
        mk2_signal.secondary.deriv = tc->secondary.deriv;
        mk2_signal.secondary.upper_reading = 0;
        mk2_signal.secondary.lower_reading = 0;
        mk2_signal.secondary.avg_upper_reading = tc->secondary.avg_upper_reading;
        mk2_signal.secondary.avg_lower_reading = tc->secondary.avg_lower_reading;
        mk2_signal.secondary.upper_errors = 0;
        mk2_signal.secondary.lower_errors = 0;

        mk2_signal.forwards = tc->forwards;
        mk2_signal.plot_upper_digit = 0;
        mk2_signal.plot_lower_digit = 0;
#endif

	if (tc->def->flags & OFFSET_MODULATION) {
		if (tc->primary.swapped) {
			signed int reading = *delayline_at_index(&tc->primary.delayline, FILTER_DELAY);
			process_mk2_bitstream(tc, reading);
		} else if (tc->secondary.swapped) {
			signed int reading = *delayline_at_index(&tc->secondary.delayline, FILTER_DELAY);
			process_mk2_bitstream(tc, reading);
                }
	} else {
		if (tc->secondary.swapped &&
		    tc->primary.positive == ((tc->def->flags & SWITCH_POLARITY) == 0)) {
			signed int m;

			/* scale to avoid clipping */
			m = abs(primary / 2 - tc->primary.zero / 2);
			process_bitstream(tc, m);
		}
	}
#ifdef MK2_PLOT
        fwrite(&mk2_signal, sizeof(struct mk2_signal), 1, fp);
        if (nwrites > (tc->def->resolution) * T) {
            fclose(fp);
            printf("%d second(s) of timecode data have been written to %s\n", T, filepath);
            exit(0);
        }
#endif
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

        if (tc->def->flags & OFFSET_MODULATION) {
            delayline_push(&tc->primary.delayline, primary);
            delayline_push(&tc->secondary.delayline, secondary);
        }

        process_sample(tc, primary, secondary);

        if (tc->def->flags & OFFSET_MODULATION) {
            update_monitor(tc, tc->primary.deriv, tc->secondary.deriv);
	} else {
            update_monitor(tc, left, right);
        }


        pcm += TIMECODER_CHANNELS;
    }
}

/*
 * Get the last known position of the timecode
 *
 * If no data is available or if too few bits have been error
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

    if (tc->def->flags & OFFSET_MODULATION) {
        if (tc->valid_counter < VALID_BITS_TRAKTOR_MK2)
            return -1;
    } else {
        if (tc->valid_counter <= VALID_BITS)
                return -1;
    }

    r = lut_lookup(&tc->def->lut, tc->bitstream);

    if (r == -1) {
        return -1;
    }

    if (when)
        *when = tc->timecode_ticker * tc->dt;

    return r;
}
