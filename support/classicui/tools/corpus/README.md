# Native-frame corpus

Frames written by the firmware's own `screenshot` command: the core's native,
pre-scaler frame, with the ClassicUI look set to None. Captured 2026-10-26 on
firmware md5 93a9802cebd3f4b27dbfa074bc84a858 (classic-ui b45a0e45). Each file was
verified single-frame where noted; colour counts are exact distinct RGB values.

Columns: file | core | game | how reached | colours | native size | core options.

GB frames are not greys: the core's own Custom Palette is on, so each game shows its
own four colours (Zelda the DMG ramp, Adventure Island the green/teal palette).

| file | core | game | how reached | colours | size | core options |
|---|---|---|---|---|---|---|
| gb/zelda-slot1.png | Gameboy | Legend of Zelda - Link's Awakening (USA, Europe) (Rev B) | shelf, savestate slot 1 (ss_load 0), Link in the house | 4 | 160x144 | Screen Shadow No, Frame blend Off, Custom Palette On |
| gb/advisland-title.png | Gameboy | Adventure Island II - Aliens in Paradise | shelf, savestate slot 2 (ss_load 1), title scene | 4 | 160x144 | as above |
| gb/advisland-play.png | Gameboy | Adventure Island II - Aliens in Paradise | shelf, savestate slot 4 (ss_load 3), play scene | 4 | 160x144 | as above |
| gbc/oracle-logo.png | Gameboy (GBC) | The Legend of Zelda - Oracle of Seasons | GameboyColor.mgl, OSD Load, savestate slot 4, scene ahead of intro | 21 | 160x144 | see note 2 |
| gbc/oracle-intro.png | Gameboy (GBC) | Oracle of Seasons | same | 10 | 160x144 | see note 2 |
| gba/minish-text.png | GBA | Zelda - The Minish Cap | shelf launch, slot 1 | 89 | 240x160 | Modify Colors Off, Flickerblend Off, Scale Normal (read on Dragon Ball Z; None look restores these) |
| gba/lttp-play.png | GBA | Zelda - A Link to the Past & Four Swords | shelf, slot 1 | 48 | 240x160 | as above |
| gg/sonicblast-title.png | SMS (Game Gear slot) | Sonic Blast | shelf, slot 4 (ss_load 3), title | 35 | 160x144 | core offers no video options; Scale untouched |
| gg/sonicblast-play.png | SMS (Game Gear slot) | Sonic Blast | same, after state load | 25 | 160x144 | same |
| lynx/basketbrawl-credits.png | AtariLynx | Basketbrawl | shelf, slot 4, credits screen (large text, flat brick) | 14 | 160x102 | Flickerblend Off, Scale Normal |
| lynx/basketbrawl-demo.png | AtariLynx | Basketbrawl | shelf, slot 4, demo gameplay | 13 | 160x102 | same |
| ws/title-logo.png | WonderSwan | Makaimura for WonderSwan | shelf, slot 4, title (also the only scene with text: PRESS START) | 4 | 224x144 | Flickerblend Off, Scale Normal |
| wsc/ff-title.png | WonderSwan (Color) | Final Fantasy (Japan) .wsc | shelf, slot 4; frame taken from a 1.2 s screenshot sequence | 18 | 224x144 | see note 2 |
| wsc/ff-text.png | WonderSwan (Color) | Final Fantasy | same sequence, intro text | 14 | 224x144 | see note 2 |
| wsc/ff-crystals.png | WonderSwan (Color) | Final Fantasy | same sequence, crystals (flat black, fine sprites) | 53 | 224x144 | see note 2 |
| ngpc/bios-logo.png | NeoGeoPocket | none: BIOS NEOGEO POCKET logo only | KOF R-1 launched from shelf, screenshot sequence | 3 | 160x151 | NGP core has no flicker option; Scale Normal |
| snes/alttp-house.png | SNES | Legend of Zelda - A Link to the Past (USA) | shelf, savestate slot 1 (ss_load 0), Link's house (sprites, fine detail) | 52 | 512x224 (256 wide, every column doubled) | Pseudo Transparency Blend (core default, on), Scale Normal, Scandoubler Fx None, Aspect Original |
| snes/actraiser-title.png | SNES | ActRaiser (USA) | shelf, savestate slot 2 (ss_load 1), title (text) | 28 | 512x224 (256 wide, doubled) | as above |
| nes/castlevania-play.png | NES | Castlevania (USA) (Rev A) | shelf, slot 1, stage 1 courtyard; animated, frame from a 0.5 s sequence | 15 | 256x224 | Palette Kitrinx, Overscan Normal, Mask Edges Off, Extra Sprites Off, Scandoubler Fx None, Video Dijitter Enabled (core default) |
| nes/zelda-play.png | NES | Legend of Zelda, The (USA) (Rev A) | shelf, slot 1, overworld (sprites) | 14 | 256x224 | as above |
| nes/zelda-title.png | NES | Legend of Zelda, The | shelf, slot 3 (ss_load 2), title, PUSH START BUTTON (text); sword glint animates | 12 | 256x224 | as above |
| genesis/sonic2-title.png | MegaDrive | Sonic The Hedgehog 2 (World) (Rev A) | shelf, cold boot, Start once (text) | 42 | 320x224 | Composite Blend Off, Scandoubler Fx None, Scale Normal, 320x224 Aspect Original, Border No, CRAM Dots Off |
| genesis/sonic2-play.png | MegaDrive | Sonic The Hedgehog 2 | same, Start twice, zone with fine metalwork | 33 | 320x224 | as above |
| psx/tombraider-menu.png | PSX | Tomb Raider (USA) (Rev 6) | shelf, savestate slot 1, title menu (text) | 3158 | 405x240 (368-wide mode, 7.67 MHz dot clock, with blanking) | Dithering On (core default), Texture Filter Off, 480i to 480p Hack Off, Sync 480i for HDMI Off, Vertical/Horizontal Crop Off, DDR3 FB Color 16, Scale Normal |
| psx/re-hall.png | PSX | Resident Evil (USA) | shelf, savestate slot 2 (ss_load 1), mansion hall (pre-rendered background, 240p) | 1344 | 352x240 (320-wide mode with blanking) | as above |
| arcade/rastan-items-text.png | arcade (JTRASTAN, MRA) | Rastan | shelf, attract mode, item list (text, sprites) | 19 | 320x240 | CRT scale enable Off, Scandoubler Fx None, Scale Normal; no saved .CFG (defaults) |
| arcade/rastan-demo.png | arcade (JTRASTAN) | Rastan | shelf, attract-mode demo gameplay (sprites, parallax) | 61 | 320x240 | as above |

Notes:
1. Adventure Island frames come from savestates (no cold boot); the GB palette is the core's per-game Custom Palette.
2. GBC and WSC: the core options were not read individually. The None look was applied through the Display screen, which restores the options a look set (log: "put back N core option(s)"); WSC runs the same WonderSwan core where Flickerblend was read Off.
3. Sizes: the NGP frame is 160x151, not 160x152 (screenshot trims a line). Lynx 160x102, WS/WSC 224x144 as expected.
4. No Game Boy Color or WS mono frame with both text and fine detail exists for a few systems; ws has one frame only (the Makaimura title is static text plus logo; no second scene was reachable by keyboard before a boot-ROM wait).
5. Not captured: NGP (mono or colour) gameplay. The four .zip carts load slowly and the core stayed on the white boot screen on repeated tries; no savestates exist for NGP. NGP mono has no separate core or cartridges (extensions .ngp/.ngc only).
6. wsc/ and ws/ frames that changed between two screenshots (animated) were not accepted; accepted frames were taken from a sequence.
7. Console/arcade rows added 2026-10-30 on firmware md5 c1a58ec7020a305f794757a3a793ef21 (classicui-crt-renders, code identical to classic-ui 6a711afa). The Consoles class has no None look; "Sharp" (no filtering, no core options, none scanned or set) was used instead, and the screenshot is pre-scaler regardless. Core options were decoded from the core's CONF_STR (firmware log, "get cfgstring") against the saved config/<core>.CFG status words, not read off the OSD; the arcade core has no .CFG so shows defaults.
8. Native size is the frame as the firmware writes it. SNES is always 512 wide (every column doubled in these frames; a hi-res game would use the full width). PSX width follows the game's mode (405 and 352 here, including blanking), so it varies per title; both frames are 240 lines (240p, not 480i). Genesis here is 320x224 (a 256-wide game was not captured). NES 256x224.
9. Not captured: TurboGrafx-16 (no games on the card; games/TGFX16 is empty, TGFX16-CD has only the BIOS). Core default options that are not neutral for a CRT corpus: SNES Pseudo Transparency Blend, PSX Dithering On, NES Video Dijitter Enabled; none were changed by hand.
