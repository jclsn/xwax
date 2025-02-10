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

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>

#include "lut.h"

/* The number of bits to form the hash, which governs the overall size
 * of the hash lookup table, and hence the amount of chaining */

#define HASH_BITS 16
/* #define HASH(timecode) ((timecode) & ((1 << HASH_BITS) - 1)) */
#define NO_SLOT ((unsigned)-1)

// Function to compute a 16-bit truncated SHA-256 hash using EVP
uint16_t HASH(__uint128_t input) {
    EVP_MD_CTX *mdctx;
    uint8_t input_bytes[16];  // 128-bit input stored as 16 bytes
    uint8_t hash[EVP_MAX_MD_SIZE];  // Buffer for hash output
    unsigned int hash_len;

    // Convert __uint128_t to a byte array (big-endian)
    for (int i = 0; i < 16; i++) {
        input_bytes[15 - i] = (input >> (i * 8)) & 0xFF;
    }

    // Create a message digest context
    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL) {
        fprintf(stderr, "EVP_MD_CTX_new failed\n");
        return 0;
    }

    // Initialize the hashing operation with SHA-256
    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "EVP_DigestInit_ex failed\n");
        EVP_MD_CTX_free(mdctx);
        return 0;
    }

    // Provide the input data
    if (EVP_DigestUpdate(mdctx, input_bytes, 16) != 1) {
        fprintf(stderr, "EVP_DigestUpdate failed\n");
        EVP_MD_CTX_free(mdctx);
        return 0;
    }

    // Finalize the hashing operation
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        fprintf(stderr, "EVP_DigestFinal_ex failed\n");
        EVP_MD_CTX_free(mdctx);
        return 0;
    }

    // Clean up
    EVP_MD_CTX_free(mdctx);

    // Extract the first 16 bits from the hash
    return (uint16_t)((hash[0] << 8) | hash[1]);
}

/* Initialise an empty hash lookup table to store the given number
 * of timecode -> position lookups */

int lut_init(struct lut *lut, int nslots)
{
    int n, hashes;
    size_t bytes;

    hashes = 1 << HASH_BITS;
    bytes = sizeof(struct slot) * nslots + sizeof(slot_no_t) * hashes;

    fprintf(stderr, "Lookup table has %d hashes to %d slots"
            " (%d slots per hash, %zuKb)\n",
            hashes, nslots, nslots / hashes, bytes / 1024);

    lut->slot = malloc(sizeof(struct slot) * nslots);
    if (lut->slot == NULL) {
        perror("malloc");
        return -1;
    }

    lut->table = malloc(sizeof(slot_no_t) * hashes);
    if (lut->table == NULL) {
        perror("malloc");
        return -1;
    }

    for (n = 0; n < hashes; n++)
        lut->table[n] = NO_SLOT;

    lut->avail = 0;

    return 0;
}


void lut_clear(struct lut *lut)
{
    free(lut->table);
    free(lut->slot);
}


void lut_push(struct lut *lut, bits_t timecode)
{
    unsigned int hash;
    slot_no_t slot_no;
    struct slot *slot;

    slot_no = lut->avail++; /* take the next available slot */

    slot = &lut->slot[slot_no];
    slot->timecode = timecode;

    hash = HASH(timecode);
    slot->next = lut->table[hash];
    lut->table[hash] = slot_no;
}

bits_t lut_lookup(struct lut *lut, bits_t timecode)
{
    unsigned int hash;
    slot_no_t slot_no;
    struct slot *slot;

    hash = HASH(timecode);
    slot_no = lut->table[hash];

    while (slot_no != NO_SLOT) {
        slot = &lut->slot[slot_no];
        if (slot->timecode == timecode) {
            /* 
             * Uncomment to print a confirmation when the LFSR state was found in the LUT 
             */
            /* printf("Found in LUT!\n\n"); */
            return slot_no;
        } 

        slot_no = slot->next;
    }

    return (bits_t)-1;
}
