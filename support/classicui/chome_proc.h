/*
  Collecting the front-end's forked children.

  Three modules here run work in a child process rather than inline, and always for the
  same reason: the work touches a device that can stop answering, and this is the thread
  that draws. chome_disc's detection helper holds /dev/sr0, chome_rip's copier reads it
  for as long as a 700 MB disc takes, chome_bt's agent talks to the adapter over D-Bus.
  See the notes at the top of chome_disc.h for the two measured freezes that forced it.

  All three therefore have to be able to stop a child *without waiting for it*: a child
  inside an uninterruptible ioctl on a dead drive has not exited yet and cannot be made
  to, so a blocking waitpid() there would freeze the front-end for exactly as long as the
  inline version it replaced. Every stop is a signal and a non-blocking check.

  And that is where all three had the same defect. A kill() followed immediately by
  waitpid(WNOHANG) essentially never reaps - the child has not died yet, which is the
  whole premise - and each site then dropped the pid, so nothing could ever try again.
  There is no SIGCHLD handler in this firmware and no waitpid(-1) sweep, so a dropped
  pid is a zombie for the life of the process.

  This file is the "try again" the three of them were missing. A pid handed to
  chome_child_stop() is signalled at once and then kept until a waitpid() has actually
  returned it; chome_child_reap() does one non-blocking attempt per outstanding pid and
  is called once a frame from HandleUI().

  Why one shared set rather than a retry inside each module: each of the three stops its
  child at the exact moment its own polling goes quiet, so none of them has a clock left
  to retry on.

    - disc_watch_stop() is called from disc_launch(), and disc_poll() is then deliberately
      not called at all for as long as the core holds the drive (the drive_is_ours test in
      chome_handle()). It is also called when classicui_disc goes off, which is the one
      case disc_poll() returns early for.
    - rip_cancel() sets rip_pid = -1, and rip_poll() returns immediately on that.
    - pair_child_stop() sets pairing = 0, which is the very flag bt_poll()'s reap block
      is nested inside; it is also reached from bt_watch(0), i.e. by leaving the screen
      whose poll would have done the reaping.

  Why not a SIGCHLD handler, which is the other obvious shape: this is a fork of someone
  else's firmware and the handler would be process-wide. menu.cpp:3540 and :7408 both
  advance a menu state on `waitpid(ttypid, ..., WNOHANG) > 0`, and one of them re-enables
  the OSD keyboard there; a handler that reaped ttypid first would make that test return
  -1 for ever and leave the document viewer's keyboard disabled with no way out.
  menu.cpp:7475 runs the classic pairing entry through popen(), and pclose() waits for
  its own pid too. Stealing another author's child is not a trade this fork gets to make
  on their behalf, and none of it is code we test.
*/

#ifndef CHOME_PROC_H
#define CHOME_PROC_H

#include <sys/types.h>

/*
  How many stopped-but-not-yet-collected children are tracked at once.

  Generous: three modules with one child each, and every one of them is stopped by a
  deliberate player action. The size exists so that a child which can never be reaped
  cannot grow this without bound, not because the normal case comes near it.
*/
#define CHOME_CHILD_MAX 16

/*
  Signal a child and take responsibility for collecting it.

    pid     the child. <= 0 is ignored, so callers need no guard of their own.
    sig     the signal to send, or 0 to send none - for a child already known to be on
            its way out and only needing collection.
    group   send to -pid instead of pid, for a child that called setpgid(0, 0) and may
            have started processes of its own. chome_bt's agent is the only one.

  Handing the same pid over twice sends the signal again and does not add a second
  entry. That is what lets chome_bt escalate a polite SIGINT to a SIGKILL when the
  player asks for a new search before the last agent has gone, and it is safe for
  exactly as long as the pid is still outstanding here - an uncollected pid cannot be
  recycled by the kernel, so the signal cannot land on a stranger.

  The caller should forget its own copy of the pid after this, or keep it only as a
  handle for chome_child_outstanding(). Never as something to signal directly: once
  this file has collected it, the number belongs to whoever the kernel gives it to next.
*/
void chome_child_stop(pid_t pid, int sig, int group);

/*
  1 while `pid` is still one this file is trying to collect - so the process may still
  be alive, and the number is still guaranteed to mean that child and no other.

  The one question a module may ask about a pid it has handed over. Asking it is what
  makes bt_pair_start()'s guard correct: "has the last agent gone" rather than "do I
  think I am still pairing", which was the bug.
*/
int chome_child_outstanding(pid_t pid);

/*
  One non-blocking attempt per outstanding child. Never blocks, whatever state any of
  them is in - which is the reason this can be called from the loop that draws.

  Called from HandleUI() in every core, on every frame, and gated on nothing: the point
  is to be somewhere none of the three modules' own switches can turn off. Costs one
  integer test when there is nothing outstanding, which is almost always.
*/
void chome_child_reap();

// How many children are still waiting to be collected. Diagnostics and the harness.
int chome_child_pending();

#ifdef CHOME_HOST_TEST
/*
  The syscalls, faked.

  The harness has no fork() and no children, so there is nothing for a real waitpid() to
  return and a real kill() would signal whatever host process happens to own that number
  - which is why this seam exists rather than a test asserting against whatever the host
  does with a made-up pid. What the harness can honestly check is the *decision*: is the
  pid still tracked, was it asked again, is it dropped once it comes back. The syscall
  behaviour itself is only observable on the device.

    sigfn   called instead of kill(); receives the pid as passed, the signal, and whether
            the group form was asked for.
    reapfn  called instead of waitpid(pid, 0, WNOHANG). Return >0 for "collected", 0 for
            "still running", <0 for "not ours and never will be" (the real ECHILD).

  Either may be 0. With no reapfn installed nothing is ever collected, which is the
  stuck-child case.
*/
void chome_proc_test_hooks(void (*sigfn)(pid_t pid, int sig, int group),
	int (*reapfn)(pid_t pid));

// How many times `pid` has been asked about. 0 for a pid that is not tracked.
int chome_child_tries(pid_t pid);

// Forget everything without signalling or collecting, so one section cannot leave
// entries lying in front of the next.
void chome_proc_test_clear();
#endif

#endif
