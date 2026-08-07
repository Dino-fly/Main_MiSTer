#ifndef CHARROM_H
#define CHARROM_H

extern unsigned char charfont[256][8];

// 1 when the file was read and the glyph table replaced, 0 when it could not be read - in
// which case charfont[] is exactly what it was. A caller offering a choice of fonts needs
// to tell those apart; boot does not, and ignores it as it always did.
int LoadFont(char* name);

// Put the compiled-in glyphs back, from the copy LoadFont() keeps before its first
// overwrite. A no-op when no font was ever loaded, because then this is already it.
void FontRestoreBuiltin();

#endif
