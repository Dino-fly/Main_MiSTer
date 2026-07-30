# Building this yourself

A guide for people who have never compiled an FPGA core before. You do not need
to understand Verilog, and you do not need an expensive machine.

Two separate things get built, and you can do either on its own:

| | What it is | Needed for |
|---|---|---|
| **Firmware** | the `MiSTer` executable that runs on the ARM/Linux side | the pad to appear at all |
| **Cores** | the `.rbf` files that program the FPGA | each core you want to use it in |

You need the firmware once. Cores you build only for the systems you care
about — a stock core simply ignores the pad rather than misbehaving.

If you just want to *use* the feature, don't build anything: grab the
[release](https://github.com/Dino-fly/Main_MiSTer/releases) instead.

---

## What you need

**Docker** is the easiest route on every platform, because the FPGA compiler
(Intel Quartus 17.0.2) is an old, awkward install. A prebuilt image exists.

| Platform | Works? | Notes |
|---|---|---|
| Windows (x86) | **best** | Quartus runs natively; fastest by a wide margin |
| Linux (x86) | **best** | same |
| macOS (Apple Silicon) | yes | runs under Rosetta; roughly 2–4× slower, and needs the settings below |
| macOS (Intel) | yes | native |
| Raspberry Pi / ARM Linux | **no** | Quartus is x86-only, and Rosetta doesn't exist there |

Disk: about **25 GB** free (the Quartus image alone is ~11 GB). Time: **8–45
minutes per core**, depending on the core's size and your CPU.

Pull the image once:

```bash
docker pull raetro/quartus:17.0
```

> **Windows tip:** run that in a normal terminal, not over SSH. Docker
> Desktop's credential helper needs an interactive login and fails otherwise.

---

## Part 1 — building the firmware

```bash
git clone -b snac-pr https://github.com/Dino-fly/Main_MiSTer.git
cd Main_MiSTer
source ./setup_default_toolchain.sh    # downloads the ARM cross-compiler
make
```

The result is `bin/MiSTer`. Two things trip people up:

- The setup script must be **sourced, not executed** — it exports the compiler
  onto your `PATH`, which a subprocess couldn't do.
- It needs **gcc 10.2** (which that script fetches). Older toolchains fail with
  confusing errors in unrelated files.

On Apple Silicon, run the whole thing inside an amd64 container:

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/mister -w /mister ubuntu:22.04 \
  bash -c 'apt-get update -qq && apt-get install -y -qq make && \
           source ./setup_default_toolchain.sh && make'
```

---

## Part 2 — building a core

Every core carries its own copy of the shared `sys/` framework. The feature
lives entirely in that folder, so **no core's own source is modified** — you
add one file and patch one file, then compile normally.

### 2.1 Get the framework change

```bash
git clone -b psx-snac https://github.com/Dino-fly/Template_MiSTer.git snac-sys
```

You need two things from it: `sys/psx_snac_pad.sv` (the new circuit) and the
edits inside `sys/sys_top.v`.

### 2.2 Apply it to a core

Clone the core you want — say the NES:

```bash
git clone --recurse-submodules https://github.com/MiSTer-devel/NES_MiSTer.git
```

> Use `--recurse-submodules`. Several cores keep their CPU or sound chip in a
> submodule, and without it the build fails with "undefined entity".

The patch script does the rest. Download it from this repository
([`tools/patch_sys.py`](tools/patch_sys.py)) and run:

```bash
python3 patch_sys.py NES_MiSTer snac-sys/sys/psx_snac_pad.sv
```

It copies the new module in, inserts the changes into that core's own
`sys/sys_top.v` at stable anchor points, and registers the file in `sys.qip`.
It is safe to run twice — it detects its own work and does nothing.

Why a script rather than copying the whole `sys/` folder: cores are on
different framework versions, and replacing the folder wholesale can drag in
years of unrelated changes. Patching in place touches only this feature.

### 2.3 Compile

```bash
cd NES_MiSTer
docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -w /work \
  raetro/quartus:17.0 bash -c 'cp -r /src/. /work && quartus_sh --flow compile NES'
```

The project name is whatever the `.qpf` file is called. The result appears in
`output_files/NES.rbf`.

> **macOS (Apple Silicon), two settings that matter enormously:**
>
> 1. Add `set_global_assignment -name NUM_PARALLEL_PROCESSORS 1` to the `.qsf`.
>    Quartus's multi-threaded mode collapses into lock contention under Rosetta:
>    measured **9% CPU with 6 threads versus 112% with 1** — about 12× faster
>    single-threaded.
> 2. **Never compile on a mounted folder.** Quartus writes tens of thousands of
>    small files; over macOS file sharing that alone drops it to 11% CPU. Copy
>    the project into the container first, as the command above does.
>
> Neither applies on Windows or Linux, where you should use several threads.

---

## Part 3 — installing

| File | Goes to |
|---|---|
| `bin/MiSTer` | `/media/fat/MiSTer` — **back up the original first** |
| `output_files/Menu.rbf` | `/media/fat/menu.rbf` — back up the original first |
| other `output_files/*.rbf` | wherever you keep cores; arcade cores in `/media/fat/_Arcade/cores/` |

Then add to `/media/fat/MiSTer.ini`:

```ini
snac_pad=1
```

Reboot, plug in a PSX SNAC adapter and pad. The pad should appear as
"MiSTer SNAC Pad 1" and drive the OSD menu.

---

## Building many cores

If you want more than a handful, run several compiles at once — Quartus is
effectively **single-threaded per build**, so throughput comes from parallel
builds, not from more threads each. On an 8-core/32 GB Windows machine, 6–8
concurrent builds worked well; 12 exhausted a 16 GB VM.

Two things that cost hours when getting this wrong:

- If you script it, make sure killing a worker also kills its child processes.
  Orphaned transfer pipelines saturate the network and can wedge the Docker
  engine.
- Don't treat "couldn't read the progress counter" as "the build is stuck". A
  finished `.rbf` should always be collected, whatever your watchdog thinks.

## Cores that will not build

A dozen or so upstream repositories cannot be compiled from a clean checkout by
anyone. Their `.qsf` contains `source files.qip`, where `files.qip` is meant to
be filled in by hand through the Quartus GUI — so the sources exist in the repo
but are never compiled, and you get "undefined entity" errors. Jaguar, MSX1,
CreatiVision, Gamate, EG2000, PMD85 and Apple-IIgs are in this group. That is
unrelated to this feature and affects stock builds equally.

## Getting help

Open an issue on this repository with: your platform, the core you were
building, and the first few `Error (…)` lines from the Quartus output. Those
error numbers are far more useful than the final "compilation unsuccessful"
line.
