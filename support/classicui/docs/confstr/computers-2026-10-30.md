# Computer cores: picture-related CONF_STR options
Lines are quoted verbatim from each core top file; filtered by keyword (ifdef/comment lines kept for context). Full CONF_STR blocks: computers-full-2026-10-30.md.

## AcornAtom
repo MiSTer-devel/AcornAtom_MiSTer | file AcornAtom.sv | commit b6e868b2acda2e07623ba09004f144aa6b72181e | CONF_STR lines 204-221 (16 non-picture lines omitted)
```
"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
```

## AcornElectron
repo MiSTer-devel/AcornElectron_MiSTer | file AcornElectron.sv | commit ce485f18a045a5ed5f0ac777e3ecf41990c830c7 | CONF_STR lines 205-228 (20 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"OD,Monitor Tape Sound,No,Yes;",
//	"O78,VIDEO,sRGB-interlaced,sRGB-non-interlaced,SVGA-50Hz,SVGA-60Hz;",
```

## AliceMC10
repo MiSTer-devel/AliceMC10_MiSTer | file AliceMC10.sv | commit 4b54eb02a5193f05d87adfe70fa79658f7b4d11b | CONF_STR lines 191-208 (17 non-picture lines omitted)
```
"O1,Aspect ratio,4:3,16:9;",
```

## Altair8800
repo MiSTer-devel/Altair8800_Mister | file Altair8800.sv | commit a142e9f74b73094c13adca8b9b7d476a3f18d449 | CONF_STR lines 238-247 (10 non-picture lines omitted)
```

```

## Amstrad
repo MiSTer-devel/Amstrad_MiSTer | file Amstrad.sv | commit a1ce58e167767617691b7bd8c4e313b523bcc30e | CONF_STR lines 48-102 (45 non-picture lines omitted)
```
"P1,Audio & Video;",
"P1OPQ,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O9A,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"d1P1OR,Vertical Crop,No,Yes;",
"P1OST,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P1OU,Pixel Clock,16MHz,Adaptive;",
"P1O2,CRTC,Type 1,Type 0;",
"P1OBD,Display,Color(GA),Color(ASIC),Green,Amber,Cyan,White;",
"P2,Hardware;",
"P2O6,CPU timings,Original,Fast;",
```

## Amstrad-PCW
repo MiSTer-devel/Amstrad-PCW_MiSTer | file PCW.sv | commit 95106499e843e00785845c81c4d4b94b4d883fcb | CONF_STR lines 55-84 (19 non-picture lines omitted)
```
"P1,Video;",
"P1O56,Screen Color,White,Green,Amber;",
"P1O7,Video System,PAL,NTSC;",
"P1F3,gbp,Load Palette;",
"P1OQR,Fake Colour,None,Loaded palette, PCWPLUS, EGA;",
"P1oAD,CRT H-Sync Adjust,0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1;",
"P1oEH,CRT V-Sync Adjust,0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1;",
"P1OKL,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%, CRT 75%;",
"P1OMN,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"OAC,Joystick Type,None,Kempston,Spectravideo,Cascade,DKTronics;",
```

## Apogee
repo MiSTer-devel/Apogee_MiSTer | file Apogee.sv | commit 02fc303d09ba96af43efee0a04597fbd8d27c40e | CONF_STR lines 59-73 (12 non-picture lines omitted)
```
"O1,Color,On,Off;",
"O67,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O23,Scandoubler Fx,None,CRT 25%,CRT 50%,CRT 75%;",
```

## Apple-I
repo MiSTer-devel/Apple-I_MiSTer | file Apple-I.sv | commit 56159e3bbbb6b450853edf18cdf55ea019daab4a | CONF_STR lines 201-210 (9 non-picture lines omitted)
```
"O1,Scanlines,Off,On;",
```

## Apple-II
repo MiSTer-devel/Apple-II_MiSTer | file Apple-II.sv | commit 170dd2db4d866a7baf49da90ea01fb3bece37d83 | CONF_STR lines 61-166 (85 non-picture lines omitted)
```
"P1,System & BIOS;",
"P1OM,PAL Mode,NTSC,PAL;",
"P1ON,Video Rom,US,LOCAL;",
"P1F1,BIN,Load 8k Video ROM;",
"P2,Audio & Video;",
"P2OJK,Display,Color,B&W,Green,Amber;",
"P2o[54:52],Video Mode,NTSC //e TV,NTSC //e RGB,AppleWin TV,AppleWin RGB,IIgs RGB,Faded Tube TV,Sharp Muted TV,Custom RGB;",
"d0P2FC2,A2P,Custom palette;",
"P2OG,Pixel Clock,Double,Normal;",
"P2OL,Lo-Res Text,Clean,Composite;",
// "P2O[125:124],NTSC Hue,0,3,6,9;",
// "P2O[123:122],NTSC Bright,0,-16,+16,+32;",
"P2O[127:126],TV Saturation,0,-16,+16,+32;",
// "P2O[121:120],NTSC Contrast,0,-16,+16,+32;",
"P2o0,NTSC Vert. Comb Filter,On,Off;",
"P2O9B,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P2OCD,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2OEF,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P3,Hardware;",
"P4,Virtual keyboard;",
"P5,Save States;",
```

## Apple-III
repo MiSTer-devel/Apple-III_MiSTer | file Apple-III.sv | commit 2f4f492b3a59d15ef44f74a4eea70b956c3e260b | CONF_STR lines 47-105 (48 non-picture lines omitted)
```
"h0OF,Text Interlace,Off,On;",
"ODE,Video,RGB,Color Composite,Mono Composite;",
"H1OGH,Display,RGB Monitor,Monitor /// Green,Amber,Color TV;",
"P1,System & ROM;",
"P1OJ,Video Standard,NTSC,PAL;",
"P2,Scaling & Filters;",
"P2O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O[21:20],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P2O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P3,Hardware;",
"P3O[34:32],Slot 1,Block Card,ProFile 1,ProFile 2,Mouse Card,Empty;",
```

## Apple-IIgs
repo MiSTer-devel/Apple-IIgs_MiSTer | file Apple-IIgs.sv | commit 9adbba8622f378253765b7d438b6cbcc4d03fc57 | CONF_STR lines 80-117 (34 non-picture lines omitted)
```
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1,Accelerator;",
"h0P1O[24],Startup Graphics,On,Off;",
"P2,System;",
```

## Apple-Lisa
repo MiSTer-devel/Apple-Lisa_MiSTer | file Apple-Lisa.sv | commit a5d6caf81a1f3cec6a1ec7d80c461670bb636fd5 | CONF_STR lines 167-185 (16 non-picture lines omitted)
```
"O9A,Aspect ratio,4:3,Original,Full Screen,[ARC1];",
"OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"ODE,Screen Color,Paper White,Green CRT,Amber CRT;",
```

## Aquarius
repo MiSTer-devel/Aquarius_MISTer | file Aquarius.sv | commit 46d5c1045f500d89a0202808ca58cdf2234b07a2 | CONF_STR lines 64-83 (17 non-picture lines omitted)
```
"OAB,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## Archie
repo MiSTer-devel/Archie_MiSTer | file Archie.sv | commit f12623a9f2dbb080fdf5f0302ad715b375633c92 | CONF_STR lines 56-60 (5 non-picture lines omitted)
```

```

## Atari800
repo MiSTer-devel/Atari800_MiSTer | file Atari800.sv | commit 617ef945845bc21a99bd80278096c5403c2a9d21 | CONF_STR lines 80-209 (100 non-picture lines omitted)
```
"P1,Drives Carts & Tape;",
"P1O[38],ATX drive timing,1050,810;",
"P1O[67],Invert turbo PWM,Disabled,Enabled;",
"P2,Hardware & OS;",
"P3,Video;",
"P3O[5],Video mode,PAL,NTSC;",
"hCP3O[112:111],NTSC rate Hz,59.92,59.94,60;",
"HCP3O[113],PAL rate Hz,49.86,50;",
"P3O[62:61],Interlace hack,Disabled,Weave,Bob;",
"dBP3O[63],VBXE FX version,1.27,1.26a;",
"dBP3FC2,ACT,VBXE Palette;",
"P3O[114],GTIA xcolor,Off,On;",
"d3P3O[31],NTSC artifacting,No,Yes;",
"d4P3O[55],Artifacting colors,Set 1,Set 2;",
"d4P3O[58],Swap artif. colors,No,Yes;",
"P3O[23:22],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P3O[19:17],Scandoubler FX,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P3O[30:29],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"d0P3O[24],Vertical Crop,Disabled,216p(5x);",
"d0P3O[28:25],Crop Offset,0,2,4,8,10,12,-12,-10,-8,-6,-4,-2;",
"P4,Audio;",
"P4O[4:3],Stereo mix (sys),None,25%,50%,100%;",
"P4O[20],PokeyMax,Off/Mono,Enabled;",
"d6P4O[71],Mono detect,On,Off;",
"d7P4O[86],SID1 filter,8580,6581;",
"d7P4O[87],SID2 filter,8580,6581;",
"d9P4O[103:102],PSG stereo,Polish,Czech,By chip,Mono;",
"P5,Input;",
"P5O[56],Mouse X,Normal,Inverted;",
"P5O[6],Mouse Y,Normal,Inverted;",
```

## AtariST
repo MiSTer-devel/AtariST_MiSTer | file AtariST.sv | commit 98d3495767ab1789a2bfb4c12e9e18d226daaf84 | CONF_STR lines 87-107 (13 non-picture lines omitted)
```
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## BBCMicro
repo MiSTer-devel/BBCMicro_MiSTer | file BBCMicro.sv | commit e786dfad4cab27c65961d924bc2f65178e32a78f | CONF_STR lines 55-78 (20 non-picture lines omitted)
```
"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O23,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"OFG,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"O79,Default video mode,0,1,2,3,4,5,6,7;",
```

## BK0011M
repo MiSTer-devel/BK0011M_MiSTer | file BK0011M.sv | commit e2e77006089994efe3646e7041692df73bbd8f86 | CONF_STR lines 164-193 (26 non-picture lines omitted)
```
"O3,Monochrome,No,Yes;",
"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O78,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"OFG,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## C128
repo MiSTer-devel/C128_MiSTer | file c128.sv | commit 9bc3c5d0db14cea51fc39aea5523209bd8b97ba1 | CONF_STR lines 56-181 (102 non-picture lines omitted)
```
"F2,PRGCRTREUTAP;",
"P1,Audio & Video;",
"HCP1O[106:105],Video Out,Follow 40/80,Side-by-side,VIC-II,VDC;",
"HCP1O[107],40/80 Display,40 col,80 col;",
"P1O[2],Video Standard,PAL,NTSC;",
"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[10:8],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"d1P1O[32],Vertical Crop,No,Yes;",
"P1O[31:30],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P1O[84:82],VIC-II Palette,Colodore,Ultimate,Pepto-PAL,Vice,Vice6569R1,Vice6569R5,Vice8565R2,Lemon64;",
`ifdef VDC_XRAY
`endif
`ifndef REDUCE_VDC_RAM
`endif
"HCP1O[116:115],VDC Palette,Default,Analogue,Monochrome,Composite;",
"HCh2P1O[114:113],VDC Mono Colour,White,Green,Amber,Red;",
"D4P1O[66:64],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"D5P1O[69:67],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"P1FC7,FLT,Load Custom Filters;",
"P2,Hardware;",
"P2FC5,CRT,Boot Cartridge              ;",
"P3,Drives;",
"P4,Input devices;",
"40/80 DISPLAY: 80,40/80 DISPLAY: 40,",    // 9/10
```

## C16
repo MiSTer-devel/C16_MiSTer | file C16.sv | commit 25d9d74366a8ebd0059affa2d1de14f08722ae17 | CONF_STR lines 49-95 (37 non-picture lines omitted)
```
"OJK,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"O78,TV Standard,from Kernal,Force PAL,Force NTSC;",
"H2d1ON,Vertical Crop,No,Yes;",
"h2d1ONO,Vertical Crop,No,270,216;",
"OPQ,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P1,Audio;",
"d5d9P1O[34:32],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"d5d8P1O[37:35],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"d5P1FC7,FLT,Load Custom Filters;",
```

## C64
repo MiSTer-devel/C64_MiSTer | file c64.sv | commit 88354ba0b69c7168f2f7439266f88d936ae64de3 | CONF_STR lines 71-170 (86 non-picture lines omitted)
```
"F1,PRGCRTREUTAP;",
"P1,Audio & Video;",
"P1O[2],Video Standard,PAL,NTSC;",
"P1O[84:82],Palette,Colodore,Ultimate,Pepto-PAL,Vice,Vice6569R1,Vice6569R5,Vice8565R2,Lemon64;",
"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[10:8],Scandoubler Fx,None,HQ2x-320,HQ2x-160,CRT 25%,CRT 50%,CRT 75%;",
"d1P1O[32],Vertical Crop,No,Yes;",
"P1O[31:30],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"D4P1O[66:64],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"D5P1O[69:67],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"P1FC7,FLT,Load Custom Filters;",
"P2,Hardware;",
"P2FC5,CRT,Boot Cartridge       ;",
"P3,Drives;",
```

## CBM-II
repo MiSTer-devel/CBM-II_MiSTer | file CBM-II.sv | commit 0543d01c8abd2e0962aafa44a9d0f47ad28cc722 | CONF_STR lines 46-126 (72 non-picture lines omitted)
```
"P1,Hardware;",
"P2,Audio & Video;",
"H2P2O[12],TV System,PAL,NTSC;",
"P2O[17:16],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O[20:18],Scandoubler Fx,None,HQ2x-320,HQ2x-160,CRT 25%,CRT 50%,CRT 75%;",
"d3P2O[21],Vertical Crop,No,Yes;",
"P2O[23:22],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P2O[28:26],SID Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
"P3,ROM/RAM Configuration;",
```

## Casio_PV-2000
repo MiSTer-devel/Casio_PV-2000_MiSTer | file Casio_PV-2000.sv | commit 130851937c493eb673b422e55c28c7c62c2933c0 | CONF_STR lines 201-216 (12 non-picture lines omitted)
```
"P2,Video Settings;",
"P2O[2],Aspect ratio,Original,Full Screen;",
"P2O[3],Show Border,No,Yes;",
"P2O[5:4],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## CoCo2
repo MiSTer-devel/CoCo2_MiSTer | file CoCo2.sv | commit 2e9f2804905dbb25b3ed3f1649e8d7409ec0231d | CONF_STR lines 222-272 (40 non-picture lines omitted)
```
"OD,Monitor Tape Sound,No,Yes;",
"P1,Video Settings;",
"P1-, -= Video Settings =-;",
"P1OJ,Aspect ratio,Original,Full Screen;",
"P1OFH,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P1O4,Overscan,Hidden,Visible;",
"P1O3,Artifact,Enable,Disable;",
"P1O2,Artifact Phase,Normal,Reverse;",
`ifdef USE_OVERLAY
"OB,Debug display,Off,On;",
`endif
```

## CoCo3
repo MiSTer-devel/CoCo3_MiSTer | file CoCo3.sv | commit ede23da6b07aaf6e060d1b150f6c8e2e86694afc | CONF_STR lines 198-264 (57 non-picture lines omitted)
```
"OH,Monitor Tape Sound,No,Yes;",
"P1,Video Settings;",
"P1-, -= Video Settings =-;",
"P1O89,Aspect ratio,Original,Full Screen,Wide,[ARC1];",
"P1O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P1o56,Artifact Type,MESS,Simple,NONE;",
"P1OI,Artifact Color Set,0,1;",
"P2,Debug Menu;",
"P2F3,BIN,Load COCO Font;",
"P2OT,Display Debug,Off,On;",
```

## ColecoAdam
repo MiSTer-devel/ColecoAdam_MiSTer | file ColecoAdam.sv | commit 337ebcd690b291eed7bd738a5a8c143a45cde788 | CONF_STR lines 220-251 (28 non-picture lines omitted)
```
"O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"O6,Border,No,Yes;",
"OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## Cray-XMP
repo MiSTer-devel/Cray-XMP_MiSTer | file Cray-XMP.sv | commit 6cedd8f5ed179fd386c0f416df74c45aeb590bb2 | CONF_STR lines 50-72 (18 non-picture lines omitted)
```
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[6:5],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"O[9:7],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"O[4:3],Text color,White,Green,Amber,Cyan;",
"O[14],Font,8x8 (15kHz),8x16 (31kHz);",
```

## EDSAC
repo MiSTer-devel/EDSAC_MiSTer | file edsac.sv | commit b076a3c5a42f71ac2e43f67bd52ee03221a373d1 | CONF_STR lines 160-172 (12 non-picture lines omitted)
```
"O1,Aspect Ratio,16:9,4:3;",
```

## Enterprise
repo MiSTer-devel/Enterprise_MiSTer | file Enterprise.sv | commit 20f49b82ccc75a8527f8d4b229f822c8c030b3a3 | CONF_STR lines 214-241 (22 non-picture lines omitted)
```
"P1,Audio  & Video;",
"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[16:15],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"H2d1P1O[28],Vertical Crop,No,Yes;",
"h2d1P1O[29:28],Vertical Crop,No,270,216;",
"P1O[27:26],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## FM-7
repo MiSTer-devel/FM-7_MiSTer | file FM-7_MiSTer.sv | commit fff3440770685ea6b21e6f41e2b1da7d056b3f34 | CONF_STR lines 84-119 (34 non-picture lines omitted)
```
// "1" exists. Changing one re-scans that drive, so it is a disk swap.
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
```

## Galaksija
repo MiSTer-devel/Galaksija_MiSTer | file Galaksija_MiSTer.sv | commit b5dc7bd9a160579625ba502b407385885fc9f27e | CONF_STR lines 177-189 (11 non-picture lines omitted)
```
"O23,Screen Color,White,Green,Amber,Cyan;",
"O1,Aspect Ratio,16:9,4:3;",
```

## Homelab
repo MiSTer-devel/Homelab-MiSTer | file Homelab.sv | commit 9cb5803e17312ff00f66c7f52c65ac9bafad5ba2 | CONF_STR lines 211-231 (16 non-picture lines omitted)
```
`ifndef DISABLE_TURBO_LOADER
`endif
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[2],Screen mode,32 CH,64 CH;",
"O[6:5],Screen Color,White,Green,Amber;",
```

## IQ151
repo MiSTer-devel/IQ151_MiSTer | file IQ151.sv | commit 98fab7b5ecfce4413c1134219ec3a07f122fad38 | CONF_STR lines 69-93 (22 non-picture lines omitted)
```
"OA,Monitor,Std,CPM;",
"O2,Video Module,Video32,Video64;",
"O3,Graphics Module,On,Off;",
```

## Interact
repo MiSTer-devel/Interact_MiSTer | file Interact.sv | commit b0483910624c8e9e0c3e2b805814b55bcd463927 | CONF_STR lines 220-235 (13 non-picture lines omitted)
```
"OAB,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## JR100
repo MiSTer-devel/JR100_MiSTer | file JR100.sv | commit 8718935f78819fc12eee9a021f6e0f626ec8c40b | CONF_STR lines 63-84 (20 non-picture lines omitted)
```
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[8:6],Display color,White,Green,Amber,Cyan,Orange,Blue,Paper,Mint;",
```

## Jupiter
repo MiSTer-devel/Jupiter_MiSTer | file Jupiter.sv | commit 206e0e7cfafc08f6a838a26ce53a6b99a3cf6a1a | CONF_STR lines 57-70 (11 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O23,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## Laser310
repo MiSTer-devel/Laser310_MiSTer | file Laser310.sv | commit 0c3c65de917075d7007db6f96ed8b39c622b87af | CONF_STR lines 200-217 (15 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
//   "O8,64x32 Video,Off,On;",
```

## Lynx48
repo MiSTer-devel/Lynx48_MiSTer | file Lynx48.sv | commit a056c72be2923f38254f87359f563b19252a0b16 | CONF_STR lines 207-225 (16 non-picture lines omitted)
```
"O[5],Aspect ratio,4:3,16:9;",
"O[2:1],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"O[4:3],Machine,Lynx 48K,Lynx 96k,96k Scorpion,Lynx 128k;",
```

## MSX
repo MiSTer-devel/MSX_MiSTer | file MSX.sv | commit 4e585b6a54296b83450bce0af4a9bc427633f909 | CONF_STR lines 43-68 (20 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O23,Scanlines,No,25%,50%,75%;",
"OF,Vertical Crop,No,Yes;",
"OGH,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"O4,Slot1,Empty,MegaSCC+ 1MB;",
"O56,Slot2,Empty,MegaSCC+ 2MB,MegaRAM ASCII-8K 1MB,MegaRAM ASCII-16K 2MB;",
```

## MSX1
repo MiSTer-devel/MSX1_MiSTer | file MSX1.sv | commit 0f3b6067ea0905e7b08ccaaf37692dacb9c67854 | CONF_STR lines 207-235 (22 non-picture lines omitted)
```
"P1,Video settings;",
"P1OG,Video mode,PAL,NTSC;",
"P1O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O3,Border,No,Yes;",
"P1O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P1OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P2,Advanced settings;",
```

## MacIIcx
repo MiSTer-devel/MacSE30_MiSTer | file MacSE30.sv | commit e7bde74128462bcdb7605506ee2c237ecd89f4ca | CONF_STR lines 77-109 (22 non-picture lines omitted)
```
`ifdef MACIICX
`else
`endif
`ifdef SE30_EXT_DRIVE
`endif
`ifndef SE30_NO_CDROM
`endif
"O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[9:8],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
`ifdef MACIICX
`endif
```

## MacLC
repo MiSTer-devel/MacLC_MiSTer | file MacLC.sv | commit eebf4eaab109d0294fc7507cfb931468ce359513 | CONF_STR lines 80-141 (48 non-picture lines omitted)
```
"O78,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"OA,Monitor @Reset,640x480 VGA,512x384 12in;",
"P1,MT32-pi;",
"P1OTV,SoundFont,0,1,2,3,4,5,6,7;",
"P1OMN,Show Info,No,Yes,LCD-On,LCD-Auto;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## MacPlus
repo MiSTer-devel/MacPlus_MiSTer | file MacPlus.sv | commit 47c7ecf138be071855b3a76a9279c7e80e89a0aa | CONF_STR lines 61-101 (39 non-picture lines omitted)
```
"O78,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## MacQuadra800
repo MiSTer-devel/MacQuadra800_MiSTer | file MacQuadra800.sv | commit 884e9a500a4066898e8b7299182dfb15b33730ca | CONF_STR lines 61-135 (56 non-picture lines omitted)
```
"O[4:3],RAM (on reset),32MB,64MB,128MB;",
`ifndef VIDEO_512_OFF
"O[5],Monitor (on reset),13in 640x480,12in 512x384;",
`endif
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[13:12],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
`ifndef ETHERNET_OFF
`endif
"P1,MT32-pi;",
"P1O[41:39],SoundFont,0,1,2,3,4,5,6,7;",
"P1O[43:42],Show Info,No,Yes,LCD-On,LCD-Auto;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## MacSE30
repo MiSTer-devel/MacSE30_MiSTer | file MacSE30.sv | commit e7bde74128462bcdb7605506ee2c237ecd89f4ca | CONF_STR lines 77-109 (22 non-picture lines omitted)
```
`ifdef MACIICX
`else
`endif
`ifdef SE30_EXT_DRIVE
`endif
`ifndef SE30_NO_CDROM
`endif
"O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[9:8],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
`ifdef MACIICX
`endif
```

## Minimig
repo MiSTer-devel/Minimig-AGA_MiSTer | file Minimig.sv | commit bf5d061b414fa0c8cd56f0e6941c586202ebfbaa | CONF_STR lines 21-41 (12 non-picture lines omitted)
```
"J,Red(Fire),Blue,Yellow,Green,RT,LT,Pause;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## MultiComp
repo MiSTer-devel/MultiComp_MiSTer | file MultiComp.sv | commit 9d554828fde6576b0d9992161b63d65cab970bd6 | CONF_STR lines 248-261 (14 non-picture lines omitted)
```

```

## NeXT
repo MiSTer-devel/NeXT_MiSTer | file NeXT.sv | commit 11c4185fc9ce494dc57c2080e34e2f556d0bc20a | CONF_STR lines 65-87 (22 non-picture lines omitted)
```
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
```

## NeXT-Color
repo MiSTer-devel/NeXT-Color_MiSTer | file NeXT-Color.sv | commit b542ed1205cc12a02d66724c7fbca1c67669c97f | CONF_STR lines 66-102 (33 non-picture lines omitted)
```
"NeXT-Color;;",
"O[4:3],Memory,32 MB,64 MB,128 MB,16 MB;",
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[125:123],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",
```

## Neon
repo MiSTer-devel/Neon_MiSTer | file Neon.sv | commit 9e2d9944ea8872d8583c09034fd63cff7e6e10e8 | CONF_STR lines 60-75 (14 non-picture lines omitted)
```
"O[4],Timer IRQ5,50 Hz frame,64 Hz clock;",
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
```

## ORAO
repo MiSTer-devel/Orao_MiSTer | file ORAO.sv | commit 7fb9e9a9291a8389ba92da505648f9c4cc7323c2 | CONF_STR lines 207-218 (10 non-picture lines omitted)
```
"O2,Screen Color,White,Green;",
"O1,Aspect Ratio,4:3,16:9;",
```

## Ondra_SPO186
repo MiSTer-devel/OndraSPO186_MiSTer | file Ondra_SPO186.sv | commit 0150b55034e319f58178345f7ffc6821a823b096 | CONF_STR lines 214-228 (15 non-picture lines omitted)
```

```

## Oric
repo MiSTer-devel/Oric_MiSTer | file Oric.sv | commit c4cf449821b8a3bf2e3538739248ae29d30b8733 | CONF_STR lines 62-118 (50 non-picture lines omitted)
```
"P1,Settings;",
"P1O[55:54],Joystick Adapter,None,PASE,IJK;",
"P1O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[12:10],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P1O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"H1O[4:3],ROM,Pravetz 8D,Oric Atmos,Oric 1;",
"h1O[4:3],ROM,Pravetz 8D,Oric Atmos,Oric 1,Loadable Bios;",
```

## PC88
repo MiSTer-devel/PC88_MiSTer | file PC8801SR.sv | commit 98e2058615198d49bb5ce27937ce3474e40f8daa | CONF_STR lines 53-96 (38 non-picture lines omitted)
```
"O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O34,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"OHJ,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"OOP,Video timing,31kHz,24kHz,15kHz;",
"OS,Color levels,Linear,Real PC-88;",
"OQR,RAM,Fx (SR/FR/FH/FA),Mx (MR/MH/MA/MC),512KB;",
```

## PCXT
repo MiSTer-devel/PCXT_MiSTer | file PCXT.sv | commit ea7cb3401ee6557bf2f902a53863367bb1328c82 | CONF_STR lines 32-185 (126 non-picture lines omitted)
```
localparam CONF_STR_HGC = ((`ENABLE_HGC && `ENABLE_CGA) ? "P1oC,PCXT CGA Graphics,Yes,No;P1oD,PCXT Hercules Graphics,Yes,No;P1O4,PCXT 1st Video,CGA,Hercules;P1-;" : "");
localparam CONF_STR_CRT = (`ENABLE_TANDY_AUDIO ? "" : "P2o23,350-line CRT,Native,480i 15 kHz,240p 15 kHz;");
localparam CONF_STR_MIDI = (`ENABLE_MIDI ? "P3O6,USER I/O,MIDI,COM2;P3-;h3P4,MT32-pi;h3P4-;h3P4OD,Use MT32-pi,Yes,No;h3P4-;h3P4o9,MT32-pi Mode,MT-32,General MIDI;h3P4O34,MT32-pi ROM,MT-32 v1,MT-32 v2,CM-32L,Reserved;h3P4oSU,MT32-pi SoundFont,#0,#1,#2,#3,#4,#5,#6,#7;h3P4-;h3P4r8,Reset Hanging Notes;h3P4-;" : "");
CONF_STR_HALT,
"P1,System & BIOS;",
CONF_STR_HGC,
"P1O7,Boot Splash Screen,Yes,No;",
CONF_STR_ROM,
"P2,Audio & Video;",
CONF_STR_CMS,
CONF_STR_OPL2,
CONF_STR_TANDY_AUDIO,
"d6P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
"d6P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2OT,Border,No,Yes;",
"h5P2O[65],Border CRT offsets,No,Yes;",
"P2O[64],CRTC Frame Stacking,Native,Rebuilt;",
"P2o8,Composite video,Off,On;",
"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
CONF_STR_CRT,
"P3,Hardware;",
CONF_STR_EMS,
CONF_STR_A000,
CONF_STR_MIDI,
CONF_STR_INFO,
```

## PCXT-EGA
repo MiSTer-devel/PCXT-EGA_MiSTer | file PCXT-EGA.sv | commit 4413fc1eaa93ffb9a4e4ed748cd3d8f4adebedb0 | CONF_STR lines 23-207 (158 non-picture lines omitted)
```
`define CONF_STR_SYSTEM (`ENABLE_MIDI ? "PCXT-EGA;UART115200:115200,MIDI;" : "PCXT-EGA;UART115200:115200;")
localparam CONF_STR_MIDI = (`ENABLE_MIDI ? "P3oO,MPU-401,Enabled,Disabled;P3-;P3O6,USER I/O,MIDI,COM2;P3-;h3P4,MT32-pi;h3P4-;h3P4OD,Use MT32-pi,Yes,No;h3P4-;h3P4o9,MT32-pi Mode,MT-32,General MIDI;h3P4O34,MT32-pi ROM,MT-32 v1,MT-32 v2,CM-32L,Reserved;h3P4oSU,MT32-pi SoundFont,#0,#1,#2,#3,#4,#5,#6,#7;h3P4-;h3P4r8,Reset Hanging Notes;h3P4-;" : "");
"h5-,HALTED: no EGA BIOS selected;"
"No EGA BIOS selected\n",
CONF_STR_HALT,
"P1,System & BIOS;",
"P1O7,Boot Splash Screen,Yes,No;",
CONF_STR_ROM,
"P1FC3,ROM,EGA BIOS:;",
"P2,Audio & Video;",
CONF_STR_A220,
CONF_STR_OPL2,
CONF_STR_TANDY,
"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
"P2O[66:64],VSync Width,Auto,1,2,3,4,5,6,7;",
"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
"P2OA,VGA 13h+ CRT,Native 70Hz,TV 60Hz;",
"P2o23,350-line CRT,Native,480i 15 kHz,240p 15 kHz;",
"P3,Hardware;",
"P3oCD,Monitor,5154/ECD,5153/CGA,5151/MDA;",
CONF_STR_EMS,
CONF_STR_UMB,
CONF_STR_MIDI,
CONF_STR_INFO,
```

## PCjr
repo MiSTer-devel/PCjr_MiSTer | file PCjr.sv | commit 84146170a439cb9a22a70e48cb463e1acd061e1c | CONF_STR lines 211-264 (37 non-picture lines omitted)
```
CONF_STR_SYSTEM,
"P1,System & BIOS;",
"P1O7,Boot Splash Screen,Yes,No;",
CONF_STR_ROM,
CONF_STR_CART,
CONF_STR_TAPE,
"P2,Audio & Video;",
CONF_STR_TANDY_AUDIO,
"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2OT,Border,No,Yes;",
"P2o8,Composite video,Off,On;",
"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
"P3,Hardware;",
```

## PDP1
repo MiSTer-devel/PDP1_MiSTer | file PDP1.sv | commit b78c294db565ae22d7c31e57b3742491b25febae | CONF_STR lines 75-95 (18 non-picture lines omitted)
```
"O[1],Aspect Ratio,Original,Wide;",
"O[8],Var. brightness,Yes,No;",
"O[9],CRT wait,No,Yes;",
```

## PET2001
repo MiSTer-devel/PET2001_MiSTer | file PET2001.sv | commit 2d0722a20a3e37b632bf5f34039df1808bb70501 | CONF_STR lines 60-95 (31 non-picture lines omitted)
```
"P1,Hardware;",
"O2,Screen Color,White,Green;",
"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O46,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## PMD85
repo MiSTer-devel/PMD85_MiSTer | file PMD85.sv | commit 2fbfdc7e817bc72827c66c28054bf9fe1e78685c | CONF_STR lines 212-227 (15 non-picture lines omitted)
```
"O12,Video,Green,TV,RGB,ColorACE;",
```

## QL
repo MiSTer-devel/QL_MiSTer | file QL.sv | commit 283caca23b1026249057461795dbaa76933e800e | CONF_STR lines 58-76 (14 non-picture lines omitted)
```
"O2,MDV direction,normal,reverse;",
"O3,Video mode,PAL,NTSC;",
"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O9A,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## RX78
repo MiSTer-devel/RX-78_MiSTer | file RX78.sv | commit 693d8560e91de8cef7ab46f89a5e57a477a6edfd | CONF_STR lines 204-220 (15 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"ODF,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
```

## SAMCoupe
repo MiSTer-devel/SAM-Coupe_MiSTer | file SAMCoupe.sv | commit 96509dd4621f2620d7e879d0889ba061d7f25e1b | CONF_STR lines 48-70 (19 non-picture lines omitted)
```
"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"d1OF,Vertical Crop,No,Yes;",
"OGH,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## SGIIndy
repo MiSTer-devel/SGIIndy_MiSTer | file sgiindy.sv | commit 5b7544d53df0309dad0e8f675e43aac8b79968cc | CONF_STR lines 92-181 (77 non-picture lines omitted)
```
"P1,Machine;",
"P2,Video;",
"P3,Input;",
"P4,Audio;",
"P5,Advanced;",
`ifdef SGIINDY_DEBUG
"P9,Debug;",
`endif
"P2O[10],Display,Graphics,Serial console;",
"P2O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
`ifdef SGIINDY_DEBUG
"P9O[14],Video debug,Off,Raw index;",
`endif
```

## SharpMZ
repo MiSTer-devel/SharpMZ_MiSTer | file SharpMZ.sv | commit 9ef7c1e2b244eadb67e54908834a798edb91371e | CONF_STR lines 78-148 (56 non-picture lines omitted)
```
"SharpMZ;UART9600:19200:38400:4800:2400:1200;",
"P1,Machine;",
"P1O[51],Keyboard Layout,PC,Sharp;",
"P2,Tape;",
"P2O[27:26],Sharp ASCII Name,Off,On Save,On Load,Both;",
"P3,Display;",
"P3O[8:7],Display Type,Default,Mono 80x25,Colour 40x25,Colour 80x25;",
"P3O[43],MZ-800 Border,Off,On;",
"P3O[46:44],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P5,Floppy;",
"P5O[34:33],Floppy Interface,Auto,On,Off;",
"P6,Printer;",
"P6O[50],Printer Charset,Sharp,ASCII;",
"P4,ROM and RAM;",
```

## SordM5
repo MiSTer-devel/SordM5_MiSTer | file sordM5.sv | commit 81b9dafad5286e6fa18fc273222743ba31f28558 | CONF_STR lines 211-235 (22 non-picture lines omitted)
```
"OCD,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"OEF,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"OG,Border,No,Yes;",
```

## Specialist
repo MiSTer-devel/Specialist_MiSTer | file Specialist.sv | commit de9a1330414b5f29b1d1b0bd4651b39916e91b70 | CONF_STR lines 54-70 (14 non-picture lines omitted)
```
"O9A,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O78,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## Sun-2
repo MiSTer-devel/Sun-2_MiSTer | file Sun-2.sv | commit 05c9b7e05cbf09e47d0b06b1c02f3e6a8a064ed7 | CONF_STR lines 80-111 (24 non-picture lines omitted)
```
"O[4:3],Tape volume,1,2,3;",
"P1,Machine;",
"P2,Video;",
"P3,Input;",
"P4,Audio;",
"P1O[12],Colour board,On,Off;",
"P2O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O[6:5],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",
```

## Sun-3
repo MiSTer-devel/Sun-3_MiSTer | file Sun-3.sv | commit e4f3a768dfc849db2e4afe7b3ee94023d66f9f77 | CONF_STR lines 98-139 (32 non-picture lines omitted)
```
"O[4:3],Tape volume,1,2,3;",
"P1,Machine;",
"P2,Video;",
"P3,Input;",
"P4,Audio;",
"P5,Advanced;",
"P1O[12],Colour board,On,Off;",
"H1P1O[19],Colour board type,cg4,GX;",
"P2O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O[6:5],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",
```

## SunSparcStation20
repo MiSTer-devel/SunSparcStation20_MiSTer | file SunSparcStation.sv | commit 990c14cfd4ef7ef75574d12855677b5bb57cc01e | CONF_STR lines 72-102 (25 non-picture lines omitted)
```
"P2,Video;",
"P3,Input;",
"P5,Advanced;",
"P2O[9],Display,Graphics,Serial console;",
"P2O[7:6],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O[30:28],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer,HV-Integer;",
```

## Svi328
repo MiSTer-devel/SVI328_MiSTer | file Svi328.sv | commit cdc4bcf4573f7276a4ba8f81fc157f812e2cfcd9 | CONF_STR lines 237-256 (16 non-picture lines omitted)
```
"O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"O6,Border,No,Yes;",
"OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## TK2000
repo MiSTer-devel/TK2000_MiSTer | file TK2000.sv | commit d6e3a6e7f670de4275c9e3cbab1bdd18b7106a9d | CONF_STR lines 211-228 (13 non-picture lines omitted)
```
//	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
//	"O2,TV Mode,NTSC,PAL;",
//	"O34,Noise,White,Red,Green,Blue;",
"O56,Display,Color,B&W,Green,Amber;",
//	"OB,Color Mode,On,Off;",
```

## TRS-80
repo MiSTer-devel/TRS-80_MiSTer | file TRS-80.sv | commit b44d863ff969d534994cc757f109b5ddf14fabda | CONF_STR lines 203-263 (44 non-picture lines omitted)
```
"P1,Display and MT32 Options;",
"P1-,Display Options;",
"P1O56,Screen Color,White,Green,Amber;",
"P1OE,Video Flicker,Off,On;",
"P1OCD,Overscan,None,Partial,Full;",
"P1OF,Overscan Status Line,Off,On;",
"P1O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"P1o89,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
"P1o57,SoundFont,0,1,2,3,4,5,6,7;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## TSConf
repo MiSTer-devel/TSConf_MiSTer | file TSConf.sv | commit 1f61dc5908c7db98159eb04a163b1950b7071ade | CONF_STR lines 68-112 (37 non-picture lines omitted)
```
"P2,Video;",
"P2o01,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"d0P2o2,Vertical Crop,Disabled,270p(5x);",
"P2o34,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P2OC,Vsync,48.8 Hz,60 Hz;",
"P1,NVRAM;",
"P1oNP,ZX Palette,Default,B.black,Light,Pale,Dark,Grayscale,Custom;",
```

## Tandy1000
repo MiSTer-devel/Tandy1000_MiSTer | file Tandy1000.sv | commit e4a8014910f37643afa220a230ed1b7abf37ff98 | CONF_STR lines 29-314 (266 non-picture lines omitted)
```
localparam CONF_STR_HGC = ((`ENABLE_HGC && `ENABLE_CGA) ? "P1oC,PCXT CGA Graphics,Yes,No;P1oD,PCXT Hercules Graphics,Yes,No;P1O4,PCXT 1st Video,CGA,Hercules;P1-;" : "");
"P1,System & BIOS;",
CONF_STR_HGC,
"P1O7,Boot Splash Screen,Yes,No;",
CONF_STR_ROM,
"P2,Audio & Video;",
CONF_STR_CMS,
CONF_STR_OPL2,
CONF_STR_TANDY_AUDIO,
"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P2OT,Border,No,Yes;",
"P2o8,Composite video,Off,On;",
"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
"P3,Hardware;",
CONF_STR_EMS,
CONF_STR_A000,
```

## TatungEinstein
repo MiSTer-devel/TatungEinstein_MiSTer | file TatungEinstein.sv | commit 88ba332837fe384a4b2f92801444adf5a22bf29c | CONF_STR lines 215-232 (15 non-picture lines omitted)
```
"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"O7,Border,Off,On;",
```

## Ti994a
repo MiSTer-devel/TI-99_4A_MiSTer | file Ti994a.sv | commit 947a8e45cb5d6a32efee1920626dba635435455d | CONF_STR lines 237-295 (52 non-picture lines omitted)
```
"P2,Video Settings;",
"P2OD,Video Mode,NTSC, PAL;",
"P2O79,Scandoubler Fx,None,HQ2x-320,CRT 25%,CRT 50%,CRT 75%;",
"P2O56,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P2O1,Aspect Ratio,Original,Full Screen;",
"D1P2OL,Vertical Crop,No,Yes;",
"P3,Hardware;",
```

## TomyTutor
repo MiSTer-devel/TomyTutor_MiSTer | file TomyTutor.sv | commit e1c3ce2a6d98ddc4307060ac16cba8237c8ecfbb | CONF_STR lines 217-250 (28 non-picture lines omitted)
```
"P1,Tape;",
"P1O[4:3],Tape Status,Never,Always,When Mounted, With Activity;",
"P3,Video Settings;",
"P3O[10:9],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P3O[11],Aspect Ratio,Original,Full Screen;",
"P3O[12],Vertical Crop,Off,On;",
```

## UK101
repo MiSTer-devel/UK101_MiSTer | file UK101.sv | commit 81b09d6131dc3e9cbd88c79db20558547046b887 | CONF_STR lines 61-125 (56 non-picture lines omitted)
```
"O[9:8],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[13:12],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
//"O34,Colours,White on blue,White on black,Green on black,Yellow on black;",
"H4d5O4,Screen resolution,Low,High;",
"h4d5D6O56,Screen resolution,Low,High,Auto;",
"H4H6O[22:21],Monitor,Cegmon,MonUK02,Wemon;",
"h4D9O[23:22],Monitor,Cegmon,Synmon (Basic),Synmon (Disk);",
"h6O[23],Monitor,Cegmon,Synmon;",
```

## UKNC
repo MiSTer-devel/UKNC_MiSTer | file UKNC_MiSTer.sv | commit 7f784c923a5d46c3fc4656a0a18d7a3291e8e9e6 | CONF_STR lines 202-221 (17 non-picture lines omitted)
```
"O[12],Palette ,GRB,RGB;",
"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O[10:8],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
```

## VIC20
repo MiSTer-devel/VIC20_MiSTer | file VIC20.sv | commit 5ac603fdffacace8be0a99e3b4ae0048f34a8d00 | CONF_STR lines 49-93 (35 non-picture lines omitted)
```
"F1,PRGCRTCT?TAP,Load;",
"FS2,CRTROM,Load Megacart;",
"OJK,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
"OCD,Screen center,Both,None,Horz,Vert;",
"OE,TV mode,PAL,NTSC;",
"H2d1ON,Vertical Crop,No,Yes;",
"h2d1ONO,Vertical Crop,No,270,216;",
"OPQ,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"O[34:33],SNAC Autofire,Off,Slow,Fast;",
```

## VT52
repo MiSTer-devel/VT52_MiSTer | file VT52.sv | commit 321e2d3bd68063ebccd5f20349f693dd9d12f05a | CONF_STR lines 213-224 (9 non-picture lines omitted)
```
"O[122:121],Aspect ratio,Original (4:3),Full Screen;",
"O[4:3],Text Color,White,Red,Green,Blue;",
"OE,Font,Terminus 8x16, VT52 rom 8x8;",
```

## Vector-06C
repo MiSTer-devel/Vector-06C_MiSTer | file Vector-06C.sv | commit 402ec64c5a3fbe83c81e2f59375822f95f7e718b | CONF_STR lines 62-86 (20 non-picture lines omitted)
```
"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"O7,Reset Palette,Yes,No;",
"d0OF,Vertical Crop,Disabled,270p(5x);",
"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## VideoBrain
repo MiSTer-devel/VideoBrain_MiSTer | file VideoBrain.sv | commit 3bf69bb2ad78380a34244d2e86ec0863224e36d4 | CONF_STR lines 48-117 (60 non-picture lines omitted)
```
"VideoBrain;UART300:110;",
"P4,Keyboard;",
"P3,Joysticks;",
"P1,Video;",
"P1T[43],Apply Video Output;",
"P1O[122:121],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[125:123],Scale,Normal,V-Integer,HV-Integer-,HV-Integer+,HV-Integer;",
"P5,Printer;",
"P2,System;",
"I,Keyboard entry started,Bad keyboard file,Tape error,Tape rewound,Tape busy,Tape idle,Keyboard entry finished,Keyboard entry aborted,Calibrating sticks,End of tape,Printer no image,Printer scanning,Printer ready,Printer starting,Printer active,Printer flushing,Printer stalled,Printer full,Printer error,Printer read only,Printer bad image,Printer incomplete image,Printer storage error,Printer framing error,Printer overflow,Printer media changed,Printer reset interrupted;",
```

## X68000
repo MiSTer-devel/X68000_MiSTer | file X68000.sv | commit bb4368b835545b4f4837a9c8c1bce5f86bc272af | CONF_STR lines 61-140 (60 non-picture lines omitted)
```
"P1,Floppy Disk;",
"P3,SRAM;",
"P4,Audio & Video;",
"P4ORS,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P4o1,Video Frequency,Original,60fps (31k);",
"P4O[70:69],Video Mode,Stretch,Native;,;",
"P4O[77],15 kHz line doubler,Off,On;",
"P4O45,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"h1P5o9A,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
"h1P5OTV,SoundFont,0,1,2,3,4,5,6,7;",
"o57,Joy1 type,2 Button,2 Turbo,MegaDrive 3,Magical 6,Capcom 6,Double-DPad,CyberStick;",
"oCE,Joy2 type,2 Button,2 Turbo,MegaDrive 3,Magical 6,Capcom 6,Double-DPad,CyberStick;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## ZX-Spectrum
repo MiSTer-devel/ZX-Spectrum_MISTer | file ZX-Spectrum.sv | commit dac94cc4a99b1d858d5bd3cc659c2a17ec4ed291 | CONF_STR lines 63-130 (52 non-picture lines omitted)
```
"P1,Video;",
"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[16:15],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"P1O[38],Narrow Border,No,Yes;",
"D4P1O[58:57],Pentagon Overscan,Off,Centered,Full;",
"H2d1P1O[28],Vertical Crop,No,Yes;",
"h2d1P1O[29:28],Vertical Crop,No,270,216;",
"P1O[27:26],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P3,Audio;",
"D5P3O[50],HQ DC Filter,On,Off;",
"D5P3O[53:51],HQ Voicing,Classic,Flat,Headphones,Warm,TV,Small Speaker;",
"P2,Hardware;",
"D3P2OP,Snow Bug,Disabled,Enabled;",
"P2O[9:8],Video Timings,ULA-48,ULA-128,Pentagon,Scorpion;",
"O[37:36],Keyboard,Normal,Ghosting,Recreated ZX,Recr+Ghosting;",
"O[49],Mouse Wheel,Normal,Inverted;",
```

## ZX81
repo MiSTer-devel/ZX81_MiSTer | file ZX81.sv | commit 432be435e789e7b33e571edca2c2c20c0e3351ce | CONF_STR lines 67-95 (20 non-picture lines omitted)
```
"OLM,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"O6,Video frequency,50Hz,60Hz;",
"O7,Inverse video,Off,On;",
"O5,Black border,Off,On;",
"OCD,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"d1ON,Vertical Crop,Disabled,216p/270p(5x);",
"OOP,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"OK,CHROMA81,Disabled,Enabled;",
"OQR,Keyboard,Normal,Ghosting,Recreated ZX,Recr+Ghosting;",
```

## ZXNext
repo MiSTer-devel/ZXNext_MISTer | file ZXNext.sv | commit 7daebd1983ec4d04f4d22541d2f52d92badbbe5f | CONF_STR lines 58-91 (28 non-picture lines omitted)
```
"O78,Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
"O56,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
"ODE,Narrow Border,No,276,288,320;",
"H2d1OS,Vertical Crop,No,Yes;",
"h2d1OST,Vertical Crop,No,270,216;",
"OQR,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```

## ao486
repo MiSTer-devel/ao486_MiSTer | file ao486.sv | commit 9d888c485bcf2e781824b303588668529a02015e | CONF_STR lines 51-139 (70 non-picture lines omitted)
```
"P1,Audio & Video;",
"P1OMN,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O4,VSync,60Hz,Variable;",
"P1O8,16/24bit mode,BGR,RGB;",
"P1oDE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
"P1oN,Border,No,Yes;",
"P2,Hardware;",
`ifndef DEBUG
`endif
"h3P3o9A,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
"h3P3OTV,SoundFont,0,1,2,3,4,5,6,7;",
"MT32-pi: SoundFont #0,",
"MT32-pi: SoundFont #1,",
"MT32-pi: SoundFont #2,",
"MT32-pi: SoundFont #3,",
"MT32-pi: SoundFont #4,",
"MT32-pi: SoundFont #5,",
"MT32-pi: SoundFont #6,",
"MT32-pi: SoundFont #7,",
```

## eg2000
repo MiSTer-devel/EG2000_MiSTer | file eg2000.sv | commit 0353337727e19244946929ac73757dd08bd65aaa | CONF_STR lines 205-230 (22 non-picture lines omitted)
```
"P1,Video Settings;",
"P1O[12:11],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
"P1O[14:13],Scandoubler,Off,On,HQ2x;",
"P1O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
```
