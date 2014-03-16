#include "monoclock.h"

#include <time.h>

uint64_t monoclock_get_seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);

	return t.tv_sec;
}

double monoclock_get(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);

	double val = (double)t.tv_sec + (double)(t.tv_nsec / 1000000000);
	return val;
}
