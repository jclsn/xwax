#include <limits.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "delayline.h"

int *delayline_at_index(struct delayline *delayline, ptrdiff_t i)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return NULL;
	}

	if (delayline->current < 0)
		delayline->current += delayline->size;

	ptrdiff_t index = delayline->current + i;

	if ((size_t)index >= delayline->size)
		index -= delayline->size;

	return &delayline->array[index];
}

void delayline_init(struct delayline *delayline)

{	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return;
	}

        delayline->size = DELAYLINE_SIZE;
        for (int i = 0; i < DELAYLINE_SIZE; i++)
            delayline->array[i] = 0;
}
void delayline_decrement(struct delayline *delayline)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return;
	}

	delayline->current--;
	if (delayline->current < 0)
		delayline->current += delayline->size;
}

void delayline_push(struct delayline *delayline, int sample)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return;
	}

	delayline_decrement(delayline);
	delayline->array[delayline->current] = sample;
}

unsigned int delayline_avg(struct delayline *delayline)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return -EINVAL;
	}

	unsigned long sum = 0;

	for (int i = 0; i < delayline->size; i++) 
		sum += delayline->array[i];

	return (unsigned int) (sum / delayline->size);
}

unsigned int delayline_rms(struct delayline *delayline)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return -EINVAL;
	}

	double sum = 0;
        int buffer[DELAYLINE_SIZE];

	for (int i = 0; i < delayline->size; i++) 
		buffer[i] = sqrt((double) delayline->array[i] / INT_MAX);

	for (int i = 0; i < delayline->size; i++) 
		sum += buffer[i];

	return (unsigned int) ((sum / delayline->size) * sum / INT_MAX) ;
}


/* Prints the circular buffer starting at the current read pointer  */
void delayline_print(struct delayline *delayline)
{
	if (!delayline) {
		printf("%s: Null pointer exception\n", __func__);
		return;
	}

	printf("{");
	for (int i = 0; i < delayline->size; i++) {
		printf("%d", *delayline_at_index(delayline, i));
		if (i < delayline->size - 1)
			printf(", ");
	}
	printf("}\n");
}
