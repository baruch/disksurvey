#include "latency.h"
#include "util.h"

#include <memory.h>

#define MIN_BUCKET 0

static double histogram_boundary[] = LATENCY_RANGES;

void latency_init(latency_t *latency)
{
    memset(latency, 0, sizeof(latency));
}

static void update_top_latencies(struct latency_summary *entry, double val)
{
    int i;

    // Update the top latencies
    for (i = 0; i < NUM_TOP_LATENCIES-1; i++) {
        if (val > entry->top_latencies[i+1])
            entry->top_latencies[i] = entry->top_latencies[i+1];
        else
            break;
    }
    entry->top_latencies[i] = val;
}

static void update_histogram(struct latency_summary *entry, double val)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(histogram_boundary); i++)
    {
        if (val <= histogram_boundary[i]*1000.0) {
            entry->hist[i]++;
            return;
        }
    }
    // Last entry covers the rest of the range
    entry->hist[i]++;
}

void latency_add_sample(latency_t *latency, double val)
{
    struct latency_summary* entry = &latency->entries[latency->cur_entry];

    if (val > entry->top_latencies[0])
        update_top_latencies(entry, val);
    
    update_histogram(entry, val);
}

void latency_tick(latency_t *latency)
{
    latency->cur_entry++;

    struct latency_summary* entry = &latency->entries[latency->cur_entry];
    memset(entry, 0, sizeof(entry));
}
