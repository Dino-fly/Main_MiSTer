# Test plan: the things only a pair of eyes can settle

For a session where the inputs are driven from here and the screen is reported from there.
Every item is something that could not be settled remotely: either the symptom is motion,
or it is a colour judgement, or the capture path cannot see it.

Answer with what you see, not what you think it should be. "No change" is a result.

**Part A is analog only — HDMI off or unplugged.** Two of the three open bugs only exist in
that state. Do not plug the HDMI screen in until Part B says so.

---

## Part A — analog only (HDMI off)

### A1. The takeover bug — highest value, do this first

Confirmed from here already: after our menu has been open once, `fb params` stays at
`320 240` and the takeover stays held, where before the menu it was `1280 720`. What is
unknown is **whether that is visible on your TV**.

| Step | I do | You look at | Report |
|---|---|---|---|
| A1.1 | Load a game, do not touch the menu | The game | Is the picture correct? Steady, right shape, right size? |
| A1.2 | Open our menu, then close it | The game again | **Did anything change?** Rolling, shifted, squashed, stretched, different size, different brightness — or genuinely identical? |
| A1.3 | Open and close twice more | The game | Does it get worse each time, or stay as it was after A1.2? |

If A1.2 shows no change at all, the bug is real but invisible on your setup, and that is
worth knowing before anyone touches the code.

### A2. Does `fb_terminal_vga` pin it?

The mechanism points at your `fb_terminal_vga=1`. This is the check that would confirm it.

| Step | I do | You look at | Report |
|---|---|---|---|
| A2.1 | Set `fb_terminal_vga=0`, reboot, load the same game | The game | Correct, as in A1.1? |
| A2.2 | Open our menu, then close it | The game | Same question as A1.2 — **any change?** |

If A1.2 changes the picture and A2.2 does not, that pins it, and it very likely also
explains the rolling image a user reported.

### A3. The rolling image, as reported

The user's report was a rolling picture on a 240p CRT via component after launching a core
from the front-end, absent with `classicui=0`.

| Step | I do | You look at | Report |
|---|---|---|---|
| A3.1 | Launch a game **through the shelf**, not by loading a core | The game | Does it roll, tear, or lose sync at any point? |
| A3.2 | Same game, `classicui=0`, launched from the stock menu | The game | Does it roll? |

### A3b. The second report of that class, on S-Video

A different user, same class, and this one adds colour loss:

> scrambled b/w mess with rolling image on my crt with svideo output, looks perfectly
> fine on hdmi, tried changing to hd, sd, lo modes but it was still messed up

Read `README.md`, "Analog video", before running this: the colour loss is settled from
the FPGA source and needs no test, and two of the four things below are settled from the
firmware. What is *not* settled is which of them this user is actually in, and whether
the sync is right once the 31 kHz trap is out of the way. **The order matters — A3b.1
answers the question the whole report turns on.**

You need the CRT on S-Video for this one, and `vga_mode=svideo`, `composite_sync=1`.

| Step | I do | You look at | Report |
|---|---|---|---|
| A3b.1 | Nothing. **You** unplug HDMI entirely, cold boot, open the front-end | The CRT | Is there a picture at all — steady, rolling, or blank? This is the whole question: with HDMI in, the front-end refuses the analog output by design and the CRT shows the core. |
| A3b.2 | Same, HDMI plugged back in | The CRT | Confirm it goes back to showing the *core* rather than this menu, and that `classicui_profile` 1/2/3 changes nothing on it. Expected, not a fault. |
| A3b.3 | HDMI out again. `forced_scandoubler=1`, reboot | The CRT | With this firmware the front-end should still be a 15 kHz picture — the takeover now ignores that setting on an encoded output. **Does the menu hold sync?** The games will be scandoubled and unwatchable; that part is upstream and expected. |
| A3b.4 | HDMI out, `forced_scandoubler=0`, `menu_pal=0` then `1` | The CRT | Which one holds sync on your set? If `0` rolls and `1` does not, that is the first report's rolling explained. |
| A3b.5 | HDMI out, front-end open | The CRT | **Is it black and white?** Expected — say so plainly so it is on record from a pair of eyes and not only from the Verilog. Then start a game: does the *game* have colour? |
| A3b.6 | Open **Options ▸ Best Settings** | The panel | Under **Analog video**, which lines are there? Should be `Black and white on S-Video/CVBS` and, with `menu_pal=0`, `60Hz out: try menu_pal=1 for PAL`. Are they legible at 240p, and does the panel still fit? |

### A3c. The canvas shape, on the two paths that stretch it

The overlapping-buttons half of the same report. The harness proves the arithmetic at
640x240 and renders it (`test/out/tv640-*.png`), but only a television shows whether the
result is the right *shape* once the mode stretches it back over a 4:3 screen.

| Step | I do | You look at | Report |
|---|---|---|---|
| A3c.1 | `vga_scaler=1` with a 15 kHz `video_mode`, reboot, open the front-end | The CRT | Are the cards roughly square-ish rather than tall and thin? Does anything overlap the button legend at the bottom? |
| A3c.2 | Same, open the suspend strip on a game with save states | The strip | Do the three tiles fit inside the panel and clear the legend? |
| A3c.3 | `direct_video=1`, reboot, open the front-end | The CRT | Same two questions. This path stretches the canvas the same way and was never tested. |
| A3c.4 | Either path, read the log | `grep "ClassicUI: profile" /tmp/debug.txt` | It should say `canvas 640x240 (half-width pixels)`. If it says 640x240 with no note, the shape test did not fire and the rest of this is meaningless. |

### A4. PSX: a state saves but will not load

Still unresolved. The log says the load is issued and the freeze release is correctly
suppressed, so whatever fails is downstream of that.

| Step | I do | You look at | Report |
|---|---|---|---|
| A4.1 | Launch Destruction Derby, play to somewhere recognisable, save to slot 1 | The suspend strip | Does the slot show a picture of where you were? |
| A4.2 | Play on somewhere clearly different, then load slot 1 | The game | Does it return to the saved moment, stay where you were, or jump somewhere else? |
| A4.3 | Same again, but on **SNES** for contrast | The game | Does the load work there? |

A4.3 matters: it tells us whether this is PSX or everything.

### A5. Palettes and the core options screen

I could set these and prove the write landed, but not photograph the result — the game
keeps moving between captures and palette differences are subtle.

| Step | I do | You look at | Report |
|---|---|---|---|
| A5.1 | NES game, open **NES ▸ Picture**, step `Palette` through its values | The game behind the panel | Do the colours change as I step? Which looks right to you? |
| A5.2 | Game Boy game, step `Custom Palette` and `Inverted color` | The game | Does each take effect immediately? |
| A5.3 | A GBC game, toggle `GBC Colors` Corrected/Raw | The game | Visible difference? Which is more natural? |
| A5.4 | A GBA game, step `Modify Colors` | The game | Do the GBA/NDS options look meaningfully different? |
| A5.5 | Super Mario 64, toggle `VI Deblur` and `VI Antialias` | Edges and text | Sharper with deblur On? Any cost — shimmer, aliasing? |

For A5.4 there is a known overlap: our Display look also applies a GBA colour curve, so
both together may over-correct. **Tell me if it looks over-saturated or too contrasty** —
that is the double-correction I flagged, and this is the test that confirms it.

### A5b. A core setting kept for one game

This is the widescreen problem you raised: right for one PSX title, wrong for the next. The
host harness proves the bookkeeping — the choice is stored against the game, re-applied at
launch, and not applied to any other game on the core — but only the machine can show that a
real core comes up with it, and that it lands at the launch rather than a moment later.

| Step | I do | You look at | Report |
|---|---|---|---|
| A5b.1 | PSX game A, set `Widescreen Hack` to 16:9 | The row | Is the value marked with a green `*`, and does the footer say it is kept for this game? |
| A5b.2 | Quit to the shelf, start PSX game B | The picture | Is game B in its normal aspect — did the setting stay behind? |
| A5b.3 | Quit, start game A again | The very first frame | Is it already widescreen, or does it snap into widescreen a moment after the game appears? |
| A5b.4 | In game A, press X on that row | The row and the picture | Does the `*` clear and the picture go back at once? |
| A5b.5 | Same as A5b.1 on **N64** with `VI Deblur`, which the core masks off under Clean HDMI | The row | Does it come back deblurred? And does the row still grey out when Video Out is Clean HDMI? |

A5b.3 is the one I cannot see from here. The setting is written before the MGL hands the ROM
over, so it should be in effect on the first frame. If you can see it change *after* the game
appears, say so — that means the apply lands too late, and options that only take effect at
load time will not work.

A5b.5 is an interaction I reasoned about rather than measured: an override on one option can
reveal or hide another, so the apply runs more than one pass over the stored settings. If a
masked-off group comes back wrong, that is where to look.

### A6. The two newest features

| Step | I do | You look at | Report |
|---|---|---|---|
| A6.1 | Load a Mega Drive game, open our menu | Top of the screen | Is the red **STILL PLAYING – NOT PAUSED** band there, fully readable, not cut off? |
| A6.2 | Options ▸ **Core Settings** | The screen | Does the classic OSD appear, with the core's own menu? |
| A6.3 | Press the menu button once, then again | The screen | First press closes the OSD; second brings back Classic Home? Or are you stuck? |

A6.3 is the trap I built a guard for. If you end up unable to get out of the core's
options, say so immediately — that is the failure mode that would need a reset.

---

## A7. The pause — new, and the reason for this session

Built since the plan was written and **not verified on hardware at all**. The harness
cannot see it: it proves the front-end asks for the hold and uses the core's pause, but the
sequence that keeps `OSD_STATUS` high is a fabric fact, and swapping it for the old code
leaves every check passing. Only your eyes settle this.

The claim: on a core that pauses "while the OSD is open", opening our menu should now
**actually stop the game** rather than freezing it with a savestate.

**How to tell a real pause from the old freeze.** Both leave a still picture. The
difference is that the freeze writes a state file and takes a moment; a pause is instant and
writes nothing. Watch for a game with continuous motion - a demo loop, an attract mode - and
open the menu at a moment you can recognise.

| Step | I do | You look at | Report |
|---|---|---|---|
| A7.1 | **NES** game, let it run to something moving, open our menu | The game behind the menu | Is it **completely still**? Any motion at all - animation, scrolling, a blinking cursor? |
| A7.2 | Close the menu | The game | Does it resume from exactly where it stopped, or jump forward as if it had kept running? |
| A7.3 | Same on **Game Boy** | The game | Still? Resumes cleanly? |
| A7.4 | Same on **GBA** | The game | Still? Resumes cleanly? |
| A7.5 | Same on **Mega Drive** | The game | Still? And **is the red STILL PLAYING band gone?** It should be - the game is genuinely paused now |
| A7.6 | **PSX** | The game | Still? |
| A7.7 | **SNES** | The game | This one has *no* pause in the core, so it should still freeze with a state. Expect a still picture and the old behaviour |

A7.5 is a double check: the warning band and the pause are decided by the same test, so if
the band still shows on Mega Drive while the game is stopped, one of the two is wrong.

**Then the thing I am most wary of.** `OSD_STATUS` is a signal cores may use for more than
pausing, and what each does with it cannot be determined from source we do not have.

| Step | I do | You look at | Report |
|---|---|---|---|
| A7.8 | On each core above, open the menu and leave it open ~30s, then close | The game and the sound | Anything odd on resume? Wrong audio, a hang, corrupted picture, lost input? |
| A7.9 | Open the menu, save a suspend point, close | The game | Does it still resume correctly? (`Info("Saving the state")` sends an OSD_ALL command that can silently drop the pause) |

A7.9 is a known fragility, not a guess - any `OSD_ALL` command clears the hold. If the game
lurches forward after saving, that is what happened.

---

## A7b. The still behind the in-game menu, and why a disc looked black

You reported the in-game menu over a game launched from a **physical disc** as having a
black background, where the same core launched from a `.cue` on the card showed the game
behind the menu. The reading that follows from that is "the capture failed on the disc
path", and the harness now says it did not: `assert_ingame_still()` opens the menu over a
running disc and counts the still's own pixels on screen, and they are there.

Two things that are real were found instead, and only the log says which of them this
device is in. The firmware now names both, once per menu open.

| Line in the log | What it means |
|---|---|
| `the still of the game: ok, 352x239` | The capture worked. Anything black on screen after this line is layout, not capture. |
| `the still of the game: the scaler has no frame - not mapped, or a core that does not write one` | The genuine failure, and nobody has seen it yet: this core puts nothing where the scaler capture is read from. |
| `the still of the game: a screenshot save is still in flight` | A screenshot the player asked for has not finished writing. Transient — close the menu and open it again. |
| `the menu background: built, canvas 640x240 px=2, picture 640x240 at 0,0` | The still fills the canvas, which is what it should now do. |
| `the menu background: ... picture 320x240 at 160,0` on a 640-wide canvas | The bug this section is about, and it means the build you are running predates the fix: the game was drawn into the middle half of the width with a black bar down each side. |
| `the still is 320x240 and the canvas is now 640x240, so it cannot be drawn` | The canvas changed size *after* the menu opened and nothing rebuilt the background. Never observed; if it ever appears, say so, because there is no fix for it in the tree yet. |
| `no still to draw the menu over, so the grid is what shows` | The background fell through to the shelf grid. Only ever follows a refused capture. |

| Step | I do | You look at | Report |
|---|---|---|---|
| A7b.1 | Launch a game **from a file**, open the menu | The screen | Is the game visible behind the shelf, dimmed? |
| A7b.2 | `grep "still of the game" /tmp/debug.txt` | The log | Copy the line. `ok` and a size is the expected answer. |
| A7b.3 | Launch the **same core from a disc**, open the menu | The screen | It opens on the disc's own dialog. Press **B** once, which reaches the shelf. Is the game visible *there*? |
| A7b.4 | `grep "menu background" /tmp/debug.txt` | The log | Copy the line. `px=2` says this is a stretched TV canvas; `picture` says how wide the game was drawn and where it landed. |
| A7b.5 | Still over the disc, back on its dialog | The edges of the screen | Is there any of the game down the sides of the dialog at all, or is the surround flat? |

**A7b.3 is the whole test.** The disc's dialog is the canvas less the inset by your own
decision, so behind it there is almost nothing of the game to see whatever the capture did —
1431 pixels of 76800 at 320x240, and half of those are then taken by the scrim every panel
screen lays over the background. If the shelf one press away shows the game, the capture is
fine and the question is a layout one: whether that dialog should let more of the game
through, and whether a background that is already dimmed to 5/16 should be scrimmed a second
time. Both of those are yours to decide and neither is changed.

If the **shelf** is black over a disc and not over a file, that is new and interesting, and
A7b.2 and A7b.4 together say which half of it broke.

---

## A8. gamelist.xml — art scraped somewhere else

Also **not verified on hardware at all**. The harness proves the file is read, that a
malformed or oversized one degrades to no art, and which layer wins; what it cannot show is
a real scraped card, a real gamelist written by a real tool, or what the read costs on the
DE10-Nano. A 3000-game gamelist parses in 81 ms on a development host; on the device,
including the read off the card, expect something nearer half a second — once per system,
the first time a card from it needs a cover.

| Step | I do | You look at | Report |
|---|---|---|---|
| A8.1 | Copy a `gamelist.xml` plus its media folder onto one system's games folder, scraped with whatever you normally use | The shelf for that system | Do the covers appear, and are they the ones you scraped rather than the ones from the art pack? |
| A8.2 | Scroll into that system for the first time after a reboot | The shelf as you arrive | Does it stutter or pause noticeably before the covers start filling in? For how long — a blink, or a second? |
| A8.3 | Same shelf, scrolled a second time | The shelf | Smooth now? (The file is read once per system per boot, so a second visit should cost nothing) |
| A8.4 | Rename one scraped picture on the card so the gamelist points at nothing | That one card | Does it fall back to the art pack or the plain plate, rather than going blank or hanging? |
| A8.5 | `grep gamelist /tmp/debug.txt` | The log | How many covers did it report per system, and did any file get rejected? |

A8.2 is the one number I most want and cannot get from here.

---

## Part B — plug the HDMI screen in

Everything here is hidden or untestable at 240p.

| Step | I do | You look at | Report |
|---|---|---|---|
| B1 | Nothing — just plug it in with a game running | Both screens | What does each show? Does the CRT keep the game while HDMI shows the front-end? |
| B2 | Open our menu | Both screens | Where does the menu appear? Is the CRT still showing the game? |
| B3 | Open **Display** | The looks screen | It is hidden at 240p, so this is its first real outing. Do the preview tiles look right, and is the zoom enough to judge a filter by? |
| B4 | Step through the looks | The game behind | Do they apply immediately, and does each look like its name? |
| B5 | Open the core options screen | The panel | Anything clipped or misaligned at 720p? |
| B6 | With HDMI attached and `vga_scaler=0`, open our menu | **The CRT**, not HDMI | Is there a dimmed box in the middle of the raw analog picture? This is the known cost of the pause change, and B6 is where it would show |

B3 and B4 are the zoomed previews and the immediate-apply change from earlier — both were
only ever verified by harness render and arithmetic, never by eye.

---

## What I need from you at the start

Say when you are at the device, and confirm:

1. HDMI off for Part A.
2. A controller in hand — a real one, not the synthetic pad; I will stop injecting input
   while you are testing so we are not fighting over the same menu.
3. Whether `/media/fat/MiSTer` should be updated first. The build on the card is from
   before the footer fit fix; everything else is current.

I will announce each step before triggering it and wait for your report before moving on.
