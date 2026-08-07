# Keeping Classic Home current with `update_all`

`update_all` runs the official MiSTer `downloader`, and the downloader only touches files
that some *database* claims. Nothing claimed ours, so a card drifted: the disc title table
stayed at whatever version was copied onto it by hand, and the SNAC cores never moved at
all.

This is our own database. Add one section to your card and `update_all` keeps our files up
to date alongside everybody else's, with the same filters, the same log and the same
uninstall.

It does **not** deliver the firmware. That is not an oversight and it is not fixable here —
[see below](#what-it-cannot-do-the-firmware).

---

## Installing it

Pick either of these. They are the same thing; the drop-in file is a copy rather than an
edit, so it is harder to get wrong.

### The drop-in file (recommended)

Create a plain-text file on the card at

    /media/fat/downloader_classichome.ini

containing exactly:

```ini
[Dino-fly/ClassicHome_MiSTer]
db_url = https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/db/db.json.zip
```

The downloader picks up any `downloader_*.ini` beside `downloader.ini` on every run, and
also anything inside a `/media/fat/downloader/` folder — `/media/fat/downloader/classichome.ini`
works just as well. Nothing else on the card changes.

### Or edit `downloader.ini`

Append the same two lines to `/media/fat/downloader.ini`. Put them at the **end of the
file**, after every other section. `downloader.ini` is sectioned, so a stray line lands in
whichever section precedes it and does nothing.

Then run `update_all` as usual, or `Scripts` → `update` for just the download step.

## What arrives

| Path on the card | What it is |
|---|---|
| `classicui/disctitles.txt` | the serial-to-title table the disc screen reads |
| `Scripts/*.sh` | our own scripts |
| `_Console/*.rbf`, `_Computer/*.rbf` | the SNAC-capable core builds |

The cores carry a datecode — `SNES_20260731.rbf` — so they sit **beside** the cores you
already have rather than over them, and you will see two entries for some systems. That is
the point: nothing of yours is replaced. When we publish a newer build the downloader
deletes the older one of *ours* and leaves yours alone, so the duplicates do not
accumulate and `snac_remove_old_cores` stops being necessary.

## What it will never touch

- **`MiSTer.ini`**, or `MiSTer_alt*.ini`. No database is allowed to supply them; your
  settings are yours.
- **`saves/`, `savestates/`, `screenshots/`** — likewise off limits to every database.
- **`games/`** — we ship no ROMs.
- Anything you put on the card yourself. The downloader only removes files it installed.

## What it cannot do: the firmware

**`update_all` will still replace the Classic Home firmware with the official build.** This
database cannot stop that, and a database that tried would not work at all.

`MiSTer` (the firmware) and `menu.rbf` are reserved to the one database whose id is
literally `distribution_mister`, which is the official one. The check is in
`db_entity.py` of Downloader_MiSTer, and it is not a warning — a database that lists either
path throws `DbEntityValidationException` and is discarded **whole**, so every file in it is
skipped, not just the offending one. If we listed the firmware you would get nothing from
us at all.

So after `update_all` your firmware is the official build and ours is in `.MiSTer.old`. The
fix for that lives on the device, not in a database. Until it ships, copy your kept binary
back over `MiSTer` after an update.

## Choosing parts of it

Every file is tagged, so `filter` works the way it does for any other database. The names
match the ones the official database uses for the same folders, so a filter you already
have means the same thing here.

| Tag | Matches |
|---|---|
| `classichome` | everything in this database |
| `essential` | `classicui/` and `Scripts/` — the small files that are not optional |
| `snac` | every core we build |
| `cores` | every `.rbf` |
| `console`, `computer`, `arcade` | by card folder (`consolecores` etc. also work) |
| `disctitles` | the disc title table |
| `snes`, `psx`, `megadrive`, … | one core, by name |

Only the table and the scripts, none of the cores:

```ini
[Dino-fly/ClassicHome_MiSTer]
db_url = https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/db/db.json.zip
filter = [MiSTer] !snac
```

`[MiSTer]` there means "and whatever my global filter says". A filter with any positive
term in it always keeps `essential`, which is why the disc title table survives a filter
like `console`.

## Removing it

```bash
update.sh --uninstall Dino-fly/ClassicHome_MiSTer
```

from a shell on the device. That removes the files it installed, keeps any path another
database also owns, and takes the section out of `downloader.ini` and out of any drop-in
file. Deleting the section by hand instead leaves the files on the card with nothing
tracking them.

---

# For whoever cuts the release

## The generator

    python3 support/classicui/tools/mkdb.py --stage SD-CARD-DB --commit <sha>
    python3 support/classicui/tools/mkdb.py --check db.json.zip
    python3 support/classicui/tools/mkdb.py --selftest

`--stage` is a tree laid out like the card root. Every file in it becomes one entry, hashed
and sized on the spot; tags and the `folders` list are derived from the paths. Read the
docstring at the top of `mkdb.py` before changing any of it — it records which upstream
constant each refusal mirrors.

`--selftest` runs the rules against a table of forbidden paths and then generates a real
database from a temporary stage and takes it apart again. Point it at a checkout of
`MiSTer-devel/Downloader_MiSTer` with `--downloader-src` and it additionally feeds the
result to the real `DbEntity`, which is the only check in there that cannot go stale.

## `db_id` is permanent

    Dino-fly/ClassicHome_MiSTer

It must never change. The downloader keys its local store on it; a new id makes every file
we already installed an orphan that nothing will ever clean up, and installs a second copy
of all of it.

The `owner/name` shape is Coin-OpCollection's, and it earns its keep: the owner segment
makes the id unique across the whole MiSTer ecosystem without anyone having to keep a
registry. It is a namespace and **not** a URL — the name says what the collection is, not
where it is served from, so moving the files to another repository later does not make it
wrong. Do not "fix" it to match wherever the files happen to live.

The section name in `downloader.ini` must match it, case-insensitively.

## Where the files are served from

The `db` branch of the fork, addressed by commit:

    db.json.zip      https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/db/db.json.zip
    base_files_url   https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/<sha>/

Two different rules, on purpose.

The **database** lives at the branch tip, because its URL goes into a user's
`downloader.ini` once and has to keep working for ever.

The **payload** is pinned to a commit sha, because `db.json.zip` and the files it describes
are fetched at different moments — minutes apart on a slow card. A branch-name URL lets a
push in between serve different bytes than the ones we hashed, and the user sees a hash
mismatch on a file that is not corrupt. A commit URL cannot. All three databases already on
a typical card — `distribution_mister`, `jtcores`, `Coin-OpCollection` — pin
`base_files_url` to a 40-character sha for this reason.

Release assets were the alternative and lose twice. An asset URL carries the release tag, so
every publication would need every user to edit their ini again. And an asset name cannot
contain `/`, so the card path could not be derived from the asset name: every file would
need an explicit `url` and a flattened name, and the flattening would have to be undone by
hand every time something needed debugging. The one thing release assets are better at is
keeping binaries out of git history — so keep `db` an **orphan** branch, with no shared
history, and cloning the fork never fetches them.

`base_files_url` is concatenated onto the URL-quoted card path with no separator, so it must
end in `/`. `mkdb.py` refuses one that does not.

## Publishing

1. Assemble the stage. It mirrors the card, so `classicui/disctitles.txt` in the stage is
   `/media/fat/classicui/disctitles.txt` on the card.
2. Commit the stage's contents to the `db` branch. Note the sha.
3. `mkdb.py --stage <stage> --commit <sha> --out db.json` — writes `db.json.zip`.
4. Commit `db.json.zip` to the tip of the `db` branch.

The zip is deflated and holds exactly one member, named after the outer file's stem, which
is what the downloader's own writer produces. Its internal timestamp comes from the
database's `timestamp` rather than from the clock, so rebuilding an unchanged database
produces an unchanged file instead of a pointless commit.

`--format json` or `--format both` if a plain `db.json` is ever wanted; the downloader
dispatches on the URL's suffix and accepts either. `.json.zip` is the default because it is
what the ecosystem serves.

## Do not ship arcade cores at `_Arcade/cores/`

An arcade `.rbf` has to keep the plain name its `.mra` expects, which means our copy and the
official copy are the **same path** — and the official `distribution_mister` claims it.

When two databases declare one path the downloader picks one, installs that one, and reports
the rest as duplicates. It is deterministic, not a fight, but the winner is the database that
claimed the path first and `distribution_mister` is first in every stock `downloader.ini`.
Our arcade core would therefore never arrive, and the only sign of it would be a
`file_duplicated` line in the log. Ship arcade cores in the SD-CARD-ROOT archive, where the
user copies them deliberately, and keep them out of the database.

The datecoded console and computer cores have no such problem: the datecode makes each of
ours a path nobody else claims.

## Tags, and the one trap in them

Tag names are matched after lowercasing and stripping `-` and `_`, against
`!?[a-z0-9]+[-_a-z0-9.]*$`. `mkdb.py` only ever emits lowercase letters and digits.

Files get a name tag; **folders deliberately do not**. A folder carries only the tags for
where it sits, because a filter that excludes one core by name must not take the folder its
siblings live in with it. An earlier version derived folder tags by handing the file tagger a
made-up filename, which put a tag called `x` on every folder in the database — `!x` would have
deleted all of them. The selftest now pins the whole tag dictionary for a fixed stage.

`essential` is load-bearing rather than decorative: the downloader appends it to any filter
that has a positive term, so it is what stops `filter = console` silently dropping the disc
title table.

## Entanglement

Two datecoded builds of one core are two paths, and the downloader has a field for exactly
that case. Every core with a datecode gets `tangle: ["classichome_<name>_core"]`, so a failed
download of the new build leaves the old one in place instead of taking a working core away.
The id is prefixed with `classichome_` because it is ours — two databases publishing SNES
cores are not publishing the same core.

## `pext`

We use none. `path: "pext"` marks a file as eligible for an external drive, and the official
database only puts it on `games/` and `docs/`, never on cores. If this database ever ships
anything under `games/`, that changes.
