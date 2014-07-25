#include "disk_mgr.h"
#include "web.h"

#include "wire.h"
#include "wire_fd.h"
#include "wire_stack.h"
#include "wire_io.h"
#include "wire_log.h"

#include <sys/signalfd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static wire_thread_t wire_thread_main;
static wire_t signal_task;

static void handle_shutdown_signal(void)
{
	wire_log(WLOG_NOTICE, "Terminating the process");
	web_stop();
	disk_manager_stop();
	/* Now we return to the loop to let everything unwind in an orderly fashion, all IOs need to return */
}

static void signal_task_run(void *arg)
{
	int fd;
	sigset_t mask;
	wire_fd_state_t fd_state;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);

	/* Block signals so that they aren't handled
	   according to their default dispositions */

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		perror("sigprocmask failed");
		return;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd == -1) {
		perror("signalfd failed");
		return;
	}

	wire_fd_mode_init(&fd_state, fd);
	wire_fd_mode_read(&fd_state);

	while (1) {
		struct signalfd_siginfo fdsi;
		ssize_t s;

		wire_fd_wait(&fd_state);

		s = read(fd, &fdsi, sizeof(struct signalfd_siginfo));
		if (s != sizeof(struct signalfd_siginfo)) {
			fprintf(stderr, "failed to read from signalfd %d: ret=%d errno=%d: %m\n", fd, (int)s, errno);
			wio_close(fd);
			return;
		}

		if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM || fdsi.ssi_signo == SIGQUIT) {
			wire_log(WLOG_NOTICE, "Got signal %d", fdsi.ssi_signo);
			handle_shutdown_signal();
			break;
		} else if (fdsi.ssi_signo == SIGHUP) {
			wire_log(WLOG_INFO, "Got sighup, saving state");
			disk_manager_save_state();
		} else {
			wire_log(WLOG_WARNING, "Read unexpected signal %d", fdsi.ssi_signo);
		}
	}

	sigprocmask(SIG_UNBLOCK, &mask, NULL);

	wire_fd_mode_none(&fd_state);
	wio_close(fd);
	wire_log(WLOG_INFO, "signal thread exiting");
}

static void register_shutdown_handler(void)
{
	wire_init(&signal_task, "signalfd", signal_task_run, NULL, WIRE_STACK_ALLOC(4096));
}

int main()
{
	wire_stack_fault_detector_install();

	wire_thread_init(&wire_thread_main);
	wire_fd_init();
	wire_io_init(8);
	wire_log_init_stdout();

	register_shutdown_handler();
	disk_manager_init();
	web_init(5001);

	wire_thread_run();
	return 0;
}
