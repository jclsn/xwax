#ifndef LFSR_H

#define LFSR_H

typedef unsigned int bits_t;

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

static inline bits_t fwd(bits_t current, bits_t taps, bits_t bits)
{
    bits_t l;

    /* New bits are added at the MSB; shift right by one */

    l = lfsr(current, taps | 0x1);
    return (current >> 1) | (l << (bits - 1));
}


/*
 * Linear Feedback Shift Register in the reverse direction
 */

static inline bits_t rev(bits_t current, bits_t taps, bits_t bits)
{
    bits_t l, mask;

    /* New bits are added at the LSB; shift left one and mask */

    mask = (1 << bits) - 1;
    l = lfsr(current, (taps >> 1) | (0x1 << (bits - 1)));
    return ((current << 1) & mask) | l;
}

#endif /* end of include guard LFSR_H */

