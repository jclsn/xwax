#include "print.h"

#include <stdio.h>

void print_seed(bits_t code)
{
    unsigned long long low = (unsigned long long) code;
    unsigned long long high = (unsigned long long) (code >> 64);
    printf(".seed = UINT128(0x%llx, 0x%llx),\n", high, low);
}

void print_uint128(bits_t code)
{
    unsigned long long low = (unsigned long long) code;
    unsigned long long high = (unsigned long long) (code >> 64);
    printf("0x%llx%llx\n", high, low);
}

void print_state_binary(bits_t state, unsigned bits) 
{
        for (int i = bits-1; i >= 0; i--)
            printf("%u", (unsigned) (state >> i) & 0x1);
    printf("\n");
}

void print_bit(bits_t state, unsigned bits)
{
    printf("%u", (unsigned)(state >> (bits - 1) & 0x1));
}


