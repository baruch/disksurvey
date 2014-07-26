#include "timer_bus.h"

#include "wire_log.h"
#include "wire_fd.h"
#include "wire_stack.h"
#include "wire_io.h"

#include <sys/timerfd.h>
#include <stdint.h>
#include <errno.h>

struct timer_bus_sleeper {
	struct list_head list;
	unsigned units_left;
	wire_wait_t wait;
};

static int timer_monotonic(wire_fd_state_t *fd_state, int msecs)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
	if (fd < 0) {
		wire_log(WLOG_INFO, "Failed to setup timerfd: %m");
		return -1;
	}

	struct itimerspec timer = {
		.it_value = { .tv_sec = msecs / 1000, .tv_nsec = (msecs % 1000) * 1000000 },
	};
	timer.it_interval = timer.it_value;

	int ret = timerfd_settime(fd, 0, &timer, NULL);
	if (ret < 0) {
		wio_close(fd);
		return -1;
	}

	wire_fd_mode_init(fd_state, fd);

	return fd;
}

static int timer_read(wire_fd_state_t *fd_state)
{
	wire_fd_mode_read(fd_state);
	wire_wait_single(&fd_state->wait);

	uint64_t timer_val = 0;
	int ret = read(fd_state->fd, &timer_val, sizeof(timer_val));
	if (ret < (int)sizeof(timer_val)) {
		if (errno == EAGAIN)
			return 0; // Wake up not timer related
		wire_log(WLOG_ERR, "Error reading from timerfd: %m");
		return -1;
	}

	if (timer_val > 0) {
		ret = 0;
		return 1;
	}

	return 0;
}

static void timer_close(wire_fd_state_t *fd_state)
{
	wire_fd_mode_none(fd_state);
	wio_close(fd_state->fd);
}


static void timer_bus_wire(void *arg)
{
	timer_bus_t *tbus = arg;
	wire_fd_state_t fd_state;
	struct list_head *next, *cur;

	timer_monotonic(&fd_state, tbus->time_unit_msec);

	while (1) {
		int ret = timer_read(&fd_state);
		if (tbus->stop || ret < 0)
			break;

		for (cur = tbus->sleepers.next, next = cur->next; cur != &tbus->sleepers; cur = next, next = cur->next) {
			struct timer_bus_sleeper *sleeper = list_entry(cur, struct timer_bus_sleeper, list);
			sleeper->units_left--;
			if (sleeper->units_left == 0) {
				list_del(cur);
				wire_wait_resume(&sleeper->wait);
			}
		}
	}

	tbus->stop = -1;
	timer_close(&fd_state);

	for (cur = tbus->sleepers.next, next = cur->next; cur != &tbus->sleepers; cur = next, next = cur->next) {
		struct timer_bus_sleeper *sleeper = list_entry(cur, struct timer_bus_sleeper, list);
		list_del(cur);
		wire_wait_resume(&sleeper->wait);
	}
}

void timer_bus_init(timer_bus_t *tbus, unsigned time_unit_msec)
{
	list_head_init(&tbus->sleepers);
	tbus->stop = 0;
	tbus->time_unit_msec = time_unit_msec;
	wire_init(&tbus->wire, "timer bus", timer_bus_wire, tbus, WIRE_STACK_ALLOC(4096));
}

void timer_bus_stop(timer_bus_t *tbus)
{
	tbus->stop = -1;
}

int timer_bus_sleep(timer_bus_t *tbus, unsigned units)
{
	struct timer_bus_sleeper sleeper;

	sleeper.units_left = units;
	wire_wait_init(&sleeper.wait);
	list_add_head(&sleeper.list, &tbus->sleepers);
	wire_wait_single(&sleeper.wait);
	return tbus->stop;
}
