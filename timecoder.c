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

/*
 * Uncomment to use the plotting script 
 */

/* #define MK2_PLOT */

#ifdef MK2_PLOT
#include <sys/stat.h>
struct channel {
    int value;
    int deriv;
    int upper_reading;
    int lower_reading;
    int reflevel;
};

struct mk2_signal {
    struct channel primary;
    struct channel secondary;
    int timecode;
    int plot_digit;
    int forwards;
};

char filepath[] = "/tmp/mk2_samples.out";
FILE *fp = NULL;
struct mk2_signal mk2_signal = {};
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
#define VALID_BITS_TRAKTOR_MK2 24

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
        .lfsr1 = {
            .bits = 20,
            .seed = 0x59017,
            .taps = 0x361e4,
        },
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
        .lfsr1 = {
            .bits = 110,
            .seed = UINT128(0x200000008400, 0x802108000000210),
            .seed2 = UINT128(0x3f83f83fc070, 0x1ce7f801ff9cff06),
            .taps = UINT128(0x400000000040, 0x0000010800000001),
        },
        .lfsr2 = {
            .bits = 111,
            /* .taps = UINT128(0x400000000040, 0x0000010800000001), */
            .taps = UINT128(0xc000000000c0 ,0x0000031800000003),
        },
        .length = 1500000,
        .safe = 1520000,
    },    
    {
        .name = "traktor_mk2_b",
        .desc = "Traktor Scratch MK2, side B",
        .resolution = 2500,
        .flags = OFFSET_MODULATION,
        .bits = 110,
        .seed = UINT128(0x3f1fc63e7c00, 0xf8f9c1ff9f0707ff),
        .taps = UINT128(0x400000000040, 0x0000010800000001),
        .length = 2295000,
        .safe = 2285000,
    },    
    {
        .name = "traktor_mk2_cd",
        .desc = "Traktor Scratch MK2, CD",
        .resolution = 3000,
        .flags = OFFSET_MODULATION,
        .bits = 110,
        .seed = UINT128(0x63e03831ff, 0x9f003e0e0000e706),
        .taps = UINT128(0x400000000000, 0x1000010800000001),
        .length = 4950000,
        .safe = 4900000,
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

static inline bits_t fwd2(bits_t current, struct lfsr *def)
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

static inline bits_t rev2(bits_t current, struct lfsr *def)
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


int lut_store(struct timecode_def *def)
{
    int hashes = 1 << 16;
    const char *home;
    ssize_t size = 1;
    FILE *fp = NULL;
    char *path;
    int r = 0;

    home = getenv("HOME");
    if (!home) 
        return -1;

    int len = strlen(home) + strlen("/.lut") + strlen(def->name) + 1;

    path = malloc(sizeof(char) * len + 2);
    if (!path) {
        perror("malloc");
        return -1;
    }
    
    size = snprintf(path, len + 2, "%s/.%s.lut", home, def->name);
    if (size != len) {
        perror("snprintf");
        free(path);
        return -1;
    }

    printf("Storing LUT at %s\n", path);

    fp = fopen(path, "w");
    if (!fp) {
        perror("fopen");
        free(path);
        return -1;
    }

    int i;
    for (i = 0; i < def->length; i++) {
        struct slot *slot = &def->lut.slot[i];

        if (!slot) {
            printf("slot_no: %d doesn't exist'\n", i);
            r = -1;
            goto out;
        }

        size = fwrite(slot, sizeof(struct slot), 1, fp);
        if(!size) {
            perror("fwrite");
            r = -1;
            goto out;
        }
    }

    int j;
    for (j = 0; j < hashes; j++) {
        slot_no_t *hash = &def->lut.table[j];

        size = fwrite(hash, sizeof(slot_no_t), 1, fp);
        if(!size) {
            perror("fwrite");
            r = -1;
            goto out;
        }
    }

    size = fwrite(&def->lut.avail, sizeof(slot_no_t), 1, fp);
    if(!size) {
        perror("fwrite");
        r = -1;
        goto out;
    }

    printf("Wrote %d slots and %d hashes to disk\n", i, j);

out:
    fclose(fp);
    free(path);

    return r;
}

int lut_load(struct timecode_def *def)
{
    int hashes = 1 << 16;
    const char *home;
    ssize_t size = 1;
    char *path;
    int r = 0;
    FILE *fp;

    home = getenv("HOME");

    int len = strlen(home) + strlen("/.lut") + strlen(def->name) + 1;

    path = malloc(sizeof(char) * (len + 2));
    if (!path) {
        perror("malloc");
        return -1;
    }

    size = snprintf(path, len + 2, "%s/.%s.lut", home, def->name);
    if (size != len) {
        perror("snprintf");
        free(path);
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        free(path);
        return -1;
    }

    printf("Loading LUT from %s\n", path);

    r = lut_init(&def->lut, def->length);
    if (r) {
        printf("Couldn't initialise LUT\n");
        goto out;
    }

    int i;
    for (i = 0; i < def->length; i++) {
        struct slot *slot = &def->lut.slot[i];

        size = fread(slot, sizeof(struct slot), 1, fp);
        if(!size) {
            perror("fread");
            r = -1;
            goto out;
        }
    }

    int j;
    for (j = 0; j < hashes; j++) {

        slot_no_t *hash = &def->lut.table[j]; 

        size = fread(hash, sizeof(slot_no_t), 1, fp);
        if(!size) {
            perror("fread");
            r = -1;
            goto out;
        }
    }

    size = fread(&def->lut.avail, sizeof(slot_no_t), 1, fp);
    if(!size) {
        perror("fwrite");
        r = -1;
        goto out;
    }

    printf("Loaded %d slots and %d hashes into memory\n", i, j);

    def->lookup = true;

out:
    fclose(fp);
    free(path);

    return r;
}

// Compute Gold Code from two LFSRs
bits_t compute_gold_code(bits_t lfsr1, bits_t lfsr2) 
{
    return lfsr1 ^ lfsr2;
}

// Compute Gold Code, which is the same no matter the polarity
bits_t stable_gold_code(bits_t lfsr1, bits_t lfsr2) 
{                                                                     
    bits_t xor_code = lfsr1 ^ lfsr2;
    bits_t flipped_xor_code = ~xor_code; // Inverted version

    return (xor_code < flipped_xor_code) ? xor_code : flipped_xor_code;
}

// Find positions where the Gold Code is incorrect
bits_t find_errors(bits_t gold_code_actual, bits_t gold_code_expected) {
    return gold_code_actual ^ gold_code_expected;
}

// Correct errors using bitwise majority voting
bits_t correct_errors(bits_t lfsr1, bits_t lfsr2, bits_t error_mask) {
    // Correct using a simple majority vote (assumes single-bit errors)
    return (lfsr1 & ~error_mask) | (lfsr2 & error_mask);
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

    current = def->lfsr1.seed;
    for (n = 0; n < def->length; n++) {
        bits_t next;

        /* timecode must not wrap */
        assert(lut_lookup(&def->lut, current) == (bits_t)-1);
        lut_push(&def->lut, current);

        next = fwd2(current, &def->lfsr1);
        assert(rev2(next, &def->lfsr1) == current);

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
        delayline_init(&ch->envelope_heights);
        ch->ref_level = 0;
    }
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

    #ifdef MK2_PLOT
    int r;
    printf("Waiting for plotting program to read from the named pipe\n");
    r = mkfifo(filepath, 0666);
    if (r) {
            perror("mkfifo");
    }

    fp = fopen(filepath, "a");
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

/*
 * Detect if the Traktor MK2 signal offset jumped up or down
 */
#define NO_JUMP     0
#define JUMPED_UP   1
#define JUMPED_DOWN 2
#define UPPER_READING 0
#define LOWER_READING 1
static int detect_offset_jump(int reading, int *last_reading, int threshold, int reading_type)
{
    /* Calculate the slope */
    int slope = reading - *last_reading;
    *last_reading = reading;
    int lower_threshold = threshold * 1.3;

    /* Define jump constraints */
    if (reading_type == UPPER_READING) {
        if (slope > threshold && reading > threshold * 2)
            return JUMPED_UP;
        else if (slope < -threshold && (reading < threshold * 2 || reading < 0) )
            return JUMPED_DOWN;
        else
            return NO_JUMP;
    } else {
        if (slope > lower_threshold && (reading > -lower_threshold * 2 || reading > 0) )
            return JUMPED_UP;
        else if (slope < -lower_threshold && reading < lower_threshold * 2)
            return JUMPED_DOWN;
        else
            return NO_JUMP;
    }
}

// Print a uint128 value in binary format.
void bits_t_print_binary(bits_t a) {
        for (int i = 109; i >= 0; i--) 
            printf("%u", (unsigned) (a >> i) & 0x1);
    printf("\n");
}

/*
 * Extract the bitstream from the sample value
 */
static void get_envelope_heights(struct timecoder *tc, signed int reading)
{
    /* 
     * Work out envelope height for both channels:
     *
     * The envelope height is the distance from the highest to lowest peak of the signal.
     * Since the signal jumps up and down the ref_level can't be used here.
     * If the signal jumps up envelope_height / MK2_OFFSET factor, a jump up is detected
     * and vice versa for the jump down.
     */

    if (tc->primary.swapped && !tc->primary.positive) {
        tc->primary.upper_reading = reading;
#ifdef MK2_PLOT
        mk2_signal.primary.reflevel = tc->primary.ref_level;
        if (tc->forwards)
            mk2_signal.primary.upper_reading = tc->primary.upper_reading;
#endif
    } else if (tc->primary.swapped && tc->primary.positive) {
        tc->primary.lower_reading = reading;

        delayline_push(&tc->primary.envelope_heights, envelope_height(tc->primary.lower_reading, tc->primary.upper_reading));
        tc->primary.avg_envelope_height = delayline_avg(&tc->primary.envelope_heights);
        tc->primary.offset_threshold = tc->primary.avg_envelope_height / MK2_OFFSET_FACTOR;
#ifdef MK2_PLOT
        mk2_signal.primary.reflevel = tc->primary.ref_level;
        if (tc->forwards)
            mk2_signal.primary.lower_reading = tc->primary.lower_reading;
#endif
    } else if (tc->secondary.swapped && !tc->secondary.positive) {
        tc->secondary.upper_reading = reading;
#ifdef MK2_PLOT
        mk2_signal.secondary.reflevel = tc->secondary.ref_level;
        if (!tc->forwards)
            mk2_signal.secondary.upper_reading = tc->secondary.upper_reading;
#endif
    } else if (tc->secondary.swapped && tc->secondary.positive) {
        tc->secondary.lower_reading = reading;

        delayline_push(&tc->secondary.envelope_heights, envelope_height(tc->secondary.lower_reading, tc->secondary.upper_reading));
        tc->secondary.avg_envelope_height = delayline_avg(&tc->secondary.envelope_heights);
        tc->secondary.offset_threshold = tc->secondary.avg_envelope_height / MK2_OFFSET_FACTOR;
#ifdef MK2_PLOT
        mk2_signal.secondary.reflevel = tc->secondary.ref_level;
        if (!tc->forwards)
            mk2_signal.secondary.lower_reading = tc->secondary.lower_reading;
#endif
    }
}

void process_subcode(bits_t *bit, bits_t *bitstream, bits_t *timecode, unsigned int *valid_counter, struct lfsr *def) {
            bits_t one = 1;
            *bitstream = (*bitstream >> one) + (*bit << (def->bits - one));
            *timecode = fwd2(*timecode, def);
	    if (*timecode == *bitstream) {
		    (*valid_counter)++;
	    } else {
		    *timecode = *bitstream;
		    *valid_counter = 0;
	    }
}

void process_subcode_rev(bits_t *bit, bits_t *bitstream, bits_t *timecode, unsigned int *valid_counter, struct lfsr *def) {
            bits_t one = 1;
            bits_t mask = ((one << def->bits) - one);

            *bitstream = ((*bitstream << one) & mask) + *bit;
            *timecode = rev2(*timecode, def);
	    if (*timecode == *bitstream) {
		    (*valid_counter)++;
	    } else {
		    *timecode = *bitstream;
		    *valid_counter = 0;
	    }
}



static void process_mk2_bitstream(struct timecoder *tc, signed int reading) {

    int primary_reading;
    int secondary_reading;
    struct timecoder_channel *primary;
    struct timecoder_channel *secondary;

    if (tc->forwards) {
        primary = &tc->primary;
        secondary = &tc->secondary;
    } else {
        primary = &tc->secondary;
        secondary = &tc->primary;
    }

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
	    primary->jump_lower = detect_offset_jump(primary_reading,
						     &primary->last_lower_reading,
						     primary->offset_threshold,
						     LOWER_READING);
	    return; 
    } else if (primary->swapped && !primary->positive)  {
	    primary->jump_upper = detect_offset_jump(primary_reading,
						     &primary->last_upper_reading,
						     primary->offset_threshold,
						     UPPER_READING);

	    return; 
    } else if (secondary->swapped && secondary->positive)  {
	    secondary->jump_lower = detect_offset_jump(secondary_reading,
						       &secondary->last_lower_reading,
						       secondary->offset_threshold,
						       LOWER_READING);

	    if ((primary->jump_lower | secondary->jump_lower ) & JUMPED_UP) {
                    tc->lower_bit = 1;
            } else if ( ((primary->jump_lower | secondary->jump_lower) & JUMPED_DOWN )) {
                    tc->lower_bit = 0;
            }

            /* printf("%d", (int) tc->lower_bit & 1); */
            tc->reading_type = LOWER_READING;

    } else if (secondary->swapped && !secondary->positive)  {
	    secondary->jump_upper = detect_offset_jump(secondary_reading,
						       &secondary->last_upper_reading,
						       secondary->offset_threshold,
						       UPPER_READING);
            /* The bits only change when an offset jump occurs. Else the previous bit is taken  */
            if ((primary->jump_upper | secondary->jump_upper ) & JUMPED_UP) {
                    tc->upper_bit = 1;
            } else if ( ((primary->jump_upper | secondary->jump_upper) & JUMPED_DOWN )) {
                    tc->upper_bit = 0;
            }

            /* printf("%d", (int) tc->upper_bit & 1); */
            tc->reading_type = UPPER_READING;
#ifdef MK2_PLOT
        mk2_signal.plot_digit = 1;
#endif

        /* Uncomment to print the bitstream to stdout */
    } 

    int needed = 5;
    /* Process the upper and lower codes */
    if (tc->forwards) {
        if (tc->reading_type == UPPER_READING) {
		process_subcode(&tc->upper_bit,
				&tc->upper_bitstream,
				&tc->upper_timecode,
				&tc->upper_valid_counter,
				&tc->def->lfsr1);

                process_subcode(&tc->upper_bit,
				&tc->upper_bitstream2,
				&tc->upper_timecode2,
				&tc->upper_valid_counter2,
				&tc->def->lfsr2);

                /* If all counters are not 0, use the upper reading for the bitstream */
		if (tc->upper_valid_counter > needed && tc->lower_valid_counter > needed &&
		    tc->lower_valid_counter2 > needed && tc->upper_valid_counter2 > needed) {
			tc->timecode = fwd2(tc->timecode, &tc->def->lfsr1);
			tc->bitstream = stable_gold_code(tc->upper_bitstream, tc->lower_bitstream);
		}

	} else {
		process_subcode(&tc->lower_bit,
				&tc->lower_bitstream,
				&tc->lower_timecode,
				&tc->lower_valid_counter,
				&tc->def->lfsr1);

                process_subcode(&tc->lower_bit,
				&tc->lower_bitstream2,
				&tc->lower_timecode2,
				&tc->lower_valid_counter2,
				&tc->def->lfsr2);

                /* If two counters are 0, use the lower reading for the bitstream (inverted signal) */
		if (!tc->upper_valid_counter && !tc->lower_valid_counter &&
		    tc->upper_valid_counter2 > needed && tc->lower_valid_counter2 > needed) {
			tc->timecode = fwd2(tc->timecode, &tc->def->lfsr1);
			tc->bitstream = stable_gold_code(tc->upper_bitstream, tc->lower_bitstream);
		}
	}

    } else {
    }

    if (tc->timecode == tc->bitstream) {
        tc->valid_counter++;
    } else {
        tc->timecode = tc->bitstream;
        tc->valid_counter = 0;
    }
    /* Take note of the last time we read a valid timecode */

    tc->timecode_ticker = 0;

    /* Reference level for the monitor. Not used for MK2 timecode */

    signed int m = abs(reading / 2 - tc->primary.zero / 2);
    tc->ref_level -= tc->ref_level / REF_PEAKS_AVG;
    tc->ref_level += m / REF_PEAKS_AVG;

    /* Inspect demodulation quality */
    /* printf("upper_valid_counter: %d, lower_valid_counter %d, upper_valid_counter2: %d, lower_valid_counter2 %d\n", */
	   /* tc->upper_valid_counter, */
	   /* tc->lower_valid_counter, */
	   /* tc->upper_valid_counter2, */
	   /* tc->lower_valid_counter2); */
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

static void process_sample(struct timecoder *tc,
			   signed int primary, signed int secondary)
{
    double alpha = 0.3;

    if (tc->def->flags & OFFSET_MODULATION) {
        tc->primary.deriv = discrete_derivative(ema(primary, &tc->primary.ema_old, alpha), &tc->primary.deriv_old);
        tc->secondary.deriv = discrete_derivative(ema(secondary, &tc->secondary.ema_old, alpha), &tc->secondary.deriv_old);
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

	dx = 1.0 / tc->def->resolution / 4;
	if (!tc->forwards)
	    dx = -dx;
	pitch_dt_observation(&tc->pitch, dx);
    }

    /* If we have crossed the primary channel in the right polarity,
     * it's time to read off a timecode 0 or 1 value */

#ifdef MK2_PLOT
        mk2_signal.primary.value = *delayline_at_index(&tc->primary.delayline, FILTER_DELAY);
        mk2_signal.secondary.value = *delayline_at_index(&tc->secondary.delayline, FILTER_DELAY);
        mk2_signal.primary.deriv = primary_deriv;
        mk2_signal.secondary.deriv = secondary_deriv;
        mk2_signal.plot_digit = 0;
        mk2_signal.forwards = tc->forwards;
        mk2_signal.primary.upper_reading = 0;
        mk2_signal.primary.lower_reading = 0;
        mk2_signal.secondary.upper_reading = 0;
        mk2_signal.secondary.lower_reading = 0;
#endif

	if (tc->def->flags & OFFSET_MODULATION) {
		if (tc->primary.swapped) {
			signed int reading = *delayline_at_index(&tc->primary.delayline, FILTER_DELAY);
                        get_envelope_heights(tc, reading);
			process_mk2_bitstream(tc, reading);
		} else if (tc->secondary.swapped) {
			signed int reading = *delayline_at_index(&tc->secondary.delayline, FILTER_DELAY);
                        get_envelope_heights(tc, reading);
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
        mk2_signal.timecode = tc->current_bit;
        fwrite(&mk2_signal, sizeof(struct mk2_signal), 1, fp);
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
            update_monitor(tc, tc->primary.deriv * 1.5, tc->secondary.deriv * 1.5);
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
