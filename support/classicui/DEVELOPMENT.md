# How Classic Home is developed, tested and released

Two goals pull against each other here. Nothing may ship with a bug that a test
would have caught — and the only complete test rig is one physical machine, one
core at a time, roughly four minutes per build-deploy-look cycle. The host
harness runs thousands of checks in under a minute and any number of copies can
run at once. So every rule below serves one of two masters, and says which:
**catch it before a user does**, or **don't spend the device on what the host
can answer**. A rule with no incident behind it did not make this list.

This document is the process. The repository facts it leans on live elsewhere
and are pointed at, not repeated: the root `CLAUDE.md` (build traps, the ini
section trap, the merge-direct policy, identity rules), `UPSTREAM.md` (every
deliberate divergence from upstream and what it costs), `README.md` ("Running
it on a laptop", "Testing without hardware"), and the `update-distribution`
skill (the mechanics of syncing upstream and cutting an archive — machine-local,
not in this tree).

---

## 1. The ladder: prove it at the cheapest rung that can

In order, cheapest first. A change is proven at the lowest rung that can
actually observe it — and going to a higher rung without exhausting the lower
ones is how three deploy-and-see evenings produced one regression and no fix
(the analog-takeover experiments, all reverted; see `UPSTREAM.md` §2).

1. **The harness** — `test/run.sh`. Compiles the real `chome_*.cpp` unmodified
   against stubs, walks every screen at four canvas sizes, renders PNGs to
   `test/out/`, asserts on index, views, savestate bookkeeping, art, the exact
   MGL emitted, and the scaler model. Zero failures is the only acceptable
   result. This is where logic, layout, ordering and bookkeeping are proven.
2. **`test/play.sh`** — the same front-end live in a browser, fake card, real
   cover fetching. For judging a screen by eye without hardware.
3. **`test/filmstrip.sh`** — motion. Deterministic clock, one PNG per 16 ms
   frame, assembled into a GIF/contact sheet for review. Three design rounds of
   the version riffle happened in one evening this way, with **one** device
   deploy at sign-off; the first riffle cut reached hardware unreviewed because
   this loop didn't exist yet. Any animation change goes through it.
4. **`test/protect.sh`** — the boot hook and packaging scripts, run for real
   against a fake card. This code runs at boot on other people's machines; its
   worst failure is a card with no working `MiSTer`. None of it may meet a
   device first.
5. **Reading source** — before deploying an experiment against a firmware or
   fabric mechanism, read it. For firmware: establish *who else writes the
   flag, who consumes it first, and whether it fires on press or release* —
   four bugs in one day came from trusting names instead
   (`user_io_osd_is_visible()`, `menu_present()`, `menu_key_set()`, the
   double-written thumbnail). For the fabric: the RTL is readable at
   `/Users/derek/workspace/Template_MiSTer/sys/` (`sys_top.v`, `ascal.vhd`).
   Two theories about the wobbling picture — a tearing page flip and a DDR
   bandwidth ceiling — were both plausible; one reading pass plus `gfxbench`
   settled it and retired a wrong fix that had already shipped to the device.
6. **The device** — only for the list in §3, and only under the protocol in §6.

## 2. What must be true before a change is committed

- The harness passes with **no new failures**, and new behaviour has new
  checks. A check whose trust is in doubt goes through §4 first.
- The ARM cross-compile is clean. `make clean` first if a header or the
  toolchain changed — the root `CLAUDE.md` has the 107-stale-objects story.
- Anything the change touches from §3's list either has a device result, or
  the commit message says **"unverified on hardware"** in those words. The
  precedent is `faa833d`: when the harness cannot reach a fix, the harness
  says so in a comment where the check would have been, because a check that
  passes either way reads as cover.
- Instrumentation added for the change obeys §6's perturbation rules before it
  is committed, not after (the repaint summary shipped, and became the fault it
  measured).
- Commit etiquette (worktrees, no PRs, `--no-ff` merges into `classic-ui`,
  identity) is `CLAUDE.md`'s; nothing here relaxes it.

## 3. What a green harness means nothing about

Say these out loud in reports. Each entry has already hidden a real bug.

- **The disc code.** `support/physical_disc/` and everything between
  `#ifndef CHOME_HOST_TEST` guards in `chome_disc.cpp` — the `/dev/sr0` half —
  is compiled out of the harness. Four bugs have lived in exactly that gap
  (wrong serial reader, spinning-up treated as empty, shape read outside the
  identify loop, stale volume label). Countermeasures that work: move the
  *decision* into the common section as a pure function the harness can hold
  (`disc_shape_identity()` exists for this), or add a source-level guard that
  reads the file and asserts the shape — a poor test and a good guard. When a
  disc bug is reported, diff the device half against the tested copy before
  theorising; twice they have silently disagreed.
- **`cfg.cpp`, `menu.cpp`, `user_io.cpp`, `input.cpp`.** Not compiled;
  `stubs.cpp` supplies a fixture option table. A test written against
  `stub_vars[]` passes while the real table is untouched. Prove such changes at
  the object level instead (`arm-none-linux-gnueabihf-nm` on the `.o`) — see
  `CLAUDE.md`, "What the host harness does and does not compile".
- **Real input hardware.** Keyboard and pad reach the front-end through
  different gates (`user_io.cpp` vs `input.cpp`). A synthetic *keyboard* once
  hid a total pad failure through an entire reported-as-done test pass. A
  synthetic uinput *gamepad* takes the real joystick path and is a fair proxy;
  state which class was tested, and leave final confirmation to a physical
  controller.
- **The scaler and the fabric.** Filters, masks, gamma, grid alignment, Screen
  Shadow are FPGA-side: `screenshot` captures core video *pre-scaler*, and the
  OSD/framebuffer are composited by the FPGA and never appear in a DDR grab.
  The harness holds a software model (§5, §7), not the silicon.
- **Cost.** DDR bandwidth, frame times, SD-card write pressure. The framebuffer
  takes ~10 ms/MB through `/dev/mem` and no host figure predicts that. Numbers
  come from `gfxstat`/`gfxbench` on the device, nothing else.
- **Actual video output.** Sync, geometry, overscan, colour on analog. The
  framebuffer was byte-correct during every one of the striped/black/seam
  faults; the eyes at the television are the ground truth.
- **Any stubbed firmware mechanism.** A stub models the *intended* meaning of a
  mechanism; real bugs live in the gap between that and the real one. All four
  shared-mechanism bugs above passed the harness.

## 4. How a test earns trust

This is the heart of "never introduce regressions", and it is a discipline,
not a vibe: **a new check is assumed wrong until it has been seen red.**

**Red-proof: break the fix, watch the test fail, restore.** Mandatory when any
of these hold:

- the test was written *after* the diagnosis (it encodes your theory of the
  bug, and your theory may be wrong — `o` vs `O`: three checks went red on
  revert, which is what made the fix believable);
- the test asserts that a guard or refusal fires;
- the test lives in a section that switches fixtures or reuses state from an
  earlier section;
- a stub had to be changed to make the test possible at all (`menu_key_set`
  had to stop making the menu appear instantly before the handoff bug was
  observable).

Watch the *specific* check redden, for the *fix's* reason. The known ways a
check passes for the wrong reason, each paid for:

- **Another guard fires first.** `protect.sh` section 12d expected a non-zero
  exit and got one — from the credentials refusal, never reaching the case it
  exists to test. After adding any refusal to a shared script, run every test
  that drives it and confirm each deliberate-failure test fails for *its own*
  reason. That same refusal left the whole suite at 34 failures for days while
  it "guarded" the boot hook by asserting nothing.
- **Unrelated standing state produces the asserted outcome.** Two versions of
  "the cover sweep stands down under the menu" passed with the guard deleted,
  because an earlier section's failed pack request left a retry standing and
  the sweep was stopped anyway (`2c913a0`). The repairs generalise: order
  sections so nothing irrelevant is left standing, assert only the **negative**
  claim the guard makes, and let the positive claim ("the sweep works at all")
  live in the section that owns it (`assert_art_fill()`).
- **The fixture parks two code paths on the same answer.** The in-game
  background asked the *cursor* instead of the *running game*; the fixture's
  cursor sat on the running game, so the suite agreed with a device that
  disagreed (`badf6db`). To test a distinction, build a fixture where the two
  sides *differ* — and if the harness cannot make them differ, write that in
  the harness where the check would be (`faa833d`), never a check that passes
  either way.
- **A stub keyed more loosely than the real identity.** The option map keyed on
  the spec string alone, so dropping `ex` used the wrong slot *consistently* —
  every read agreed with every write and no assertion could tell. Key stubs on
  everything the real identity depends on (`(spec, ex)`), and add a fixture
  that exercises the collision (`O8` and `o8` together).

## 5. Coverage shapes that keep biting

When writing checks for anything geometric, sizing or per-core, cover these —
each is a shape a real bug hid behind:

- **Every canvas, including the shipped default.** The in-game background was
  tested at full resolution only; `classicui_halfres=1` — the default users
  boot — went untested, and the identical fault was reported twice
  (`2c913a0`). *Whatever an option's shipped value is, the harness runs it.*
- **A non-square-pixel core.** SNES emits 512×224 for a 4:3 picture (2.29×
  horizontal, 4× vertical at 1080p). A test using only NES 256×224 passes
  whether the axes are independent or not; use 512×224 to tell them apart.
  Proportions come from the scaler's output rect (`vp_output_rect()`), latched
  *before* the takeover, never from native × scale.
- **A rect bigger than the picture on one axis and smaller on the other.**
  976×732 of 1170×896 — no fit/crop case covered it until the device found
  black where the suite found a picture (`36d80a9`). Use the sizes the screens
  *really* ask for, not round numbers.
- **A core with pause and one without.** They take entirely different in-game
  paths (OSD_STATUS hold vs savestate freeze); `assert_ingame()` switches
  fixtures mid-section for exactly this.
- **Both directions of every toggle.** Two fixes in one day shipped having
  exercised only the direction the bug was reported on (menu open fixed, close
  broken). And a held button repeats: harness input sends the release, because
  two sequences that never released the menu button made a too-broad guard
  look safe.
- **An empty library and a huge one** (20,000 games — `f9c4e74`), analog
  (320×240, no Display screen, overscan inset) and HDMI profiles.
- **Never judge a polyphase feature at the wrong scale.** A 1:1 pass is a
  no-op by construction, and a feature narrower than 256/N phases does
  *nothing* at magnification N with every log line looking healthy — the LCD
  grid vanished at exactly 4× while working at 5×. Run the arithmetic
  (`tools/simulate_look.py`) over the real file at the scales real cores hit.

## 6. The device protocol

The machine-specific facts (address, credentials, deploy incantation, log
setup) stay out of the tree by policy; the *shape* of the rules is here.

**Before trusting anything:** md5 the deployed binary against the local build,
and check the process state — `pidof MiSTer`, then parentage, because the disc
helper is a fork that looks like a second instance and a genuine second
instance makes every result random. "The machine is stuck" means *check the
process first*; a dead firmware is indistinguishable from a frozen console
from the sofa, and diagnosing a stuck core instead once cost a dozen round
trips.

**Make each visit count.** The cycle is ~4 minutes and single-threaded, so:

- Go with a written batch, not a question. `TEST-PLAN.md` is the pattern: each
  step names who does what, what to look at, and what to report; "no change"
  is a result; the steps that need human eyes are separated from the ones the
  log can answer.
- **Capture at every screen visited, every time** — not only on failure. The
  captures are release-note and `GUIDE.md` assets; a session that throws them
  away has done half its work. They land in `docs/img/device/`, named for the
  screen, with the build identified.
- **Never send a blind key sequence.** Capture, look, then choose the next
  key; cross-check `/tmp/debug.txt`, which names the transitions a picture
  cannot. Blind navigation broke the day a menu entry was added — the old quit
  sequence hit the new Power row and restarted the box.

**Drive it deterministically, not with a stopwatch.** `/dev/MiSTer_cmd`
(handled in `input.cpp`) accepts `menu`, `key <name>`, `ss_load N`,
`ss_save N`, `core_opt Name=Value`, `gfxstat`, `gfxbench`, alongside the stock
`load_core` and `screenshot`. Add a hook like these when a question needs a
number, or a repetition, without a hand on the pad — "does it feel slow" was
three days of theories, and `gfxstat` was ten minutes to the real answer. The
bar a hook must clear (`5d58b9d`): it travels the *player's path* (synthesise
the key, both press and release), never a private shortcut that measures a
path nobody takes.

**The capture traps, each of which has produced a confident wrong conclusion:**

- The front-end double-buffers: grab **both** menu buffers and take the one
  that changed. A capture identical to the last one is the first thing to
  distrust, not proof a key was ignored.
- Re-read the geometry **per grab**, and believe the firmware's own
  `video: mode now …, fb N at WxH` log line over the module parameters — the
  canvas has changed mid-session, and reading a small frame at a large size
  yields a smear that reads as a rendering bug.
- A killed front-end leaves its last frame on screen forever. Liveness test: two
  grabs of the disc dialog a second apart; byte-identical (md5, not eyes) means
  the screen is dead, not that the UI is broken.
- The classic OSD is composited by the FPGA and never enters the HPS
  framebuffer; `screenshot` captures core video pre-scaler. Neither can prove
  the other's content present or absent.
- Once the front-end takes the screen, the scaler header describes *us*, not
  the game — any game geometry must be latched before the takeover
  (`bf577cb`).

**Instrumentation must not perturb.** Three of these are one incident: the
wobble was partly *caused by its own diagnostics*.

- Nothing writes to the log on a timer. With `debug=2` a log line is an
  SD-card write, and a write while a game runs behind the menu is DDR3 the
  video reader is competing for (`0b1f8b3`, `8fb289a`). Counters accumulate
  silently; a hook prints them on demand.
- Any long pass in the main loop pumps `user_io_core_alive_poll()` — the PSX
  core treats the CD poll as a heartbeat with a ~31 ms deadline, and a debug
  printf has tipped it over.
- A diagnostic prints what the code *did* (the rectangle that was drawn), never
  recomputes its own answer (`3b30ed5`) — and it must not be silenceable by
  the fault it reports (a `debug=` line in the wrong ini section produces no
  log from exactly the user who needs one).

## 7. Keeping the device off the critical path

- **Model the hardware once, pin the model.** The scaler's arithmetic exists
  twice on the host — `vp_render_exact()` in `chome_video.cpp` and
  `tools/simulate_look.py` — both derived from the RTL, pinned to each other by
  `assert_scaler_matches_model()`. That is the template: when a fabric
  behaviour matters to the front-end, model it from the source, add a harness
  check that pins the model, and thereafter iterate on the host. The model's
  gaps are declared (gamma and adaptive filters are not modelled), which is
  what keeps it honest.
- **Fixtures come from measurements, not guesses.** `docs/confstr/` holds real
  CONF_STR dumps from the card's cores; `test/placeholder_games.txt` is 528
  No-Intro/Redump names so cover fetching is exercised for real. When a core's
  behaviour is in question, capture the real thing once and make it a fixture.
- **Parallelise host work; serialise the device.** Harness, filmstrip,
  protect.sh and builds are independent container runs — agents in separate
  worktrees can hold their own. The device has exactly one owner at a time:
  interleaved sessions fight over input devices, `/tmp/debug.txt` and the one
  live ssh, and a transfer stalls every other connection. Batch device
  questions from all in-flight work into one session rather than queueing
  visits.
- **When a mechanism is in doubt, the next step is reading, not deploying**
  (§1 rung 5). Every measurement in the takeover saga said the firmware's
  values were correct; the fault was downstream, and only source reading could
  say where. Deploy-and-see against fabric behaviour is the most expensive
  move this project has.

## 8. Understood is not proven

Reproducing *a* cause of a symptom is not confirming *the* cause. A hardware
reproduction that matches the shape of a user's report proves the mechanism
produces that symptom — not that it produced *their* symptom, and this project
has a trap (the ini section scoping) that mimics almost any "setting does
nothing" report.

- The **fix** may ship on the strength of the reproduction. The **diagnosis**
  stays hedged until the discriminating artifact is in hand — and ask for that
  artifact *before* publishing, not after (`grep -n "^\[" MiSTer.ini` was one
  line and would have settled a public thread that instead needed a
  retraction).
- The honest sentence is: "this is a real bug that produces your symptom;
  whether it is *your* bug needs X."
- Anything a third party reads is drafted and approved first, and no user is
  sent upstream while running our binary — both rules and their scars are in
  `CLAUDE.md`.

## 9. The release gate

The mechanics — upstream sync, the capability sweep, the container build, the
packager, the archive — are the `update-distribution` skill's and are not
repeated here. That skill covers *syncs*; it is silent on what qualifies a
**feature release** as tested, which is this section. Before any archive goes
out:

1. **Harness at zero failures**, and every guard added since the last release
   has been red-proofed (§4). *(The cover-sweep guard passed twice with the fix
   deleted.)*
2. **`protect.sh` at zero failures, with its deliberate-failure sections
   verified failing for their own reasons.** *(34 failures ignored for days,
   and one green check that never reached its subject.)*
3. **The binary being shipped is the binary that was tested**: build with the
   official toolchain, `make clean` if any other toolchain touched `bin/`,
   `SS_ENV` mounted, devid verified with `grep -a` on stdin — then package
   with `--expect-md5` of *that* md5. *(Beta 2 nearly shipped an untested
   rebuild under notes describing hardware tests of a different binary; a
   mixed-toolchain incremental build once shipped without the scraper.)*
4. **A device smoke that covers the shapes, not just the features**: shipped
   defaults as shipped (`classicui_halfres=1` included), one pausing core and
   one freeze core, one square-pixel and one non-square core (SNES), analog
   *and* HDMI when anything video-adjacent changed, a **pad** (not a
   keyboard), and a real disc if `chome_disc.cpp` or `support/physical_disc/`
   changed at all — the harness is silent on that code by construction.
   *(Half-res shipped untested; the pad path failed while the keyboard
   passed; four disc bugs behind a green suite.)*
5. **Captures from that smoke filed** under `docs/img/device/` with the build
   identified. *(Standing rule: they are the release-note and GUIDE assets;
   a session without them has to be re-run to illustrate the release.)*
6. **Release notes**: "tested" may only describe checks actually run against
   the shipped md5, named concretely ("PSX disc boot", "pad navigation at
   240p"); everything else is "unverified on hardware". Record what the
   capability sweep found *or that it found nothing* — a silent sweep is
   indistinguishable from a skipped one. Notes are drafted for approval like
   any public text (§8).

If a step cannot be done — the device is unreachable, a disc system has no
test media — the release notes say so rather than the gate quietly shrinking.
An honest hole is recoverable; a claimed test that didn't happen is how the
same fault gets reported twice.
