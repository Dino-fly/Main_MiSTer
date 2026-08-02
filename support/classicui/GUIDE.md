# Classic Home — a guide

A game-first front-end for MiSTer. You get a shelf of your games with cover art,
save states you can see, and a menu you can reach from inside a game — without
meeting a single line of technical text.

It replaces the menu core's screen only. Everything MiSTer normally does still
works, and the classic menu is one deliberate choice away on **Options ▸ Advanced
Settings**.

All the pictures below are rendered by the test harness from the real UI code, so
they are exactly what the code draws.

---

## The shelf

![The shelf](img/shelf.png)

Your whole library in one row: cover art, the system, and how many times you have
played each game. Left and right walk the shelf; the shoulder buttons page it.

The row starts with **Favourites** and **Systems**, so from anywhere in the shelf
**B** jumps straight back to them rather than making you walk.

The strip of pips under a card is its save-state slots — filled ones are green.

### System icons

![System icons](img/system-icons.png)

Every system has an icon. They come from licensed icon sets, not hand-drawn — see
[ICONS.md](ICONS.md) for the attribution.

---

## Save states, called Suspend Points

![Suspend points](img/suspend-points.png)

Press **down** on a game to see its save states. Three slots, each with a picture
of the moment it holds.

- **A** plays from that point
- **Y** saves the moment you are in now
- **X** deletes a slot (twice, deliberately)
- **down** locks a slot so it cannot be overwritten

Opening the menu inside a game does **not** let the game run on. On a core that can
pause properly, it pauses; on one that cannot, the moment is held with a state and
put back when you leave. Either way the game is where you left it.

### Systems that cannot save

![No save states](img/no-savestates.png)

Not every system supports save states — Neo Geo, Mega Drive, N64 and most home
computers do not. Rather than showing you three empty slots to try and fail at, the
shelf says so before you launch, and says it again inside the game.

---

## The menu, from inside a game

![In-game menu](img/in-game-menu.png)

Press the menu button while playing and the whole front-end comes up over a still
of your game: the shelf, your save states, settings, everything. Press it again to
go straight back.

The classic MiSTer OSD never appears on its own.

---

## Buttons look like your controller

![Button glyphs](img/button-glyphs.png)

Wherever the interface names a button it draws it, in the shape and colour that is
actually printed on the pad in your hands. Four sets: PlayStation shapes, Nintendo
letters, Xbox letters, and key names for a keyboard. It follows whichever
controller you used last.

| PlayStation | Nintendo, Xbox and the rest |
|---|---|
| ![PlayStation legend](img/legend-psx-240p.png) | ![Lettered legend](img/legend-letters-240p.png) |

Xbox pads keep the same *positions* as everyone else, which means different
letters: the button that confirms is the east one, which a Nintendo pad calls **A**
and an Xbox pad calls **B**.

---

## Settings

![The menu bar](img/menubar.png)

Press **up** from the shelf for the menu bar: Display, Options, Power, About.

![Options](img/options.png)

### Best Settings

![Best settings](img/best-settings.png)

MiSTer ships with several pop-ups that interrupt a game — the resolution banner,
the button-map panel, the autofire announcement. This turns them off in one go.

It shows you exactly which lines of `MiSTer.ini` it will change, keeps a copy of
your old file, and needs a second press to do anything.

### More Settings

![More settings](img/more-settings.png)

The `MiSTer.ini` options worth changing, with names a person can read —
**Button pop-up: Hidden** rather than `controller_info=0`. Grouped by what they
affect, with a line of help for the selected one.

Anything not at its usual value is **amber**, and the footer tells you what the
usual value is. **X** puts a setting back. Nothing is written until you choose
Save Changes.

Options that could leave you with no picture at all are deliberately not here.

---

## Controllers

![Controllers](img/controllers.png)

Every controller the machine can see — USB, Bluetooth and SNAC — with which player
number it is. Pairing a wireless pad is at the bottom of the list.

![Controller test](img/controller-test.png)

Choose a controller to test it. Press a button and it lights up on the diagram,
drawn in that pad's own button set. Useful for "is this thing connected?" and for
"which button is which?".

---

## Wi-Fi

![Wi-Fi scanning](img/wifi-scanning.png)

Networks, signal strength, and an on-screen keyboard for the password. Scanning
and joining show their progress, so a slow network looks busy rather than broken.
If joining fails it puts your old settings back.

---

## Installing

**You need:** a MiSTer with a working SD card, and the ability to copy one file to it.

1. **Build the firmware.** From a checkout of this fork:

   ```
   source ./setup_default_toolchain.sh     # downloads the ARM toolchain the first time
   make
   ```

   The result is `bin/MiSTer`. Building on macOS needs an `linux/amd64` container;
   see `CLAUDE.md` at the repo root.

2. **Choose your branch.**

   | Branch | Take this if |
   |---|---|
   | `classic-ui` | you want the front-end |
   | `deploy-all` | you also want PSX controllers over SNAC in every core |

3. **Copy it to the card.** The firmware is the single file `MiSTer` in the root of
   the SD card. Keep the old one:

   ```
   cp /media/fat/MiSTer /media/fat/MiSTer.prev
   cp bin/MiSTer /media/fat/MiSTer
   ```

   Copying over the running firmware fails with "text file busy" — `mv` a new file
   over it, or copy while the machine is off.

4. **Turn it on** in `/media/fat/MiSTer.ini`:

   ```
   classicui=1
   ```

5. **Reboot.** First boot scans your `games/` folders and builds the library, which
   takes a moment; after that it is cached.

If something goes wrong, `classicui=0` gives you the stock menu back, and
`MiSTer.prev` is the firmware you were running before.

### Settings it adds

All optional; the defaults are what most people want.

| Key | Default | What it does |
|---|---|---|
| `classicui` | `0` | Turns the front-end on |
| `classicui_profile` | `0` | Layout size: auto, or force hd/sd/240p |
| `classicui_overscan` | `6` | Percent kept clear of the screen edge, for a CRT |
| `classicui_artdir` | `boxart` | Where cover art lives, under the games folder |
| `classicui_artfetch` | `0` | Download missing cover art over the network |
| `classicui_freeze` | `1` | Hold the game still while the menu is open |

**`classicui_freeze` is worth knowing about.** Holding a game still means asking the
core for a save state, and at least one core cannot survive being asked at a bad
moment: the SNES core dies — black picture, no more save states — if asked during a
demanding scene, and only reloading the core recovers it. That is a bug in the core,
not here, and it happens equally from MiSTer's own Alt-F1 hotkey. If you hit it, set
`classicui_freeze=0`: the game keeps playing behind the menu, which is what already
happens on cores with no save states at all.

---

## If it does not work

Everything the front-end decides, it prints. With `debug=2` in `MiSTer.ini`:

```
grep ClassicUI /tmp/debug.txt
```

- **Stock menu instead of the shelf** — `classicui=1` missing, or the firmware did
  not replace. `grep CLASSICUI=1 /tmp/debug.txt` says which.
- **"No games found"** — your games are not under `games/<System>/`. The log lists
  every folder it looked at and what it found.
- **No cover art** — art goes in `games/<System>/boxart/`, named after the ROM.
  `classicui_artfetch=1` downloads what is missing.

---

## For developers

[README.md](README.md) covers how it is built: the modules, how it draws, the
integration points in the rest of the firmware, and the host test harness that
renders every screen in this guide.
