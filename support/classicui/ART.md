# Cover art: where it lives, how it is found, and why it works this way

Every picture on the shelf comes from one of four places, and which one it came from
decides almost everything else about it — whether it cost a network request, whether it
can be replaced later, and where to look when it is wrong. This is the whole of that,
in one file.

`GUIDE.md` is the player's version and says what to switch on. This one says why.

---

## The one thing to know first

**The folder art is fetched *into* is the same folder art is looked for *in*. There is no
separate cache.**

```
<SD root>/<classicui_artdir>/<System Name>/Named_Boxarts/<ROM name>.png
```

`classicui_artdir` defaults to `boxart`, so on an ordinary card that is
`/media/fat/boxart/Nintendo - Super Nintendo Entertainment System/Named_Boxarts/…`.
`<System Name>` is libretro's long platform name, because that is what the community
art packs already use — drop a pack in and it is found with no conversion.

A cover downloaded on Tuesday is a local file on Wednesday, and on Wednesday nothing
asks the network about it again. That single fact explains most of the design below:
**a fetch is permanent**, so the order things are tried in matters far more than it
looks like it should.

There is nothing under `games/`, nothing under `media/`, and no hidden cache directory.
If you want to start again, delete that folder.

---

## The ladder

`art_next_source()` is the whole order, as one function, so there is one copy of it to
be wrong. Every pass of `art_step()` walks one rung.

| | Rung | What it is |
|---|---|---|
| 1 | **The card** | Anything already on the SD, in any of the layouts below |
| 2 | **ScreenScraper** | Only with `classicui_artfetch=1` *and* an account of your own |
| 3 | **The libretro pack** | Only with `classicui_artfetch=1` |
| 4 | **Nothing** | A plate in the system's colour |

Two things about that order are decisions rather than accidents.

**ScreenScraper above libretro.** It used to be the other way round — spend the free
repository, keep the metered account for what it cannot supply. That is wrong here,
because of the paragraph above: a player who has entered their own credentials has said
which database they want their shelf built from, and filling the card from libretro
first gives them the answer they did not ask for, *permanently*.

**The card above both.** Taken literally, "use ScreenScraper for everything" would mean
re-fetching over covers that are already there — including ones scraped on a PC with
another tool. A cover on the card is an answer, not a gap. So "first priority" applies
to art that has to be **fetched**, and re-scraping over the card is a separate feature
with a switch of its own (`classicui_ss_replace_pack`, below).

---

## Rung 1: everything on the card that counts as "already have it"

Tried in this order. The first hit wins and nothing further is looked at.

### 1. `gamelist.xml`, if the system's ROM folder has one

The EmulationStation metadata file that Skraper, Skyscraper, Batocera, Recalbox,
RetroPie and ES-DE all read or write. If you scraped this card on a PC, it already
describes where every picture is, and reading it is the only thing standing between
that scrape and this shelf. `<image>`, `<thumbnail>` and `<boxart>` are honoured;
paths may be absolute or relative to the ROM folder.

This is deliberately the top of the ladder: it is the art *you chose*.

Turn it off with `classicui_gamelist=0`. Bounded on purpose — it is a user-supplied
file of unknown size on a card we did not write, so oversized files are not opened at
all and a parse error throws the lot away rather than half-using it.

ES-DE is the exception worth knowing: it writes a `gamelist.xml` with no media paths in
it at all. Those are still read, for the `<name>` — which the next step then looks for
on disk.

### 2. The scraper media folders, beside the ROMs

For a card scraped without a gamelist, or whose gamelist names pictures by `<name>`
rather than by path, or names none. Ten locations, relative to the system's games dir:

```
media/box2d      media/Box2D      boxart          images
media/images     media/mixed      media/          media/screenshot
screenshots      media/<rom>-BG
```

Where each comes from: `media/box2d`, `media/screenshot`, `media/images`, `media/mixed`
are Skraper's romset layout, which is what most people who scrape with ScreenScraper end
up with. `media/Box2D` with capitals is Zapatoo's packs — redundant on FAT, not redundant
at all on an ext4 USB drive. `boxart` and `images` are what Batocera's own scraper writes.
`media/<rom>.png` and `media/<rom>-BG.png` are Taki's consolemode packs, which put the
picture straight in `media/` with no type folder.

Box art first, screenshots last: any of them beats a blank plate, but a box is what the
card is shaped for. A background is worse than a screenshot, so it goes after it.

Each is tried by the gamelist `<name>`, by the ROM name, and by the full file name,
because packs disagree about which. One `stat` per system decides whether a `media/`
folder exists at all, so a card that has never been scraped does not pay twelve failed
lookups per game to learn nothing.

### 3. `classicui_artdir` — the libretro layout, and our own downloads

```
<artdir>/<System Name>/Named_Boxarts/<ROM name>.png     ← packs, and everything we fetch
<artdir>/<games dir>/<ROM name>.png                     ← flat per-system folder
<artdir>/<games dir>/<display title>.png                ← for hand-made packs
```

`/media/usb0` is searched as well as the SD root, for both.

### 4. Beside the ROM itself

`<game>.png` next to `<game>.rom`.

### How a ROM becomes a filename

The ROM's own file name, extension removed. For a game inside a zip, the **zip's** name
is used, not the member's. Then the characters that cannot be in a filename —
`& * / : \` < > ? \ |` — become `_`, which is the same substitution libretro and the
scrapers make, so the name we look for is the name a pack ships.

`.png` and `.jpg` are both tried everywhere.

---

## Rung 2: ScreenScraper

Off unless you set `classicui_artfetch=1` **and** give it `classicui_ss_user` /
`classicui_ss_pass`. It is opt-in because it necessarily sends your game names to a
third party.

The query is `systemeid` + `romnom` — the platform's numeric id and the ROM's file name.
Getting the platform id wrong does not fail, it silently matches a *different* platform,
so the mapping is explicit per system rather than derived: `.gbc` is a different
`systemeid` from `.gb`, `.gg` from `.sms`.

**Physical discs are asked differently, and this was measured rather than assumed.** A
disc is asked for by `serialnum`, its product number, sent *without* a `romnom`:

| Key | Result |
|---|---|
| PlayStation, `serialnum` alone | 9 of 9 correct |
| Saturn, `serialnum` alone | 11 of 37 — Sega product numbers are thinly indexed |
| either, serial passed as `romnom` | **wrong** — `SLUS-00594` came back as "Beyblade Burst" |

`romnom` is fuzzy-matched, so a serial in it answers confidently and wrongly, and nothing
downstream can tell. `serialnum` is exact. That last row is why the field exists.

Requests are one at a time, no closer together than **1.2 seconds**, forked as `curl` and
polled without blocking.

---

## Rung 3: the libretro thumbnail pack

```
https://thumbnails.libretro.com/<System Name>/Named_Boxarts/<ROM name>.png
```

Free, no account, no per-day allowance. `classicui_arturl` points elsewhere if you mirror
it. The file lands in `classicui_artdir` at exactly the path rung 1 will find it at next
boot.

---

## The two things the card remembers

Both live in `<SD root>/classicui/`. They say **different** things and no function reads
both — a mark read as a miss writes off a game the database does have; a miss read as a
mark re-asks about a game that already said no.

### `ss-misses.txt` — "they have not got it"

**Why it exists, because it is not visible from the code.** This used to be a flag on an
in-memory slot: correct, and thrown away on every core change — which on this device is
every time you launch a game. So on a 1469-game shelf the front-end asked the database
about every coverless game, learned it had no cover, and asked again on the next boot,
and the next. Nothing was written down.

What that spends is not the daily request budget (200 of 20000 on 2026-08-05) but the
**unmatched** one, a tenth the size, which a filename matcher against regional variants,
hacks and homebrew burns far faster. Spent, the account is refused for the rest of the
day: *"Faite du tri dans vos fichiers roms et repassez demain."* That is what one request
from this machine got, and this store is why it will not get it again.

- Keyed on the **query** — `<systemeid>/<rom name>` — not on the shelf slot, so it
  survives a rescan renumbering the shelf and the disc dialog can share it.
- Written only when the database gave a **verdict**. A quota refusal, a timeout or a
  dead network leaves every game exactly as unasked as it found it. One afternoon at the
  wrong end of the allowance must not blank a library for good.
- A miss stands for **7 days**, not for ever — ScreenScraper is a database people add
  to — and not for one day, which would put us back to re-asking the whole shelf every
  week's worth of boots.
- The **record** is kept for 28 days in all — four windows — saying nothing after the
  first seven except "this was asked about once". That is the fairness rule, not tidiness: a re-ask must queue behind a game
  nobody has ever asked about, and "never asked" cannot be told from "aged out this
  morning" if the evidence was deleted the moment it stopped counting.
- Capped at 4096; full, the oldest goes, which is the one closest to expiring anyway.
- Appended immediately, never saved at shutdown — the firmware is *killed* on every core
  change, so anything held for a tidy save at the end is a save that never happens.

### `art-from-libretro.txt` — "we have something, from somewhere else"

A pack cover is written into the same file a ScreenScraper one would have been, and from
that moment nothing on the card says where it came from. So a player who enters their
credentials *after* the card filled from the pack has no way to ever get the art they
asked for — permanently, because a fetched cover is found at rung 1 for ever.

So the provenance is written down, keyed the same way, with its own explanation at the
top of the file (a stray file on somebody's SD that does not say what it is for is a
support question).

Three things it is not, each a decision:

- **Not a sidecar.** `<cover>.png.from` beside every cover would put a second file in
  every `Named_Boxarts` folder, in the very layout people copy around by hand. One file
  in our own directory is one thing to explain and one thing to delete.
- **Not part of the miss store.** Opposite claims about a game; a single store would
  sooner or later have one read as the other.
- **Not a source of requests.** Marking is free and silent. Nothing is re-asked because
  a mark exists — unless you set `classicui_ss_replace_pack`, which is off, is never
  written on your behalf, and turns each mark into exactly one retry.

**Only covers this front-end downloaded are marked.** A pack you installed by hand is
indistinguishable from a scrape you did yourself, and guessing would mean overwriting art
you chose.

---

## The background sweep

Everything above is demand-driven: the shelf asks for the covers it is about to draw.
That fills in the part of a library you have scrolled through and nothing else, so on a
1469-game card most of the shelf stays coloured plates for ever.

`classicui_artfill` (on by default, but it needs `classicui_artfetch` to do anything)
sweeps the whole library from an idle shelf. It is the lowest-priority thing the module
does: it runs only when there is no decode queued, no download in flight and no retry
waiting, and it **gives back** a download it has already started the moment the shelf
wants the slot.

It walks the same ladder and the same throttles as the demand path — it cannot ask about
a game the miss store has answered for, and it cannot ask faster than the 1.2-second gap.
Two library items per pass, one pass in fifteen frames: about eight items a second, a
1500-game library swept in a few minutes of sitting on the shelf. Capped at 400 fetches
a session, which is not meant to bind — the gap and the miss store are — but to bound
the case with no limit of its own: a device with no network, where every attempt fails
as transport, writes nothing down, and would otherwise retry for ever.

It runs in two phases: games nobody has ever asked about first, and only when a whole
sweep finds none of those do the aged-out re-asks get their turn.

---

## In memory, while the shelf is drawn

- **One decode per frame, on the main thread.** Imlib2 keeps its state in a global
  context, so decoding on a worker would race the wallpaper code in `video.cpp`. One
  228×167 PNG per frame is enough to fill a shelf as fast as you can scroll it, which is
  what "lazy" should feel like.
- **24 MB LRU cache**, keyed by path and decoded size. Decoded size follows the card
  size, so the `hd` profile's larger cards cost more each and the cap holds fewer.
- Requests carry a priority: the distance from the selection. Nearer wins.
- Suspend-point thumbnails use a small separate ring cache. A slot's picture is rewritten
  in place whenever its state is, and `stat` cannot notice — two frames of one game
  compress to a similar size and FAT timestamps are granular to two seconds — so whoever
  rewrites one calls `art_forget()`.

---

## Physical discs

A disc has no filename, so its scan is keyed on the **disc identity** — the same string
that names its savestates. Stored under `classicui/` and read back through the same
thumbnail cache as everything else.

Kept at **144 px** on the long side. ScreenScraper's `support-2D` for a PlayStation game
is a 600×600 RGBA PNG of 417 KB; 144 px is where the title printed round the disc stops
being a smudge and starts being legible on a television, at about 35 KB. The original is
eight times the size of every other picture the card holds for a game, for a sprite drawn
at a fraction of it, on a device whose art cache is 24 MB and whose storage is somebody's
SD card.

The downscale premultiplies by alpha before summing and divides by the sum of the alphas,
not by the pixel count. The transparent pixels in these scans are RGB (0,0,0), so a plain
box average drags every partly-covered pixel toward black — a dirty grey rim round the
disc and a smudged ring round the hub. Invisible until somebody looks at a real disc on a
real TV.

One attempt per disc per session, with one exception: a failure that never reached the
network at all arms a retry, five tries reaching about eight minutes. That is the normal
case on this hardware — a disc is identified within seconds of boot and Wi-Fi has not
associated yet, so the single attempt would otherwise be spent on a name that could not
be resolved, and the art could never arrive while the machine stayed up.

---

## The settings, in one place

| Key | Default | What it does |
|---|---|---|
| `classicui_artdir` | `boxart` | Where art is looked for **and** where downloads land |
| `classicui_artfetch` | `0` | Download missing covers at all |
| `classicui_artfill` | `1` | Sweep the whole library from an idle shelf; needs `artfetch` |
| `classicui_arturl` | libretro thumbnails | Where the pack is fetched from |
| `classicui_gamelist` | `1` | Read `gamelist.xml` |
| `classicui_screenscraper` | `0` | Use ScreenScraper |
| `classicui_ss_user` / `_pass` | — | Your own account |
| `classicui_ss_replace_pack` | `0` | Retry ScreenScraper for covers *we* took from the pack |

---

## When something is wrong

**"It downloaded covers, where did they go?"** `<SD root>/<classicui_artdir>/`. Nowhere
else, ever.

**"It stopped scraping."** Look at `classicui/ss-misses.txt`. A game in there inside the
7-day window is not being asked about, on purpose. `art_ss_miss_count()` and
`art_ss_miss_held()` differ by the records that have aged out of the window but not out
of the store.

**"It got the wrong cover for a disc."** Check whether the serial went out as `serialnum`
or as `romnom`. Only the first is exact; the second fuzzy-matches and is confidently
wrong.

**"I entered my credentials and it still shows pack art."** That is rung 1 doing its job
— the cover is on the card. `classicui_ss_replace_pack=1` retries the ones we fetched
from the pack, and only those.

**"My own scrape is being ignored."** It should win outright. Check the system's ROM
folder really has the `gamelist.xml`, and that `classicui_gamelist` is not 0 — and check
which `MiSTer.ini` section that 0 is in, which is this project's most expensive trap.

**Diagnostics that do not show in a pixel** — the ordering *is* the feature, and a game
with a cover on the card looks identical whether the cover was used or fetched over:
`art_next_source()`, `art_queue_next()`, `art_ss_last_ask()`, and the `art_fill_*`
counters. The harness asserts on these against the same functions the step itself calls.
