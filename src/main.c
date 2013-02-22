#include "disk_mgr.h"
#include "web.h"

#include <ev.h>
#include <assert.h>

int main()
{
	struct ev_loop *loop = ev_default_loop(EVFLAG_AUTO|EVFLAG_SIGNALFD);
	assert(loop);

	disk_manager_init(loop);
	web_init(loop, 5001);

	ev_run(loop, 0);
	return 0;
}
