#ifndef LATENCY_H
#define LATENCY_H

#include <stdint.h>

typedef struct latency latency_t;

void latency_init(latency_t *hist);
void latency_add_sample(latency_t *hist, double val);
void latency_tick(latency_t *latency);

#define LATENCY_RANGES {0.5, 1.0, 3.0, 7.0, 10.0, 15.0}
#define LATENCY_RANGE_COUNT 7
#define NUM_TOP_LATENCIES 5

struct latency_summary {
    double top_latencies[NUM_TOP_LATENCIES];
    uint16_t hist[LATENCY_RANGE_COUNT];
};

struct latency {
    int cur_entry;
    struct latency_summary entries[12 * 24 * 30];
};

#endif
