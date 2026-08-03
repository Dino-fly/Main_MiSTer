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

### A6. The two newest features

| Step | I do | You look at | Report |
|---|---|---|---|
| A6.1 | Load a Mega Drive game, open our menu | Top of the screen | Is the red **STILL PLAYING – NOT PAUSED** band there, fully readable, not cut off? |
| A6.2 | Options ▸ **Core Settings** | The screen | Does the classic OSD appear, with the core's own menu? |
| A6.3 | Press the menu button once, then again | The screen | First press closes the OSD; second brings back Classic Home? Or are you stuck? |

A6.3 is the trap I built a guard for. If you end up unable to get out of the core's
options, say so immediately — that is the failure mode that would need a reset.

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
