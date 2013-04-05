#include "disk_mgr.h"
#include "web.h"

#include <ev.h>
#include <assert.h>
#include <stdio.h>

static ev_signal ev_signal_int;
static ev_signal ev_signal_term;

static void handle_shutdown_signal(struct ev_loop *loop, ev_signal *watcher, int revents)
{
	printf("Terminating the process\n");
	web_stop();
	disk_manager_stop();
	/* Now we return to the loop to let everything unwind in an orderly fashion, all IOs need to return */
}

static void register_shutdown_handler(struct ev_loop *loop)
{
	ev_signal *evsig;

	evsig = &ev_signal_int;
	ev_signal_init(evsig, handle_shutdown_signal, SIGINT);
	ev_signal_start(loop, evsig);

	evsig = &ev_signal_term;
	ev_signal_init(evsig, handle_shutdown_signal, SIGTERM);
	ev_signal_start(loop, evsig);
}

int main()
{
	struct ev_loop *loop = ev_default_loop(EVFLAG_AUTO|EVFLAG_SIGNALFD);
	assert(loop);

	register_shutdown_handler(loop);

	disk_manager_init(loop);
	web_init(loop, 5001);

	ev_run(loop, 0);
	return 0;
}
