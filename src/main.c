#include "disk.h"

#include <ev.h>
#include <assert.h>

int main()
{
	struct ev_loop *loop = ev_default_loop(EVFLAG_AUTO|EVFLAG_SIGNALFD);
	assert(loop);

	struct disk_conf_t disk_conf = { "/dev/sg0" };
	struct disk_state_t disk_state;
	disk_init(&disk_state, &disk_conf);

	ev_run(loop, 0);
	return 0;
}
