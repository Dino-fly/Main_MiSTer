#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "chome_proc.h"

/*
  A pid and how many times it has been asked about. The count is not used to give up -
  nothing here ever gives up on a child while the kernel still says it is ours - it is
  what the harness reads to prove the retry happens at all.
*/
static struct { pid_t pid; int tries; } dying[CHOME_CHILD_MAX];
static int ndying = 0;

#ifdef CHOME_HOST_TEST
static void (*test_sig)(pid_t pid, int sig, int group) = 0;
static int  (*test_reap)(pid_t pid) = 0;

void chome_proc_test_hooks(void (*sigfn)(pid_t pid, int sig, int group),
	int (*reapfn)(pid_t pid))
{
	test_sig = sigfn;
	test_reap = reapfn;
}

void chome_proc_test_clear() { ndying = 0; }
#endif

static void send(pid_t pid, int sig, int group)
{
#ifdef CHOME_HOST_TEST
	if (test_sig) test_sig(pid, sig, group);
#else
	kill(group ? -pid : pid, sig);
#endif
}

/*
  One attempt. >0 collected, 0 still running, <0 not ours.

  ECHILD is "not ours", and it is treated as done rather than retried: a pid this process
  does not own cannot become one it owns, so retrying is a syscall a frame for ever. It
  should not happen - nothing in this firmware reaps children behind our back, which is
  the same assumption net_poll() and bt_poll() write down at their own waitpid() calls -
  but if it ever starts happening, dropping the entry is the behaviour that does not spin.
*/
static int reap_one(pid_t pid)
{
#ifdef CHOME_HOST_TEST
	return test_reap ? test_reap(pid) : 0;
#else
	int st = 0;
	pid_t r = waitpid(pid, &st, WNOHANG);
	if (r == pid) return 1;
	if (r < 0) return -1;
	return 0;
#endif
}

static int find(pid_t pid)
{
	for (int i = 0; i < ndying; i++) if (dying[i].pid == pid) return i;
	return -1;
}

static void forget(int i)
{
	dying[i] = dying[--ndying];
}

int chome_child_pending() { return ndying; }

int chome_child_outstanding(pid_t pid)
{
	return (pid > 0 && find(pid) >= 0) ? 1 : 0;
}

void chome_child_reap()
{
	/*
	  Backwards, because forget() fills the hole from the end: walking forwards would
	  skip whatever entry got moved down into the slot just vacated. With three modules
	  the visible symptom would have been one leaked pid on the frame two of them were
	  collected together, which is not a thing anybody would have noticed.
	*/
	for (int i = ndying - 1; i >= 0; i--)
	{
		dying[i].tries++;
		if (reap_one(dying[i].pid)) forget(i);
	}
}

void chome_child_stop(pid_t pid, int sig, int group)
{
	if (pid <= 0) return;

	if (sig) send(pid, sig, group);

	int i = find(pid);
	if (i >= 0) return;              // already ours to collect; the signal above is the news

	/*
	  A full table means CHOME_CHILD_MAX children none of which the kernel will give
	  back. Sweep first - that is free and it is what usually makes room - and only then
	  give up on the newest one, out loud.

	  Giving up is a leaked zombie, which is what this whole file exists to stop, so it
	  is worth being clear about what it costs and does not cost: the process is already
	  signalled, so it is on its way out either way, and the firmware process is replaced
	  on the next core load (app_restart() forks and _exit()s the old one), at which point
	  init inherits and collects everything outstanding.
	*/
	if (ndying >= CHOME_CHILD_MAX)
	{
		chome_child_reap();
		if (ndying >= CHOME_CHILD_MAX)
		{
			printf("ClassicUI: %d children still not collected, leaving pid %d to init\n",
				ndying, (int)pid);
			return;
		}
	}

	dying[ndying].pid = pid;
	dying[ndying].tries = 0;
	ndying++;
}

#ifdef CHOME_HOST_TEST
int chome_child_tries(pid_t pid)
{
	int i = find(pid);
	return (i >= 0) ? dying[i].tries : 0;
}
#endif
