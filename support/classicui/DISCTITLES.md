# disctitles.generated.txt — where the disc title table came from

`disctitles.generated.txt` beside this file is **generated data, committed on purpose**.
Nothing hand-edits it: `tools/disctitles.py` writes it, and it is the file a release
copies onto a card as

    /media/fat/classicui/disctitles.txt

which is where `chome_titles.cpp` looks for it and binary-searches it in place. It is in
the repository so that regenerating it is never a prerequisite for shipping — building a
release should not depend on a third-party host being up, or on whoever cuts the release
knowing to run a script first. This note exists so that nobody has to re-derive its
provenance from a commit message.

## What it is

    12,761 rows, 455,267 bytes, format "#classicui-disctitles 1"

A magic line, then `KEY<TAB>Title` rows sorted by byte order over keys normalised to
`A-Z0-9`. `SLES01506` is `Metal Gear Solid`. The format, the normalisation and the sort
are one agreement between this file, `tools/disctitles.py` and `chome_titles.cpp`; the
`1` in the magic line is what says so, and changing any of the three means changing it.

Copying it onto a card is optional and always was. With no file there the front-end shows
what it showed before the table existed — for a PlayStation disc, the bare serial — and
says nothing about it in the log.

## How it was built

    python3 support/classicui/tools/disctitles.py --fetch -o disctitles.generated.txt

Cut on **2026-08-06** from the four Redump DATs the `--fetch` path downloads:

| Source | URL | Keys reported |
|---|---|---|
| PlayStation | `https://redump.info/datfile/psx/serial` | 11,854 |
| Mega CD | `https://redump.info/datfile/mcd/serial` | 452 |
| PC Engine CD | `https://redump.info/datfile/pce/serial` | 479 |
| Neo Geo CD | `https://redump.info/datfile/ngcd/serial` | 134 |

Those counts sum to more than the 12,761 rows that came out, and the difference is not a
loss: a key can be counted twice at the source — a region-suffix-stripped form that an
exact serial already claims, or the same serial in two DATs — and `write()` collapses
each of those to one row. See `add()` and `write()` in the generator.

Note the `/serial` suffix on those URLs. It is mandatory: without it Redump generates the
DAT with no `<serial>` elements at all and the script finds nothing. Note the domain too
— Redump moved from `redump.org` to `redump.info` in June 2026 and the old host does not
answer.

## Why it is redistributed here, and on what basis

<https://redump.info/about> states that Redump's data "is considered public domain to be
used however people see fit". That is an explicit statement of intent from the people who
assembled the data, and it is the whole basis for the copy in this repository.

Being plain about what it is not: it is a sentence on a web page, **not a formal licence
grant**. There is no CC0 deed, no SPDX identifier and no licence file behind it, and
nobody here has asked Redump to confirm it applies to a derived table redistributed with
a firmware. This is a stated intent taken at its word, which is a judgement call and is
recorded as one rather than dressed up as a licence.

Two things follow from taking it at its word:

- The table is a **derivative**, not a copy. What survives from the DATs is a serial and
  a cleaned title, with regions and bracket groups stripped, transliterated to ASCII and
  cut at 63 characters. No hashes, no track layouts, no dumper credits — none of the part
  of Redump's work that took the effort.
- If that intent is ever withdrawn or disputed, the replacement is already supported and
  needs no new code: MAME's `hash/psx.xml`, `megacd.xml`, `pcecd.xml` and `neocd.xml` are
  CC0 1.0 — stated in MAME's `COPYING`, in `hash/README.md` and in each file — and
  `tools/disctitles.py` reads them. The cost is coverage: roughly 2,800 PlayStation
  serials against Redump's 11,854. Regenerate from those and commit.
  (`libretro-database` also works and is CC-BY-SA-4.0, which is viral and would attach
  ShareAlike to the result. DuckStation's `gamedb.yaml` is the most convenient shape of
  all and is CC BY-NC-**ND** — deliberately unsupported.)

## Regenerating it

```
python3 support/classicui/tools/disctitles.py --selftest        # the format still agrees
python3 support/classicui/tools/disctitles.py --fetch \
        -o support/classicui/disctitles.generated.txt
```

Then look at the diff, which is half the reason the format is text: a refresh should read
as the few hundred rows that changed rather than as 445 KB of moved bytes. Redump gains
discs every month, so a stale table costs coverage and nothing else — a key it does not
have reads as the serial, exactly as it did before any of this existed.

## Not linked into the binary, and that was a decision

Putting the table inside the `MiSTer` executable was built and then dropped. It works —
`ld -r -b binary` links it the way the PNGs are linked, and the reader can binary-search
a blob as easily as a file — and the case for it was that `GUIDE.md` promises the install
is one file, so a table on the card is a second one that many people will never copy.

The judgement was that this is not worth the complication. The benefits are the one-file
install promise and there being no missing-file case to handle; against that, it puts
~445 KB into a ~1.3 MB binary for every user whether they own a disc drive or not, adds a
linker-name coupling between a filename and a symbol spelled out in C++, and means a
newer table cannot be installed without a toolchain. The card file already works, is
already searched in place with no heap, and can be replaced by anyone with a text editor.
So the table ships as data next to the firmware, not inside it.
