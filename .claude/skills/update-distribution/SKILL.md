---
name: update-distribution
description: Update the Classic Home distribution from its two upstreams — MiSTer-devel/Main_MiSTer (firmware releases) and Anime0t4ku/Main_MiSTer_Physical_Disc (the vendored CD support) — then build, verify on the device, and package. Use when asked to sync/merge upstream MiSTer, pull in a new MiSTer release, refresh the physical-disc code, or cut an updated distribution.
---

# Updating the distribution

Two upstreams, two very different processes. Never conflate them: one is a git
merge, the other is a vendored-file refresh.

Branch topology: `master` tracks MiSTer-devel; **`classic-ui`** carries the
front-end AND snacpad and is what ships (the old `deploy-all` split ended when
snacpad merged in; verified gone 2026-08-14). Work in a worktree — never
switch branches or `make clean` in the main `Main_MiSTer` checkout (long
builds may be running there).

## Part A — MiSTer upstream (MiSTer-devel/Main_MiSTer)

Upstream has **no tags**. A release is a dated binary committed as
`releases/MiSTer_YYYYMMDD`. Sync at release cadence, merging **that exact
commit**, not upstream master.

`support/classicui/tools/upstream_watch.sh` automates the whole loop:
detect → merge into a throwaway branch off the base (its default is still the
retired `deploy-all`, so pass `--base classic-ui`) → ARM cross-build →
Classic Home harness → **draft** PR. Run it with `--dry-run` first. A merge
that fails to build or drops a harness check never reaches GitHub — that is
deliberate; leave those for a human.

Conflict spots that have needed judgement before (do not auto-resolve):

- `user_io.cpp`: our hooks (`snacpad_poll()`, `chome_core_boot()`, the
  `vp_core_side_suspend/resume` shield in `user_io_status_save()`) land where
  upstream adds code. Keep both; ours are `cfg.classicui`/no-op-gated.
- `MiSTer.ini` is **LF** everywhere since upstream normalized it (verified
  2026-08-14; our copy followed in f4137a1). Only the device's *live* ini may
  still be CRLF from old edits — irrelevant to merges.
- `video.cpp`: `vga_nag()` and `vga_fb_takeover_update()` carry deliberate
  front-end divergences. `support/classicui/UPSTREAM.md` documents every
  intentional divergence from master and why — read it before discarding
  either side of a hunk.
- Any change to `cfg.h`'s struct: `make clean` after, always. Incremental
  builds have shipped a binary with two struct layouts (107 stale objects).

## Part B — CD support (Anime0t4ku/Main_MiSTer_Physical_Disc)

`support/physical_disc/physical_disc.{cpp,h}` are **that author's code taken
whole, deliberately not paraphrased**. This is a file refresh, not a merge:

1. Clone/fetch the fork; diff its `physical_disc.*` against ours.
2. Port upstream's changes **as written**. Do not "improve" the lifecycle —
   the one time it was rewritten smaller it froze two consoles.
3. Re-apply our adaptations: each is marked `adaptation` in the `.cpp` and
   enumerated at the top of that file (kernel-transport-first because SG_IO
   READ CD wedges the GUD1N drive; CDROMREADAUDIO for audio sectors;
   subchannel settling; `physical_disc_acoustic` dropped; the ident-file
   wrapper on `physical_disc_save_name()`). If an adaptation stops applying,
   update the list at the top of the `.cpp` — that list is the merge map.
4. Keep `support/physical_disc/CREDITS.md`, the origin comment at the top of
   `physical_disc.h`, and the credit in release notes. GPLv3 obliges the
   source; naming the author is simply correct.

**The disc code is compiled OUT of the host harness** — a green harness says
nothing about it, and four bugs have hidden in exactly that gap. A refresh is
untested until a real disc plays on the device (PSX at minimum, plus one of
Mega CD / PC Engine CD / Neo Geo CD). Remember every ioctl on `/dev/sr0`
serialises behind the drive, and a fork of the firmware (the disc helper)
looks like a second MiSTer instance in `ps` — check parentage before killing.

## Part B½ — the capability sweep (mandatory after A or B)

A sync is not done when it compiles. Upstream changelogs carry **capability
changes our settings must mirror**, and nothing breaks loudly when they are
missed — a new feature simply never appears in the front-end. After every
merge/refresh, read the changelog (for A: `git log <last-merged>..<release>`
on upstream, plus the release notes in the binary commit message; for B: the
fork's commit log) and sweep for these, each with the files it obligates:

- **A console gains physical/direct CD support** (fork side, usually):
  the shelf needs a CD entry of its own — systems table row in
  `chome_lib.cpp` (CD folder, CD core, the disc goes in the core's CD slot
  via `chome_slot`, not the ROM slot), disc wiring in `chome_disc.*`,
  and the art-matching key measured for that system (they differ per
  system: PSX matches by serial, Saturn by header title — measure, never
  assume), plus `disctitles` coverage.
- **A core gains save states / pause**: the systems table's `CH_SS_*` flag
  in `chome_lib.cpp`, which gates suspend points, Resume, and the slot
  strip. Verify by loading the core on the device and reading the log's
  "core hooks - sdcard:… save:… load:… pause:…" line — the CONF_STR is the
  truth, not the release note. Cores update on their own cadence, so run
  this check when the card's cores update too, not only on firmware syncs.
- **A core gains display options** (a palette slot, color correction,
  flickerblend, a grid/shadow feature): the Display looks should drive the
  core's option instead of a scaler approximation — preset table
  `core_opts`/`palette` in `chome_video.cpp` — and the option needs a tier
  in `chome_core.cpp`'s curation lists (picture/system/risky; an unknown
  name lands on the system tier by default, which is survivable but
  uncurated). Update `CORE-OPTIONS.md` with the measured CONF_STR.
- **New MiSTer.ini options upstream**: our commented `MiSTer.ini` example,
  the parser table in `cfg.cpp` (three-edit rule: `cfg.h` struct + parser +
  ini example), and a judgement call whether Classic Home should own it
  (Best Settings in `chome_ini.cpp`), surface it, or deliberately hide it
  (the video-routing keys are hidden on purpose — see `chome_video.h`).
- **New UIO commands / sys framework features**: check `user_io.h` UIO_*
  against the core framework; anything we consume needs the
  magic-byte-degradation convention so old cores read as "not supported".

Record what the sweep found (or that it found nothing) in the PR/release
notes — a silent sweep is indistinguishable from a skipped one.

## Part C — build, verify, package (both parts end here)

1. **Release build** (Apple Silicon: emulated amd64 container, official
   toolchain, ScreenScraper env mounted — a build without `SS_ENV` silently
   strips the scraper credentials):

   ```
   docker run --rm --platform linux/amd64 \
     -v <worktree>:/mister \
     -v <main>/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf:/tc:ro \
     -v $HOME/.config/classicui/ss.env:/ss.env:ro \
     -w /mister ubuntu:20.04 bash -c \
     'apt-get update -qq && apt-get install -qq -y make; export PATH=/tc/bin:$PATH; make SS_ENV=/ss.env -j$(nproc)'
   ```

   `make clean` first if any other toolchain touched `bin/` — mixed-toolchain
   object reuse once shipped a credential-less binary whose header dependency
   didn't exist. Verify without printing the secret (`-a` is mandatory or the
   answer inverts on macOS): devid on **stdin**, `grep -q -a -F -f - bin/MiSTer`.

2. **Harness**: `support/classicui/test/run.sh` — expect 0 failures. It stubs
   `video.cpp` and the disc code, so it proves the front-end logic only.

3. **Device smoke** (the card's quirks are in the memory notes: Wi-Fi lease
   moves — sweep for port 22+21; root/`1`; `debug=2` for `/tmp/debug.txt`):
   scp to `/media/fat/MiSTer.new`, md5-compare, then
   `killall MiSTer; mv MiSTer.new MiSTer; setsid ./MiSTer &`. A restart does
   not unload the FPGA core — `load_core /media/fat/menu.rbf` to reach the
   shelf. `echo "ss_load 0" > /dev/MiSTer_cmd` restores a savestate slot for
   repeatable captures; `screenshot NAME.png` captures core video pre-scaler
   (core-side effects visible, scaler effects never).

4. **Package**: `support/classicui/tools/make_sdcard_root.sh` builds the
   sdcard zip — it refuses a firmware without the scraper devid, stamps the
   protect scripts, and ships `classicui/lookshots/`. Releases are GitHub
   (pre-)releases on `Dino-fly/Main_MiSTer`, one zip, house-style title
   ("Classic Home vN [beta M] — <what changed, plainly>").

Etiquette gates, non-negotiable: commit/push only when asked; public posts
(PR comments, issues, replies to reporters) are drafted and approved before
posting; never send a user to upstream's tracker while they run our binary.
