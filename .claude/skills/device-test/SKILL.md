---
name: device-test
description: Verify a firmware change on the real MiSTer - build, deploy, drive the UI, capture the screen - or diagnose a device symptom ("stuck", "nothing happens", "the key did nothing"). Use whenever hardware is involved; the host harness cannot see the device half.
---
Scripts, absolute: `~/workspace/Main_MiSTer/.claude/scripts/` - `mister-find`, `mister-build`, `mister-deploy`, `mister-capture`, `mister-ui`, `mister-fresh`, `mister-log` (each has `--help` and `--dry-run`). Targets come from `~/.config/classicui/devices.env`, never from a literal address.

Two helpers that replace hand-typed ssh: `mister-fresh [--restart] --sys X --game Y --launch` puts the device in a known state in one call (menu core, optional firmware restart ON the menu core so savestate setup runs, then a launch booted by this firmware); `mister-log mark` / `since <mark> -g RE` / `count RE` / `wait RE` read /tmp/debug.txt between steps, restart-aware and capped at 40 lines. Never `ssh ... tail /tmp/debug.txt` into the transcript.

In order, no step skipped:
1. `mister-find` - a silent device means "ask whether it is on", not "sweep harder".
2. Symptom "stuck" / "cannot reach the menu": `pidof MiSTer` and the ppid shape BEFORE any theory. One parent with ppid 1 plus one child of it is healthy; two with ppid 1, or none, is the bug.
3. `mister-build <worktree>` - never the main checkout. Run it with `run_in_background` (3-6 min) and do something else; same for `mister-harness <worktree>` (about 2.5 min, one verdict line). Its verdict line (md5, stale_objects=0, devid=present) is the build's identity; quote it.
4. `mister-deploy <worktree>/bin/MiSTer` - staged, md5-verified, dated rollback, restart, verified. If it stops before the restart, hand the owner the printed one-liner and wait; do not reshape the command.
5. `mister-capture <screen>` after EVERY step, including the ones that look fine: both buffers, two grabs a second apart. Identical md5s mean a dead or static screen - distrust the picture, do not report "key ignored". Look at the capture, then decide the next key; never send a blind key sequence.
   To REACH a point, don't walk there by keys: `mister-ui goto <sys> [<game>]`, `look <name>`, `display`, `launch`, `menu`, `state` place the front-end directly and print its one-line `ui <verb> -> ok|FAILED` answer (needs firmware with the `ui` command, 7e4b73f6+). Capture once you are there and the picture is the point. A step reached by `mister-ui` exercised no input path at all.
6. Baseline before blaming: if a result looks like a regression, swap in the previous binary (`MiSTer.pre-*` on the card, or `--stage-only` an older build) and capture the same state. Identical = not this change.
7. Say which input class was exercised: keyinj (uinput keyboard) proves nothing about the pad path.
8. Keep every capture - `support/classicui/docs/img/device/<screen>.png` is the docs convention - and name the firmware md5 next to it in the report.
