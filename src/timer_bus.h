#ifndef TIMER_BUS_H
#define TIMER_BUS_H

#include "wire_wait.h"

typedef struct timer_bus timer_bus_t;

void timer_bus_init(timer_bus_t *tbus, unsigned time_unit_msec);
void timer_bus_stop(timer_bus_t *tbus);

int timer_bus_sleep(timer_bus_t *tbus, unsigned units);

struct timer_bus {
	struct list_head sleepers;
	int stop;
	wire_t wire;
	unsigned time_unit_msec;
};

#endif
