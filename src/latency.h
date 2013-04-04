#ifndef LATENCY_H
#define LATENCY_H

#include "src/disk_def.h"
#include <stdio.h>

void latency_init(latency_t *hist);
void latency_add_sample(latency_t *hist, double val);
void latency_tick(latency_t *latency);

#endif
