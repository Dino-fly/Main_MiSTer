# Phase 4: computer looks - approved plan

Rule (Dinofly, 2026-10-26): computers are grouped by **what each machine actually
output to in its era**, not one class per family and not four fixed buckets. A
machine that shipped with, or was commonly sold with, two kinds of display gets both.

Evidence: every computer core's own picture options, from its CONF_STR
(`confstr/computers-2026-10-30.md`; full blocks in `confstr/computers-full-2026-10-30.md`).
A core's own option beats a scaler trick, and the scaler's gamma LUT cannot mix
channels, so **a phosphor look on a colour picture must come from the core**. Where the
core outputs pure black and white already, a per-channel LUT can tint it.

## Classes

Existing class numbers 2 and 3 keep their meaning; the rest are appended (13+).

| # | class | era display | looks, default first |
|---|---|---|---|
| 2 | TV computer | RF / composite into a television | PAL TV, Composite, S-Video, Sharp |
| 13 | Colour monitor | 15 kHz RGB monitor (1084S, SC1224, CTM644, Microvitec) | Colour Monitor, TV, Sharp |
| 14 | Mono monitor | built-in or bundled phosphor monitor | Green, Amber, Paper White, Sharp |
| 15 | Colour or mono monitor | either, bought separately | Colour Monitor, Green, Amber, White, Sharp |
| 16 | TV or mono monitor | a TV, or a cheap mono monitor | Composite, Green, Amber, Sharp |
| 17 | Hi-res mono | 9-12" paper-white built-in, 60-72 Hz | Paper White (no scanlines), Sharp |
| 3 | 31 kHz monitor | VGA / workstation | VGA, Sharp |

New presets:
- **Colour Monitor**: home-computer RGB monitor - coarser dot pitch than a PVM, softer
  focus, visible scanlines, dot/slot mask.
- **Green / Amber / Paper White**: mono tubes had **no mask** (continuous phosphor), so
  scanlines and a soft glow only. Colour from the core's option where it has one
  (`Display`, `Screen Color`, `Text color`, `VDC Mono Colour`, `Display color`... listed
  side by side, each core skips the names it lacks); a scaler LUT tint only where the
  core already outputs black and white.
- **Composite** on cores with artifact colour (CoCo2/3, Atari 800, Apple II, PCXT
  family) also switches the core's artifacting on; every other look switches it off.

Analog output: only what reaches a tube survives - the phosphor colour (core option)
does, the scanlines/mask/glow do not.

## Machines

On the shelf by default:
| machine | class | why |
|---|---|---|
| Amiga | Colour monitor | 1084S; A520 RF to a TV covered by the TV look |
| Atari ST | Colour monitor | SC1224 colour; SM124 mono needs the hi-res mode (open question) |
| C64 | TV computer | TV, or the 1702 via luma/chroma = S-Video look |
| ZX Spectrum | TV computer | RF only |
| Amstrad CPC | Colour or mono | CTM644 colour or GT65 green; core `Display` has Color(GA)/Color(ASIC)/Green/Amber/Cyan/White |
| MSX | TV computer | MSX1 on a TV; MSX2 SCART RGB covered by S-Video/Sharp (open) |
| PC / DOS | 31 kHz | VGA |
| Apple II | TV or mono | colour TV / composite, Monitor II green, amber; core `Display` |

Other computer cores (a user puts one on the shelf through the systems file; the
class is chosen from the core name so they need not name it):
- **TV computer**: ZX81, VIC20, C16, Atari800, MSX1, Svi328, Oric, CoCo2, AliceMC10,
  Aquarius, Ti994a, Jupiter, Laser310, Lynx48, SordM5, TomyTutor, RX78, Casio_PV-2000,
  VideoBrain, Interact, ColecoAdam, AcornAtom, eg2000, Ondra_SPO186, Vector-06C,
  Galaksija, Homelab, ORAO, UK101, JR100, Apple-I, Apogee, Specialist
  (the last eight were black-and-white on a TV; their core has a Screen Color option,
  so they may belong in TV or mono - open question)
- **Colour monitor**: BBCMicro, AcornElectron, QL, Enterprise, SAMCoupe, TatungEinstein,
  FM-7, PC88, X68000, Apple-IIgs, CoCo3, ZXNext, TSConf, Archie
- **Mono monitor**: PET2001, TRS-80, CBM-II, Amstrad-PCW, VT52, Cray-XMP
- **Colour or mono**: PCXT, Tandy1000, PCjr, PCXT-EGA, C128 (VDC), Apple-III, SharpMZ,
  UKNC, BK0011M
- **TV or mono**: TK2000, PMD85, IQ151
- **Hi-res mono**: MacPlus, MacSE30, Apple-Lisa, NeXT
- **31 kHz**: ao486, MacLC, MacIIcx, MacQuadra800, NeXT-Color, Sun-2, Sun-3,
  SunSparcStation20, SGIIndy, MultiComp
- **No look at all (Sharp only)**: Altair8800 (no video), EDSAC, PDP1 (point-plot CRTs
  no preset can draw)

## Dinofly's calls (2026-10-30)

1. Mono default: **each machine's own bundled colour first** (PET, TRS-80 white; PCW,
   GT65 green...), not one fixed order.
2. Atari ST: **add Paper White** to its list (Colour Monitor, TV, Paper White, Sharp);
   the player picks it for high-res mono software.
3. Black-and-white-on-a-TV machines (Galaksija, Homelab, ORAO, UK101, JR100, Apple-I,
   Apogee, Specialist): **TV or mono**.
4. MSX: **TV plus a Colour Monitor look** (MSX1 on a TV, MSX2 on SCART RGB).

## Review log

| # | look | frame | verdict |
|---|---|---|---|
| 1 | Green Screen | Apple II monitor prompt, core Display=Green (device), sim 1080p | looks good (2026-10-30) |
| 2 | Colour Monitor | Minimig boot text (device), sim 1080p | looks good (2026-10-30) |
| 3 | Amber Screen | Apple II monitor prompt, core Display=Amber (device), sim 1080p | looks good (2026-10-30) |
| 4 | White Screen | Apple II monitor prompt, core Display=B&W (device), sim 1080p | looks good (2026-10-30) |
| 5 | Paper White | Apple II B&W frame standing in for a Mac (no Mac/ST frame yet), sim 1080p | looks good (2026-10-30); a real Mac/ST frame is still owed |
