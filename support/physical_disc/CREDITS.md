# Where physical disc playback came from

**The code in this directory is Anime0t4ku's, not ours.**

`physical_disc.cpp` and `physical_disc.h` are taken from

> **[Anime0t4ku/Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc)**

a fork of Main_MiSTer by **Anime0t4ku**, GPLv3 as this tree is. Playing a real CD on a
MiSTer at all is that author's work. Without it there would be nothing here to build a
front-end on top of.

## What is theirs

The streaming sector reader, taken **whole and deliberately not paraphrased**:

- the ring buffer and the read-ahead worker thread
- the transport selection and the drive-speed cap
- drive-loss detection and re-attach after a USB reset
- the disc-swap machinery
- the udev rule that stops the storage rules probing the drive on insert
- the TOC and pregap handling, including the PlayStation serial and label reads

It is kept as it was written because every part of it is subtle. The one time that
lifecycle was rewritten smaller during this work, it cost two frozen consoles. Local
changes are marked `adaptation` in the `.cpp` and listed at the top of that file, so a
future merge with upstream has nothing to guess at.

## What this tree added

Detection and identification off the drawing thread (`support/classicui/chome_disc.*`,
which runs a helper *process* because every ioctl on `/dev/sr0` serialises behind
whatever the drive is doing); the disc screen in the front-end; the disc's identity for
savestates, memory cards and per-game core options; the title table; artwork; and the
per-core wiring for PC Engine CD, PlayStation, Mega CD and Neo Geo CD.

## If you are packaging or forking this

Keep this file, keep the origin comment at the top of `physical_disc.h`, and keep the
credit visible wherever disc playback is announced to users — release notes included.
GPLv3 obliges the licence and the source; naming the author who did the hard part is
simply correct.

See also `../classicui/ICONS.md` for the icon artwork, which is likewise other people's
work under a licence that requires attribution.
