#ifndef PRINT_H

#define PRINT_H

#include "lut.h"

void print_seed(bits_t code);
void print_uint128(bits_t code);
void print_state_binary(bits_t state, unsigned bits);
void print_bit(bits_t state, unsigned bits);

#endif /* end of include guard PRINT_H */

