---
paths:
  - "support/classicui/**"
---
# Working on the Classic Home front-end (support/classicui)

- After any change, run the host harness through `.claude/scripts/mister-harness <worktree>`
  in the background; it prints one `HARNESS OK checks=N failures=M` line. Add a check for the
  behaviour you changed, next to the section it belongs to.
- The harness compiles the disc helper and other device-only code out. Say what it could not
  exercise, and prove device-only paths on the device (`device-test` skill).
- Never block the main loop for long. Cores watch the HPS: the PSX core treats about 31 ms of
  silence as "busy" and parks its savestate machine, and the NGPC core cannot reopen its
  savestate gate while the loop is stalled. Defer to the per-frame poll (`chome_pend_poll`)
  instead of waiting in place.
- The front-end writes core status bits directly, past the OSD. Honour what the OSD would
  have enforced: an entry's `D`/`d` gate, its option values, the momentary-pulse shape.
- Device captures go to `support/classicui/docs/img/device/<screen>.png`, with the firmware
  md5 in the name or next to it.
