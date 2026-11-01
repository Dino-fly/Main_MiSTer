## AcornAtom (AcornAtom_MiSTer/AcornAtom.sv)
parameter CONF_STR = {
	"AcornAtom;;",
	"-;",
	"S,VHD;",
	"-;",
	"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
	"-;",
	"O45,Audio,Atom,SID,TAPE,off;",
	"O67,Keyboard,UK,US,orig,game;",
	"O8,character set,original,xtra;",
	"O9,Background,Black,Dark;",
	"OA,Mode,Atom,BBC;",
	"-;",
	"R0,Reset;",
	"JA,Fire;",
	"V,v",`BUILD_DATE
};

## AcornElectron (AcornElectron_MiSTer/AcornElectron.sv)
parameter CONF_STR = {
	"AcornElectron;;",
	"-;",
	"S0,VHD;",
//	"OC,Autostart,Yes,No;",
	"-;",
	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
	"OA,Swap Joysticks,No,Yes;",
	"-;",
	"OC,Tape Input,File,ADC;",
	"H0F2,UEF,Load Cassette;",
	"H0TF,Stop & Rewind;",
	"OD,Monitor Tape Sound,No,Yes;",
	"OE,Show Tape Status,Yes,No;",
	"-;",
//	"O4,Model,B(MOS6502),Master(R65SC12);",
//	"O56,Co-Processor,None,MOS65C02;",
//	"O78,VIDEO,sRGB-interlaced,sRGB-non-interlaced,SVGA-50Hz,SVGA-60Hz;",
	"-;",
	"R0,Reset;",
	"JA,Fire;",
	"V,v",`BUILD_DATE
};

## AliceMC10 (AliceMC10_MiSTer/AliceMC10.sv)
localparam CONF_STR = {
	"AliceMC10;;",
	"-;",
	"O1,Aspect ratio,4:3,16:9;",
	"OC,16k expansion,Off,On;",
	"-;",
	"OF,Tape Input,File,ADC;",
	"D0F1,c10,Tape Load;",
	"D0TA,Tape Play/Pause;",
	"D0TB,Tape Rewind;",
	"OD,Show tape stream,Off,On;",
	"OE,Enable tape audio,Off,On;",
	"-;",
	"T0,Reset;",
	"R0,Reset and close OSD;",
	"J1,Button;",
	"V,v",`BUILD_DATE
};

## Altair8800 (Altair8800_Mister/Altair8800.sv)
localparam CONF_STR = {
	"Altair8800;;",
	"-;",
    "F0,ROM,Load Program;",  // S0 enables directory browsing for files with .ROM extension
	"OA,Enable TurnMon,No,Yes;",
    "OC,Serial Port,Console Port,User IO Port;",  // New serial port selection option
	"T0,Reset;",
	"-;",
	"V,v2.0.",`BUILD_DATE
};

## Amstrad (Amstrad_MiSTer/Amstrad.sv)
localparam CONF_STR = {
	"Amstrad;;",
	"S0,DSK,Mount A:;",
	"S1,DSK,Mount B:;",
	"-;",
	"FC0,ROM,Load Main ROM;",
	"FC3,E??,Load expansion;",
	"-;",
	"F4,CDT,Load tape;",
	"F5,ROM,Load Dandanator ROM;",
	"F6,SNA,Load snapshot;",
	"F7,E??,Load CPC464 ROM;",
	"OK,Tape sound,Disabled,Enabled;",
	"-;",
	"O[62:61],SNAC,Off,Player 1,Player 2;",
	"-;",
	"OI,Joysticks swap,No,Yes;",
	"-;",
	
	"P1,Audio & Video;",
	"P1-;",
	"P1OPQ,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O9A,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"P1-;",
	"d1P1OR,Vertical Crop,No,Yes;",
	"P1OST,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P1OU,Pixel Clock,16MHz,Adaptive;",
	"P1-;",
	"P1O2,CRTC,Type 1,Type 0;",
	"P1OBD,Display,Color(GA),Color(ASIC),Green,Amber,Cyan,White;",
	"P1-;",
	"P1O78,Stereo mix,none,25%,50%,100%;",
	"P1OO,Playcity,Disabled,Enabled;",

	"P2,Hardware;",
	"P2-;",
	"P2OJ,Mouse,Enabled,Disabled;",
	"P2OL,AMX Mouse,Disabled,Joystick 1;",
	"P2OM,Right Shift,Backslash,Shift;",
	"P2ON,Keypad,Numbers,Symbols;",
	"P2-;",
	"P2OEF,Multiface 2,Enabled,Hidden,Disabled;",
	"P2O6,CPU timings,Original,Fast;",
	"P2OGH,FDC,Original,Fast,Disabled;",
	"P2-;",
	"P2oAC,Distributor,Amstrad,Orion,Schneider,Awa,Solavox,Saisho,Triumph,Isp;",
	"P2O[5:4],Model,CPC 6128,CPC 664,CPC 464;",
	"P2OV,Tape progressbar,Off,On;",

	"-;",
	"R0,Reset & apply model;",
	"R[32],Reset & Detach Cartridge;",
	"J,Fire 1,Fire 2,Fire 3;",
	"V,v",`BUILD_DATE
};

## Amstrad-PCW (Amstrad-PCW_MiSTer/PCW.sv)
localparam CONF_STR = {
	"Amstrad PCW;;",
	"S0,DSK,Mount A:;",
	"S1,DSK,Mount B:;",
	"-;",
	"O4,System Model,8256/8512,9256/9512+;",
	"OFG,Memory Size,256K,512K,1MB,2MB;",
	"O89,Clockspeed (MHz),4.00(1x),8.00(2x),16.00(4x),32.00(x8);",

	"-;",
	"P1,Video;",
	"P1-;",	
	"P1O56,Screen Color,White,Green,Amber;",
	"P1O7,Video System,PAL,NTSC;",
	"P1F3,gbp,Load Palette;",
	"P1OQR,Fake Colour,None,Loaded palette, PCWPLUS, EGA;",
	"P1oAD,CRT H-Sync Adjust,0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1;",
	"P1oEH,CRT V-Sync Adjust,0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1;",
	"P1OKL,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%, CRT 75%;",
	"P1OMN,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"OAC,Joystick Type,None,Kempston,Spectravideo,Cascade,DKTronics;",
	"ODE,Mouse Type,None,AMX,Kempston,Keymouse;",
	"OH,DKTronics I/F,Disabled,Enabled;",
	"-;",
	"R0,Reset;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE
};

## Apogee (Apogee_MiSTer/Apogee.sv)
localparam CONF_STR = 
{
	"APOGEE;;",
	"-;",
	"F,RKARKRGAM;",
	"O5,Autostart,Yes,No;",
	"-;",
	"O1,Color,On,Off;",
	"O67,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O23,Scandoubler Fx,None,CRT 25%,CRT 50%,CRT 75%;",
	"-;",
	"O4,CPU speed,Normal,Double;",
	"R0,Reset;",
	"V,v",`BUILD_DATE
};

## Apple-I (Apple-I_MiSTer/Apple-I.sv)
localparam CONF_STR = {
	"APPLE-I;;",
	"F,TXT,Load Ascii;",
//	"F,CAS,Load Cassette;",
	"O1,Scanlines,Off,On;",
	"O2,RAM Size,8K,32K;",
	"-;",
	"R0,Reset;",
	"V,v",`BUILD_DATE
};

## Apple-II (Apple-II_MiSTer/Apple-II.sv)
parameter CONF_STR = {
	"Apple-II;SS3E000000:200000,UART19200:9600:4800:2400:1200:300;",
	"-;",
	"S0,NIBDSKDO PO WOZ,Drive 1;",
	"S2,NIBDSKDO PO WOZ,Drive 2;",
	"OQR,Write Protect,None,Drive 1,Drive 2,Drive 1 & 2;",
	"-;",
	"S1,HDV;",
	"P0-;",
	"P1,System & BIOS;",
	"P1-;",
	"P1O5,CPU,65C02,6502;",
	"P1OM,PAL Mode,NTSC,PAL;",
	"P1oC,Pause when OSD is open,Off,On;",
	"P1-;",
	"P1ON,Video Rom,US,LOCAL;",
	"P1F1,BIN,Load 8k Video ROM;", 
	"P1-;",
	"P2,Audio & Video;",
	"P2-;",
	"P2O78,Stereo mix,none,25%,50%,100%;",
	"P2-;",
	"P2OJK,Display,Color,B&W,Green,Amber;",
	// Video Mode (P2o[54:52]): consolidated 8-mode video option replacing
	// Display Type + Color TV Preset + RGB palette; decode in video_mode_decode.v.
	"P2o[54:52],Video Mode,NTSC //e TV,NTSC //e RGB,AppleWin TV,AppleWin RGB,IIgs RGB,Faded Tube TV,Sharp Muted TV,Custom RGB;",
	// d0 prefix = line hidden unless menumask bit 0 is set; the core sets
	// bit 0 only in Custom RGB mode (video_mode == 7).
	"d0P2FC2,A2P,Custom palette;",
	"P2-;",
	"P2OG,Pixel Clock,Double,Normal;",
	"P2OL,Lo-Res Text,Clean,Composite;",
	// NTSC fine-tune: only TV Saturation is exposed; Hue/Bright/Contrast are
	// commented out below for easy restore (their bits then read 0 = no
	// offset in video_pipeline). Comb Filter stays permanent at bit 32.
	// "P2O[125:124],NTSC Hue,0,3,6,9;",
	// "P2O[123:122],NTSC Bright,0,-16,+16,+32;",
	"P2O[127:126],TV Saturation,0,-16,+16,+32;",
	// "P2O[121:120],NTSC Contrast,0,-16,+16,+32;",
	"P2o0,NTSC Vert. Comb Filter,On,Off;",
	"P2-;",
	"P2O9B,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
	"P2OCD,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P2OEF,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2-;",
	"P3,Hardware;",
	"P3-;",	
	"P3OST,Slot 4,Mocking board,Mouse,Z80 Softcard,Empty;",
	"P3OUV,Slot 5,Mouse,Mocking board,256K Saturn,Empty;",
	"P3-;",
	"P3O6,Analog X/Y,Normal,Swapped;",
	"P3OHI,Paddle as analog,No,X,Y;",
	"P3o46,Analog X center,0,-16,-32,-48,-64,-72,+32,+48;",
	"P3o7,Joystick mode,Absolute,Relative;",
	"P3-;",	
	"P3o3,Disk LED overlay,Yes,No;",
	// Disk drive sound: taps on the WOZ flux drives (step/track-0/IO +
	// motor); boot default On (2x) via the one-shot write in status_in.
	"P3o12,Disk drive sound,Off,On (2x),On (4x);",
	"P3-;",
	"P4,Virtual keyboard;",
	"P4-;",
	"P4oA,Virtual keyboard,Off,On;",
	"P4o89,Keypad visibility,100%,75%,50%,25%;",
	"P4-;",
	"P4oB,Joystick to keys,Off,On;",
	"P4F3,A2K,Load Joy Map;",
	"P4-;",
	"P5,Save States;",
	"P5-;",
	"d8P5oD,Savestates to SDCard,Off,On;",
	"P5oEF,Savestate Slot,1,2,3,4;",
	"P5rG,Save state (F6);",
	"P5rH,Restore state (F5);",
	"P5-;",
	"-;",
	"R0,Cold Reset;",
	// Gamepad layout (ABXYLR + Start/Select), one slot per hps_io bus bit.
	// jn names map to bus bits 4-11 (D-pad is auto on bits 0-3): verify the
	// exact bit positions on hardware (see WOZ_MERGE.md / joy-to-key notes).
	"JA,Btn1|A,Btn2|B,Start,Select,L,R,X,Y;",
	"jn,Btn1|A,Btn2|B,Start,Select,L,R,X,Y;",
	"jp,Y|P,B;",
	// Save-state info strings (index 0 is the "I" marker): 1-4 active slot,
	// 5-12 "State N saved/loaded" (5 + 2*slot + load), 13 Saturn, 14 invalid,
	// 15 incompatible, 16 "Savestates to SDCard" toggle Off. Driven on
	// hps_io.info by rtl/savestates/savestate_ui.sv.
	"I,",
	"Active slot 1,",
	"Active slot 2,",
	"Active slot 3,",
	"Active slot 4,",
	"State 1 saved,",
	"State 1 loaded,",
	"State 2 saved,",
	"State 2 loaded,",
	"State 3 saved,",
	"State 3 loaded,",
	"State 4 saved,",
	"State 4 loaded,",
	"Save states unavailable (Saturn),",
	"Invalid or empty state,",
	"Incompatible state format or CPU,",
	"Save states to SDCard is Off;",
	"V,v",`BUILD_DATE
};

## Apple-III (Apple-III_MiSTer/Apple-III.sv)
localparam CONF_STR = {
	"Apple-III;UART19200:9600:4800:2400:1200:600:300:150:110:75:50;",
	"-;",
	"S0,WOZDSKDO PO NIB2MG,Mount Drive 1;",
	"S1,WOZDSKDO PO NIB2MG,Mount Drive 2;",
	"S2,WOZDSKDO PO NIB2MG,Mount Drive 3;",
	"S3,WOZDSKDO PO NIB2MG,Mount Drive 4;",
	"O6,Write Protect 1,Off,On;",
	"O7,Write Protect 2,Off,On;",
	"OB,Write Protect 3,Off,On;",
	"OC,Write Protect 4,Off,On;",
	"-;",
	"h5S6,PO HDV2MG,ProFile Disk 1;",
	"h6S7,PO HDV2MG,ProFile Disk 2;",
	"h4S4,PO HDV2MG,Block Disk 1;",
	"h4S5,PO HDV2MG,Block Disk 2;",
	"-;",
	"O8,Model,Apple ///,/// Plus;",
	"h0OF,Text Interlace,Off,On;",
	"ODE,Video,RGB,Color Composite,Mono Composite;",
	"H1OGH,Display,RGB Monitor,Monitor /// Green,Amber,Color TV;",
	"-;",
	"P1,System & ROM;",
	"P1-;",
	"h7P1O[31:30],Memory,256K,128K,512K;",
	"H7P1O[31:30],Memory,256K,128K;",
	"P1OJ,Video Standard,NTSC,PAL;",
	"P1-;",
	"P1O[26],Boot ROM,Apple,soshdboot;",
	"P1F2,ROMBIN,Load Boot ROM;",
	"P2,Scaling & Filters;",
	"P2-;",
	"P2O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P2O[21:20],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2-;",
	"P2O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"h2P2O[22],Deinterlacing,Weave,Bob;",
	"P3,Hardware;",
	"P3-;",
	"P3O[34:32],Slot 1,Block Card,ProFile 1,ProFile 2,Mouse Card,Empty;",
	"P3O[37:35],Slot 2,Empty,Block Card,ProFile 1,ProFile 2,Mouse Card;",
	"P3O[40:38],Slot 3,Empty,Block Card,ProFile 1,ProFile 2,Mouse Card;",
	"P3O[43:41],Slot 4,ProFile 1,ProFile 2,Mouse Card,Empty,Block Card;",
	"h3P3O[25:24],Mouse Speed,Normal,Fast,Faster,Fastest;",
	"P3-;",
	"P3OA,Joystick 1 on,Port B,Port A;",
	"P3O9,Serial CTS,Always ready,Host RTS;",
	"P3O[29],Serial DSR,Always ready,Host DTR;",
	"P3O[28:27],Serial DCD,Always on,Host DTR,Off;",
	"-;",
	"R0,Reset;",
	"J,Button,Switch;",
	"jn,A,B;",
	"jp,Y,B;",
	"v,0;", // [optional] config version 0-99. 
	        // If CONF_STR options are changed in incompatible way, then change version number too,
			  // so all options will get default values on first start.
	"V,v",`BUILD_DATE 
};

## Apple-IIgs (Apple-IIgs_MiSTer/Apple-IIgs.sv)
localparam CONF_STR = {
	"Apple-IIgs;UART19200:9600:4800:2400:1200:300;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"-;",
	"S0,HDVPO 2MG;",
	"S1,HDVPO 2MG;",
	"S2,WOZPO 2MG,WOZ 3.5;",
	"S3,WOZDSKDO PO NIB2MG,WOZ 5.25;",
	"-;",
	"O[14:12],CPU Speed,2.8 MHz (Std),3.6 MHz,4.8 MHz,7.2 MHz,14.3 MHz;",
	"P1,Accelerator;",
	"P1-;",
	"P1O[16:15],Card,ZipGS,TransWarp GS,None (OSD speed only);",
	"H0P1O[17],Speaker Delay,Enabled,Disabled;",
	"H0P1O[18],Joystick Delay,Enabled,Disabled;",
	"H0P1O[20],Counter Delay,Enabled,Disabled;",
	"H0P1O[19],Sync to Sys 1MHz (CPS),Off,On;",
	"P1O[23],AppleTalk/IRQ Delay,Enabled,Disabled;",
	"h0P1O[24],Startup Graphics,On,Off;",
	"h0P1O[25],Startup Sound,On,Off;",
	"P2,System;",
	"P2-;",
	"P2O[11],ROM Version,ROM1,ROM3;",
	"P2O[10],Force Self Test,OFF,ON;",
	"P2-;",
	"P2SC4,RAM,PRAM NVRAM;",
	"P2R[21],Save NVRAM;",
	"P2R[22],Load NVRAM;",
	"-;",

	"R0,Warm Reset;",
	"R1,Cold Reset;",
	"JA,Fire 1,Fire 2,Fire 3;",
	"jn,A|P,B,Y;",
	"jp,Y|P,B,Y;",
	"V,v",`BUILD_DATE
};

## Apple-Lisa (Apple-Lisa_MiSTer/Apple-Lisa.sv)
    localparam CONF_STR = {
        "Apple-Lisa;UART19200:9600:4800:2400:1200:300;",
        "-;",
        "S0,IMGVHD,Mount Hard Disk;",
        "S1,DC4DSKIMG,Mount Floppy;",
        "-;",
        "O9A,Aspect ratio,4:3,Original,Full Screen,[ARC1];",
        "OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
        "-;",
        "O34,RAM Size,512KB,1MB,1.5MB,2MB;",
        "O56,CPU Speed,1x (5MHz),2x,3x;",
        "O7,CPU ROM,H ROM,3A ROM;",
        "O8,I/O ROM,A8 ROM,40 ROM;",
        "ODE,Screen Color,Paper White,Green CRT,Amber CRT;",
        "-;",
        "R0,Reset & Apply;",
        "v,0;",
        "V,v",`BUILD_DATE
    };

## Aquarius (Aquarius_MISTer/Aquarius.sv)
localparam CONF_STR =
{
	"AQUARIUS;;",
	"-;",
	"F,BIN,Load Cartridge;",
	"F,CAQ,Load Tape;",
	"O9,Fast loading,No,Yes;",
	"-;",
	"OAB,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O56,CPU Speed,Normal,x2,x4,x8;",
	"O78,RAM expansion,16KB,32KB,None,4KB;",
	"-;",
	"R0,Reset;",
	"-;",
	"J,K1,K2,K3,K4,K5,K6;",
	"V,v",`BUILD_DATE
};

## Archie (Archie_MiSTer/Archie.sv)
localparam CONF_STR = {
	"ARCHIE;;",
	"J,Fire;",
	"V,v",`BUILD_DATE
};

## Atari800 (Atari800_MiSTer/Atari800.sv)
localparam CONF_STR = {
	"ATARI800;;",
	"-;",
	"S6,ATRXEXXDFATX,Boot D1;",
	"S5,XEXCOMEXE,Load XEX;",
	"F8,CARROMBIN,Load Cart;",
	"S8,CAS,Boot Tape;",
	"-;",
	"S0,ATRXEXXFDATX,Mount D1;",
	"S1,ATRXEXXFDATX,Mount D2;",
	"S2,ATRXEXXFDATX,Mount D3;",
	"S3,ATRXEXXFDATX,Mount D4;",
	"S4,IMG,Mount HDD;",
	"-;",
	"S7,CAS,Load Tape;",
	"F9,CARROMBIN,Second Cart;",
	"-;",
	"P1,Drives Carts & Tape;",
	"P1-;",
	"P1O[16],SIO Connected to,Emu,User I/O;",
	"P1O[57],Mount images R/O,Disabled,Enabled;",
	"P1-;",
	"d2P1O[45:44],D1 mode,OS/Stock,PBI,HSIO;",
	"d2P1O[47:46],D2 mode,OS/Stock,PBI,HSIO;",
	"d2P1O[49:48],D3 mode,OS/Stock,PBI,HSIO;",
	"d2P1O[51:50],D4 mode,OS/Stock,PBI,HSIO;",
	"P1-;",
	"P1O[12:10],SIO drive speed,Standard,Fast-6,Fast-5,Fast-4,Fast-3,Fast-2,Fast-1,Fast-0;",
	"P1O[38],ATX drive timing,1050,810;",
	"P1-;",
	"P1O[68],On cart (u)mount,PwrReset,Nothing;",
	"P1O[69],Cart auto-save,Disabled,Enabled;",
	"P1R[70],Save cart(s);",
	"P1-;",
	"P1O[66:64],Tape turbo system,Standard,SIO/Cmd,Turbo-D,K.S.O.,K.S.O. 2,Blizzard,Rambit,T6000;",
	"P1O[67],Invert turbo PWM,Disabled,Enabled;",
	"P2,Hardware & OS;",
	"P2-;",
	"P2O[9:7],CPU speed,1x,2x,4x,8x,16x;",
	"P2-;",
	"P2O[2],Machine,XL/XE,400/800;",
	"d1P2O[110],400/800 OS ROM type,10KB,16KB;",
	"H1P2O[15:13],RAM XL,64K,128K,320K(Compy),320K(Rambo),576K(Compy),576K(Rambo),1MB,4MB(Axlon);",
	"h1P2O[37:35],RAM 800,8K,16K,32K,48K,52K,4MB(Axlon);",
	"d5P2O[42],PBI BIOS,Disabled,Enabled;",
	"d2P2O[43],PBI splash,Disabled,Enabled;",
	"d2P2O[54:52],PBI boot drive,Default,APT,D1:,D2:,D3:,D4:,D5:,D6:;",
	"P2-;",
	"P2O[41],Use bootX.rom,Enabled,Disabled;",
	"P2-;",
	"P2FC4,ROMBIN,OS 16K;",
	"P2FC5,ROMBIN,Basic;",
	"P2FC6,ROMBIN,OS 10K (400/800);",
	"P2FC3,ROMBIN,TurboFreezer;",
	"P3,Video;",
	"P3-;",
	"P3O[5],Video mode,PAL,NTSC;",
	"hCP3O[112:111],NTSC rate Hz,59.92,59.94,60;",
	"HCP3O[113],PAL rate Hz,49.86,50;",
	"P3O[62:61],Interlace hack,Disabled,Weave,Bob;",
	"P3-;",
	"P3O[60:59],VBXE,Disabled,$D640,$D740;",
	"dBP3O[63],VBXE FX version,1.27,1.26a;",
	"dBP3O[115],VBXE Blitter,Normal,Turbo;",
	"dBP3FC2,ACT,VBXE Palette;",
	"P3-;",
	"P3O[114],GTIA xcolor,Off,On;",
	"P3O[34],GTIA clip,Disabled,Enabled;",
	"P3-;",
	"d3P3O[31],NTSC artifacting,No,Yes;",
	"d4P3O[55],Artifacting colors,Set 1,Set 2;",
	"d4P3O[58],Swap artif. colors,No,Yes;",
	"P3-;",
	"P3O[23:22],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P3O[19:17],Scandoubler FX,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"P3O[30:29],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"d0P3O[24],Vertical Crop,Disabled,216p(5x);",
	"d0P3O[28:25],Crop Offset,0,2,4,8,10,12,-12,-10,-8,-6,-4,-2;",
	"P4,Audio;",
	"P4-;",
	"P4O[4:3],Stereo mix (sys),None,25%,50%,100%;",
	"P4O[20],PokeyMax,Off/Mono,Enabled;",
	// Since there is no alternative for this really, Main loads this automatically
	// from a name fixed file
	//"P4FC7,ROMBIN,SID wave data;",
	"P4-;",
	"d6P4O[71],Mono detect,On,Off;",
	"P4O[32],Output Left Channel,On,Off;",
	"P4O[33],Output Right Channel,On,Off;",
	"P4O[73:72],Post-divide Left,4,8,1,2;",
	"P4O[75:74],Post-divide Right,4,8,1,2;",
	"P4O[77:76],GTIA mix-in,Left+Right,None,Left,Right;",
	"P4O[79:78],Tape volume,2x,4x,0x,1x;",
	"P4-;",
	"d6P4O[81:80],Number of Pokeys,4,1,2;",
	"P4O[82],Pokey volume,Saturated,Linear;",
	"P4O[83],Channel mode,Normal,Split;",
	"d6P4O[84],Multi IRQs,Off,On;",
	"P4-;",
	"d6P4O[85],SIDs,Enabled,Disabled;",
	"d7P4O[86],SID1 filter,8580,6581;",
	"d7P4O[87],SID2 filter,8580,6581;",
	"d7P4O[89:88],SID1 DFix/audio-in,DigiFix,None,Mixer LB;",
	"d7P4O[91:90],SID2 DFix/audio-in,DigiFix,None,Mixer LB;",
	"P4-;",
	"d6P4O[95],Covox/Sample,Enabled,Disabled;",
	"P4-;",
	"dAP4O[94:92],SID/CVX1+2 LB src,Pokey1+2,Pokey3+4,Covox,SID,PSG,GTIA,Tape;",
	"d8P4O[98:96],Covox3+4 LB src,Pokey3+4,Covox,SID,PSG,GTIA,Tape,Pokey1+2;",
	"P4-;",
	"d6P4O[99],PSGs,Enabled,Disabled;",
	"d9P4O[101:100],PSG clock,2MHz,1MHz,1.79MHz;",
	"d9P4O[103:102],PSG stereo,Polish,Czech,By chip,Mono;",
	"d9P4O[104],PSG envelope,32 steps,16 steps;",
	"d9P4O[106:105],PSG volume,Log 0,Log 1,Log 2,Linear;",
	"P4-;",
	"P4-,      (Reset to apply);",
	"P5,Input;",
	"P5-;",
	"P5O[21],Swap Joysticks 1&2,No,Yes;",
	"P5-;",
	"P5O[56],Mouse X,Normal,Inverted;",
	"P5O[6],Mouse Y,Normal,Inverted;",
	"-;",
	"R[39],Warm Reset (F9);",
	"R[40],Cold Reset (F10);",
	"R[0],Reset (Detach All);",
	"J,Fire 1,Fire 2,Fire 3,Paddle LT,Paddle RT,Start,Select,Option,Reset(F9),Reset(F10);",
	"V,v",`BUILD_DATE
};

## AtariST (AtariST_MiSTer/AtariST.sv)
parameter CONF_STR = {
	"AtariST;UART19200:9600:4800:2400:1200,MIDI;",
	"J,A,B,C,Option,Pause,#,*,0,1,2,3,4/L,5,6/R,7/Z,8/Y,9/X;",
	"jn,A,B,X,Select,Start,,,,,,,L,,R;",
	"I,",
	"ST joysticks,",
	"STe joysticks,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"V,v",`BUILD_DATE
};

## BBCMicro (BBCMicro_MiSTer/BBCMicro.sv)
parameter CONF_STR = {
	"BBCMicro;;",
	"-;",
	"S0,VHD;",
	"H0S1,SSDDSD;",
	"H0S2,SSDDSD;",
	"OC,Autostart,Yes,No;",
	"H0OH,Dflt Boot,MMC (vhd),Floppy (SSD/DSD);",
	"-;",
	"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O23,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"OFG,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"OA,Mouse as Joystick,Yes,No;",
	"OB,Swap Joysticks,No,Yes;",
	"-;",
	"O4,Model,B(MOS6502),Master(R65SC12);",
	"O56,Co-Processor,None,MOS65C02;",
	"O79,Default video mode,0,1,2,3,4,5,6,7;",
	"-;",
	"R0,Reset;",
	"JA,Fire;",
	"V,v",`BUILD_DATE
};

## BK0011M (BK0011M_MiSTer/BK0011M.sv)
localparam CONF_STR = 
{
	"BK0011M;;",
	"F,BIN;",
	"D2S1,DSKIMGBKD,Mount FDD A;",
	"D2S2,DSKIMGBKD,Mount FDD B;",
	"D3S3,DSKIMGBKD,Mount FDD C;",
	"D3S4,DSKIMGBKD,Mount FDD D;",
	"D4SC0,HDIHDSVHD,Mount HDD;",
	"-;",
	"OIK,Disk controller,Standard,A16M,SMK512,Samara,None;",
	"OH,FDD write protect,No,Yes;",
	"-;",
	"O3,Monochrome,No,Yes;",
	"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O78,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"OFG,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"OAB,Stereo mix,none,25%,50%,100%;",
	"OC,Sound mode,PSG,Covox;",
	"-;",
	"O5,Model,BK0011M,BK0010;",
	"H0O1,CPU Speed,4MHz,8MHz;",
	"H1O1,CPU Speed,3MHz,6MHz;",
	"-;",
	"R2,Reset;",
	"J,A,B,C,D;",
	"jn,A,B,X,Y;",
	"V,v",`BUILD_DATE
};

## C128 (C128_MiSTer/c128.sv)
localparam CONF_STR = {
   "C128;UART9600:2400;",
   "H7S0,D64G64D71G71D81T64,Mount #8                    ;",
   "H0S1,D64G64D71G71D81T64,Mount #9                    ;",
   "-;",
   "F2,PRGCRTREUTAP;",
   "hAdBR[61],Save cartridge;",
   "hAO[62],Autosave,Off,On;",
   "h3-;",
   "h3R[7],Tape Play/Pause;",
   "h3R[23],Tape Unload;",
   "h3O[11],Tape Sound,Off,On;",
   "-;",

   "P1,Audio & Video;",
   "HCP1O[106:105],Video Out,Follow 40/80,Side-by-side,VIC-II,VDC;",
   "HCP1O[107],40/80 Display,40 col,80 col;",
   "HCP1-;",
   "P1O[2],Video Standard,PAL,NTSC;",
   "P1-;",
   "P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
   "P1O[10:8],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
   "d1P1O[32],Vertical Crop,No,Yes;",
   "P1O[31:30],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
   "P1-;",
   "hCP1O[35:34],VIC-II Variant,656x,856x,Early 856x;",
   "P1O[84:82],VIC-II Palette,Colodore,Ultimate,Pepto-PAL,Vice,Vice6569R1,Vice6569R5,Vice8565R2,Lemon64;",
   "P1O[111:110],VIC-II Jailbars,Off,Low,Medium,High;",
   "HCP1-;",
   "HCP1O[126:125],VDC Variant,Auto,8563R9,8568;",
   "HCP1O[124],VDC Position,Centered,Original;",
`ifdef VDC_XRAY
   "HCP1O[127],VDC XRay,Off,On;",
`endif
`ifndef REDUCE_VDC_RAM
   "HCH6P1O[117],VDC Memory,16k,64k;",
`endif
   "HCP1O[116:115],VDC Palette,Default,Analogue,Monochrome,Composite;",
   "HCh2P1O[114:113],VDC Mono Colour,White,Green,Amber,Red;",
   "P1-;",
   "P1O[14:13],Left SID,Auto,6581,8580;",
   "P1O[16:15],Right SID,Auto,6581,8580;",
   "D4P1O[66:64],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
   "D5P1O[69:67],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
   "D4D8P1O[72:70],Left Fc Offset,0,1,2,3,4,5;",
   "D5D9P1O[75:73],Right Fc Offset,0,1,2,3,4,5;",
   "P1O[21:20],Right SID Port,Same,D420,DE00,DF00;",
   "P1O[37],8580 Digifix,On,Off;",
   "P1FC7,FLT,Load Custom Filters;",
   "P1-;",
   "P1O[12],Sound Expander,Disabled,OPL2;",
   "P1O[41:40],DigiMax,Disabled,DE00,DF00;",
   "P1O[19:18],Stereo Mix,None,25%,50%,100%;",

   "P2,Hardware;",
   "HCP2O[112],C64 mode,C128 extensions,Pure C64;",
   "HCP2-;",
   "HCP2O[118],Internal Memory,128K,256K;",
   "P2O[52],GeoRAM,Disabled,4MB;",
   "P2O[54:53],REU,Disabled,512KB,2MB,16MB;",
   "hFP2O[63],REU wrap,512KB,None;",
   "P2-;",
   "P2O[25],External IEC,Disabled,Enabled;",
   "P2O[43],Expansion,Joysticks,RS232;",
   "P2O[51],RS232 mode,UP9600,VIC-1011;",
   "P2O[33],RS232 connection,Internal,External;",
   "P2O[36],Real-Time Clock,Auto,Disabled;",
   "P2O[46:45],CIA,Auto,6526,8521;",
   "P2-;",
   "P2O[24],Clear RAM on Reset,Yes,No;",
   "P2O[50],Reset & Run PRG,Yes,No;",
   "P2O[42],Pause When OSD is Open,No,Yes;",
   "P2O[39],Tape Autoplay,Yes,No;",
   "P2O[38],Boot EasyFlash,Yes,No;",
   "P2-;",
   "P2FC3,ROMBIN,System ROMs                 ;",
   "P2FC4,ROMBIN,Drive ROMs                  ;",
   "HCP2FC6,ROMBIN,Internal Function ROM      ;",
   "P2FC5,CRT,Boot Cartridge              ;",

   "P3,Drives;",
   "P3O[58:57],Enable Drive #8,If Mounted,Always,Never;",
   "P3O[56:55],Enable Drive #9,If Mounted,Always,Never;",
   "P3-;",
   "D7P3O[122:121],Drive #8 5.25\" model,Auto,1541,1571;",
   "D0P3O[120:119],Drive #9 5.25\" model,Auto,1541,1571;",
   "P3-;",
   "P3O[44],Parallel port,Enabled,Disabled;",
	"P3O[86:85],Drives OSD,Activity Only,If Mounted,Always,Off;",
   "P3-;",
   "P3R[6],Reset Disk Drives;",

   "P4,Input devices;",
   "P4O[27:26],Pot 1/2,Joy 1 Fire 2/3,Mouse,Paddles 1/2;",
   "P4O[29:28],Pot 3/4,Joy 2 Fire 2/3,Mouse,Paddles 3/4;",
   "P4-;",
	"P4O[88:87],SNAC Joystick,Disabled,Joy 1,Joy 2;",
   "P4-;",
   "P4O[60:59],Key modifier,L+R Shift,L Shift,R Shift;",
   "HCP4O[109:108],Caps Lock mode,Auto,Caps Lock,ASCII/DIN;",
   "P4-;",
   "P4O[1],Release Keys on Reset,Yes,No;",

   "-;",
   "O[3],Swap Joysticks,No,Yes;",
   "-;",
   "O[49:48],8502 Speed,Standard,x2,x3,x4;",
   "HCO[104],Z80 Speed,Standard,x2;",
   "-;",
   "HChER[123],Reset & Remove Int.Func.ROM;",
   "hDR[17],Reset & Remove Cartridge;",
   "R[0],Reset;",
   "J,Fire 1,Fire 2,Fire 3,Paddle Btn,Mod1,Mod2;",
   "jn,A,B,Y,X|P,R,L;",
   "jp,A,B,Y,X|P,R,L;",
   "I,",
   "Missing/invalid boot0.rom/MRA,",          // 1
   "Missing/invalid boot1.rom/MRA,",          // 2
   "SHIFT LOCK: Off,SHIFT LOCK: On,",         // 3/4
   "CAPS LOCK: On,CAPS LOCK: Off,",           // 5/6
   "ASCII/DIN: DIN,ASCII/DIN: ASCII,",        // 7/8
   "40/80 DISPLAY: 80,40/80 DISPLAY: 40,",    // 9/10
   "NO SCROLL LOCK: Off,NO SCROLL LOCK: On;", // 11/12
   "DEFMRA,/_Computer/C128def.mra;",
   "V,v",`BUILD_DATE
};

## C16 (C16_MiSTer/C16.sv)
parameter CONF_STR = {
	"C16;;",
	"S0,D64G64,Mount #8;",
	"S1,D64G64,Mount #9;",
	"-;",
	"h4F1,PRGTAPBIN,Load;",
	"H4F1,PRGTAP,Load;",
	"-;",
	"h3RG,Tape Play/Pause;",
	"h3RI,Tape Unload;",
	"h3OH,Tape Sound,Off,On;",
	"h3OA,Tape Autoplay,Yes,No;",
	"h3-;",
	"O5,Joysticks swap,No,Yes;",
	"-;",
	"OJK,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"O78,TV Standard,from Kernal,Force PAL,Force NTSC;",
	"H2d1ON,Vertical Crop,No,Yes;",
	"h2d1ONO,Vertical Crop,No,270,216;",
	"OPQ,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",

	"P1,Audio;", 
	"P1OD,SID card,Disabled,Enabled;",
	"d5P1O[14],Left SID,6581,8580;",
	"d5P1O[15],Right SID,6581,8580;",
	"d5d9P1O[34:32],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
	"d5d8P1O[37:35],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
	"d5d9D7P1O[40:38],Left Fc Offset,0,1,2,3,4,5;",
	"d5d8D6P1O[48:46],Right Fc Offset,0,1,2,3,4,5;",
	"d5P1O[43],SID Ports,Mirror,Split;",
	"d5P1O[49],SID Card Mode,Plus/4,C64;",
	"d5P1O[45],8580 Digifix,On,Off;",
	"d5P1FC7,FLT,Load Custom Filters;",

	"-;",
	"OB,External IEC,Disabled,Enabled;",
	"-;",
	"O9,Model,C16,Plus/4;",
	"D0O6,Kernal,Loaded,Original;",
	"FC3,ROM,Load Kernal;",
	"-;",
	"R0,Reset & Apply;",
	"J,Fire;",
	"V,v",`BUILD_DATE
};

## C64 (C64_MiSTer/c64.sv)
localparam CONF_STR = {
	"C64;UART9600:2400;",
	"H7S0,D64G64T64D81,Mount #8;",
	"H0S1,D64G64T64D81,Mount #9;",
	"O[77:76],Mount Write Protected,Off,#8,#9,#8 & #9;",
	"-;",
	"F1,PRGCRTREUTAP;",
	"hAdBR[61],Save cartridge;",
	"hAO[62],Autosave,Off,On;",
	"h3-;",
	"h3T[7],Tape Play/Pause;",
	"h3T[95],Tape Stop;",
	"h3T[93],Tape Rewind;",
	"h3T[94],Tape Fast Forward;",
	"h3T[91],Tape Counter Reset;",
	"h3-;",
	"h3O[92],Tape Counter,Off,On;",
	"h3O[11],Tape Sound,Off,On;",
	"h3R[23],Tape Unload;",
	"-;",

	"P1,Audio & Video;", 
 	"P1O[2],Video Standard,PAL,NTSC;",
	"P1O[35:34],VIC-II,656x,856x,Early 856x;",
	"P1O[84:82],Palette,Colodore,Ultimate,Pepto-PAL,Vice,Vice6569R1,Vice6569R5,Vice8565R2,Lemon64;",
	"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O[10:8],Scandoubler Fx,None,HQ2x-320,HQ2x-160,CRT 25%,CRT 50%,CRT 75%;",
	"d1P1O[32],Vertical Crop,No,Yes;",
	"P1O[31:30],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P1-;",
	"P1O[13],Left SID,6581,8580;",
	"P1O[16],Right SID,6581,8580;",
	"D4P1O[66:64],Left Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
	"D5P1O[69:67],Right Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
	"D4D8P1O[72:70],Left Fc Offset,0,1,2,3,4,5;",
	"D5D9P1O[75:73],Right Fc Offset,0,1,2,3,4,5;",
	"P1O[22:20],Right SID Port,Same,DE00,D420,D500,DF00;",
	"P1O[37],8580 Digifix,On,Off;",
	"P1FC7,FLT,Load Custom Filters;",
	"P1-;",
	"P1O[12],Sound Expander,Disabled,OPL2;",
	"P1O[41:40],DigiMax,Disabled,DE00,DF00;",
	"P1O[19:18],Stereo Mix,None,25%,50%,100%;",

	"P2,Hardware;", 
	"P2O[52],GeoRAM,Disabled,4MB;",
	"P2O[54:53],REU,Disabled,512KB,2MB,16MB;",
	"hCP2O[63],REU wrap,512KB,None;",
	"P2-;",
	"P2O[25],External IEC,Disabled,Enabled;",
	"P2O[43],Expansion,Joysticks,RS232;",
	"P2O[51],RS232 mode,UP9600,VIC-1011;",
	"P2O[33],RS232 connection,Internal,External;",
	"P2O[36],Real-Time Clock,Auto,Disabled;",
	"P2O[45],CIA,6526,8521;",
	"P2-;",
	"P2O[27:26],Pot 1/2,Joy 1 Fire 2/3,Mouse,Paddles 1/2;",
	"P2O[29:28],Pot 3/4,Joy 2 Fire 2/3,Mouse,Paddles 3/4;",
	"P2-;",
	"P2O[60:59],Key modifier,L+R Shift,L Shift,R Shift;",
	"P2-;",
	"P2O[1],Release Keys on Reset,Yes,No;",
	"P2O[24],Clear RAM on Reset,Yes,No;",
	"P2O[50],Reset & Run PRG,Yes,No;",
	"P2O[42],Pause When OSD is Open,No,Yes;",
	"P2O[39],Tape Auto Play,Yes,No;",
	"P2O[96],Tape Auto Unload,Yes,No;",
	"P2O[38],Boot EasyFlash,Yes,No;",
	"P2-;",
	"P2FC8,ROM,System ROM C64+C1541 ;",
	"P2FC9,ROM,System ROM C1581     ;",
	"P2FC5,CRT,Boot Cartridge       ;",
	"P2-;",
	"P2O[15:14],System ROM,Loadable C64,Standard C64,C64GS,Japanese;",

	"P3,Drives;",
	"P3O[58:57],Enable Drive #8,If Mounted,Always,Never;",
	"P3O[56:55],Enable Drive #9,If Mounted,Always,Never;",
	"P3O[98:97],Parallel Cable,Auto,On,Off;",
	"P3O[86:85],Drives OSD,Activity Only,If Mounted,Debug,Off;",
	"P3-;",
	"P3O[80:78],Drive RPM    (G64),300.0,300.1,300.5,301.0,302.0,299.0,299.5,299.9;",
	"P3O[81],Drive Wobble (G64),Off,On;",
	"P3-;",
	"P3R[6],Reset Disk Drives;",

	"-;",
	"O[3],Swap Joysticks,No,Yes;",
	"O[88:87],SNAC Joystick,Disabled,Joy 1,Joy 2;",
	"-;",
	"O[47:46],Turbo mode,Off,C128,Smart;",
	"d6O[49:48],Turbo speed,2x,3x,4x;",
	"-;",
	"R[0],Reset;",
	"R[17],Reset & Detach Cartridge;",
	"J,Fire 1,Fire 2,Fire 3,Paddle Btn,Mod1,Mod2;",
	"jn,A,B,Y,X|P,R,L;",
	"jp,A,B,Y,X|P,R,L;",
	"V,v",`BUILD_DATE
};

## CBM-II (CBM-II_MiSTer/CBM-II.sv)
localparam CONF_STR = {
	"CBM-II;;",
	"HAO[50],Drive #8 Type,8250,4040;",
	"HAHCS0,D80D82, Mount #8.0;",
	"HAHCS1,D80D82, Mount #8.1;",
	"HAhCS0,D64, Mount #8.0;",
	"HAhCS1,D64, Mount #8.1;",
	"HA-;",
	"HBO[51],Drive #9 Type,8250,4040;",
	"HBHDS2,D80D82, Mount #9.0;",
	"HBHDS3,D80D82, Mount #9.1;",
	"HBhDS2,D64, Mount #9.0;",
	"HBhDS3,D64, Mount #9.1;",
	"HB-;",
	"F9,PRG;",
	"-;",

	"P1,Hardware;",
	"P1O[4:2],System,P500,B610,B620,B710,B720,Custom;",
	"h0P1-;",
	"h0P1O[6:5],Model,Professional,Business LP,Business HP;",
	"h0H1P1O[11],CPU Clock,1 MHz,2 MHz;",
	"h0P1O[10:9],RAM,128K,256K,896K;",
	"h0h1P1-;H1h6P1-;",
	"h0h1P1O[37],Enable Joysticks,No,Yes;",
	"h6P1O[33:32],Pot 1/2,Joy 1 Fire 2/3,Mouse,Paddles 1/2;",
	"h6P1O[35:34],Pot 3/4,Joy 2 Fire 2/3,Mouse,Paddles 3/4;",
	"h6P1O[36],Swap Joysticks,No,Yes;",
	"P1-;",
	"P1O[47:46],Enable Drive #8,If Mounted,Always,Never;",
	"P1O[49:48],Enable Drive #9,If Mounted,Always,Never;",
	"P1O[45],Enable External IEC,No,Yes;",
	"P1R[44],Reset Drives;",
	"P1-;",
	"P1O[13],Release Keys on Reset,Yes,No;",
	"P1O[14],Clear All RAM on Reset,Yes,No;",
	"P1O[15],Pause When OSD is Open,No,Yes;",

	"P2,Audio & Video;",
	"H2P2O[12],TV System,PAL,NTSC;",
	"H1P2O[25],VIC-II Revision,Early,Later;",
	"H2P2-;",
	"P2O[17:16],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P2O[20:18],Scandoubler Fx,None,HQ2x-320,HQ2x-160,CRT 25%,CRT 50%,CRT 75%;",
	"d3P2O[21],Vertical Crop,No,Yes;",
	"P2O[23:22],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2-;",
	"P2O[28:26],SID Filter,Default,Custom 1,Custom 2,Custom 3,Adjustable;",
	"D5P2O[31:29],Fc Offset,0,1,2,3,4,5;",

	"P3,ROM/RAM Configuration;",
	"P3-,System ROM;",
	"P3FC1,ROMBIN, P500            ;",
	"P3FC2,ROMBIN, B6x0            ;",
	"P3FC3,ROMBIN, B7x0            ;",
	"P3-;",
	"P3-,External ROM/RAM (Seg 15);",
	"P3O[38], $0800-1FFF,Disabled,RAM;",
	"P3O[39], $2000-3FFF,ROM,RAM;",
	"D7P3FC4,ROMBIN,  Load;",
	"P3O[40], $4000-5FFF,ROM,RAM;",
	"D8P3FC5,ROMBIN,  Load;",
	"P3O[41], $6000-7FFF,ROM,RAM;",
	"D9P3FC6,ROMBIN,  Load;",
	"P3-;",
	// "P3-,Drive ROM;",
	// "P3FC7,ROMBIN, 4040            ;",
	// "P3FC8,ROMBIN, 8250            ;",
	// "P3-;",
	"P3-,Load options;",
	"P3O[24], Ignore PRG start,Yes,No;",

	"-;",
 	"R[0],Hard reset;",
	"R[1],Soft reset;",
	"J,Fire 1,Fire 2,Fire 3,Paddle Btn;",
	"jn,A,B,Y,X|P;",
	"jp,A,B,Y,X|P;",
	"v,0;",
	"V,v",`BUILD_DATE
};

## Casio_PV-2000 (Casio_PV-2000_MiSTer/Casio_PV-2000.sv)
localparam CONF_STR = {
	"Casio_PV-2000;;",
	"-;",
	"F1,BINROM,Load Cartridge;",
	"-;",

	"P2,Video Settings;",
	"P2O[2],Aspect ratio,Original,Full Screen;",
	"P2O[3],Show Border,No,Yes;",
	"P2O[5:4],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"R[6],Eject Cartridge and Reset;",
	"R[0],Reset;",
	"J,Attack 0,Attack 1,Select,Start;",
	"V,v",`BUILD_DATE 
};

## CoCo2 (CoCo2_MiSTer/CoCo2.sv)
localparam CONF_STR = {
	"CoCo2;;",
	"-;",
	"OE,Cart Slot,Cartridge,Disk;",
	"-;",
	"H1F1,CCCROM,Load Cartridge;",
	"H2S0,DSK,Load Disk Drive 0;",
	"H2S1,DSK,Load Disk Drive 1;",
	"H2S2,DSK,Load Disk Drive 2;",
	"H2S3,DSK,Load Disk Drive 3;",
	"-;",
	"OC,Tape Input,File,ADC;",
    "-;",
	"H0F2,CAS,Load Cassette;",
	"H0TR,Stop & Rewind;",
    "-;",
    "H0S4,CAS,Save Cassette;",
    "H0OS,Cass Rwd=0 / Rec=1,0,1;",
    "-;",

	"OD,Monitor Tape Sound,No,Yes;",
	"OQ,Show Tape Status,Yes,No;",
	"-;",
	"P1,Video Settings;",
	"P1-;",
	"P1-, -= Video Settings =-;",
	"P1-;",
	"P1OJ,Aspect ratio,Original,Full Screen;",
	"P1OFH,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
	"P1O4,Overscan,Hidden,Visible;",
	"P1O3,Artifact,Enable,Disable;",
	"P1O2,Artifact Phase,Normal,Reverse;",
//	"O58,Count Offset,1,2,3,4;",
	"-;",
	"OM,Keyboard Layout,PC,CoCo;",
	"OL,D-Pad Joystick emu,No,Yes;",
	"OA,Swap Joysticks,Off,On;",
	"-;",
	"O89,Machine,CoCo2,Dragon32,Dragon64;",
//	"O7,Turbo,Off,On;",			// Code left in place for menu based activation / deactivation
	"-;",
`ifdef USE_OVERLAY
	"OB,Debug display,Off,On;",
	"-;",
`endif
	"RN,Hard Reset;",
	"R0,Reset;",
	"J,Button;",
	"jn,A;",
    "V,v",{`BUILD_DATE,"-",`BUILD_NUMBER}
};

## CoCo3 (CoCo3_MiSTer/CoCo3.sv)
localparam  CONF_STR = {
        "COCO3;UART19200:9600:4800:2400:1200:300;",
        "-;",
        "OCD,Multi-Pak,(Slot 2) CoCoSDC,(Slot3) ECB /Cart,(Slot 4) Disk;",
        "H3-;",
        "H3S2,DSK,Load Disk Drive 0;",
        "H3S3,DSK,Load Disk Drive 1;",
        "H3S4,DSK,Load Disk Drive 2;",
        "H3S5,DSK,Load Disk Drive 3;",
        "H2-;",
        "H2F1,CCC,Load Cartridge;",
        "H1-;",
        "H1S0,DSKVHD,Load SDC Drive 0;",
        "H1S1,DSKVHD,Load SDC Drive 1;",
        "H1-;",
        "H1S2,DSK,Load OS9 Floppy /d0;",
        "H1S3,DSK,Load OS9 Floppy /d1;",
        "H1S4,DSK,Load OS9 Floppy /d3;",
        "H1S5,DSK,Load OS9 Floppy /d4;",
        "-;",

        "F2,CAS,Load Cassette;",
        "TF,Stop & Rewind;",
        "OH,Monitor Tape Sound,No,Yes;",
        "-;",
        "S6,CAS,Save Cassette;",
        "OE,Cass Rwd=0 / Rec=1,0,1;",
        "-;",

        "P1,Video Settings;",
        "P1-;",
        "P1-, -= Video Settings =-;",
        "P1-;",
        "P1O89,Aspect ratio,Original,Full Screen,Wide,[ARC1];",
        "P1O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
        "P1-;",
        "P1OL,CoCo3 LC,Yes,No-(SG6=Yes);",
        "P1-;",
        "P1o56,Artifact Type,MESS,Simple,NONE;",
        "P1OI,Artifact Color Set,0,1;",
        "-;",
        "P2,Debug Menu;",
        "P2-;",
        "P2-, -= Debug Menu =-;",
        "P2-;",
        "P2F3,BIN,Load COCO Font;", 
        "P2OG,Swap MiSTer Uart,RS CART,Default;",
        "P2OU,Swap Uart to IO,RS IO,MiSTer;",
        "P2OT,Display Debug,Off,On;",
        "-;",
        "ON,D-Pad Joystick emu,No,Yes;",
        "O6,Swap Joysticks,Off,On;",
        "OS,Use Mouse for Rt Joystk,Off,On;",
        "RA,Easter Egg;",
        "-;",
        "OO,Force Turbo,No,Yes;",
        "o02,Turbo Speed,CoCo Ctrl, 0.89 Mhz,1.78 Mhz,2.86 Mhz, 3.58 Mhz,7.16 Mhz,9.54 Mhz;",
        "OPR,Memory Size,512K,1M,2M,16M;",
        "o89,Auto Run,No,DOS,AUTO.BAS;",
        "-;",
        "RM,Cold Boot;",
        "R0,Reset;",
        "J,Button1,Button2;",
        "jn,A,B;",
		"v,51;",
        "V,v",{`BUILD_DATE,"-",`BUILD_NUMBER}
};

## ColecoAdam (ColecoAdam_MiSTer/ColecoAdam.sv)
parameter CONF_STR = {
        "Adam;;",
        "-;",
        "F,COLBINROM,Load CART;",
        "S0,DSK,Load Floppy 1;",
        "S1,DSK,Load Floppy 2;",
        "S2,DSK,Load Floppy 3;",
        "S3,DSK,Load Floppy 4;",
        "S4,DDP,Load Tape 1;",
        "S5,DDP,Load Tape 2;",
        "S6,DDP,Load Tape 3;",
        "S7,DDP,Load Tape 4;",
        "-;",
        "O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
        "O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
        "-;",
        "O6,Border,No,Yes;",
        "OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
        "-;",
        "O3,Joysticks swap,No,Yes;",
        "OD,Keypad on numpad,Off,On;",
        "OE,Stick keypad,Off,On;",
        "OFG,Spinner,Off,Spinner,Stick X,Stick XY;",
        "-;",
        "OC,Mode,Computer,Console;",
        // A three bit field, so it had to move off bits 4 and 5. 64K stays first so that a
        // zeroed status word still means the expander the core has always defaulted to.
        "OHJ,Expansion RAM,64K,256K,512K,1M,2M,None;",
        "R0,Reset;",
        "J,Fire 1,Fire 2,*,#,0,1,2,3,4,5,6,7,8,9,Purple Tr,Blue Tr;",
        "V,v",`BUILD_DATE
};

## Cray-XMP (Cray-XMP_MiSTer/Cray-XMP.sv)
localparam CONF_STR = {
	"Cray-XMP;;",
	"-;",
	"F1,IOS,Load boot file,34000000;",
	"S0,IMG,Expander disk;",
	"S1,IMG,Disk drives;",
	"S2,TXT,Printer file;",
	"S3,TAP,Tape;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[6:5],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"O[9:7],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"O[4:3],Text color,White,Green,Amber,Cyan;",
	"O[14],Font,8x8 (15kHz),8x16 (31kHz);",
	"O[15],Device times,Fast,Real;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	"v,1;", // [optional] config version 0-99. 
	        // If CONF_STR options are changed in incompatible way, then change version number too,
			  // so all options will get default values on first start.
	"V,v",`BUILD_DATE 
};

## EDSAC (EDSAC_MiSTer/edsac.sv)
localparam CONF_STR = 
{
   "EDSAC;;",
   "-;",
   "F1,TAP,Load tape;",
   "-;",
   "OG,Tape Reader Speed,Normal,Fast;",
   "OF,Initial Orders ver,2,1;",
   "OE,Tank group,One,Two;",
   "O36,Memory Tank,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
   "O1,Aspect Ratio,16:9,4:3;",
   "V,v0.3.",`BUILD_DATE
};

## Enterprise (Enterprise_MiSTer/Enterprise.sv)
localparam CONF_STR = {
	"Enterprise;;",

	"SC0,VHD,Mount SD;",
	"S1,IMGDSK,Mount A:;",
	"-;",
	"F0,ROM,Load ROM;",
	"-;",
	"O[11:9],Available RAM,1 MB,2 MB,3 MB,64 KB,128 KB,256 KB,512 KB;",
	"O[8:7],CPU Speed,4 MHz,8 MHz,16 MHz;",

	"-;",
	"P1,Audio  & Video;",
	"P1-;",
	"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O[16:15],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"P1-;",
	"H2d1P1O[28],Vertical Crop,No,Yes;",
	"h2d1P1O[29:28],Vertical Crop,No,270,216;",
	"P1O[27:26],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P1-;",
	"P1O[3:2],Stereo Mix,none,25%,50%,100%;",
	
	"R[0],Reset;",
	"R[12],Cold Reset;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE 
};

## FM-7 (FM-7_MiSTer/FM-7_MiSTer.sv)
localparam CONF_STR = {
  "FM-7;;",
  "-;",
  "F1,t77,Load Tape;",
  "S0,d77d88,Mount Disk 1;",
  "S1,d77d88,Mount Disk 2;",
  // A .d77/.d88 may be a CONTAINER of several complete disks -- 51 of them in
  // the Neo Kobe set, holding 2 to 6 each. These pick which one the drive
  // presents; they do nothing on an ordinary single-disk image, where only
  // "1" exists. Changing one re-scans that drive, so it is a disk swap.
  "O[15:13],Disk 1 image,1,2,3,4,5,6,7,8;",
  "O[18:16],Disk 2 image,1,2,3,4,5,6,7,8;",
  "T[8],Tape Rewind;",
  "O[9],Tape Audio,Off,On;",
  "O[11:10],Boot ROM,0 disk,1 alt,2 dos-a,3 empty;",
  "O[12],Machine,FM-7,FM77AV;",
  // Which system ROM set to page in from the uploaded boot1.rom. Does nothing
  // unless that file is present -- with no file the machine runs the ROMs
  // baked into the .rbf, which are set 0's. Changing this resets the machine:
  // swapping the BASIC ROM under a running interpreter is not a thing.
  "O[19],System ROM,Japanese,Spanish;",
  // Pad 1 as keys, for the many titles that never read the joystick port:
  // d-pad = keypad 1-9, letting go = keypad 5 (the FM-7's stop key). The PSG
  // joystick port keeps working either way. See rtl/JOYKEYS.v for the map.
  "O[20],Joypad keys,Off,On;",
  "-;",
  "O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
  "-;",
  "T[0],Reset;",
  "R[0],Reset and close OSD;",
  // J1/jn MUST stay last, after the reset entries. 
  "J1,Button A,Button B,Button X,Button Y,L,R,Select,Start;",
  "jn,A,B,X,Y,L,R,Select,Start;",
  "v,4;",
  "V,v",`BUILD_DATE
};

## Galaksija (Galaksija_MiSTer/Galaksija_MiSTer.sv)
localparam CONF_STR = {
	"Galaksija;;",
	"-;",
   "F,TAP;",
	"-;",
	"O23,Screen Color,White,Green,Amber,Cyan;",
   "O1,Aspect Ratio,16:9,4:3;",
	"R5,Break;",
	"R9,Reset;",	
	"-;",	
	"T4,Type OLD before loading!;",
	"V,v0.1.",`BUILD_DATE
};

## Homelab (Homelab-MiSTer/Homelab.sv)
localparam CONF_STR = {
	"Homelab;;",
	"-;",
	"F1,HTP,Load Rom;",
	"T[10],Play;",
	"T[11],Stop;",
	"T[12],Rewind;",
`ifndef DISABLE_TURBO_LOADER
	"O[13],Turbo HTP Loading,Off,On;",
`endif
	"-;",
	"FC2,CHR,Load Alternate CHR;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[2],Screen mode,32 CH,64 CH;",
	"O[6:5],Screen Color,White,Green,Amber;",	
	"O[3],Machine,Homelab3,Homelab4;",		
	"-;",
	"R[0],Reset;",
	"V,v",`BUILD_DATE 
};

## IQ151 (IQ151_MiSTer/IQ151.sv)
localparam CONF_STR = {
   "IQ151;;",
   "-;",
   "O1,RAM,32kB,64kB;",
   "OA,Monitor,Std,CPM;",

   "-;",
   "O2,Video Module,Video32,Video64;",
   "O3,Graphics Module,On,Off;",
   "-;",
   "O45,Language Module,None,Basic G,Basic 6,Amos ASM+PAS+PAS1;",
   "-;",
   "O6,SD-Rom (Staper) Module,On,Off;",
   "O7,RS232 Module,Sestyk (8251),None;",
//   "O78,RS232 Module,None,Sestyk (8251),SERI (Z80-SIO);",
   
   "-;",
   "O9,Floppy Module,Disc2 (8272),None;",
   "S1,DSK,Floppy 0 (A):;",
   "S0,DSK,Floppy 1 (B):;",

   "-;",
   "R0,Reset IQ151;",
   "V,v",`BUILD_DATE 
};

## Interact (Interact_MiSTer/Interact.sv)
localparam CONF_STR = {
	"Interact;;",
	"F,CINK7 ,Load tape;",
	"TE,Play;",
	"TF,Stop & Rewind;",
	"-;",
	"OAB,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O6,Test Pattern,Off,On;",
	"-;",
	"T0,Reset;",
	"R0,Reset and close OSD;",
	"V,v",`BUILD_DATE 
};

## JR100 (JR100_MiSTer/JR100.sv)
localparam CONF_STR = {
	"JR100;;",
	"-;",
	"F0,rom,Load BASIC ROM;",
	"F1,prg,Load PRG;",
	"F2,bas,Load BAS;",
	"-;",
	"S0,prg,Mount Save File;",
	"T[3],Save BASIC to file;",
	"-;",
	"S1,cmt,Mount Tape;",
	"T[4],Tape Play;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[8:6],Display color,White,Green,Amber,Cyan,Orange,Blue,Paper,Mint;",
	"O[2],Extended RAM (reset),Off,On;",
	"O[5],Autostart loaded program,No,Yes;",
	"-;",
	"R[0],Reset;",
	"J,Fire;",
	"V,v",`BUILD_DATE
};

## Jupiter (Jupiter_MiSTer/Jupiter.sv)
parameter CONF_STR = {
	"Jupiter;;",
	"-;",
	"F,ACE;",
	"-;",
	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O23,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O45,CPU Speed,Normal,x2,x4;",
	"R0,Reset;",
	"J,Fire;",
	"V,v",`BUILD_DATE
};

## Laser310 (Laser310_MiSTer/Laser310.sv)
localparam CONF_STR = {
        "Laser;;",
        "-;",
        "F1,VZ ,Load VZ Image;",
        "-;",
        "O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	     "O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",

        "O5,Turbo,Off,On;",
        "O6,Dos Rom,Off,On;",
        "O7,SHRG,Off,On;",
     //   "O8,64x32 Video,Off,On;",

        "-;",
        "R0,Reset;",
		  "J,Fire 1,Button 2;",
        "V,v",`BUILD_DATE
};

## Lynx48 (Lynx48_MiSTer/Lynx48.sv)
localparam CONF_STR = {
	"Lynx48;;",
	"-;",
	"F[1],TAP,Load Cassette;",
	"-;",
	"O[5],Aspect ratio,4:3,16:9;",
	"O[2:1],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"-;",
	"O[4:3],Machine,Lynx 48K,Lynx 96k,96k Scorpion,Lynx 128k;",
	"O[6],BASIC Autostart,On,Off;",	
	"-;",
	"OD,Joysticks Swap,No,Yes;",
	"-;",	
	"T[0],Reset;",	
	"R[0],Reset and close OSD;",
	"J,Button;",
	"jn,A;",
	"V,v",`BUILD_DATE 
};

## MSX (MSX_MiSTer/MSX.sv)
localparam CONF_STR = {
	"MSX;;",
	"S,VHD;",
	"OE,Reset after Mount,No,Yes;",
	"-;",
	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O23,Scanlines,No,25%,50%,75%;",
	"-;",
	"OF,Vertical Crop,No,Yes;",
	"OGH,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O1,CPU speed,Normal,Turbo(+F11);",
	"OB,CPU type,Z80,R800;",
	"-;",
	"O4,Slot1,Empty,MegaSCC+ 1MB;",
	"O56,Slot2,Empty,MegaSCC+ 2MB,MegaRAM ASCII-8K 1MB,MegaRAM ASCII-16K 2MB;",
	"-;",
	"O7,Internal Mapper,2048KB RAM,4096KB RAM;",
	"-;",
	"OD,Joysticks Swap,No,Yes;",
	"OC,Mouse,Port 1,Port 2;",
	"-;",
	"RA,Reset;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE
};

## MSX1 (MSX1_MiSTer/MSX1.sv)
localparam CONF_STR = {
	"MSX1;;",
	"-;",
	"h2OHK,SLOT A,ROM mapper auto,ROM mapper none,ROM mapper gamemaster2,ROM mapper Konami,ROM mapper KonamiSCC,ROM mapper ASCII8,ROM mapper ASCII16,ROM mapper linear64k,ROM mapper R-TYPE,FDD VY0010;",   
	"H2OHK,SLOT A,Empty,FDD VY0010,Gamemaster2;",
	"h1S0,DSK,Mount Drive A:;",
	"H3F2,ROM,SLOT A load;",
	"OLO,SLOT B,ROM mapper Auto,ROM mapper none,ROM mapper gamemaster2,ROM mapper Konami,ROM mapper KonamiSCC,ROM mapper ASCII8,ROM mapper ASCII16,ROM mapper linear64k,ROM mapper R-TYPE;",
	"F3,ROM,SLOT B load;",
	"-;",
	"OC,Tape Input,File,ADC;",
	"H0F4,CAS,Cas File;",
	"H0TD,Tape Rewind;",
	"-;",
	"P1,Video settings;",
	"P1OG,Video mode,PAL,NTSC;",
	"P1O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O3,Border,No,Yes;",
	"P1O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"P1OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",	
	"P2,Advanced settings;",
	"P2F1,ROM,Load BIOS;",	
	"-;",
	"T0,Reset;",
	"RF,Reset & Detach ROM Cartridge;",
	"R0,Reset and close OSD;",
   "I,Unkn,NOmpaer,GM2,Konami,KonamiSCC,Ascii8,Ascii16;",
	"V,v",`BUILD_DATE 
};

## MacIIcx (MacSE30_MiSTer/MacSE30.sv)
localparam CONF_STR = {
`ifdef MACIICX
	"MACIICX;;",
`else
	"MACSE30;;",
`endif
	"-;",
	"S0,DSKIMG,Mount Internal Floppy;",
`ifdef SE30_EXT_DRIVE
	"S1,DSKIMG,Mount External Floppy;",
`endif
	"-;",
	"SC2,IMGVHD,Mount SCSI-0;",
	"SC3,IMGVHD,Mount SCSI-1;",
`ifndef SE30_NO_CDROM
	"SC4,ISOTO*,Mount CD-ROM;",
`endif
	"-;",
	"SC5,NVR,Mount PRAM;",
	"-;",
	"O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[9:8],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"O[4],Memory,8 MB,16 MB;",
	"O[7:6],Mouse speed,Normal,/2,/4,/8;",
`ifdef MACIICX
	"O[5],Power switch,Normal,Locked on;",
`endif
	"-;",
	"R[0],Reset & Apply Memory;",
	"R[3],Wipe PRAM (erases settings!);",
	"v,0;",
	"V,v",`BUILD_DATE
};

## MacLC (MacLC_MiSTer/MacLC.sv)
	localparam CONF_STR = {
		"MACLC;UART57600:115200,MIDI;",
		"-;",
		"S6,DSKIMG,Mount Floppy;",
		// Default OFF, and it must stay that way: the ROM's write primitive
		// polls the IWM handshake in an UNBOUNDED loop, so the failure mode of
		// a write bug is a HUNG machine, not a failed write. Gated further in
		// flp_int_wp below - a read-only mount stays write-protected whatever
		// this says; a DC42 container does not.
		"OE,Floppy Write,Off,On;",
		"-;",
		"SC0,IMGVHDHDA,Mount SCSI-0;",
		"SC1,IMGVHDHDA,Mount SCSI-1;",
		"SC2,NVR,Mount PRAM;",
		"-;",
		// CD-ROM (SCSI ID 3). ISO/TOAST (TO* matches .toast) are raw
		// 2048-byte images and work today; CUE/BIN/CHD are listed for the
		// Main_MiSTer translation layer (docs/plan_scsi_cdrom.md Phase 2) —
		// on a stock Main a 2048-byte-sector .bin also works mounted directly.
		"SC4,ISOTO*CUEBINCHD,Mount CD-ROM;",
		"OI,CD-ROM Drive,Enabled,Disabled;",
		// Default OFF for distribution (2026-08-24): the ethernet card needs
		// the paired Main (releases/MiSTer) — with an older ethernet Main a
		// card-ON boot hangs, so users opt in via the OSD after installing
		// the Main. Bit clear (0) = first entry = Off; ena_osd below is the
		// matching un-inverted status[19].
		"OJ,Ethernet,Off,On;",
		"o45,Net interface,eth0,tap0,macvlan,eth1;",
		"o03,MAC suffix,0,1,2,3,4,5,6,7,8,9,A,B,C,D,E,F;",
		"-;",
		"O78,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
		"OCD,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
		"OA,Monitor @Reset,640x480 VGA,512x384 12in;",
		"-;",
		"O4,Memory,2MB,10MB;",
		"-;",
		"R5,Interrupt (NMI / MacsBug);",
		"R6,WIPE PRAM (erases settings!);",
		"R0,Reset & Apply CPU+Memory;",
		"-;",
		"P1,MT32-pi;",
		"P1-;",
		"P1OO,Use MT32-pi,Yes,No;",
		"P1OQ,Synth,Munt,FluidSynth;",
		"P1ORS,Munt ROM,MT-32 v1,MT-32 v2,CM-32L;",
		"P1OTV,SoundFont,0,1,2,3,4,5,6,7;",
		"P1OMN,Show Info,No,Yes,LCD-On,LCD-Auto;",
		"I,",
		"MT32-pi: SoundFont #0,",
		"MT32-pi: SoundFont #1,",
		"MT32-pi: SoundFont #2,",
		"MT32-pi: SoundFont #3,",
		"MT32-pi: SoundFont #4,",
		"MT32-pi: SoundFont #5,",
		"MT32-pi: SoundFont #6,",
		"MT32-pi: SoundFont #7,",
		"MT32-pi: MT-32 v1,",
		"MT32-pi: MT-32 v2,",
		"MT32-pi: CM-32L,",
		"MT32-pi: Unknown mode;",
		"V,v",`BUILD_DATE
	};

## MacPlus (MacPlus_MiSTer/MacPlus.sv)
localparam CONF_STR = {
	"MACPLUS;UART115200;",
	"-;",
	"S2,DSK,Mount Pri Floppy;",
	"S3,DSK,Mount Sec Floppy;",
	"-;",
	"D0SC0,IMGVHD,Mount SCSI-6;",
	"D0SC1,IMGVHD,Mount SCSI-5;",
	"-;",
	// CD-ROM (SCSI ID 3): CUE/BIN/CHD/TOAST are flattened host-side by
	// Main_MiSTer's support/mac to 2048-byte sectors; audio tracks are played
	// by rtl/cd_audio.sv. The extension list must match MacLC.sv's.
	// D0: greyed out on a model with no SCSI bus (status_menumask bit 0)
	"D0SC4,ISOTO*CUEBINCHD,Mount CD-ROM;",
	// Apple HD20 on the external floppy port, a hard disk image like SCSI
	"SC5,IMGVHD,Mount HD20;",
	"D0OI,CD-ROM Drive,Enabled,Disabled;",
	// index 0 = Full: `status` defaults to zero. Independent of the Mac's own
	// volume, as the drive's knob was
	"D0OFG,CD Volume,Full,3/4,1/2,Off;",
	"-;",
	"O78,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	// three bits for five models; layout change, hence the config version bump
	"O13,Model,Plus,SE,512K,128K,512Ke;",
	"O5,Speed,8MHz,16MHz;",
	"O6,Floppy Write,Off,On;",
	// host mouse counts per Mac count: the Plus counts 180/inch, so cpi/180 is pixel-exact
	"OMP,Mouse Speed,8,1,2,3,4,5,6,7,9,10,11,12,13,14,15,16;",
	"ODE,CPU,68000,68010,68020;",
	"D1O4,Memory,1MB,4MB;",
	"-;",
	//"OA,Serial,Off,On;",
	//"-;",
	"R0,Reset & Apply CPU+Memory;",
	"v,1;", // [optional] config version 0-99.
	        // If CONF_STR options are changed in incompatible way, then change version number too,
			// so all options will get default values on first start.
	"V,v",`BUILD_DATE
};

## MacQuadra800 (MacQuadra800_MiSTer/MacQuadra800.sv)
localparam CONF_STR = {
	// UART token: 57600/115200 for MidiLink + PPP, plus MIDI (31250).  The SCC
	// reaches 31250 through its own WR11/TRxC path (rtl/scc.v); the token is
	// what makes the Main offer the MIDI mode, and the mode it reports back in
	// uart_mode is what gates the user-port MIDI-in merge on serialIn below.
	"MacQuadra800;UART57600:115200,MIDI;",
	// SC0, not S0: the letter after S is a flag, and 'C' is what sets
	// store_name in the Main's option parser -- i.e. what makes MiSTer write
	// config/MacQuadra800.s0 and re-mount the image on the next core start. With a
	// plain S0 the mount works but is forgotten every boot, so the disk had to
	// be picked from the OSD by hand each time and the deploy's slot-0 seed was
	// inert. Every sibling Mac core (MacLC, MacLCII, MacIIvi, MacPlus,
	// LBMacTwo) uses SC0 for this reason.
	"SC0,HDAVHD,Mount SCSI disk 0;",
	"SC1,HDAVHD,Mount SCSI disk 1;",
	// slot 2 is the 512-byte PRAM image (rtl/rtc3430042.sv's XPRAM): loaded
	// before the machine leaves reset, saved back when the guest changes it
	"SC2,NVR,Mount PRAM;",
	// slot 4 is the CD-ROM (SCSI ID 3).  CUE/BIN/CHD need the Main fork's
	// Mac CD layer (support/mac/mac_cdrom.cpp), which serves them as a flat
	// 2048-byte-sector disc plus a TOC blob; ISO/TOAST work on a stock Main.
	"SC4,ISOTO*CUEBINCHD,Mount CD-ROM;",
	"-;",
	"O[4:3],RAM (on reset),32MB,64MB,128MB;",
	// The monitor on the DA-15.  The ROM samples the DAFB sense lines once
	// at boot and QuickDraw lays out for that geometry, so this is latched
	// under reset like the RAM size (MacLC does the same).
`ifndef VIDEO_512_OFF
	"O[5],Monitor (on reset),13in 640x480,12in 512x384;",
`endif
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[13:12],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	// Built-in Ethernet (rtl/sonic_mbx.sv + the Main fork's support/mac).  Off by
	// default: the machine is then bit for bit the one without it.  The core reads
	// only [6]; the interface choice is the Main's.  The guest's MAC is 08:00:07 +
	// the last three octets of the MiSTer's own, so there is nothing else to set.
`ifndef ETHERNET_OFF
	"-;",
	"O[6],Ethernet (on reset),Off,On;",
	"O[8:7],Net interface,eth0,eth1,wlan0,tap0;",
	"-;",
	"O[46:44],ADB controller (on reset),Gravis GamePad,None,Gravis MouseStick II,Gravis Firebird,SideWinder 3D Pro;",
	"O[47],Stick moves pointer,No,Yes;",
`endif
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	"-;",
	"P1,MT32-pi;",
	"P1-;",
	"P1O[35],Use MT32-pi,Yes,No;",
	"P1O[36],Synth,Munt,FluidSynth;",
	"P1O[38:37],Munt ROM,MT-32 v1,MT-32 v2,CM-32L;",
	"P1O[41:39],SoundFont,0,1,2,3,4,5,6,7;",
	"P1O[43:42],Show Info,No,Yes,LCD-On,LCD-Auto;",
	// "Show Info = Yes" popup strings, indexed 1-based by mt32_info_disp
	"I,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"v,0;",
	"J,Trigger,Thumb,Button 3,Button 4,Button 5,Base 1,Base 2,Base 3;",
	"jn,A,B,X,Y,L,R,Select,Start;",
	"jp,B,A,Y,X,L,R,Select,Start;",
	"V,v",`BUILD_DATE
};

## MacSE30 (MacSE30_MiSTer/MacSE30.sv)
localparam CONF_STR = {
`ifdef MACIICX
	"MACIICX;;",
`else
	"MACSE30;;",
`endif
	"-;",
	"S0,DSKIMG,Mount Internal Floppy;",
`ifdef SE30_EXT_DRIVE
	"S1,DSKIMG,Mount External Floppy;",
`endif
	"-;",
	"SC2,IMGVHD,Mount SCSI-0;",
	"SC3,IMGVHD,Mount SCSI-1;",
`ifndef SE30_NO_CDROM
	"SC4,ISOTO*,Mount CD-ROM;",
`endif
	"-;",
	"SC5,NVR,Mount PRAM;",
	"-;",
	"O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[9:8],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"O[4],Memory,8 MB,16 MB;",
	"O[7:6],Mouse speed,Normal,/2,/4,/8;",
`ifdef MACIICX
	"O[5],Power switch,Normal,Locked on;",
`endif
	"-;",
	"R[0],Reset & Apply Memory;",
	"R[3],Wipe PRAM (erases settings!);",
	"v,0;",
	"V,v",`BUILD_DATE
};

## Minimig (Minimig-AGA_MiSTer/Minimig.sv)
localparam CONF_STR = {
	"Minimig;UART115200:230400,MIDI;",
	"J,Red(Fire),Blue,Yellow,Green,RT,LT,Pause;",
	"jn,A,B,X,Y,R,L,Start;",
	"jp,B,A,X,Y,R,L,Start;",
	"-;",
	"I,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"V,v",`BUILD_DATE
};

## MultiComp (MultiComp_MiSTer/MultiComp.sv)
parameter CONF_STR = {
	"MultiComp;;",
	"S,IMG;",
	"OF,Reset after Mount,No,Yes;", 
	"-;",
	"O68,CPU-ROM,Z80-CP/M,Z80-BASIC,6502-Basic,6809-Basic,6809-Forth;",
	"-;",
	"O9B,Baud Rate tty,115200,38400,19200,9600,4800,2400;",
	"OC,Serial Port,Console Port,User IO Port;",
	"OE,Flow Control,None,RTS/CTS;",  // New flow control option
	"-;",
	"RE,Reset;",
	"V,v",`BUILD_DATE
};

## NeXT (NeXT_MiSTer/NeXT.sv)
localparam CONF_STR = {
	"NeXT;;",
	// SC slots: main remembers the mounted image in config/NeXT.s<n> and
	// re-mounts it at core start, so the ROM auto-boots from the disk.
	// The removable media (CD-ROM, floppy, MO) stay plain S slots.
	"F1,BINROM,Boot ROM;",
	"SC0,VHDIMG,SCSI Disk 0;",
	"SC1,VHDIMG,SCSI Disk 1;",
	"S3,ISOCUEBINCHD,CD-ROM;",
	"S4,IMGIMAFLPVFDFD ,Floppy;",
	"S5,IMGMO OD ,Magneto-optical;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[54:52],Network,Off,eth0,eth1,macvlan,tap0;",
	"O[58],Ethernet cable,Connected,Disconnected;",
	"O[59],Audio input,Silence,ADC;",
	"O[57:55],Boot device,Auto,Disk,Floppy,Network,ROM Default,Optical,CD-ROM probe;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	"v,0;",
	"V,v",`BUILD_DATE
};

## NeXT-Color (NeXT-Color_MiSTer/NeXT-Color.sv)
localparam CONF_STR = {
	"NeXT-Color;;",
	"-;",
	// SC slots: Main remembers the mounted image in config/NeXT-Color.s<n>
	// and re-mounts it at core start, so the ROM can boot from the disk.
	// Slot n = SCSI target n; slot 3 is the CD-ROM (Main's NEXT_CDROM_SLOT,
	// which also carries the target-response windows, rtl/tc_scsi.sv).
	"SC0,HDAVHDIMG,SCSI disk 0;",
	"SC1,HDAVHDIMG,SCSI disk 1;",
	"S3,ISOCUEBINCHD,CD-ROM;",
	// slot 4: the 2.88 MB floppy drive (rtl/next_floppy.sv; 720K, 1.44M and
	// 2.88M images, the geometry follows the size).  Same slot and
	// extensions as the mono NeXT core.
	"S4,IMGIMAFLPVFDFD ,Floppy;",
	"-;",
	// The first entry is the default.  32 MB fits every SDRAM module (64 MB
	// needs a 64 or 128 MB module, 128 MB a 128 MB one); "v,1" below drops
	// the settings saved with the old order (64 MB, Prompt, Normal first).
	"O[4:3],Memory,32 MB,64 MB,128 MB,16 MB;",
	"O[5],Power-on self test,On,Off;",
	"O[8:6],Boot device,SCSI disk,Prompt (NeXT>),Network,Floppy;",
	// Ethernet: Main's next_enet daemon reads the mode from status[54:52]
	// (0 off, 1 eth0 shared + MAC filter, ...); this switch is bit 52 alone,
	// so Connected = mode 1.  rtl/tc_enet.sv: a twisted-pair link.
	"O[52],Ethernet,Disconnected,Connected;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	// V-Integer first (the default): 1:1 1120x832 on a 1080p display.
	"O[125:123],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"F1,ROMBIN,Load boot ROM;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	"v,1;",
	"V,v",`BUILD_DATE
};

## Neon (Neon_MiSTer/Neon.sv)
localparam CONF_STR = {
	"Neon;;",
	"-;",
	"S0,DSK,Floppy FD0;",
	"S1,DSK,Floppy FD1;",
	"S2,IMG,Hard disk;",
	"-;",
	"O[3:2],RAM,4 MB,512 KB,1 MB,2 MB;",
	"O[4],Timer IRQ5,50 Hz frame,64 Hz clock;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	"V,v",`BUILD_DATE
};

## ORAO (Orao_MiSTer/ORAO.sv)
localparam CONF_STR = 
{
   "ORAO;;",
   "-;",
   "F,TAP;",
   "-;",
	"O2,Screen Color,White,Green;",
   "O1,Aspect Ratio,4:3,16:9;",
   "-;",
   "T6,Reset;",
   "V,v0.9.",`BUILD_DATE
};

## Ondra_SPO186 (OndraSPO186_MiSTer/Ondra_SPO186.sv)
localparam CONF_STR = {
   "Ondra SPO 186;;",
   "-;",
   "O56,ROM,ViLi,Tesla v5,Ondra PLUS 1.4,Test ROM;",
   "-;",
   "O7,ADC line pass through,On,Off;",
   "O8,Cassette out to audio,On,Off;",
   "-;",
   "O9,Ondra SD,On,Off;",
   "OA,Ondra Melodik,On,Off;",
   "-;",
   "R0,Reset Ondra;",
   "J,Fire 1;",
   "V,v",`BUILD_DATE
};

## Oric (Oric_MiSTer/Oric.sv)
localparam CONF_STR = {
	"Oric;SS3E000000:200000;",
	"FS1,TAP,Load TAP file;",
	"FS4,SNA,Load Snapshot;",
	"h0T[53],Rewind Tape;",
	"-,F1-F4 Save / F5-F8 Restore State;",
	"-;",
	"h6S0,NIBDSKDO ,Mount Drive A:;",
	"h6S1,NIBDSKDO ,Mount Drive B:;",
	"H6S0,DSK,Mount Drive A:;",
	"H6S1,DSK,Mount Drive B:;",
	"H6S2,DSK,Mount Drive C:;",
	"H6S3,DSK,Mount Drive D:;",
	"H2O[17],Drive A Write Protect,Off,On;",
	"h2-,Drive A is Write Protected;",
	"H3O[18],Drive B Write Protect,Off,On;",
	"h3-,Drive B is Write Protected;",
	"H6H4O[19],Drive C Write Protect,Off,On;",
	"H6h4-,Drive C is Write Protected;",
	"H6H5O[20],Drive D Write Protect,Off,On;",
	"H6h5-,Drive D is Write Protected;",
	"-;",
	"P1,Settings;",
	"P1O[6:5],FDD Controller,Auto,Off,On;",
	"P1FC2,ROM,Load Alternative Bios;",
	"P1-;",
	"P1O[51:50],Tape Audio,Mute,Low,High;",
	"P1O[52],Tape Input,File,ADC;",
	"P1O[59:58],Tape Load,Fast,Ultra,Off;",
	"P1O[57],Autoload TAP,On,Off;",
	"P1O[60],Named CLOAD Rewind,On,Off;",
	"P1-;",
	"P1O[55:54],Joystick Adapter,None,PASE,IJK;",
	"P1-;",
	"P1O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O[12:10],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"P1O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P1-;",
	"P1O[9:8],Audio,Stereo,ABC (West Europe),ACB (East Europe);",
	"H1O[4:3],ROM,Pravetz 8D,Oric Atmos,Oric 1;",
	"h1O[4:3],ROM,Pravetz 8D,Oric Atmos,Oric 1,Loadable Bios;",
	
	"-;",
	"R0,Reset & Apply;",
	"J,Fire;",
	"I,",
	"Saved state 1,",
	"Saved state 2,",
	"Saved state 3,",
	"Saved state 4,",
	"Restored state 1,",
	"Restored state 2,",
	"Restored state 3,",
	"Restored state 4,",
	"Slot is empty;",
	"V,v",`BUILD_DATE
};

## PC88 (PC88_MiSTer/PC8801SR.sv)
parameter CONF_STR = {
	"PC8801;;",
	"-;",
	"O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O34,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"OHJ,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"OOP,Video timing,31kHz,24kHz,15kHz;",
	"OS,Color levels,Linear,Real PC-88;",
	"-;",
	"O78,Mode,N88V2,N88V1H,N88V1S,N;",
	"O9,Speed,4MHz,8MHz;",
	"-;",
	"S0,D88,FDD0;",
	"S1,D88,FDD1;",
	"S4,CMTT88,Tape;",
	"OE,Tape Record,Off,On;",
	"RF,SYNC FD0;",
	"RG,SYNC FD1;",
	"-;",
	"OA,Basic mode,Basic,Terminal;",
	"OB,Cols,80,40;",
	"OC,Lines,25,20;",
	"OD,Disk boot,Enable,Disable;",
	"-;",
	"OK,Input,Joypad,Mouse;",
	"OLM,Sound Board,Normal(SR),OnBoard(FA/MA+),Add-on (SB2);",
	"OTU,Stereo Mix,None,25%,50%,100%;",
	"OQR,RAM,Fx (SR/FR/FH/FA),Mx (MR/MH/MA/MC),512KB;",
	"-;",
	"R6,Reset;",
	"J,Fire 1,Fire 2;",
	"I,",
	"Tape: recording,",
	"Tape: saved,",
	"Tape: OVERRUN - stopped,",
	"Tape: image full - stopped,",
	"Tape: image changed - stopped,",
	"Tape: no writable image,",
	"Tape: image is read-only,",
	"Tape: not a .t88 image,",
	"Tape: image too small,",
	"Tape: unsupported serial mode;",
	"V,v",`BUILD_DATE
};

## PCXT (PCXT_MiSTer/PCXT.sv)
`define CONF_STR_SYSTEM (`SYSTEM_VARIANT_TANDY ? (`ENABLE_MIDI ? "Tandy1000;UART115200:115200,MIDI;" : "Tandy1000;UART115200:115200;") : (`ENABLE_MIDI ? "PCXT;UART115200:115200,MIDI;" : "PCXT;UART115200:115200;"))
`endif
`ifndef ENABLE_TANDY_VIDEO
`define ENABLE_TANDY_VIDEO 0
`endif
`ifndef ENABLE_TANDY_AUDIO
`define ENABLE_TANDY_AUDIO 0
`endif
`ifndef ENABLE_TANDY_KBD
`define ENABLE_TANDY_KBD 0
`endif
`ifndef ENABLE_A000_UMB
`define ENABLE_A000_UMB 0
`endif
`ifndef ENABLE_CGA
`define ENABLE_CGA 1
`endif
`ifndef ENABLE_HGC
`define ENABLE_HGC 0
`endif
`ifndef ENABLE_OPL2
`define ENABLE_OPL2 0
`endif
`ifndef ENABLE_CMS
`define ENABLE_CMS 0
`endif
`ifndef ENABLE_EMS
`define ENABLE_EMS 0
`endif

module emu
    (
`include "sys/emu_ports.vh"
    );

    ///////// Default values for ports not used in this core /////////

    assign ADC_BUS  = 'Z;
    //assign USER_OUT = '1;
    //assign {UART_RTS, UART_TXD, UART_DTR} = 0;
    assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
    //assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML, SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;
    assign SDRAM_CLK = clk_chipset;
    assign VGA_SCALER = 0;
    assign VGA_DISABLE = 0;
    assign HDMI_FREEZE = 0;
    assign HDMI_BLACKOUT = 0;
    assign HDMI_BOB_DEINT = 0;

    assign LED_DISK = 0;
    assign LED_POWER = 0;
    assign BUTTONS = 0;

    assign LED_USER = 0;
    //led fdd_led(clk_cpu, |mgmt_req[7:6], LED_USER);


    //////////////////////////////////////////////////////////////////
    // Status Bit Map:
    //              Upper                          Lower
    // 0         1         2         3          4         5         6
    // 01234567890123456789012345678901 23456789012345678901234567890123
    // 0123456789ABCDEFGHIJKLMNOPQRSTUV 0123456789ABCDEFGHIJKLMNOPQRSTUV
    // XXXXX XXXXXXXXXXXXXXXXXXXXXXXXXX XXXXXXXX
    // status[64]: CRTC Frame Stacking (Native / Rebuilt)

	`include "build_id.v"

    localparam CONF_STR_HGC = ((`ENABLE_HGC && `ENABLE_CGA) ? "P1oC,PCXT CGA Graphics,Yes,No;P1oD,PCXT Hercules Graphics,Yes,No;P1O4,PCXT 1st Video,CGA,Hercules;P1-;" : "");
    localparam CONF_STR_ROM = (`ROM_IS_TANDY ? "P1FC1,ROM,Tandy BIOS:;P1-;" : "P1FC0,ROM,PCXT BIOS:;");
    localparam CONF_STR_CMS = (`ENABLE_CMS ? "P2OA,C/MS Audio,Enabled,Disabled;" : "");
    localparam CONF_STR_OPL2 = (`ENABLE_OPL2 ? "P2oAB,OPL2,Adlib 388h,SB FM 388h/228h, Disabled;" : "");
    localparam CONF_STR_TANDY_AUDIO = (`ENABLE_TANDY_AUDIO ? "P2o23,Tandy Volume,1,2,3,4;" : "");
    // P2o23 occupies status[35:34].  It is shared with Tandy Volume in the
    // optional Tandy-audio build, so keep the CRT converter out of that
    // mutually exclusive configuration rather than aliasing two controls.
    localparam CONF_STR_CRT = (`ENABLE_TANDY_AUDIO ? "" : "P2o23,350-line CRT,Native,480i 15 kHz,240p 15 kHz;");
    localparam CONF_STR_EMS = (`ENABLE_EMS ? "P3OB,Lo-tech 2MB EMS,Enabled,Disabled;P3OCD,EMS Frame,C000,D000,E000;P3-;" : "");
    localparam CONF_STR_A000 = (`ENABLE_A000_UMB ? "P3o9,A000 UMB,Enabled,Disabled;P3-;" : "");
    localparam CONF_STR_MIDI = (`ENABLE_MIDI ? "P3O6,USER I/O,MIDI,COM2;P3-;h3P4,MT32-pi;h3P4-;h3P4OD,Use MT32-pi,Yes,No;h3P4-;h3P4o9,MT32-pi Mode,MT-32,General MIDI;h3P4O34,MT32-pi ROM,MT-32 v1,MT-32 v2,CM-32L,Reserved;h3P4oSU,MT32-pi SoundFont,#0,#1,#2,#3,#4,#5,#6,#7;h3P4-;h3P4r8,Reset Hanging Notes;h3P4-;" : "");
    // This line is shown at the top of the menu while the required PCXT BIOS
    // is absent, so the splash hold has an actionable explanation.
    localparam CONF_STR_HALT = "h4-,HALTED: no PCXT BIOS selected;";
    localparam CONF_STR_INFO = {
        "I,",
        "No PCXT BIOS selected\n",
        "Machine halted\n",
        "OSD: System & BIOS;"
    };

	localparam CONF_STR = {
		`CONF_STR_SYSTEM,
		CONF_STR_HALT,
		"S0,IMGIMAVFD,Floppy A:;",
		"S1,IMGIMAVFD,Floppy B:;",
		"OJK,Write Protect,None,A:,B:,A: & B:;",
		"-;",
		"S2,VHD,IDE 0-0;",
		"S3,VHD,IDE 0-1;",
		"OLM,2nd SD card,Disable,IDE 0-0,IDE 0-1;",
		"-;",
		"OHI,CPU Speed,4.77MHz,7.16MHz,9.54MHz,Max;",
		"-;",
		"P1,System & BIOS;",
		"P1-;",
		CONF_STR_HGC,
		"P1O7,Boot Splash Screen,Yes,No;",
		"P1oV,CPU Type,8088,8086;",
		"P1oL,Fake 286 FLAGS,Off,On;",
		"P1-;",
		CONF_STR_ROM,
		"P1FC2,ROM,EC00 BIOS:;",
		"P1-;",
		"P1OUV,BIOS Writable,None,EC00,Main,All;",
		"P1-;",	
		"P2,Audio & Video;",
		"P2-;",
		CONF_STR_CMS,
		CONF_STR_OPL2,
		"P2o01,Speaker Volume,1,2,3,4;",
		CONF_STR_TANDY_AUDIO,
		"P2o45,Audio Boost,No,2x,4x;",
		"P2o67,Stereo Mix,none,25%,50%,100%;",
		"P2-;",
		"d6P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
		"d6P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",        
		"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
		"P2oPR,HSync Width,Auto,1,2,3,4,5,6,7;",
        "P2-;",
		"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
		"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
		"P2OT,Border,No,Yes;",
		"h5P2O[65],Border CRT offsets,No,Yes;",
		"P2O[64],CRTC Frame Stacking,Native,Rebuilt;",
		"P2o8,Composite video,Off,On;",
		"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
		CONF_STR_CRT,
		"P2-;",
		"P3,Hardware;",
		"P3-;",
		CONF_STR_EMS,
		CONF_STR_A000,
		"P3ONO,Joystick 1, Analog, Digital, Disabled;",
		"P3OPQ,Joystick 2, Analog, Digital, Disabled;",
		"P3OR,Sync Joy to CPU Speed,No,Yes;",
		"P3OS,Swap Joysticks,No,Yes;",
		"P3-;",
		CONF_STR_MIDI,
		"-;",
		"R0,Reset & apply settings;",
		"J,Fire 1,Fire 2;",
		CONF_STR_INFO,
		"V,v",`BUILD_DATE
	};

## PCXT-EGA (PCXT-EGA_MiSTer/PCXT-EGA.sv)
`define CONF_STR_SYSTEM (`ENABLE_MIDI ? "PCXT-EGA;UART115200:115200,MIDI;" : "PCXT-EGA;UART115200:115200;")
`endif
`ifndef ENABLE_OPL2
`define ENABLE_OPL2 0
`endif
`ifndef ENABLE_CMS
`define ENABLE_CMS 0
`endif
`ifndef ENABLE_EMS
`define ENABLE_EMS 0
`endif
`ifndef ENABLE_UMB
`define ENABLE_UMB 0
`endif
`ifndef ENABLE_TANDY_AUDIO
`define ENABLE_TANDY_AUDIO 0
`endif
`ifndef ENABLE_SB
`define ENABLE_SB 0
`endif

module emu
    (
`include "sys/emu_ports.vh"
    );

    ///////// Default values for ports not used in this core /////////

    assign ADC_BUS  = 'Z;
    //assign USER_OUT = '1;
    //assign {UART_RTS, UART_TXD, UART_DTR} = 0;
    assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
    //assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML, SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;
    assign SDRAM_CLK = clk_chipset;
    assign VGA_SCALER = 0;
    assign VGA_DISABLE = 0;
    assign HDMI_FREEZE = 0;
    assign HDMI_BLACKOUT = 0;
    assign HDMI_BOB_DEINT = 0;

    assign LED_DISK = 0;
    assign LED_POWER = 0;
    assign BUTTONS = 0;

    assign LED_USER = 0;
    //led fdd_led(clk_cpu, |mgmt_req[7:6], LED_USER);


    //////////////////////////////////////////////////////////////////
    // Status Bit Map:
    //              Upper                          Lower
    // 0         1         2         3          4         5         6
    // 01234567890123456789012345678901 23456789012345678901234567890123
    // 0123456789ABCDEFGHIJKLMNOPQRSTUV 0123456789ABCDEFGHIJKLMNOPQRSTUV
    // XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX XXXXXXXXXXXXXXXXXXXXXXX.....XXXX
    //
    // The first 64 status bits are fully allocated. The extended status
    // vector continues at bit 64; its first six bits carry the VSync and
    // HSync width options and bit 70 carries the Sound Blaster IRQ choice,
    // so those settings remain available to the OSD and are also persisted
    // by the normal core CFG file.
    //
    // Spend the extended status bits carefully. Anything already reachable through XTEGACTL
    // is a candidate to give its bit back the same way - Sync Joy to CPU
    // Speed, Fake 286 FLAGS and MT32-pi Mode are each one more bit, and the
    // CRT H and V offsets another seven, all without losing the setting.

	`include "build_id.v"

    localparam CONF_STR_ROM = "P1FC0,ROM,PCXT BIOS:;";
    // The Sound Blaster and the C/MS both live at 220h and collide on
    // 226h/227h, where one puts its DSP reset and the other its detection
    // register, so at most one of them can answer. When both are built that
    // is one three-way choice rather than two switches that quietly override
    // each other.
    //
    // It costs two status bits and there were none left - all 64 are spoken
    // for - so bit 28 came from Swap Joysticks, which is gone from the menu
    // entirely. That setting is not lost: XTEGACTL still carries it in
    // reg_inp[5:4], the same way Tandy audio has always been reached. With
    // only one of the two cards built there is nothing to choose between,
    // so bit 29 stays the plain enable it has always been and bit 28 is
    // simply free.
    localparam CONF_STR_A220 =
        (`ENABLE_CMS && `ENABLE_SB) ? "P2OST,Audio 220h,C/MS,Sound Blaster,Disabled;" :
        (`ENABLE_SB)                ? "P2OT,Sound Blaster,Enabled,Disabled;"          :
        (`ENABLE_CMS)               ? "P2OT,C/MS Audio,Enabled,Disabled;"             : "";
    localparam CONF_STR_OPL2 = (`ENABLE_OPL2 ? "P2oAB,OPL2,Adlib 388h,SB FM 388h/228h, Disabled;" : "");
    localparam CONF_STR_TANDY = (`ENABLE_TANDY_AUDIO ? "P2oN,Tandy Sound,Disabled,Enabled;" : "");
    localparam CONF_STR_EMS = (`ENABLE_EMS ? "P3O5,2MB EMS D000-DFFF,Enabled,Disabled;P3-;" : "");
    localparam CONF_STR_UMB = (`ENABLE_UMB ? "P3OC,UMB C400-CFFF,Enabled,Disabled;P3-;" : "");
    // MPU-401 comes first in this block: it is the master switch the other
    // two settings depend on - USER I/O routing and the whole MT32-pi page
    // are moot once the card itself is off the bus, same as a real MPU-401
    // being physically removed. A game that probes 330h then falls through
    // to Adlib/OPL2 instead of finding a Roland device with nothing behind it.
    localparam CONF_STR_MIDI = (`ENABLE_MIDI ? "P3oO,MPU-401,Enabled,Disabled;P3-;P3O6,USER I/O,MIDI,COM2;P3-;h3P4,MT32-pi;h3P4-;h3P4OD,Use MT32-pi,Yes,No;h3P4-;h3P4o9,MT32-pi Mode,MT-32,General MIDI;h3P4O34,MT32-pi ROM,MT-32 v1,MT-32 v2,CM-32L,Reserved;h3P4oSU,MT32-pi SoundFont,#0,#1,#2,#3,#4,#5,#6,#7;h3P4-;h3P4r8,Reset Hanging Notes;h3P4-;" : "");

    // Menumask bits 4 and 5 mark a missing PCXT or EGA BIOS.  The machine is
    // held in reset until both are present, so say so at the top of the menu
    // rather than leaving the user to guess why nothing boots.
    localparam CONF_STR_HALT = {
		"h4-,HALTED: no PCXT BIOS selected;",
		"h5-,HALTED: no EGA BIOS selected;"
	};

    // Read back by the framework through info_req/info and drawn as an OSD
    // notice.  The box is 32 columns wide; commas separate the two messages,
    // so neither may contain one.
    localparam CONF_STR_INFO = {
		"I,",
		"No PCXT BIOS selected\n",
		"Machine halted\n",
		"OSD: System & BIOS,",
		"No EGA BIOS selected\n",
		"Machine halted\n",
		"OSD: System & BIOS,",
		"That setting is applied\n",
		"when the machine resets;"
	};

    localparam CONF_STR = {
		`CONF_STR_SYSTEM,
		CONF_STR_HALT,
		"S0,IMGIMAVFD,Floppy A:;",
		"S1,IMGIMAVFD,Floppy B:;",
		"OJK,Write Protect,None,A:,B:,A: & B:;",
		"-;",
		"S2,VHD,IDE 0-0;",
		"S3,VHD,IDE 0-1;",
		"OLM,2nd SD card,Disable,IDE 0-0,IDE 0-1;",
		"-;",
		"OHI,CPU Speed,4.77MHz,7.16MHz,9.54MHz,Max;",
		"-;",
		"P1,System & BIOS;",
		"P1-;",
		"P1O7,Boot Splash Screen,Yes,No;",
		"P1oV,CPU Type,8088,8086;",
		"P1oL,Fake 286 FLAGS,Off,On;",
		"P1-;",
		CONF_STR_ROM,
		"P1FC2,ROM,EC00 BIOS:;",
		"P1FC3,ROM,EGA BIOS:;",
		"P1-;",
		"P1OUV,BIOS Writable,None,EC00,Main,All;",
		"P1-;",	
		"P2,Audio & Video;",
		"P2-;",
		CONF_STR_A220,
		CONF_STR_OPL2,
		CONF_STR_TANDY,
		"P2o01,Speaker Volume,1,2,3,4;",
		"P2o45,Audio Boost,No,2x,4x;",
		"P2o67,Stereo Mix,none,25%,50%,100%;",
		"P2-;",
		"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
		"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",        
        "P2O[66:64],VSync Width,Auto,1,2,3,4,5,6,7;",
        "P2O[69:67],HSync Width,Auto,1,2,3,4,5,6,7;",
        "P2-;",
		"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
		"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
		"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
		"P2OA,VGA 13h+ CRT,Native 70Hz,TV 60Hz;",
		"P2o23,350-line CRT,Native,480i 15 kHz,240p 15 kHz;",
		"P2-;",
		"P3,Hardware;",
		"P3-;",
		"P3oCD,Monitor,5154/ECD,5153/CGA,5151/MDA;",
		"P3-;",
		CONF_STR_EMS,
		CONF_STR_UMB,
		"P3ONO,Joystick 1, Analog, Digital, Disabled;",
		"P3OPQ,Joystick 2, Analog, Digital, Disabled;",
		"P3OR,Sync Joy to CPU Speed,No,Yes;",
		"P3oM,Swap Joysticks,No,Yes;",
		(`ENABLE_SB ? "P3O[70],Sound Blaster IRQ,5,7;" : ""),
		"P3-;",
		CONF_STR_MIDI,
		"-;",
		"R0,Reset & apply settings;",
		"J,Fire 1,Fire 2;",
		CONF_STR_INFO,
		"V,v",`BUILD_DATE
	};

## PCjr (PCjr_MiSTer/PCjr.sv)
    localparam CONF_STR_SYSTEM = "PCjr;UART115200:115200;";
    localparam CONF_STR_ROM = "P1FC1,ROM,PCjr BIOS:;P1-;";
    localparam CONF_STR_CART = "P1FC2,JRC,Cartridge 1 (JRC):;P1FC3,JRC,Cartridge 2 (JRC):;";
    localparam CONF_STR_TAPE = "d1P1FC4,JRT,Cassette Tape (JRT):;";
    localparam CONF_STR_TANDY_AUDIO = "P2o23,PCjr Volume,1,2,3,4;";

    localparam CONF_STR = {
		CONF_STR_SYSTEM,
		"S0,IMGIMAVFD,Floppy A:;",
		"OJ,Write Protect,No,Yes;",
		"-;",
		"O36,RAM Size,128KB,192KB,256KB,320KB,384KB,448KB,512KB,576KB,640KB;",
		"-;",
		"P1,System & BIOS;",
		"P1-;",
		"P1O7,Boot Splash Screen,Yes,No;",
		"P1-;",
		CONF_STR_ROM,
        "P1-;",
		CONF_STR_CART,
		"P1-;",	
        CONF_STR_TAPE,
		"P1-;",
		"P2,Audio & Video;",
		"P2-;",
		"P2o01,Speaker Volume,1,2,3,4;",
        "d2P2o9,Tape sound,Disabled,Enabled;",
		CONF_STR_TANDY_AUDIO,
		"P2o45,Audio Boost,No,2x,4x;",
		"P2o67,Stereo Mix,none,25%,50%,100%;",
		"P2-;",
		"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
		"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
		"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
		"P2oPR,HSync Width,Auto,1,2,3,4,5,6,7;",
        "P2-;",
		"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
		"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
		"P2OT,Border,No,Yes;",
		"P2o8,Composite video,Off,On;",
		"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
		"P2-;",
		"P3,Hardware;",
		"P3-;",
		"P3ONO,Joystick 1, Analog, Digital, Disabled;",
		"P3OPQ,Joystick 2, Analog, Digital, Disabled;",
		"P3OR,Sync Joy to CPU Speed,No,Yes;",
		"P3OS,Swap Joysticks,No,Yes;",
		"P3-;",	
		"-;",
		"R0,Reset & apply settings;",
		"J,Fire 1,Fire 2;",
		"V,v",`BUILD_DATE
	};

## PDP1 (PDP1_MiSTer/PDP1.sv)
localparam CONF_STR = {
   "PDP1;;",
   "-;",
   "F1,PDPRIMBIN,Load PDP/RIM/BIN;",
   "T[5],Enable RIM mode;",
   "T[6],Disable RIM mode;", 
   "-;",
   "R[7],Reset;",
   "T[0],Reset;",
   "R[0],Reset and close OSD;",
   "-;",
   "O[1],Aspect Ratio,Original,Wide;",
   "O[4],Hardware multiply,No,Yes;",
   "-;",
   "O[8],Var. brightness,Yes,No;",  
   "O[9],CRT wait,No,Yes;",
   "-;", 
   "J,Left,Right,Thrust,Fire,HyperSpace;",
   "v,1;",
   "V,v",`BUILD_DATE
};

## PET2001 (PET2001_MiSTer/PET2001.sv)
localparam CONF_STR = 
{
	"PET2001;;",
	"HAO[20],Drive #8 Type,8250,4040;",
	"H0H2S0,D80D82, Mount #8.0;",
	"H0H2S1,D80D82, Mount #8.1;",
	"H0h2S0,D64, Mount #8.0;",
	"H0h2S1,D64, Mount #8.1;",
	"H0-;",
	"HBO[21],Drive #9 Type,8250,4040;",
	"H1H3S2,D80D82, Mount #9.0;",
	"H1H3S3,D80D82, Mount #9.1;",
	"H1h3S2,D64, Mount #9.0;",
	"H1h3S3,D64, Mount #9.1;",
	"H1-;",
	"F1,TAPPRG;",
	"-;",
	"P1,Hardware;",
	"P1O[17:16],Enable Drive #8,If Mounted,Always,Never;",
	"P1O[19:18],Enable Drive #9,If Mounted,Always,Never;",
	"P1R[15],Reset Drives;",
	"P1-;",
	"P1O78,TAP mode,Fast,Normal,Normal+Sound;",
	"P1O9A,CPU Speed,Normal,x2,x4,x8;",
	"P1O3,Diag,Off,On(needs Reset);",
	"-;",
	"O2,Screen Color,White,Green;",
	"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O46,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"FC2,ROM,Load System ROM;",
	"-;",
	"R0,Reset;",
	"V,v",`BUILD_DATE
};

## PMD85 (PMD85_MiSTer/PMD85.sv)
localparam CONF_STR = {
	"PMD85;;",
	"-;",	
	"F1,rmmmrm,Load to ROM Pack;",
	"R7,Eject ROM Pack;",
	"-;",
   "O8,PMD85 version,2A,3;",
	"O12,Video,Green,TV,RGB,ColorACE;",
	"D6O9,Sound,Beeper,Beeper + MIF85 (K2);",
	"O45,Joystick,None,K3,K4;",
	"D9O6,Mouse,None,K2;",		
	"-;",	
	"R0,Reset PMD;",
	"J,Fire;",
	"V,v",`BUILD_DATE 
};

## QL (QL_MiSTer/QL.sv)
parameter CONF_STR = {
	"QL;;",
	"SC0,WIN,Mount HD image;",	
	"-;",
	"F2,MDV,Load MDV image;",
	"O2,MDV direction,normal,reverse;",
	"-;",
	"F4,ROM,Load OS;",
	"-;",
	"O3,Video mode,PAL,NTSC;",
	"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O9A,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O78,CPU speed,QL,16 Mhz,24 Mhz,Full;",
	"O45,RAM,128k,640k,896k,4096k;",
	"R0,Reset & unload MDV;",
	"V,v",`BUILD_DATE
};

## RX78 (RX-78_MiSTer/RX78.sv)
localparam CONF_STR = {
	"RX78;;",
	"-;",
	"F1,BIN,Load Cartridge;",
	"-;",
	"OA,Swap Joysticks,Off,On;",
	"OB,EXT RAM,Off,On;",
	"OC,Pause,Off,On;",
	"-;",
	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"ODF,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;", 
	"-;",
	"R0,Reset;",
	"J,Fire1,Fire2,Start;",
	"jn,A,B,Start;",
	"V,v",`BUILD_DATE 
};

## SAMCoupe (SAM-Coupe_MiSTer/SAMCoupe.sv)
localparam CONF_STR = 
{
	"SAMCOUPE;;",
	"-;",
	"S0,DSKMGTIMG,Drive 1;",
	"S1,DSKMGTIMG,Drive 2;",
	"-;",
	"ODE,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"-;",
	"d1OF,Vertical Crop,No,Yes;",
	"OGH,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O67,Stereo mix,none,25%,50%,100%;",
	"-;",
	"O8A,CPU Speed,Normal,6MHz,9.6MHz,12MHz,24MHz;",
	"OBC,ZX Mode Speed,Emulated,Full,Real;",
	"O5,External RAM,on,off;",
	"-;",
	"R0,Reset;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE
};

## SGIIndy (SGIIndy_MiSTer/sgiindy.sv)
localparam CONF_STR = {
	"SGIIndy;;",
	"-;",
	// `SC` RATHER THAN `S`, AND THE C IS THE WHOLE POINT: it makes the mount
	// SURVIVE A CORE RELOAD. The framework parses this string on the ARM side;
	// `S` mounts an image and forgets it, `SC` also writes the path to
	// /media/fat/config/SGIIndy.s<n> when it is selected (Main_MiSTer
	// menu.cpp:2782) and mounts it again from there at every core start
	// (user_io.cpp:977-1006), with no OSD interaction at all - the same shape
	// as boot.rom arriving at index 0.
	//
	// That matters here more than on most cores because this machine takes
	// thirty seconds to draw its boot screen and is relaunched constantly
	// during bring-up, and because the screenshot API does not capture the OSD,
	// so blind menu navigation cannot be verified.
	//
	// The saved path is written when an image is chosen in the OSD.
	// scripts/mount.sh writes it directly instead, because the screenshot API
	// does not capture the OSD and a blind menu walk cannot be verified. The
	// format is not a text file: FileSaveConfig writes the whole of menu.cpp's
	// `char selPath[1024]`, so the path goes in NUL-terminated and zero-padded.
	//
	// The PROM boots from SCSI ID 1 by default, so that slot is the `Pri.` one.
	"SC1,IMGISOCHD,Pri. disk (ID 1);",
	"SC2,IMGISOCHD,Disk (ID 2);",
	"SC3,IMGISOCHD,CD-ROM (ID 6);",
	// Index 0, NOT remembered (`F`, not `FC`): every start loads boot.rom, and
	// this picks another PROM for one session only. See PROM DOWNLOAD below.
	"F0,ROMBIN,Load ROM;",
	"-;",
	"O[13:12],Memory,32MB,48MB,64MB,128MB;",
	"-;",
	"P1,Machine;",
	"P2,Video;",
	"P3,Input;",
	"P4,Audio;",
	"P5,Advanced;",
`ifdef SGIINDY_DEBUG
	"P9,Debug;",
`endif
	"-;",
	"h0-,Changes pending;",
	"R[0],Apply and reset;",

	"P1O[11],Primary caches,On,Off;",

	// FITTING THE GRAPHICS BOARD MOVES THE CONSOLE OFF THE SERIAL PORT (see
	// gfx_present below), so this is the standard's "display source".
	"P2O[10],Display,Graphics,Serial console;",
	"P2O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",

	"P3O[23],Swap Alt/Win keys,No,Yes;",

	// THE AUDIO SWITCH (rtl/sgi/hal2.sv). Off makes HAL2 report itself absent,
	// so the PROM plays no tune and IRIX never loads its audio driver - the
	// machine as it was before there was a sample path.
	"P4O[18],Audio,On,Off;",

	// THE SCSI BLOCK CACHE (docs/design/scsi-block-cache.md; rtl/scsi/scsi_cache.sv). Off makes every
	// block request a single-sector HPS transaction again, as before build
	// 26 - the control for measuring the cache on one bitstream, and the
	// way out if it ever misbehaves. scripts/setopt.sh knows it as scsicache.
	"P5O[17],SCSI cache,On,Off;",

`ifdef SGIINDY_DEBUG
	// A BRING-UP INSTRUMENT, NOT A FEATURE. The first hardware run drew a
	// perfect 1318x1024 raster and a black screen, and black has two causes
	// that look the same from outside: the frame buffer read returning
	// nothing, or the palette answering black for every index. This shows the
	// frame buffer's own colour index as grey with CMAP taken out of the path,
	// and serves index 0x80 for a line-cache miss instead of black - so the
	// three cases finally look different from each other.
	"P9O[14],Video debug,Off,Raw index;",
	// THE OTHER BRING-UP INSTRUMENT. With the graphics board unfitted the
	// machine demonstrably runs - main memory carries szmem's walking-bit
	// patterns - and the ARM's /dev/ttyS1 has counted rx:0 bytes, ever. So the
	// console is absent, and two very different things would look identical
	// from outside: the SCC not transmitting, or nothing between UART_TXD and
	// the HPS UART working at all. This puts a transmitter of the core's own
	// on the pin, in one clock domain or the other.
	"P9O[16:15],UART debug,Off,0x55 clk_sys,0x55 sclk;",
	// The A/B switches setopt.sh calls lookahead, dinstrict, ipf and dpf.
	"P9O[19],DATA IN look-ahead,On,Off;",
	"P9O[20],DATA IN guard,On,Off;",
	"P9O[21],I-fetch prefetch,On,Off;",
	"P9O[22],D-fetch prefetch,On,Off;",
`endif
	"v,1;",
	"V,v",`BUILD_DATE
};

## SharpMZ (SharpMZ_MiSTer/SharpMZ.sv)
localparam CONF_STR =
{
	"SharpMZ;UART9600:19200:38400:4800:2400:1200;",
	"-;",
	// Tape and disk entries stay on the top level: MGL files can only load F/S entries that are
	// not on a sub-page.
	"F2,MZF,Load Direct to RAM;",
	"S0,MZTMZF,Tape Image;",
	"S1,DSKD88D77,Floppy Drive A;",
	"S2,DSKD88D77,Floppy Drive B;",
	"S3,QDFMZQMZTMZF,Quick Disk;",
	"-;",
	"P1,Machine;",
	"P1O[38:35],Model,MZ-80K,MZ-80C,MZ-80A,MZ-1200,MZ-80B,MZ-2000,MZ-700,MZ-800,MZ-1500;",
	"P1O[6:4],CPU Speed,Normal,2x,4x,8x,16x,Fastest;",
	"P1O[30],MZ-80B/2000 Boot Reset,Off,On;",
	"P1O[32],MZ-800 Mode,MZ-700,MZ-800;",
	"P1O[56],Region,Europe,Japan;",
	"P1O[39],MZ-1X03 Joysticks,Off,On;",
	"P1O[51],Keyboard Layout,PC,Sharp;",
	"-;",
	"P2,Tape;",
	"P2T[31],Rewind Tape Image;",
	"P2O[25:24],Tape Buttons,Auto,Off,Play,Record;",
	"P2O[23:21],Fast Tape,Fastest,Off,2x,4x,8x,16x;",
	"P2O[27:26],Sharp ASCII Name,Off,On Save,On Load,Both;",
	"P2O[54:53],Tape Sound,Off,Mix,Only;",
	"P2O[49],Load Direct Start,Program,Reset Only;",
	"P2O[52],Tape Image Start,Tape,Direct;",
	"P2O[59],Auto Load Tape,Off,On;",
	"P2O[55],Tape Messages,On,Off;",
	"P2O[57],Tape Details,Off,On;",
	"P2R[58],Show Tape Details;",
	"-;",
	"P3,Display;",
	"P3O[8:7],Display Type,Default,Mono 80x25,Colour 40x25,Colour 80x25;",
			"P3O[18],VRAM Wait,On,Off;",
	"P3O[43],MZ-800 Border,Off,On;",
	"P3O[46:44],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"-;",
	"P5,Floppy;",
	"P5O[34:33],Floppy Interface,Auto,On,Off;",
	"P5O[40],MZ-800 RAM Disk,Off,64 KB;",
	"P5O[42],Floppy CRC Errors,Ignore,Report;",
	"P5O[48],Drive B Unit,2nd drive,3rd (CP/M 4.1 HD);",
	"-;",
	"P6,Printer;",
	"P6O[47],Printer,None,UART (mister_printerd);",
	"P6O[50],Printer Charset,Sharp,ASCII;",
	"P6-;",
	"P6-,Set UART connection to Printer;",
	"P6-,in the MiSTer UART menu;",
	"-;",
	"P4,ROM and RAM;",
	"P4O[28],User ROM,Off,On;",
	"P4O[29],FDC ROM,Off,On;",
	// System ROM and CGROM are remembered: Main loads the last one chosen at every core start (also boot.rom).
	"P4FC3,ROMBIN,Load System ROM;",
	"P4F4,ROMBIN,Load System RAM;",
	"P4FC6,ROMBIN,Load CGROM;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	// The joystick entries go after every menu entry: placed before Reset they broke the OSD's Reset and
	// Reset and close OSD (testers: "you press reset and nothing happens").
	"J1,Fire 1,Fire 2;",
	"jn,A,B;",
	"v,6;",
	"V,v",`BUILD_DATE
};

## SordM5 (SordM5_MiSTer/sordM5.sv)
localparam CONF_STR = {
	"Sord M5;;",
	"-;",
  "O02,Memory extension,None,EM-5,EM-64,64KBF,64KRX,BrnoMod;",
  "h0O34,Cartrige,None,BASIC-I,BASIC-G,BASIC-F;",
  "h1O5,EM64 mode,64KB,32KB;",
  "h1O6,EM64 mon. deactivate,Dis.,En.;",
  "h1O7,EM64 wp low 32KB,Dis.,En.;",
  "h1O8,EM64 boot on,ROM,RAM;",  
  "h2F1,binROM,Load to ROM;",
  "-;",
  "OI,Tape Input,File,ADC;",
  "H3F2,CAS,Load Tape;",
  "H3O9,Fast Tape Load,On,Off;",
  "H3OA,Tape Sound,On,Off;",
	"-;",
	"-;",
  "OCD,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
  "OEF,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
  "OG,Border,No,Yes;",
  "-;",
	"TH,Reset;",
	"RH,Reset and close OSD;",
	"V,v",`BUILD_DATE 
};

## Specialist (Specialist_MiSTer/Specialist.sv)
localparam CONF_STR =
{
	"SPMX;;",
	"-;",
	"F0,RKS,Load Tape;",
	"S0,ODI,Mount Disk;",
	"-;",
	"O9A,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O78,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"OBC,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O4,CPU Speed,2MHz,4MHz;",
	"O23,Model,Original,MX & Disk,MX;",
	"-;",
	"R6,Cold Reset;",
	"V,v",`BUILD_DATE
};

## Sun-2 (Sun-2_MiSTer/Sun-2.sv)
    localparam CONF_STR = {
        "Sun-2;UART9600;",
        "-;",
        "SC0,IMGVHD,Pri. disk (ID 0);",
        "S1,QIC,Tape (ID 4);",
        "O[4:3],Tape volume,1,2,3;",
        "F0,ROM,Load ROM;",
        "-;",
        "O[11:9],Network interface,eth0,None,eth1,macvlan,tap0;",
        "-;",
        "P1,Machine;",
        "P2,Video;",
        "P3,Input;",
        "P4,Audio;",
        "-;",
        "h0-,Changes pending;",
        "R[0],Apply and reset;",

        "P1O[12],Colour board,On,Off;",
        "P1-;",
        "P1T[13],Stop-A (abort);",

        "P2O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
        "P2O[6:5],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",

        "P3O[14],Swap Alt/Win keys,No,Yes;",

        "P4O[8:7],Keyboard bell,Normal,Loud,Quiet,Off;",

        "v,1;",
        "V,v",`BUILD_DATE
    };

## Sun-3 (Sun-3_MiSTer/Sun-3.sv)
    localparam CONF_STR = {
        "Sun-3;UART9600;",
        "-;",
        "SC0,IMGVHD,Pri. disk (ID 0);",
        "SC1,IMGVHD,Disk (ID 1);",
        "S2,QIC,Tape (ID 4);",
        "O[4:3],Tape volume,1,2,3;",
        "SC4,ISO,CD-ROM (ID 6);",
        "F0,ROM,Load ROM;",
        "-;",
        "O[11:9],Network interface,eth0,None,eth1,macvlan,tap0;",
        "-;",
        "P1,Machine;",
        "P2,Video;",
        "P3,Input;",
        "P4,Audio;",
        "P5,Advanced;",
        "-;",
        "h0-,Changes pending;",
        "R[0],Apply and reset;",

        "P1O[14:13],CPU clock,20 MHz,25 MHz,33 MHz;",
        "P1O[12],Colour board,On,Off;",
        "H1P1O[19],Colour board type,cg4,GX;",
        "P1-;",
        "P1T[17],Stop-A (abort);",

        "P2O[2:1],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
        "P2O[6:5],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer;",

        "P3O[18],Swap Alt/Win keys,No,Yes;",

        "P4O[8:7],Keyboard bell,Normal,Loud,Quiet,Off;",

        "P5O[15],Diag switch,Normal,Diag;",
        "P5O[16],Clock,MiSTer's time,28 years back;",
        "P5-;",
        "P5SC3,NVR,EEPROM image;",

        "v,1;",
        "V,v",`BUILD_DATE
    };

## SunSparcStation20 (SunSparcStation20_MiSTer/SunSparcStation.sv)
localparam CONF_STR = {
	"SunSparcStation;;",
	"-;",
	"SC0,VHDIMGHDARAW,Pri. disk (ID 3);",
	"SC1,VHDIMGHDARAW,Disk (ID 1);",
	"SC2,ISOCUECHD,CD-ROM (ID 6);",
	"F0,ROM,Load ROM;",
	"-;",
	"O[23:22],Memory,464MB,256MB,128MB,64MB;",
	"O[26:24],Network interface,eth0,None,eth1,macvlan,tap0;",
	"-;",
	"P2,Video;",
	"P3,Input;",
	"P5,Advanced;",
	"-;",
	"h0-,Changes pending;",
	"R[0],Apply and reset;",

	"P2O[9],Display,Graphics,Serial console;",
	"P2O[7:6],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P2O[30:28],Scale,V-Integer,Normal,Narrower HV-Integer,Wider HV-Integer,HV-Integer;",

	"P3O[13:12],Keyboard layout,US,French,German,Spanish;",
	"P3O[27],Swap Alt/Win keys,No,Yes;",

	"P5O[8],Autoboot,On,Off;",
	"P5-;",
	"P5SC3,NVR,NVRAM image;",

	"V,v",`BUILD_DATE
};

## Svi328 (SVI328_MiSTer/Svi328.sv)
parameter CONF_STR = {
	"SVI328;;",
	"-;",
	"F1,BINROM,Load Cartridge;",
	"OF,Tape Input,File,ADC;",
	"D0F2,CAS,Cas File;",
	"D0TD,Tape Rewind;",
	"-;",
	"O12,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O79,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"-;",
	"O6,Border,No,Yes;",
	"OAB,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O3,Joysticks swap,No,Yes;",
	"R0,Reset;",
        "J,Button;",
        "jn,A;",
	"V,v",`BUILD_DATE
};

## TK2000 (TK2000_MiSTer/TK2000.sv)
localparam CONF_STR = {
	"TK2000;;",
	//"Apple-II;;",
	"-;",
//	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
//	"O2,TV Mode,NTSC,PAL;",
//	"O34,Noise,White,Red,Green,Blue;",
	"O56,Display,Color,B&W,Green,Amber;",
	"-;",
	"S0,NIBDSKDO PO ;",
	"S1,NIBDSKDO PO ;",
	"OA,Disk Rom,On,Off;",
//	"OB,Color Mode,On,Off;",
	"-;",
	"R0,Reset;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE 
};

## TRS-80 (TRS-80_MiSTer/TRS-80.sv)
localparam CONF_STR = {
	"TRS-80;SS3E000000:10000,UART31250:9600:4800:2400:1200:300:110,MIDI;",
	"S1,DSKJV1,Mount Disk 0:;",  // Don't use slot0 because it gets overwritten when using file FS3 below. Probably a MisterMain bug.
 	"S2,DSKJV1,Mount Disk 1:;",
 	"S3,DSKJV1,Mount Disk 2:;",
 	"S4,DSKJV1,Mount Disk 3:;",
	"-;",
	"F2,CMDBAS,Load Program;",
	"F1,CAS,Load Cassette;",
	"OLM,CMD Exec Method,CAS,JMP,NONE;",
	"-;",
	"FS3,SAV,Snapshot;",
	"D1OJK,Savestate Slot,1,2,3,4;",
	"D1RH,Load State;",
	"D1RI,Save State;",
	"-;",
	"P1,Display and MT32 Options;",
	"P1-,Display Options;",
	"P1-;",
	"P1O56,Screen Color,White,Green,Amber;",
	"P1OE,Video Flicker,Off,On;",
	"P1O7,Lowercase Type,Normal,Symbol;",
	"P1OCD,Overscan,None,Partial,Full;",
	"P1OF,Overscan Status Line,Off,On;",
	"P1O13,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"P1ON,TRS80 Skin,Off,On;",
	"P1-;",
	
	"P1o1,Use MT32-pi,No,Yes;",
	"P1o89,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
	"P1o2,Synth,Munt,FluidSynth;",
	"P1o34,Munt ROM,MT-32 v1,MT-32 v2,CM-32L;",
	"P1o57,SoundFont,0,1,2,3,4,5,6,7;",
	"P1-;",
	"P1r0,Reset Hanging Notes;",
	
	"-;",
	"O4,Kbd Layout,TRS-80,PC;",
	"OAB,TRISSTICK,None,BIG5,ALPHA;",
	"O89,Clockspeed (MHz),1.78(1x),3.56(2x),5.34(3x),21.29(12x);",
	"OO,Omikron CP/M,Off,On;",
//	"OP,Omikron Write,Off,On;",
	"-;",
	"RG,Erase memory and reset;",
	"R0,Reset;",
	"J,Fire;",
	"I,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"V,v",`BUILD_DATE
};

## TSConf (TSConf_MiSTer/TSConf.sv)
localparam CONF_STR = {
	"TSConf;",
	"UART115200,MIDI;",
	"SC0,VHD,Mount SD image;",
	"-;",
	"OFH,Joystick 1,Kempston,Sinclair 1,Sinclair 2,Cursor,QAOPM;",
	"OIK,Joystick 2,Kempston,Sinclair 1,Sinclair 2,Cursor,QAOPM;",
	"OB,Swap mouse buttons,OFF,ON;",
	"-;",
	"P2,Video;",
	"P2o01,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"d0P2o2,Vertical Crop,Disabled,270p(5x);",
	"P2o34,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2-;",
	"P2OC,Vsync,48.8 Hz,60 Hz;",
	"P2OD,VDAC1,ON,OFF;",
	"O34,Stereo mix,None,25%,50%,100%;",
	"O5,PSG panning,ABC,ACB;",
	"O6,Audio Boost,On,Off;",
	"-;",
	"OE,CPU Type,CMOS,NMOS;",
	"P1,NVRAM;",
	"P1o56,CPU Speed (MHz),3.5,7,14;",
	"P1o7,CPU Cache,ON,OFF;",
	"P1o89,#7FFD span,128K,128K Auto,1024K,512K;",
	"P1oAC,Reset to,BD boot.$C,BD sys.rom,ROM #00,ROM #04,RAM #F8;",
	"P1oDE,Reset bank,TR-DOS,Basic 48,Basic 128,SYS;",
	"P1oFH,CS Reset to,BD boot.$C,BD sys.rom,ROM #00,ROM #04,RAM #F8;",
	"P1oIJ,CS Reset bank,TR-DOS,Basic 48,Basic 128,SYS;",
	"P1oKM,Boot Device,SD Z-controller,IDE Nemo Master,IDE Nemo Slave,RS-232,IDE Smuc Master,IDE Smuc Slave,SD2 Z-controller;",
	"P1oNP,ZX Palette,Default,B.black,Light,Pale,Dark,Grayscale,Custom;",
	"P1oQ,NGS Reset,OFF,ON;",
	"P1oR,FT8xx Reset,OFF,ON;",
	"P1oSU,INT Offset,1,2,3,4,5,6,7,0;",
	"P1-;",
	"P1T7,Apply;",
	"-;",
	"R0,Reset;",
	"J,Fire 1,Fire 2,Fire 3,Fire 4;",
	"jn,A,B,X,Y;",
	"jp,B,A,Y,X;",
	"v,2;",
	"V,v",`BUILD_DATE
};

## Tandy1000 (Tandy1000_MiSTer/Tandy1000.sv)
`define CONF_STR_SYSTEM (`SYSTEM_VARIANT_TANDY ? "Tandy1000;UART115200:115200;" : "PCXT;UART115200:115200;")
`endif
`ifndef ENABLE_TANDY_VIDEO
`define ENABLE_TANDY_VIDEO 0
`endif
`ifndef ENABLE_TANDY_AUDIO
`define ENABLE_TANDY_AUDIO 0
`endif
`ifndef ENABLE_TANDY_KBD
`define ENABLE_TANDY_KBD 0
`endif
`ifndef ENABLE_A000_UMB
`define ENABLE_A000_UMB 0
`endif
`ifndef ENABLE_CGA
`define ENABLE_CGA 1
`endif
`ifndef ENABLE_HGC
`define ENABLE_HGC 0
`endif
`ifndef ENABLE_OPL2
`define ENABLE_OPL2 0
`endif
`ifndef ENABLE_CMS
`define ENABLE_CMS 0
`endif
`ifndef ENABLE_EMS
`define ENABLE_EMS 0
`endif

module emu
    (
        //Master input clock
        input         CLK_50M,

        //Async reset from top-level module.
        //Can be used as initial reset.
        input         RESET,

        //Must be passed to hps_io module
        inout  [48:0] HPS_BUS,

        //Base video clock. Usually equals to CLK_SYS.
        output        CLK_VIDEO,

        //Multiple resolutions are supported using different CE_PIXEL rates.
        //Must be based on CLK_VIDEO
        output        CE_PIXEL,

        //Video aspect ratio for HDMI. Most retro systems have ratio 4:3.
        //if VIDEO_ARX[12] or VIDEO_ARY[12] is set then [11:0] contains scaled size instead of aspect ratio.
        output [12:0] VIDEO_ARX,
        output [12:0] VIDEO_ARY,

        output  [7:0] VGA_R,
        output  [7:0] VGA_G,
        output  [7:0] VGA_B,
        output        VGA_HS,
        output        VGA_VS,
        output        VGA_DE,    // = ~(VBlank | HBlank)
        output        VGA_F1,
        output [1:0]  VGA_SL,
        output        VGA_SCALER, // Force VGA scaler
        output        VGA_DISABLE,

        input  [11:0] HDMI_WIDTH,
        input  [11:0] HDMI_HEIGHT,
        output        HDMI_FREEZE,
        output        HDMI_BLACKOUT,
	output        HDMI_BOB_DEINT,

		`ifdef MISTER_FB
        // Use framebuffer in DDRAM (USE_FB=1 in qsf)
        // FB_FORMAT:
        //    [2:0] : 011=8bpp(palette) 100=16bpp 101=24bpp 110=32bpp
        //    [3]   : 0=16bits 565 1=16bits 1555
        //    [4]   : 0=RGB  1=BGR (for 16/24/32 modes)
        //
        // FB_STRIDE either 0 (rounded to 256 bytes) or multiple of pixel size (in bytes)
        output        FB_EN,
        output  [4:0] FB_FORMAT,
        output [11:0] FB_WIDTH,
        output [11:0] FB_HEIGHT,
        output [31:0] FB_BASE,
        output [13:0] FB_STRIDE,
        input         FB_VBL,
        input         FB_LL,
        output        FB_FORCE_BLANK,

		`ifdef MISTER_FB_PALETTE
        // Palette control for 8bit modes.
        // Ignored for other video modes.
        output        FB_PAL_CLK,
        output  [7:0] FB_PAL_ADDR,
        output [23:0] FB_PAL_DOUT,
        input  [23:0] FB_PAL_DIN,
        output        FB_PAL_WR,
		`endif
		`endif

        output        LED_USER,  // 1 - ON, 0 - OFF.

        // b[1]: 0 - LED status is system status OR'd with b[0]
        //       1 - LED status is controled solely by b[0]
        // hint: supply 2'b00 to let the system control the LED.
        output  [1:0] LED_POWER,
        output  [1:0] LED_DISK,

        // I/O board button press simulation (active high)
        // b[1]: user button
        // b[0]: osd button
        output  [1:0] BUTTONS,

        input         CLK_AUDIO, // 24.576 MHz
        output [15:0] AUDIO_L,
        output [15:0] AUDIO_R,
        output        AUDIO_S,   // 1 - signed audio samples, 0 - unsigned
        output  [1:0] AUDIO_MIX, // 0 - no mix, 1 - 25%, 2 - 50%, 3 - 100% (mono)

        //ADC
        inout   [3:0] ADC_BUS,

        //SD-SPI
        output        SD_SCK,
        output        SD_MOSI,
        input         SD_MISO,
        output        SD_CS,
        input         SD_CD,

        //High latency DDR3 RAM interface
        //Use for non-critical time purposes
        output        DDRAM_CLK,
        input         DDRAM_BUSY,
        output  [7:0] DDRAM_BURSTCNT,
        output [28:0] DDRAM_ADDR,
        input  [63:0] DDRAM_DOUT,
        input         DDRAM_DOUT_READY,
        output        DDRAM_RD,
        output [63:0] DDRAM_DIN,
        output  [7:0] DDRAM_BE,
        output        DDRAM_WE,

        //SDRAM interface with lower latency
        output        SDRAM_CLK,
        output        SDRAM_CKE,
        output [12:0] SDRAM_A,
        output  [1:0] SDRAM_BA,
        inout  [15:0] SDRAM_DQ,
        output        SDRAM_DQML,
        output        SDRAM_DQMH,
        output        SDRAM_nCS,
        output        SDRAM_nCAS,
        output        SDRAM_nRAS,
        output        SDRAM_nWE,

		`ifdef MISTER_DUAL_SDRAM
        //Secondary SDRAM
        //Set all output SDRAM_* signals to Z ASAP if SDRAM2_EN is 0
        input         SDRAM2_EN,
        output        SDRAM2_CLK,
        output [12:0] SDRAM2_A,
        output  [1:0] SDRAM2_BA,
        inout  [15:0] SDRAM2_DQ,
        output        SDRAM2_nCS,
        output        SDRAM2_nCAS,
        output        SDRAM2_nRAS,
        output        SDRAM2_nWE,
		`endif

        input         UART_CTS,
        output        UART_RTS,
        input         UART_RXD,
        output        UART_TXD,
        output        UART_DTR,
        input         UART_DSR,

        // Open-drain User port.
        // 0 - D+/RX
        // 1 - D-/TX
        // 2..6 - USR2..USR6
        // Set USER_OUT to 1 to read from USER_IN.
        input   [6:0] USER_IN,
        output  [6:0] USER_OUT,

        input         OSD_STATUS
    );

    ///////// Default values for ports not used in this core /////////

    assign ADC_BUS  = 'Z;
    //assign USER_OUT = '1;
    //assign {UART_RTS, UART_TXD, UART_DTR} = 0;
    assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
    //assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML, SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;
    assign SDRAM_CLK = clk_chipset;
    assign {DDRAM_CLK, DDRAM_BURSTCNT, DDRAM_ADDR, DDRAM_DIN, DDRAM_BE, DDRAM_RD, DDRAM_WE} = '0;

    assign VGA_F1 = 0;
    assign VGA_SCALER = 0;
    assign VGA_DISABLE = 0;
    assign HDMI_FREEZE = 0;
    assign HDMI_BLACKOUT = 0;
    assign HDMI_BOB_DEINT = 0;

    assign LED_DISK = 0;
    assign LED_POWER = 0;
    assign BUTTONS = 0;

    assign LED_USER = 0;
    //led fdd_led(clk_cpu, |mgmt_req[7:6], LED_USER);


    //////////////////////////////////////////////////////////////////
    // Status Bit Map:
    //              Upper                          Lower
    // 0         1         2         3          4         5         6
    // 01234567890123456789012345678901 23456789012345678901234567890123
    // 0123456789ABCDEFGHIJKLMNOPQRSTUV 0123456789ABCDEFGHIJKLMNOPQRSTUV
    // XXXXX XXXXXXXXXXXXXXXXXXXXXXXXXX XXXXXXXX

	`include "build_id.v"

    localparam CONF_STR_HGC = ((`ENABLE_HGC && `ENABLE_CGA) ? "P1oC,PCXT CGA Graphics,Yes,No;P1oD,PCXT Hercules Graphics,Yes,No;P1O4,PCXT 1st Video,CGA,Hercules;P1-;" : "");
    localparam CONF_STR_ROM = (`ROM_IS_TANDY ? "P1FC1,ROM,Tandy BIOS:;P1-;" : "P1FC0,ROM,PCXT BIOS:;");
    localparam CONF_STR_CMS = (`ENABLE_CMS ? "P2OA,C/MS Audio,Enabled,Disabled;" : "");
    localparam CONF_STR_OPL2 = (`ENABLE_OPL2 ? "P2oAB,OPL2,Adlib 388h,SB FM 388h/228h, Disabled;" : "");
    localparam CONF_STR_TANDY_AUDIO = (`ENABLE_TANDY_AUDIO ? "P2o23,Tandy Volume,1,2,3,4;" : "");
    localparam CONF_STR_EMS = (`ENABLE_EMS ? "P3OB,Lo-tech 2MB EMS,Enabled,Disabled;P3OCD,EMS Frame,C000,D000,E000;P3-;" : "");
    localparam CONF_STR_A000 = (`ENABLE_A000_UMB ? "P3o9,A000 UMB,Enabled,Disabled;P3-;" : "");

    localparam CONF_STR = {
		`CONF_STR_SYSTEM,
		"S0,IMGIMAVFD,Floppy A:;",
		"S1,IMGIMAVFD,Floppy B:;",
		"OJK,Write Protect,None,A:,B:,A: & B:;",
		"-;",
		"S2,VHD,IDE 0-0;",
		"S3,VHD,IDE 0-1;",
		"OLM,2nd SD card,Disable,IDE 0-0,IDE 0-1;",
		"-;",
		"OHI,CPU Speed,4.77MHz,7.16MHz,9.54MHz,PC/AT 3.5MHz;",
		"-;",
		"P1,System & BIOS;",
		"P1-;",
		CONF_STR_HGC,
		"P1O7,Boot Splash Screen,Yes,No;",
		"P1-;",
		CONF_STR_ROM,
		"P1FC2,ROM,EC00 BIOS:;",
		"P1-;",
		"P1OUV,BIOS Writable,None,EC00,Main,All;",
		"P1-;",
		"P2,Audio & Video;",
		"P2-;",
		CONF_STR_CMS,
		CONF_STR_OPL2,
		"P2o01,Speaker Volume,1,2,3,4;",
		CONF_STR_TANDY_AUDIO,
		"P2o45,Audio Boost,No,2x,4x;",
		"P2o67,Stereo Mix,none,25%,50%,100%;",
		"P2-;",
		"P2oEH,CRT H offset,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15;",
		"P2oIK,CRT V offset,0,1,2,3,4,5,6,7;",
		"P2oMO,VSync Width,Auto,1,2,3,4,5,6,7;",
		"P2oPR,HSync Width,Auto,1,2,3,4,5,6,7;",
        "P2-;",
		"P2O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
		"P2O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
		"P2OT,Border,No,Yes;",
		"P2o8,Composite video,Off,On;",
		"P2OEG,Display,Full Color,Green,Amber,B&W,Red,Blue,Fuchsia,Purple;",
		"P2-;",
		"P3,Hardware;",
		"P3-;",
		CONF_STR_EMS,
		CONF_STR_A000,
		"P3ONO,Joystick 1, Analog, Digital, Disabled;",
		"P3OPQ,Joystick 2, Analog, Digital, Disabled;",
		"P3OR,Sync Joy to CPU Speed,No,Yes;",
		"P3OS,Swap Joysticks,No,Yes;",
		"P3-;",
		"-;",
		"R0,Reset & apply settings;",
		"J,Fire 1,Fire 2;",
		"V,v",`BUILD_DATE
	};

## TatungEinstein (TatungEinstein_MiSTer/TatungEinstein.sv)
localparam CONF_STR = {
	"TatungEinstein;;",
	"S0,DSK,Mount Disk 0:;",
 	//"S1,DSK,Mount Disk 1:;",
	"-;",
	"O89,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O35,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"-;",
	"OA,Einstein Version,TC01,256;",
	"-;",
	"O2,Joystick,Digital,Analog;",
	"O6,Diagnostic ROM mounted,Off,On;",
	"O7,Border,Off,On;",
	"-;",
	"T0,Reset;",
	"R0,Reset and close OSD;",
	"V,v",`BUILD_DATE 
};

## Ti994a (TI-99_4A_MiSTer/Ti994a.sv)
parameter CONF_STR = {
	"TI-99_4A;;",
	"F1,M99BIN,Load Full Cart;",
	"F2,BIN,Load Rom Cart;",
	"F3,BIN,Load Grom Cart;",
	"d6S0,DSK,Drive 1;",
	"d6S1,DSK,Drive 2;",
	"d6S2,DSK,Drive 3;",
	"h2OS,TIFDC Disk Speed,Normal,Turbo;",
	"h3P4,MyArc FDC Dip Switches;",
	"P4OS,MyArc Disk Speed,Normal,Turbo;",
	"H4P4OT,Drive 1 Seek Speed,6ms,20/2ms;",
	"H4P4OU,Drive 2 Seek Speed,6ms,20/2ms;",
	"H4P4OV,Drive 3 Seek Speed,6ms,20/2ms;",
	"h4P4OT,Drive 1 Drive Type,5-1/4\",3-1/2\";",
	"h4P4OU,Drive 2 Drive Type,5-1/4\",3-1/2\";",
	"h4P4OV,Drive 3 Drive Type,5-1/4\",3-1/2\";",
	"-;",
	"OIK,Cart Type,Normal,MBX,Paged7,Paged378,Paged379,MiniMem;",	//Normal = 0, MBX = 1, Paged7 = 2, UberGrom = 3, Paged378 = 4, MiniMem = 5
	"H0P1,Mini Mem;",
	"P1S3,DAT,NVRAM File;",
	"D5P1rS,Load NVRAM;",
	"D5P1rT,Save NVRAM;",

	"P2,Video Settings;",
	"P2OD,Video Mode,NTSC, PAL;",
	"P2O79,Scandoubler Fx,None,HQ2x-320,CRT 25%,CRT 50%,CRT 75%;",
	"P2O56,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P2O1,Aspect Ratio,Original,Full Screen;",
	"D1P2OL,Vertical Crop,No,Yes;",
	
	"P3,Hardware;",
	"P3OA,Turbo,Off,On;",
	"P3OE,Scratchpad RAM,256B,1KB;",
	"P3OGH,Speech,Off,5220,5200;",
	"P3OF,Alpha Lock on Power Up, Off, On;",
	"P3OM,SAMS Memory,Disabled,Enabled;",
	"P3ON,TiPi,Disabled,Enabled;",
	"P3OOP,TiPi CRU Base,1000,1100,1200,1400;",
	"P3OQ,PCode,Disabled,Enabled;",
	"P3-;",
	"P3FC4,BIN,Select System Grom;",
	"P3FC5,BIN,Select System Rom ;",
	"P3FC6,BIN,Select Speech Rom ;",
	"P3FC7,BIN,Select Disk DSR   ;",
	"P3FC8,BIN,Select TIPI DSR   ;",		//File is 32k but only 4k is used.  So ignore anyting over x1000
	"P3FC9,BIN,Select P-Code Rom ;",		//Index 10 since it's the 11th menu item
	"-;",
	"OC,Arrow Keys,Cursor, Joystick;",
	"OB,Swap joysticks,NO,YES;",
	"o01,Mouse,Disabled,Mechatronics,Joy1,Joy2;",
//	"-;",
//	"oA,Pause When OSD is Open,No,Yes;",
	"RR,Reset & Detach Cart;",
	"R0,Reset;",
	"-;",
	"J,Fire 1,Fire 2,1,2,3,Enter,Back,Redo;",
	"V,v",`BUILD_DATE
};

## TomyTutor (TomyTutor_MiSTer/TomyTutor.sv)
localparam CONF_STR = {
	"TomyTutor;;",
	"FC2,BIN,Load Cartridge;",

	"P1,Tape;",
	"P1S0,CAS,Mount Tape Image;",
	"D0P1O[2],Tape Read Only,No,Yes;",
	"P1O[4:3],Tape Status,Never,Always,When Mounted, With Activity;",
	"P1O[5],Tape Audio,Muted,UnMuted;",
	"D1P1T[6},Stop   [F5];",
	"D1P1T[7],Rewind [F6];",

	"h2P2,Dev Menu;",
	"h2P2O[13],4000-5FFF Ram,Off,On;",
	"h2P2O[14],6000-7FFF Ram,Off,On;",
	"h2P2O[15],8000-9FFF Ram,Off,On;",
	"h2P2O[16],A000-BFFF Ram,Off,On;",
	"h2P2O[17],C000-DFFF Ram,Off,On;",
	"-;",

	"P3,Video Settings;",
	"P3O[8],MAX Num of Sprites,32,4;",
	"P3O[10:9],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P3O[11],Aspect Ratio,Original,Full Screen;",
	"P3O[12],Vertical Crop,Off,On;",
	"-;",
	"FC1,ROM,Select System Rom;",
	"-;",
   "R[1],Eject Cartridge;",
	"-;",
	"R[0],Reset;",
	"J,Fire 1,Fire 2,1,2,Enter/RT;",
	"V,v",`BUILD_DATE 
};

## UK101 (UK101_MiSTer/UK101.sv)
localparam CONF_STR = {
	"UK101;;",
	"-;",
	"D0F,TXTBASLOD,Load Ascii;",
	"S0,TXT,Save Ascii;",
	"R[11],Save;",
	"S1,65D65U,Mount Disk;",
	"S2,65D65U,Mount Disk B;",
	// D7/D8 (2026-08-01): grey out Mount Disk C/D whenever "Drives fitted"
	// (below) doesn't include them, verified against the real MiSTer
	// framework source (menu.cpp) rather than guessed - see status_menumask
	// below for the grey_mount_c/grey_mount_d bit assignment.
	"D7S3,65D65U,Mount Disk C;",
	"D8S4,65D65U,Mount Disk D;",
	// Mirrors a real WinOSI settings panel this core had no equivalent of
	// (2026-08-01): "how many drive connectors exist" as its own explicit
	// choice, separate from whether a diskette is in any of them. Defaults
	// to "2" (status[2:1]="00") - the ONLY configuration ever hardware-
	// confirmed with UCSD Pascal - so an unmounted C/D reads back exactly as
	// "no such drive" unless the user deliberately opts into more. See
	// uk101.vhd's disk_drives_fitted port comment for why an always-real C/D
	// would otherwise be a different signal to software that scans drives by
	// index-hole behaviour.
	"O[2:1],Drives fitted,2,3,4;",
	"O[20],Disk writes,Protected,Enabled;",
	"O[3],Load programs from,File,UART;",
	"O[7],Baud Rate,9600,300;",
	"-;",
	"O[9:8],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[13:12],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	//"O34,Colours,White on blue,White on black,Green on black,Yellow on black;",
	// D6 dropped here (2026-07-21): it used to force-grey this row whenever
	// C1P was selected (status_menumask bit6 = status[28], set only for
	// C1P) - the only reason C1P had no selectable screen mode at all. C2P's
	// own row below keeps D6 (vestigial - status[27]/status[28] can't both
	// be set, so it never actually fires there, but left alone since it's
	// provably inert).
	"H4d5O4,Screen resolution,Low,High;",
	"h4d5D6O56,Screen resolution,Low,High,Auto;",
	"-;",
	// D9 (2026-08-01): greyed whenever 48K memory is selected, since UCSD
	// Pascal's 48K requirement only works booted as OSI C2P + Synmon (Disk) -
	// mem48k below both forces machine_type/monitor_type to that combination
	// and pushes it back into status[] via hps_io's status_in/status_set (see
	// below) so the OSD's own display reflects it, not just internal
	// behaviour.
	"D9O[28:27],Machine,UK101,OSI C2P,OSI C1P;",
	"O[19:17],Clock speed,1Mhz,2Mhz,4Mhz,8Mhz,10Mhz;",
	// 48K banks the 8K BASIC ROM out of $A000-$BFFF - the two cannot coexist,
	// since that is exactly the range taking contiguous RAM from 40K to 48K.
	// Needed by UCSD Pascal (its hang was insufficient RAM, nothing else -
	// see uk101.vhd's n_basRomCS comment). Labelled so the trade-off is
	// visible at the point of choice rather than discovered afterwards.
	"O[26:24],Memory Size,4K,8K,32K,41K,48K (Basic disabled);",
	"H4H6O[22:21],Monitor,Cegmon,MonUK02,Wemon;",
	"h4D9O[23:22],Monitor,Cegmon,Synmon (Basic),Synmon (Disk);",
	"h6O[23],Monitor,Cegmon,Synmon;",
	"-;",
	"-;",
	"R[10],Reset;",
	"-;",
	"-;",
	"V,v",`BUILD_DATE
};

## UKNC (UKNC_MiSTer/UKNC_MiSTer.sv)
localparam CONF_STR = {
	"UKNC;UART19200;",
	"-;",
	"S0,DSK,Mount FDD 0;",
	"S1,DSK,Mount FDD 0;",
	"S2,VHD,Mount HDD;",
//	"S2,DSK,Mount Drive 3;",
//	"S3,DSK,Mount Drive 4;",
//	"S4,VHD,Mount Drive;",
	"-;",
	"O[11],CPU Speed ,Normal,Turbo;",
	"O[12],Palette ,GRB,RGB;",

	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O[10:8],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"-;",
	"-;",
	"R[0],Reset;",
	"V,v",`BUILD_DATE 
};

## VIC20 (VIC20_MiSTer/VIC20.sv)
parameter CONF_STR = {
	"VIC20;;",
	"S1,D64G64,Mount #8;",
	"S2,D64G64,Mount #9;",
	"-;",
	"F1,PRGCRTCT?TAP,Load;",
	"FS2,CRTROM,Load Megacart;",
	"h4RS,Save NVRAM;",
	"h4OT,Autosave,Off,On;",
	"-;",
	"h3RG,Tape Play/Pause;",
	"h3RI,Tape Unload;",
	"h3OH,Tape Sound,Off,On;",
	"h3OM,Tape Autoplay,Yes,No;",
	"h3-;",
	"OJK,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O24,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%,CRT 75%;",
	"OCD,Screen center,Both,None,Horz,Vert;",
	"OE,TV mode,PAL,NTSC;",
	"H2d1ON,Vertical Crop,No,Yes;",
	"h2d1ONO,Vertical Crop,No,270,216;",
	"OPQ,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"H4O6,ExtRAM $0400(3KB),Off,On;",
	"H4O7,ExtRAM $2000(8KB),Off,On;",
	"H4O8,ExtRAM $4000(8KB),Off,On;",
	"H4O9,ExtRAM $6000(8KB),Off,On;",
	"H4OA,ExtRAM $A000(8KB),Off,On;",
	"H4-;",
	"OL,External IEC,Disabled,Enabled;",
	"OB,Cart is writable,No,Yes;", 
	"-;",
	"OF,Kernal,Loadable,Standard;",
	"FC6,ROM,Load Kernal;",
	"-;",
	"OU,Swap paddles,No,Yes;",
	"-;",
	"O[32],SNAC Joystick,Off,On;",
	"O[34:33],SNAC Autofire,Off,Slow,Fast;",
	"-;",
	"R0,Reset;",
	"RR,Reset & Detach Cartridge;",
	"J,Fire,Paddle Fire|P;",
	"V,v",`BUILD_DATE
};

## VT52 (VT52_MiSTer/VT52.sv)
localparam CONF_STR = {
   "VT52;;",
   "-;",
   "O[122:121],Aspect ratio,Original (4:3),Full Screen;",
   "O[4:3],Text Color,White,Red,Green,Blue;",
   "OC,Serial Port,User IO Port,Console Port;",
   "OE,Font,Terminus 8x16, VT52 rom 8x8;", 
   "-;",
   "T[0],Reset;",
   "R[0],Reset and close OSD;",
   "V,v",`BUILD_DATE 
};

## Vector-06C (Vector-06C_MiSTer/Vector-06C.sv)
localparam CONF_STR =
{
	"VECTOR06;;",
	"-;",
	"F,ROMCOMC00EDD;",
	"-;",
	"S0,FDD,Mount Disk A;",
	"S1,FDD,Mount Disk B;",
	"-;",
	"OBC,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O12,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"O7,Reset Palette,Yes,No;",
	"-;",
	"d0OF,Vertical Crop,Disabled,270p(5x);",
	"ODE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O9A,Stereo mix,none,25%,50%,100%;",
	"-;",
	"O4,CPU Speed,3MHz,6MHz;",
	"O5,CPU Type,i8080,Z80;",
	"-;",
	"R6,Cold Reboot;",
	"J,Fire 1,Fire 2;",
	"V,v",`BUILD_DATE
};

## VideoBrain (VideoBrain_MiSTer/VideoBrain.sv)
localparam CONF_STR = {
	"VideoBrain;UART300:110;",
	"F1,BIN,Load Cartridge;",
	"-;",
	"d8S0,VBT,Mount Playback Tape;",
	"T[23],Rewind Playback Tape;",
	"d8S1,VBT,Mount Record Tape;",
	"T[30],Rewind Record Tape;",
	"-;",
	"P4,Keyboard;",
	"P4-;",
	"P4F4,VBK,Load Keyboard File;",
	"P4-;",
	"P4O[31],OSD Aborts VBK,On,Off;",
	"H5P4O[18],Music Mode,Playback,Record;",
	"h5D5P4O[18],Music Mode,Playback;",
	"P4O[19],Music Repeat,Off,On;",
	"P4-;",
	"P4O[29],Esc Soft Reset,On,Off;",
	"P3,Joysticks;",
	"P3-;",
	"P3O[5],Joysticks,Connected,Disconnected;",
	"D4P3R[20],Calibrate Sticks;",
	"P3-;",
	"D4P3O[15:14],Stick Preset,Auto,Tennis,Gladiator;",
	"D4P3O[38],Analog Mode,Absolute,Relative;",
	"DDP3O[40:39],Relative Speed,Slow,Normal,Fast;",
	"P3-;",
	"D4P3O[17:16],Paired Sticks,Off,1+2 / 3+4,1+3 / 2+4 (Doubles);",
	"P1,Video;",
	"P1-;",
	"P1O[42:41],Output,480i (Original),240p,480p;",
	"P1T[43],Apply Video Output;",
	"P1-;",
	"P1O[122:121],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O[125:123],Scale,Normal,V-Integer,HV-Integer-,HV-Integer+,HV-Integer;",
	"P5,Printer;",
	"P5-;",
	"d6P5S2,VBP,Mount Capture Image;",
	"P5O[33],Capture Baud,300,110;",
	"P5-;",
	"d7P5T[34],Start Capture;",
	"DEP5T[35],Stop Capture and Save;",
	"DEP5T[36],Show Capture Status;",
	"P5-;",
	"P5O[37],Info. Manager Fix,Off,On;",
	"P5-;",
	"h9P5-,Printer Active;",
	"h11P5-,Printer Busy;",
	"h12P5-,Printer Error;",
	"P2,System;",
	"P2-;",
	"P2F0,BIN,Load BIOS;",
	"P2F2,BIN,Load Expander 1 ROM;",
	"P2-;",
	"P2O[32],UART Output,Modem,Printer;",
	"P2O[11:9],Mapper Fallback,Standard,Timeshare,Money Minder,APL/S,Info. Manager;",
	"P2O[24],Reset on Cartridge Load,On,Off;",
	"P2O[22],Info Messages,On,Off;",
	"P2-;",
	"P2T[27],Master Control (Soft Reset);",
	"-;",
	"DAR[28],Unload Cartridge;",
	"R[0],Reset;",
	"J,Fire,Fire (Paired);",
	"jn,A,B;",
	"I,Keyboard entry started,Bad keyboard file,Tape error,Tape rewound,Tape busy,Tape idle,Keyboard entry finished,Keyboard entry aborted,Calibrating sticks,End of tape,Printer no image,Printer scanning,Printer ready,Printer starting,Printer active,Printer flushing,Printer stalled,Printer full,Printer error,Printer read only,Printer bad image,Printer incomplete image,Printer storage error,Printer framing error,Printer overflow,Printer media changed,Printer reset interrupted;",
	"v,1;",
	"V,v",`BUILD_DATE
};

## X68000 (X68000_MiSTer/X68000.sv)
parameter CONF_STR = {
	"X68000;UART115200,MIDI;",
	"-;",
	"P1,Floppy Disk;",
	"P1S0,D88XDFDIMHDM,FDD0;",
	"P1R9,Save FDD0 changes to SD;",
	"P1RB,Eject FDD0;",
	"P1-;",
	"P1S1,D88XDFDIMHDM,FDD1;",
	"P1RA,Save FDD1 changes to SD;",
	"P1RC,Eject FDD1;",
	"P1-;",
	"P1-,Use same type in disks;",
	"P1oFG,FDD wait (XDF/DIM),disable,seek,data,seek+data;",
	"SC2,HDF,SASI Hard Disk;",
	"SC4,HDS,SXSI Hard Disk 1;",
	"SC5,HDS,SXSI Hard Disk 2;",
	"SC6,HDS,SXSI Hard Disk 3;",
	"SC7,HDS,SXSI Hard Disk 4;",
	"-;",
	"P3,SRAM;",
	"P3SC3,RAM,SRAM;",
	"P3RD,Load SRAM from SD Card;",
	"P3RE,Save SRAM to SD Card;",
	"P3-;",
	"P3R[75],Inject SXSI driver (+reset);",
	"P3O[76],Load SXSI driver on boot,No,Yes;",
	"P3O[74:71],Visible Memory,Auto,1MB,2MB,3MB,4MB,5MB,6MB,7MB,8MB,9MB,10MB,11MB,12MB;",
	"-;",
	"P4,Audio & Video;",
	"P4-;",
	"P4oQ,OPM Chip,JT51,IKAOPM;",
	"P4O23,Stereo Mix,None,25%,50%,100%;",
	"P4ORS,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P4-;",
	"P4o1,Video Frequency,Original,60fps (31k);",
	"P4O[70:69],Video Mode,Stretch,Native;,;",
	"P4O[77],15 kHz line doubler,Off,On;",
	"P4O45,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"h1P5,MT32-pi;",
	"h1P5-;",
	"h1P5OI,Use MT32-pi,Yes,No;",
	"h1P5o9A,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
	"h1P5-;",
	"h1P5-,Default Config:;",
	"h1P5OJ,Synth,Munt,FluidSynth;",
	"h1P5OKL,Munt ROM,MT-32 v1,MT-32 v2,CM-32L;",
	"h1P5OTV,SoundFont,0,1,2,3,4,5,6,7;",
	"h1P5-;",
	"h1P5r8,Reset Hanging Notes;",
	"- ;",
	"o57,Joy1 type,2 Button,2 Turbo,MegaDrive 3,Magical 6,Capcom 6,Double-DPad,CyberStick;",
	"oCE,Joy2 type,2 Button,2 Turbo,MegaDrive 3,Magical 6,Capcom 6,Double-DPad,CyberStick;",
	"oB,Joystick swap,No,Yes;",
	"o24,KBD layout,MiSTer,JP Func.,JP Pos.,US Std,US Alt;",
	"-;",
	"o0,CPU speed,Normal,Turbo;",
	"O[67],MIDI Board,Off,On;",
	"R7,NMI Button;",
	"R8,Power Button;",
	"R0,Reset;",
	"- ;",
	"J,Button 1,Button 2,Run,Select,Button 3,Button 4,Button 5,Button 6;",
	"jn,A,B,Run,Select,X,Y,L,R;",
	"jp,A,B,Run,Select,X,Y,L,R;",
	"I,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"V,v",`BUILD_DATE
};

## ZX-Spectrum (ZX-Spectrum_MISTer/ZX-Spectrum.sv)
localparam CONF_STR = {
	"Spectrum;;",
	"S0,TRDIMGDSKMGT,Load Disk;",
	"F2,TAPCSWTZXPZX,Load Tape;",
	"F4,Z80SNA,Load Snapshot;",
	"S1,VHD,Load DivMMC;",
	"F6,ROMMLD,Load Dandanator;",
	"SC2,SAV,Savestate Disk;",
	"-;",

	"P1,Video;",
	"P1-;",
	"P1O[5:4],Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O[16:15],Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"P1-;",
	"P1O[38],Narrow Border,No,Yes;",
	"D4P1O[58:57],Pentagon Overscan,Off,Centered,Full;",
	"H2d1P1O[28],Vertical Crop,No,Yes;",
	"h2d1P1O[29:28],Vertical Crop,No,270,216;",
	"P1O[27:26],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",

	"P3,Audio;",
	"P3-;",
	"P3O[21:20],General Sound,512KB,1MB,2MB,Disabled;",
	"P3O[61:59],Covox,Disabled,#FB,#DD (Scorpion),#5F/#3F (Profi),#DF (SpecDrum);",
	"P3O[3:2],Stereo Mix,none,25%,50%,100%;",
	"P3-;",
	"P3O[39],PSG/FM,Enabled,Disabled;",
	"P3O[40],PSG Stereo,ABC,ACB;",
	"P3O[41],PSG Model,YM2149,AY8910;",
	"P3-;",
	"P3O[55],HQ Audio,On,Off;",
	"d5P3O[54],PSG Anti-alias,On,Off;",
	"D5P3O[56],HQ Punch,On,Off;",
	"D5P3O[48],HQ FIR,On,Off;",
	"D5P3O[50],HQ DC Filter,On,Off;",
	"D5P3O[47:44],HQ Room,9dB,Off,15dB,14dB,13dB,12dB,6dB,3dB,2dB,1dB;",
	"D5P3O[53:51],HQ Voicing,Classic,Flat,Headphones,Warm,TV,Small Speaker;",

	"P2,Hardware;",
	"P2-;",
	"P2O[7],Port #FE,Issue 2,Issue 3;",
	"P2O[13],Port #FF,Timex,SAA1099;",
	"P2O[14],ULA+,Enabled,Disabled;",
	"D3P2OP,Snow Bug,Disabled,Enabled;",
	"P2-;",
	"P2O[9:8],Video Timings,ULA-48,ULA-128,Pentagon,Scorpion;",
	"P2O[12:10],Memory,Spectrum 128K/+2,Pentagon 1024K,Profi 1024K,Spectrum 48K,Spectrum +2A/+3,Scorpion ZS-256;",
	"P2-;",
	"P2O[33:32],MMC Mode,Auto(VHD),SD Card 14MHz,SD Card 28MHz;",
	"P2O[31:30],MMC Version,DivMMC+ESXDOS,DivMMC,ZXMMC;",
	"P2-;",
	"P2O[62],Dandanator,Disabled,Enabled;",

	"-;",
	"O[37:36],Keyboard,Normal,Ghosting,Recreated ZX,Recr+Ghosting;",
	"O[19:17],Joystick,Kempston,Sinclair I,Sinclair II,Sinclair I+II,Cursor;",
	"O[35:34],Mouse,Disabled,Kempston L/R,Kempston R/L;",
	"O[49],Mouse Wheel,Normal,Inverted;",
	"O[6],Fast Tape Load,On,Off;",
	"O[1],Tape Sound,On,Off;",
	"O[24:22],CPU Speed,Original,7MHz,14MHz,28MHz,56MHz;",
	"-;",
	"R[0],Reset & Apply;",
	"R[63],Reset & Detach Cartridge;",
	"J,Fire 1,Fire 2,Savestates;",
	"V,v",`BUILD_DATE
};

## ZX81 (ZX81_MiSTer/ZX81.sv)
localparam CONF_STR = {
	"ZX81;;",
	"F,O  P  ,Load tape;",
	"-;",
	"OLM,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O6,Video frequency,50Hz,60Hz;",
	"O7,Inverse video,Off,On;",
	"O5,Black border,Off,On;",
	"OCD,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"-;",
	"d1ON,Vertical Crop,Disabled,216p/270p(5x);",
	"OOP,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"-;",
	"O23,Stereo mix,none,25%,50%,100%;", 
	"-;",
	"O4,Model,ZX81,ZX80;",
	"OHI,Slow mode speed,Original,NoWait,x2,x8;",
	"OAB,Main RAM,16KB,32KB,48KB,1KB;",
	"OG,Low RAM,Off,8KB;",
	"D0OEF,CHR$128/UDG,128 Chars,64 Chars,Disabled;",
	"OJ,QS CHRS,Enabled(F1),Disabled;",
	"OK,CHROMA81,Disabled,Enabled;",
	"-;",
	"O89,Joystick,Cursor,Sinclair,ZX81;",
	"OQR,Keyboard,Normal,Ghosting,Recreated ZX,Recr+Ghosting;",
	"-;",
	"R0,Reset;",
	"V,v",`BUILD_DATE
};

## ZXNext (ZXNext_MISTer/ZXNext.sv)
localparam CONF_STR = {
	"ZXNext;;",
	"SC0,VHD,Mount C:;",
	"SC1,VHD,Mount D:;",
	"O1,Hard Reset on C: mount,No,Yes;",
   "-;",
	"F1,TZXCSW,Load Tape,30000000;",
   "-;",
	"O78,Aspect Ratio,Original,Full Screen,[ARC1],[ARC2];",
	"O56,Scandoubler Fx,None,HQ2x,CRT 25%,CRT 50%;",
	"ODE,Narrow Border,No,276,288,320;",
        "OF,Center Image,Yes,No;",
	"H2d1OS,Vertical Crop,No,Yes;",
	"h2d1OST,Vertical Crop,No,270,216;",
	"OQR,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"O34,Stereo Mix,none,25%,50%,100%;",
	"O2,Joysticks Swap,No,Yes;",
	"-;",
	"h3TA,CPU Clock:           3.5MHz;",
	"h4TA,CPU Clock:             7MHz;",
	"h5TA,CPU Clock:            14MHz;",
	"h6TA,CPU Clock:            28MHz;",
	"R0,Soft Reset;",
	"R9,Hard Reset;",
	
	"h7-;",
	"h7P3,Debug;",
	"h7P3-;",
	"h7P3O[11],Machine,Mister,Next;",
	"h7P3O[12],Issue,2,4;",
	"-   ;",
	"J,A,B,C,X,Y,Z,Start;",
 	"V,v",`BUILD_DATE
};

## ao486 (ao486_MiSTer/ao486.sv)
localparam CONF_STR =
{
	"AO486;UART115200:4000000(Turbo 115200),MIDI;",
	"S0,IMGIMAVFD,Floppy A:;",
	"S1,IMGIMAVFD,Floppy B:;",
	"O12,Write Protect,None,A:,B:,A: & B:;",
	"-;",
	"S2,VHD,IDE 0-0;",
	"S3,VHD,IDE 0-1;",
	"-;",
	"S4,VHDISOCUECHD,IDE 1-0;",
	"S5,VHDISOCUECHD,IDE 1-1;",
	"-;",
	"oJM,CPU Preset,User Defined,~PC XT-7MHz,~PC AT-8MHz,~PC AT-10MHz,~PC AT-20MHz,~PS/2-20MHz,~386SX-25MHz,~386DX-33Mhz,~386DX-40Mhz,~486SX-33Mhz,~486DX-33Mhz,MAX (unstable);",
	"-;",
	"P1,Audio & Video;",
	"P1-;",
	"P1OMN,Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	"P1O4,VSync,60Hz,Variable;",
	"P1O8,16/24bit mode,BGR,RGB;",
	"P1O9,16bit format,1555,565;",
	"P1oS,Low-Res,Native,4x;",
	"P1oDE,Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
	"P1oN,Border,No,Yes;",
	"P1-;",
	"P1O3,FM mode,OPL2,OPL3;",
	"P1OH,C/MS,Disable,Enable;",
	"P1OIJ,PC Speaker Volume,1,2,3,4;",
	"P1OKL,Audio Boost,No,2x,4x;",
	"P1oBC,Stereo Mix,none,25%,50%,100%;",
	"P1oO,SB Swap L/R,Off,On;",
	"P1OP,MT32 Volume Ctl,MIDI,Line-In;",

	"P2,Hardware;",
	"P2o01,Boot 1st,Floppy/Hard Disk,Floppy,Hard Disk,CD-ROM;",
	"P2o23,Boot 2nd,NONE,Floppy,Hard Disk,CD-ROM;",
	"P2o45,Boot 3rd,NONE,Floppy,Hard Disk,CD-ROM;",
	"P2-;",
	"P2o6,IDE 1-0 CD Hot-Swap,Yes,No;",
	"P2o7,IDE 1-1 CD Hot-Swap,No,Yes;",
	"P2-;",
	"P2OB,RAM Size,256MB,16MB;",
	"P2oP,D0000h-EFFFFh RAM,Off,On;",
`ifndef DEBUG
	"H5P2-;",
	"H5D2D1P2O56,CPU Clock,90MHz,15MHz,30MHz,56MHz;",
	"H5h0P2O7,Overclock,Off,100Mhz;",
	"H5D2P2OF,L1 Cache,On,Off;",
	"H5D2P2OG,L2 Cache,On,Off;",
`endif
	"P2-;",
	"P2OA,USER I/O,MIDI,COM2;",
	"P2-;",
	"P2OCE,Joystick Type,2 Buttons,4 Buttons,Gravis Pro,None,CH Flightst., Thrustmaster;",
	"P2oFG,Joystick Mode,2 Joysticks,2 Sticks,2 Wheels,4-axes Wheel;",
	"P2oQR,Joystick Axes,Timed,Count 8+141,Count 0+256,Count 6+256;",
	"P2oH,Joystick 1,Enabled,Disabled;",
	"P2oI,Joystick 2,Enabled,Disabled;",

	"h3P3,MT32-pi;",
	"h3P3-;",
	"h3P3OO,Use MT32-pi,Yes,No;",
	"h3P3o9A,Show Info,No,Yes,LCD-On(non-FB),LCD-Auto(non-FB);",
	"h3P3-;",
	"h3P3-,Default Config:;",
	"h3P3OQ,Synth,Munt,FluidSynth;",
	"h3P3ORS,Munt ROM,MT-32 v1,MT-32 v2,CM-32L;",
	"h3P3OTV,SoundFont,0,1,2,3,4,5,6,7;",
	"h3P3-;",
	"h3P3r8,Reset Hanging Notes;",
	"-;",
	"R0,Reset and apply HDD;",
	"J,Button 1,Button 2,Button 3,Button 4,Start,Select,R1,L1,R2,L2;",
	"jn,A,B,X,Y,Start,Select,R,L;",
	"I,",
	"MT32-pi: SoundFont #0,",
	"MT32-pi: SoundFont #1,",
	"MT32-pi: SoundFont #2,",
	"MT32-pi: SoundFont #3,",
	"MT32-pi: SoundFont #4,",
	"MT32-pi: SoundFont #5,",
	"MT32-pi: SoundFont #6,",
	"MT32-pi: SoundFont #7,",
	"MT32-pi: MT-32 v1,",
	"MT32-pi: MT-32 v2,",
	"MT32-pi: CM-32L,",
	"MT32-pi: Unknown mode;",
	"V,v",`BUILD_DATE
};

## eg2000 (EG2000_MiSTer/eg2000.sv)
localparam CONF_STR = {
	"eg2000;;",
	"-;",
	"F1,CAS,Load Cassette;",
	"H2-;",
	"h3-,-Basic Tape Loaded-;",
	"h4-,-System Tape Loaded-;",
	"h5-,-Data Tape Loaded-;",
	"-;",
	"h0d0T1,Play/Pause;",
	"h0D1T2,Rewind;",
	"h0D1T3,Eject;",
	"h4R4,Paste Filename;",
	"-;",
	"O[6:5],Tape Volume,Muted,Low,High;",
	"O[7],Joystick 1,Analog,DPAD;",
	"O[8],Joystick 2,Analog,DPAD;",
	"P1,Video Settings;",
	"P1O[12:11],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
   "P1O[14:13],Scandoubler,Off,On,HQ2x;",
	"P1O[16:15],Scale,Normal,V-Integer,Narrower HV-Integer,Wider HV-Integer;",
    "-;",
	"R[0],Reset;",
	"J,*,#,0,1,2,3,4,5,6,7,8,9;",
	"V,v",`BUILD_DATE 
};
