# Keeping the SNAC cores from being silently replaced

The 274 rebuilt cores are chosen by the datecode in their filename. Nothing
declares that a core supports SNAC and nothing checks it, so the day upstream
commits a release dated later than ours, the firmware quietly loads upstream's
build and the PSX pad stops working in that core. No error, no log line, no
clue in the OSD. The user is told to check their adapter.

This directory documents and implements the loop that prevents that: notice
which cores upstream has overtaken, rebuild only those, and install them under
a name the firmware will still choose.

The compile itself is unchanged. It runs on VINNIEPC in the `raetro/quartus:17.0`
container exactly as it did for wave 2, and `win_container_build.sh` here is a
verbatim copy of the script that built those cores.

## What the wave-2 farm already got right

Read `/Users/derek/workspace/snac-build` before changing anything here. It is
not a prototype; it is six iterations of hard-won detail, and most of what looks
arbitrary in it is load-bearing:

- **The Mac clones and patches, Windows only compiles.** Windows has neither git
  nor python, and the source never touches a bind mount, because bind mounts cut
  Quartus to 11% CPU on macOS and silently resolved to an empty directory under
  colima. Source goes in and the `.rbf` comes out through `docker cp -` and a
  tar stream.
- **The container script is shipped in as a file.** Output comes back through
  `cmd.exe`; quoting a build script through `ssh -> cmd.exe -> bash` does not
  survive.
- **`.qpf` selection excludes `Q13`.** Those are Quartus 13 project files and
  picking one wastes the whole build.
- **`NUM_PARALLEL_PROCESSORS` is appended to the `.qsf` at build time** rather
  than baked in, and it is set to 2. Each Quartus process is effectively
  single-threaded, so throughput comes from concurrent builds, not threads, and
  the limit is RAM at roughly 2.5GB each in a 15.6GB VM.
- **Liveness is judged from container state alone.** The earlier per-poll
  `docker exec` into the cgroup accounted for half the API traffic that wedged
  the engine; a genuinely hung build is caught by the hard cap instead.
- **`builds.tsv` records the upstream commit per build.** Wave 1 did not, and
  the cost of that is permanent: 141 of the shipped cores have no recorded
  commit and `MANIFEST_wave1.txt` can only approximate it with "master HEAD at
  manifest time".

## What was missing

Everything about the *second* wave. The farm can build a list of cores; it has
no idea which cores need building.

1. **No upstream comparison.** Nothing recorded what upstream was shipping at
   build time, so there was no baseline to compare against later.
2. **No record of the name we installed under.** `builds.tsv` stores the
   upstream commit but not the output filename, and the datecode was applied
   afterwards while assembling `SD-CARD-ROOT` by a step that was never scripted
   at all. The only surviving record of what wave 2 actually installed is the
   file listing inside the 322MB release archive.
3. **The output name was derived from the repository.** `NAME=${REPO%_MiSTer}`
   throws away the Quartus revision name, which is the name upstream's own
   release carries. Three cores shipped under names no `.mra` can resolve; see
   below.
4. **No distinction between a hopeless core and an unlucky one.** Thirteen cores
   fail to compile from a clean checkout and eight more merely ran out of time.
   Both look like a non-`OK` row.
5. **The machine's address was hard-coded.** `192.168.1.19` is a Wi-Fi lease and
   it has since moved to `.23`.

## The refresh loop

```
upstream_scan.py  --from-builds     what upstream ships now      -> upstream.tsv
shipped_scan.py   --zip <archive>   what we installed (one-off)  -> shipped.tsv
stale.py                            the verdict per core         -> stale-report.tsv
stale.py --list > rebuild.txt       the cores to rebuild
build_via_windows_snac.sh rebuild.txt
```

`upstream_scan.py` and `shipped_scan.py` write their snapshots atomically and
re-read them on the next run, so a scan interrupted at core 200 of 285 resumes,
and running the loop twice does no work the second time and yields a
byte-identical report.

The loop closes on itself: `stale.py` also reads the `out_rbf` column of
`builds-snac.tsv`, so a core this pipeline has rebuilt is held to its new
filename immediately. Rerunning after a build round reports it up to date
instead of queueing it a second time, without waiting for an archive to be
assembled.

### How cores publish releases

Checked in detail on eight repositories — `NES`, `SNES`, `PSX`, `Genesis`,
`Amstrad`, `Minimig-AGA` and the two arcade cores `Arcade-ActFancer` and
`Arcade-Gauntlet` — and then swept across all 285:

| convention | count |
|---|---|
| dated `.rbf` committed under `releases/` | 285 of 285 |
| git tags | 0 of 8 sampled |
| GitHub releases | 0 of 8 sampled |

Every one of the 285 has a `releases/` folder containing at least one
`<name>_YYYYMMDD.rbf`, so there is one convention and no per-core variation to
record. What does vary
is the basename inside `releases/`, and that is where the trouble is.

`releases/` is not a clean folder: it also holds `.mra` files, ROM images,
`rommap.txt`, `alternatives/` subdirectories and the occasional `.DS_Store`, so
the scan groups the dated `.rbf` files by basename and takes the group with the
most releases.

`master` moving is tracked separately as `SOURCE_MOVED`, because an unreleased
upstream commit cannot shadow anything. It is reported but does not force a
rebuild unless `--include-moved` is passed.

### The verdicts

| status | meaning | rebuilt? |
|---|---|---|
| `SHADOWED` | upstream's release datecode is >= the one we shipped | yes |
| `MISNAMED` | our filename cannot be reached by any `.mra` | yes |
| `RETRYABLE` | last attempt never reached a verdict (`overran`, `stalled`, transfer failure) | yes |
| `SOURCE_MOVED` | `master` moved but nothing was released | only with `--include-moved` |
| `UP_TO_DATE` | our datecode still beats upstream's | no |
| `KNOWN_FAIL` | last attempt failed to compile | no, needs a fix |
| `NO_UPSTREAM_RELEASE` | upstream ships no dated `.rbf` to compare against | no |
| `NOT_SHIPPED` | built but not in any shipped archive | no |
| `NO_PROVENANCE` | no upstream repository recorded anywhere | no, needs mapping |

Two judgement calls worth knowing about:

**A shipped artifact outranks the build log.** `builds-rebuild.tsv` is not a
newer attempt at the same cores; it was a separate wave that recompiled
already-shipped arcade cores purely to capture their provenance, and it was
abandoned part-way, leaving eight `overran` rows. Taking the newest log row
would flag eight cores that are on the card and working. So a core we shipped is
judged on its datecode, and only an unshipped core is judged on its last result.

The same rule resolves `MiSTerLaggy` and `PCFX`, which carry `BUILD_FAIL` in
`builds.tsv` and yet are in the archive: the Windows farm failed them, but wave 1
had already built them on the Mac. Thirteen cores have a compile failure on
record; eleven of them really have no artifact.

**Known compile failures are refused, not retried.** The same source fails the
same way; retrying eleven cores on every sweep is hours of someone else's PC for
a guaranteed failure. `FORCE=1` overrides it after an actual fix.

### Joining three name spaces

The repository is `Arcade-ATetris_MiSTer`, the farm calls the core
`Arcade-ATetris`, the Quartus revision is `Arcade-AtariTetris` and the file we
shipped was `ATetris_20260731.rbf`. Nothing agrees with anything, so the join
key drops the `Arcade-` prefix, drops the `_MiSTer` suffix and lowercases. The
suffix matters: three rows of `MANIFEST_wave1.txt` carry the repository name in
the core column, and without stripping it they become phantom cores with no
upstream.

Repository resolution falls back in three steps, because no single record covers
the shipped set: `builds.tsv` names the repository for 144 cores,
`MANIFEST_wave1.txt` for 131 more, and ten shipped arcade cores from the end of
the alphabet appear in neither and are resolved by name against the cached org
listing `allrepos.txt`.

## The naming rule

Three separate pieces of firmware resolve a core name to a file, and only one
naming scheme satisfies all three. Line references are to this repository.

- `bootcore.cpp` `matchesCore_yyyyMMdd_rbf()` matches exactly
  `<core>_YYYYMMDD.rbf` — fixed total length, `_` separator, eight digits, and a
  **case-sensitive** `strncmp` on the prefix. `findCore()` then takes the
  numerically highest datecode, using `candidate.date > best.date`, so a tie
  leaves whichever `readdir()` returned first.
- `support/arcade/mra_loader.cpp` `get_rbf()` scans `_Arcade/cores` for a file
  starting, case-insensitively, with the `.mra`'s `<rbf>` value **or** with
  `Arcade-` + that value, provided the next character is `.` or `_`. Among
  matches it keeps the **lexicographic maximum of the whole filename**.
- `file_io.cpp` `get_display_name()` splits the browser label at `_20` and shows
  the tail as a datecode, which `rbf_hide_datecode` can hide — so two builds of
  one core can look identical in the menu.

Three consequences, and the rule falls out of them.

**1. Only `<name>_YYYYMMDD.rbf` works everywhere.** A suffix such as
`NES_20260807_snac.rbf` still satisfies the arcade prefix scan, and in fact
sorts *above* the plain form because `_` > `.`, but it fails
`matchesCore_yyyyMMdd_rbf` outright — so `.mra` lookups would prefer it while
bootcore and `.mgl` lookups stopped seeing the core at all. Mixed behaviour is
worse than either, so the datecode has to carry the distinction alone.

**2. The basename must be copied from upstream, never derived from the repo.**
Because the arcade scan takes a lexicographic maximum over the full filename,
the choice of prefix can beat the datecode outright. `Gauntlet_20260807.rbf`
sorts above `Arcade-Gauntlet_20250425.rbf` and wins, but the mirror image loses:
ship `Arcade-Bagman_20260807.rbf` next to an upstream `Bagman_20260101.rbf` and
upstream wins forever, because `B` > `A`, no matter how new we are. Matching
upstream's basename makes the prefixes identical, which reduces the comparison
to the datecode, which is the only thing we control.

The Quartus revision name in the core's `.qpf` is the same string, because
upstream compiles the same project file — so the builder can derive the correct
name from the tree it just patched, without consulting GitHub.

**3. The datecode must be strictly greater than upstream's.** Equal is not
enough: it produces the identical filename, so our file overwrites upstream's
instead of sitting beside it, and where both survive the tie-break is undefined.
The format has only day resolution, so there is no way to express "same day but
later" — when we build on the day upstream released, we date one day ahead. That
is a deliberate lie of up to 24 hours and it is the price of the mechanism.

### What this rule found in the shipped archive

Wave 2 named cores `${REPO%_MiSTer}` with the `Arcade-` prefix stripped. For 15
of the 16 arcade cores where that differs from upstream's basename it happens to
be harmless — their `.mra` `<rbf>` value is the plain name, verified against the
real `.mra` files for Arkanoid, SpaceInvaders, Bagman, BattleZone,
ScooterShooter, Berzerk and Breakout. Three cores are genuinely broken:

| core | we shipped | upstream ships | `.mra` asks for |
|---|---|---|---|
| `Arcade-ATetris` | `ATetris_20260731.rbf` | `Arcade-AtariTetris_20240525.rbf` | `<rbf>ataritetris</rbf>` |
| `Arcade-RushnAttack` | `RushnAttack_20260731.rbf` | `Arcade-RshnAtk_20240602.rbf` | — |
| `Arcade-GundamSD` | `GundamSD_20260731.rbf` | `SDGundamPS_20260714.rbf` | — |

`Tetris.mra` asks for `ataritetris`, so it matches `Arcade-AtariTetris_*` and
`ataritetris_*` and nothing else. `ATetris_20260731.rbf` is not reachable from
any `.mra`, so Atari Tetris has silently loaded upstream's 2024 build — without
SNAC — for every user since the archive shipped. Datecodes never entered into
it; the file was simply invisible.

### One upstream hazard, not ours to fix

`get_rbf()` declares `static char lastfound[256]` and never clears it between
calls (`mra_loader.cpp:1261`). After the first arcade launch it holds the
previous winner, and the scan only replaces it on a strictly greater name, so
which `.rbf` a later `.mra` resolves to can depend on what was launched before
it. Anything that relies on exact `.mra` resolution should know this exists.
It is upstream behaviour and out of scope here.

## The build contract

`build_via_windows_snac.sh` is `build_via_windows6.sh` with five changes, kept as
a copy so it reads as a diff rather than a new program. The transport, the
container handling and the timeout policy are untouched.

**Handed to the compile step:** a patched core tree streamed to `/work` in a
fresh container, plus `NPROC`. Nothing else; no network, no bind mount.

**Returned:** `/work/BUILT.rbf`, the fit summary lines for logic utilisation and
register count on stdout, and one of these verdicts:

| exit | stdout marker | meaning |
|---|---|---|
| 0 | `RBF_OK` | built; `.rbf` is at `/work/BUILT.rbf` |
| 4 | `NO_QPF` / `NO_QSF` | no usable project file in the tree |
| 5 | `NO_RBF` | Quartus ran and produced nothing; first six `Error` lines follow |

**Recorded** in `builds-snac.tsv`, which is `builds.tsv` plus two appended
columns so `awk '$5'` and every existing reader keep working:

```
core  repo  upstream_sha  upstream_date  result  upstream_release  out_rbf
```

**Failure is reported, never silent.** The harness distinguishes its own
failures (`CLONE_FAIL`, `COPYIN_FAIL`, `COPYOUT_FAIL`, `overran`) from the
compile's (`BUILD_FAIL`, `NO_QPF`, `PATCH_FAIL`), because only the first kind is
worth retrying. A `COPYOUT_FAIL` deletes the zero-length artifact rather than
leaving a file that looks built.

**The patch is verified before the compute is spent.** `patch_sys.py` edits at
regex anchors; it reports a missing anchor but nothing checked that the result
was wired up, so a core could compile cleanly with no pad in it.
`verify_patch.py` checks the module is byte-identical to the framework, `sys.qip`
lists it once, all seven `USER_IO` drivers and all seven `user_in` assignments
were rewritten, the reader's clock and both user-port input pins are connected,
both dual-SDRAM fallbacks exist, and nothing outside `sys/` changed — that last
one because the entire claim of this project is that there are no per-core source
changes.

## Running it

```sh
# refresh the picture
python3 tools/snac_farm/upstream_scan.py --from-builds
python3 tools/snac_farm/stale.py

# rebuild what needs it
python3 tools/snac_farm/stale.py --list > /Users/derek/workspace/snac-refresh/rebuild.txt
WIN=dinoi@192.168.1.23 JOB=snacjob_r0 NPROC=2 \
  tools/snac_farm/build_via_windows_snac.sh /Users/derek/workspace/snac-refresh/rebuild.txt
```

For more than a handful of cores, split the list round-robin and run one worker
per part exactly as `win_farm.sh` does — the concurrency limit is RAM on
VINNIEPC, not the list.

A 274-core sweep is many hours of someone else's machine. Do not start one
without saying so.

## Exercised end to end on 2026-08-07

One core, `Arcade-ATetris`, chosen because it is the one the naming rule says was
broken:

```
[10:02:13] upstream e40df8751d166efa10f98f31b1deee07d478bd6a
[10:02:13] will install as Arcade-AtariTetris_20260807.rbf
[10:02:44] compile Arcade-ATetris (2 cpus, native x86)
[10:10:49] RESULT Arcade-ATetris OK 8m 2982480b -> Arcade-AtariTetris_20260807.rbf
```

8 minutes, 22% of the ALMs, 14691 registers, and the artifact carries the name
`Tetris.mra` can actually resolve. Rerunning the same list short-circuits on
`ALREADY_BUILT` without touching the PC, and rerunning `stale.py` afterwards
drops the core from the list. Peak disk for the whole exercise, including three
shallow clones for the patch test, was 153MB.
