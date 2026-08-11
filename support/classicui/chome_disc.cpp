#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "chome_disc.h"
#include "chome_titles.h"
#include "chome_proc.h"

/*
  cfg.h is wanted in both configurations now, not only in the half that owns the drive: the
  harness's disc_poll() reads cfg.classicui_disc too, because the setting is a row on a
  screen and giving the drive back when it goes off is the one part of that function a host
  test can say something true about.
*/
#include "../../cfg.h"

#ifndef CHOME_HOST_TEST
#include <fcntl.h>
#include <unistd.h>
// Before linux/cdrom.h, which defines CDSL_CURRENT as INT_MAX without including
// this itself. The fork this derives from carries the same include for the same
// reason; the harness never hits it because it stubs the whole drive out.
#include <climits>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#endif

/*
  See chome_disc.h for what this is and what it deliberately is not.

  Structure: everything above "the drive" is pure and runs against the installed
  reader, so the harness tests it against synthetic discs. Everything below talks to
  /dev/sr0 and is compiled out of the harness entirely (CHOME_HOST_TEST), because a
  test that needs a real disc in a real drive is a test nobody runs.
*/

/* --------------------------------------------------------------- reading ---- */

static disc_reader_fn reader = 0;
static void *reader_ctx = 0;

void disc_set_reader(disc_reader_fn fn, void *ctx)
{
	reader = fn;
	reader_ctx = ctx;
}

static int read_raw(int lba, uint8_t *dst)
{
	if (!reader) return -1;
	return reader(lba, DISC_READ_RAW, dst, reader_ctx);
}

static int read_user(int lba, uint8_t *dst)
{
	if (!reader) return -1;
	return reader(lba, DISC_READ_USER, dst, reader_ctx);
}

// memmem is a GNU extension and this file is built for two toolchains; a short
// search over 2 KB is not worth an ifdef.
static const uint8_t *find_bytes(const uint8_t *hay, int haylen, const void *needle, int nlen)
{
	if (nlen <= 0 || haylen < nlen) return 0;
	for (int i = 0; i <= haylen - nlen; i++)
	{
		if (!memcmp(hay + i, needle, nlen)) return hay + i;
	}
	return 0;
}

/* ------------------------------------------------------------ identifying --- */

static uint32_t iso_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
  Walk the ISO root directory looking for two things that are only visible in the
  file list rather than in a signature:

    - a Mega Drive+ disc, which is an ISO carrying both <name>.md and <name>.cue for
      the same name. Neither file alone means anything; the pair is the format.
    - an MSU-1 SNES disc, which carries a .sfc or .smc.

  Bounded at 1 MB of directory data. A root directory is a few KB; anything
  claiming megabytes is either corrupt or hostile, and this runs before we know
  which.
*/
static int iso_root_features(int data_lba0, int *has_mdplus, int *has_snes)
{
	uint8_t pvd[DISC_USER_SIZE];

	*has_mdplus = 0;
	*has_snes = 0;

	if (read_user(data_lba0 + 16, pvd)) return 0;
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return 0;

	const uint8_t *root = pvd + 156;
	if (root[0] < 34) return 0;

	uint32_t extent = iso_le32(root + 2);
	uint32_t size = iso_le32(root + 10);
	if (!extent || !size) return 0;

	enum { NAMES_MAX = 64, NAME_LEN = 128 };
	char md[NAMES_MAX][NAME_LEN];
	char cue[NAMES_MAX][NAME_LEN];
	int nmd = 0, ncue = 0;

	for (uint32_t done = 0; done < size && done < 1024 * 1024; done += DISC_USER_SIZE)
	{
		uint8_t sec[DISC_USER_SIZE];
		if (read_user(data_lba0 + extent + done / DISC_USER_SIZE, sec)) break;

		int off = 0;
		while (off < DISC_USER_SIZE && done + off < size)
		{
			int len = sec[off];
			if (!len) break;                                  // rest of this sector is padding
			if (len < 34 || off + len > DISC_USER_SIZE) break; // malformed: stop trusting it

			int nlen = sec[off + 32];
			if (nlen > 0 && nlen < NAME_LEN - 8 && off + 33 + nlen <= DISC_USER_SIZE)
			{
				char name[NAME_LEN];
				memcpy(name, sec + off + 33, nlen);
				name[nlen] = 0;

				// ISO9660 names carry a ";1" version suffix.
				char *semi = strchr(name, ';');
				if (semi) *semi = 0;

				char *dot = strrchr(name, '.');
				if (dot)
				{
					if (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc")) *has_snes = 1;
					else if (!strcasecmp(dot, ".md") && nmd < NAMES_MAX)
					{
						*dot = 0;
						snprintf(md[nmd++], NAME_LEN, "%s", name);
					}
					else if (!strcasecmp(dot, ".cue") && ncue < NAMES_MAX)
					{
						*dot = 0;
						snprintf(cue[ncue++], NAME_LEN, "%s", name);
					}
				}
			}
			off += len;
		}
	}

	for (int i = 0; i < nmd && !*has_mdplus; i++)
	{
		for (int j = 0; j < ncue; j++)
		{
			if (!strcasecmp(md[i], cue[j])) { *has_mdplus = 1; break; }
		}
	}

	return 1;
}

int disc_identify_at(int data_lba0)
{
	// No data track anywhere: that is an audio CD, and there is nothing to read.
	if (data_lba0 < 0) return DISC_T_AUDIO;

	uint8_t raw[DISC_RAW_SIZE * 2];

	/*
	  Mega Drive+ first. Such a disc is also a perfectly valid ISO and would be
	  caught by a later test as something else, so the more specific answer has to
	  come before the more general one.
	*/
	int has_mdplus = 0, has_snes = 0;
	iso_root_features(data_lba0, &has_mdplus, &has_snes);
	if (has_mdplus) return DISC_T_MDPLUS;

	// Signatures sitting in the first raw sector, past its 16-byte header.
	if (!read_raw(data_lba0, raw))
	{
		if (!memcmp(raw + 16, "SEGADISCSYSTEM", 14)) return DISC_T_MEGACD;
		if (!memcmp(raw + 16, "SEGA SEGASATURN", 15)) return DISC_T_SATURN;
		if (raw[16] == 0x01 && raw[17] == 0x5A && raw[18] == 0x5A &&
			raw[19] == 0x5A && raw[20] == 0x5A && raw[21] == 0x5A) return DISC_T_3DO;
	}

	/*
	  The ISO primary volume descriptor, 16 sectors in. Read raw because a CD-i
	  disc's descriptor sits at a different offset within the sector than a
	  CD-ROM's, so both places are tried.
	*/
	if (!read_raw(data_lba0 + 16, raw))
	{
		const uint8_t *iso = raw + 16;
		if (memcmp(iso + 1, "CD001", 5) && memcmp(iso + 1, "CD-I", 4)) iso = raw + 24;

		if (!memcmp(iso + 1, "CD001", 5))
		{
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return DISC_T_PSX;
			if (!memcmp(iso + 8, "NGCD", 4)) return DISC_T_NEOGEO;
		}
		if (!memcmp(iso + 1, "CD-I", 4)) return DISC_T_CDI;
	}

	// Discs whose only tell is a file in the root.
	for (int s = 16; s <= 40; s++)
	{
		uint8_t user[DISC_USER_SIZE];
		if (read_user(data_lba0 + s, user)) continue;
		if (find_bytes(user, sizeof(user), "IPL.TXT", 7)) return DISC_T_NEOGEO;
		if (find_bytes(user, sizeof(user), "CDI_APPL", 8)) return DISC_T_CDI;
	}

	/*
	  PC Engine CD puts its string somewhere in the first two sectors rather than at
	  a fixed offset, so this is a search rather than a compare.
	*/
	if (!read_raw(data_lba0, raw) && !read_raw(data_lba0 + 1, raw + DISC_RAW_SIZE))
	{
		if (find_bytes(raw, sizeof(raw), "PC Engine CD-ROM SYSTEM", 23)) return DISC_T_PCECD;
	}

	// Last, because a .sfc on a disc is weaker evidence than any signature above.
	if (has_snes) return DISC_T_SNES;

	return DISC_T_UNKNOWN;
}

int disc_label_at(int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	uint8_t user[DISC_USER_SIZE];
	if (read_user(data_lba0 + 16, user)) return 0;
	if (user[0] != 1 || memcmp(user + 1, "CD001", 5)) return 0;

	char lbl[33];
	memcpy(lbl, user + 40, 32);
	lbl[32] = 0;

	int end = 32;
	while (end > 0 && (lbl[end - 1] == ' ' || !lbl[end - 1])) end--;
	lbl[end] = 0;

	// ISO labels use underscores for spaces, and there is no promise the rest is
	// printable - this ends up on screen.
	for (int i = 0; i < end; i++)
	{
		if (lbl[i] == '_') lbl[i] = ' ';
		else if (lbl[i] < 0x20 || (uint8_t)lbl[i] > 0x7E) lbl[i] = ' ';
	}

	// Trim again: the substitutions above can leave trailing spaces.
	end = (int)strlen(lbl);
	while (end > 0 && lbl[end - 1] == ' ') end--;
	lbl[end] = 0;

	/*
	  Every PlayStation disc is labelled "PLAYSTATION", which tells the player
	  nothing they cannot see from the icon. Refused so the caller falls through to
	  the serial, which at least identifies the game.
	*/
	if (!strcasecmp(lbl, "PLAYSTATION")) return 0;

	snprintf(out, outsz, "%s", lbl);
	return (int)strlen(out);
}

int disc_serial_at(int data_lba0, char *out, int outsz)
{
	// Sony's publisher prefixes. A disc's serial is written into its boot
	// configuration as e.g. "SLUS_006.26;1".
	static const char *const pfx[] =
	{
		"SCES", "SLES", "SCUS", "SLUS", "SCPS", "SLPS", "SLPM", "SCPM",
		"SIPS", "SCED", "SLED", "SCZS", "PAPX", "PCPX", "PEPX", "PUPX",
	};

	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	for (int s = 16; s <= 64; s++)
	{
		uint8_t user[DISC_USER_SIZE];
		if (read_user(data_lba0 + s, user)) continue;

		for (size_t p = 0; p < sizeof(pfx) / sizeof(pfx[0]); p++)
		{
			const uint8_t *m = find_bytes(user, sizeof(user), pfx[p], 4);
			if (!m) continue;

			int avail = (int)(sizeof(user) - (m - user));
			const uint8_t *semi = find_bytes(m, avail, ";", 1);
			if (!semi) continue;

			int len = (int)(semi - m);
			if (len < 8 || len > 11) continue;      // "SLUS_006.26" is 11

			char id[16];
			memcpy(id, m, len);
			id[len] = 0;

			// Redump/ScreenScraper form: SLUS_006.26 -> SLUS-00626.
			if (id[4] == '_') id[4] = '-';
			char *dot = strchr(id, '.');
			if (dot) memmove(dot, dot + 1, strlen(dot));

			snprintf(out, outsz, "%s", id);
			return (int)strlen(out);
		}
	}

	return 0;
}

/* ------------------------------------------------------- the Sega header ---- */

/*
  Saturn and Mega CD both open their first data sector with a fixed-width header, and
  both put a product code in it. That is the whole reason this section exists: those
  two discs carry an identifier as exact as a PlayStation serial, and until now nothing
  read it, so they went out to the world under their ISO volume label instead.

  Read out of the *raw* sector at offset 16, which is not a detail to gloss over. It is
  the same read disc_identify_at() already made and the same offset it matched the
  signature at, so the bytes below are by construction the bytes that were identified -
  rather than a second read through read_user(), which resolves a Mode 2 sector at a
  different offset and would silently hand back the header shifted by eight.

  Both fields are fixed width and space-padded, so trimming is the whole of the parse.
  Everything that is not printable ASCII becomes a space first: this string is drawn on
  screen and used as a filename for the cached artwork, and a disc with a torn header is
  not a reason to write control characters into either.
*/
static int sega_field(const uint8_t *user, int off, int len, char *out, int outsz)
{
	if (!out || outsz < 2 || len <= 0 || off + len > DISC_USER_SIZE) return 0;
	if (len > outsz - 1) len = outsz - 1;

	int n = 0;
	for (int i = 0; i < len; i++)
	{
		unsigned char c = user[off + i];
		out[n++] = (c < 0x20 || c > 0x7E) ? ' ' : (char)c;
	}
	out[n] = 0;

	while (n > 0 && out[n - 1] == ' ') out[--n] = 0;

	int lead = 0;
	while (out[lead] == ' ') lead++;
	if (lead) memmove(out, out + lead, (size_t)(n - lead) + 1);

	return (int)strlen(out);
}

/*
  Whether what came out of a header field is worth calling an identifier.

  Four characters and at least one digit. Every real product code on either console has
  digits in it, and the two shapes this rejects are the two that actually turn up: a
  field that is all padding, and one whose digits were mangled badly enough that only a
  prefix survives ("MK-"). Returning either would be worse than returning nothing - the
  caller has no way to tell a bad code from a good one, so it would go out to
  ScreenScraper as the disc's name, spend an unmatched request, and cache the miss.
*/
static int sega_plausible(const char *s)
{
	if ((int)strlen(s) < 4) return 0;
	for (; *s; s++) if (*s >= '0' && *s <= '9') return 1;
	return 0;
}

/*
  Saturn: "SEGA SEGASATURN " at 0x00, then the maker id, then a ten-byte product number
  at 0x20 - "GS-9061", "MK-81088", "T-1809G" - with the version at 0x2A and the release
  date at 0x30 immediately behind it. Left-aligned and space-padded, and already the
  form Redump records for a Japanese disc, so nothing is rewritten on the way out.

  Confirmed against support/physical_disc/physical_disc.cpp in this same tree, which
  reads the identical offset and width for its save-folder name; against the field table
  in Sega's own Disc Format Standards as reproduced by three emulators and two dumping
  tools; and against the header written out verbatim in Lobotomy Software's released
  source for Exhumed, which is a real European disc and says "MK-81084  ".
*/
int disc_saturn_serial_at(int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	uint8_t raw[DISC_RAW_SIZE];
	if (read_raw(data_lba0, raw)) return 0;
	if (memcmp(raw + 16, "SEGA SEGASATURN", 15)) return 0;

	if (!sega_field(raw + 16, 0x20, 10, out, outsz)) return 0;
	if (!sega_plausible(out)) { out[0] = 0; return 0; }

	return (int)strlen(out);
}

/*
  Mega CD: "SEGADISCSYSTEM" at 0x00, then - 0x100 further in - the ordinary Mega Drive
  ROM header, whose fourteen-byte serial field at 0x180 nominally reads

      "GM MK-4407 -00"
       ^^ ^^^^^^^^ ^^
       |  |        +-- revision, or a country code in the same two digits
       |  +----------- the product code, padded to eight
       +-------------- media type: GM for a game, AI for the education titles

  and in practice does not. Real discs, from a matcher that keys on this exact field:

      "GM MK-4407 -00"   Sonic CD (USA)
      "GM MK-4407-00 "   Sonic CD (Europe)     - flush left, padded on the right instead
      "GM  T-81025-00"   Mortal Kombat         - two spaces, the code right-aligned
      "GM T-127015-00"   Lunar                 - a nine-character code, no padding at all
      "GM T-111065 -0"   Mad Dog II            - malformed: the revision fell off the end
      "GM MK- 4430  -"   Yumemi Mystery Mansion- malformed: the digits are inside the pad

  So this cannot be parsed by fixed sub-offsets, and trying to would give the wrong
  answer on two of those six. What is invariant is that the product code is one run of
  non-space characters: the padding is always beside it and never inside it, except on
  the discs that are broken anyway. Hence - trim, drop the media type, take the first
  word, then drop a revision if one is still attached to it.

  The revision is only removed when the tail is a dash and exactly two digits, which is
  what stops "T-81027" being eaten by its own last five.

  What comes out is the code as the disc carries it. Redump does not write it that way -
  its serial is transcribed from the printed disc face, and for Sega's own releases that
  face says "4407" where the header says "MK-4407" - so the two are reconciled at the
  other end, in key_forms() in tools/disctitles.py, which names this case and measures it.
*/
static void megacd_trim(char *s)
{
	int n = (int)strlen(s);

	// The media type, only when it really looks like one: two capitals then a space.
	if (n > 3 && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] == ' ')
	{
		memmove(s, s + 3, (size_t)(n - 3) + 1);
		n -= 3;
	}

	// Whatever padding that left in front, before the first word is taken - or the
	// "GM  T-81025-00" spelling would yield an empty one.
	int lead = 0;
	while (s[lead] == ' ') lead++;
	if (lead) { memmove(s, s + lead, (size_t)(n - lead) + 1); n -= lead; }

	// The code is one word. Everything past the first space is padding or a detached
	// revision, and on a malformed field it is the part that is wrong.
	char *sp = strchr(s, ' ');
	if (sp) { *sp = 0; n = (int)(sp - s); }

	// The revision, and nothing that merely ends in digits.
	if (n > 3 && s[n - 3] == '-' && s[n - 2] >= '0' && s[n - 2] <= '9' &&
		s[n - 1] >= '0' && s[n - 1] <= '9')
	{
		n -= 3;
		s[n] = 0;
	}
}

int disc_megacd_serial_at(int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	uint8_t raw[DISC_RAW_SIZE];
	if (read_raw(data_lba0, raw)) return 0;
	if (memcmp(raw + 16, "SEGADISCSYSTEM", 14)) return 0;

	if (!sega_field(raw + 16, 0x180, 14, out, outsz)) return 0;
	megacd_trim(out);
	if (!sega_plausible(out)) { out[0] = 0; return 0; }

	return (int)strlen(out);
}

/*
  The serial for a disc we have already identified, which is the only form the rest of
  the front-end asks in.

  Typed rather than tried-in-turn, and that is the point of it. The old code ran the
  PlayStation scan against every disc whatever it was - up to forty-nine sector reads on
  a Saturn disc that could never contain a Sony prefix - and then fell back to the volume
  label for the systems it had nothing for. Dispatching on the type it has already
  established costs one read for the Sega discs, none for the systems this cannot answer
  for, and it is what stops a disc being described by a key belonging to another console.

  UNKNOWN keeps the PlayStation scan. A disc that failed every signature but still has a
  Sony serial written into it is better identified than not, and it is exactly the case
  the scan was written to be loose about.

  PC Engine CD and Neo Geo CD return nothing on purpose - see disc_display_name().
*/
/*
  The title a Sega disc carries in its own header, which is not the same string as the
  ISO9660 volume id and is a much better one.

  Measured against 37 Saturn discs on the card, asking ScreenScraper for each:

    ISO volume id     23/37 matched, and SEVEN discs have no volume id at all - Daytona
                      USA, Virtua Cop, Panzer Dragoon, Myst, Bug!, Magic Knight Rayearth
                      and Clockwork Knight are simply nameless on the shelf today.
    header title      30/37 matched, present on 37 of 37, and not one wrong answer in
                      the whole set.

  The volume id is also the worse string to *show* somebody, because it is a filename:
  "B_RANGERS" for Burning Rangers, "S_BOMBERMAN", "AZEL_1" for Panzer Dragoon Saga,
  "SEGARALLY_CHAMPIONSHIP" with the space missing. The header spells the title out.

  Offsets, from the same field tables as the serial readers above:

    Saturn   0x60, 112 bytes. One field, space-padded.
    Mega CD  0x150, 48 bytes for the international title, with 0x120 (the domestic one)
             behind it - a Japanese disc leaves the international field blank rather
             than absent, so an empty result there falls through rather than winning.

  PlayStation is deliberately absent: a PSX disc has no such header, its volume id is
  the string "PLAYSTATION" on every single disc (disc_label_at() refuses it by name),
  and its serial is both present and indexed by ScreenScraper - which the same study
  measured at 9 of 9 correct. Nothing here would improve it.
*/
int disc_title_at(int type, int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	uint8_t raw[DISC_RAW_SIZE];
	if (read_raw(data_lba0, raw)) return 0;

	if (type == DISC_T_SATURN)
	{
		if (memcmp(raw + 16, "SEGA SEGASATURN", 15)) return 0;
		return sega_field(raw + 16, 0x60, 112, out, outsz);
	}

	if (type == DISC_T_MEGACD)
	{
		if (!find_bytes(raw + 16, 64, "SEGADISCSYSTEM", 14)) return 0;
		if (sega_field(raw + 16, 0x150, 48, out, outsz)) return (int)strlen(out);
		return sega_field(raw + 16, 0x120, 48, out, outsz);
	}

	return 0;
}

int disc_serial_for(int type, int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;

	switch (type)
	{
	case DISC_T_PSX:
	case DISC_T_UNKNOWN: return disc_serial_at(data_lba0, out, outsz);
	case DISC_T_SATURN:  return disc_saturn_serial_at(data_lba0, out, outsz);
	case DISC_T_MEGACD:  return disc_megacd_serial_at(data_lba0, out, outsz);
	}

	return 0;
}

/* ------------------------------------------------------------ names, cores --- */

const char *disc_type_name(int type)
{
	switch (type)
	{
	case DISC_T_MEGACD:  return "Mega CD";
	case DISC_T_SATURN:  return "Saturn";
	case DISC_T_PSX:     return "PlayStation";
	case DISC_T_PCECD:   return "PC Engine CD";
	case DISC_T_NEOGEO:  return "Neo Geo CD";
	case DISC_T_3DO:     return "3DO";
	case DISC_T_CDI:     return "CD-i";
	case DISC_T_MDPLUS:  return "Mega Drive+";
	case DISC_T_SNES:    return "SNES MSU-1";
	case DISC_T_AUDIO:   return "Audio CD";
	case DISC_T_UNKNOWN: return "Unknown disc";
	}
	return "";
}

/*
  Which shelf system this disc BELONGS to - asked of the disc, and answered whether or
  not anything in this firmware can play the pressed disc.

  This is the plain "what console is this" question, and it is the one every question
  about *the card* is asked through: which folder a copy is filed in, which core reads
  it back afterwards. None of that involves the drive, so none of it may be gated on
  whether a daemon can stream sectors off one.

  Nothing answers here that is not a system in chome_lib's table. 3DO and CD-i are
  identified by the sector parser but have no shelf entry at all, so there is no folder
  to name and no core to name it for; an audio CD is nobody's game. Those three, and
  UNKNOWN, are the cases where "we identified it" and "we have somewhere to put it" are
  genuinely different answers, and the caller has to ask both.

  Mega CD answers "md", and that is the console rather than the destination: the Mega
  Drive row is where a player looks for Sega, and its launch already overrides the rbf
  to the MegaCD core. Where a *copy* of an md disc goes is one more step on from here
  and is answered in exactly one place - rip_target::dest in chome_ui.cpp - because a
  folder of tracks in games/Genesis is not a Mega Drive game and never became a card.
  PC Engine CD and Neo Geo CD are the same shape.
*/
const char *disc_console_id(int type)
{
	switch (type)
	{
	case DISC_T_PSX:    return "psx";
	case DISC_T_MEGACD: return "md";      // the Mega Drive core loads Mega CD
	case DISC_T_SATURN: return "saturn";
	case DISC_T_PCECD:  return "tg16";
	case DISC_T_NEOGEO: return "neogeo";
	case DISC_T_MDPLUS: return "md";
	case DISC_T_SNES:   return "snes";
	}
	return 0;
}

/*
  Which of our shelf systems can be handed the *pressed disc* - which is the console
  above, less the one console whose daemon cannot read a drive.

  Saturn is that one, and it is worth being precise about since the obvious reading is
  wrong: Saturn IS a shelf system, and a .cue or .chd in games/Saturn is a card that
  launches the Saturn core. What it has no entry in is disc_playables - saturncdd.cpp
  has not been taught to stream sectors from a drive the way megacdd and pcecdd have -
  so there is no core to hand the *drive* to, and offering a Play here could only fail.

  Subtracting from disc_console_id() rather than listing a second table is the point of
  the split. The two questions had one answer for as long as they agreed, and what that
  cost was a Saturn disc the shelf could read, name and file, and offered no Copy for -
  because the copy was being asked which core could play it. This function is the only
  place the drive's limits are allowed to narrow the answer, and the narrowing is one
  line long and says why.
*/
const char *disc_system_id(int type)
{
	if (type == DISC_T_SATURN) return 0;
	return disc_console_id(type);
}

/*
  Which shelf system to ask the *database* about - a third question, and the third
  different answer, which is why it is a third function rather than an argument to one
  of the two above.

  disc_console_id() answers "md" for a Mega CD disc because the Mega Drive row is where
  a player looks for Sega and where a copy is filed. To ScreenScraper that is the wrong
  platform outright: Mega-CD is systeme 20 and Mega Drive is systeme 1, they hold
  different games, and asking the cartridge platform for a CD game is a request that
  cannot match. The same held for PC Engine CD asked as tg16 and Neo Geo CD asked as
  neogeo. Three consoles were quietly scraping against the platform next door.

  Nothing about the folder or the core changes here - both of those still go through
  disc_console_id(), which is why that stayed exactly as it was. This is only ever read
  by the artwork request.

  Mega Drive+ is the one disc that genuinely belongs to the cartridge platform: it is a
  Mega Drive ROM carried on a CD, and the game it holds is a Mega Drive game.
*/
const char *disc_scrape_id(int type)
{
	switch (type)
	{
	case DISC_T_PSX:    return "psx";
	case DISC_T_SATURN: return "saturn";
	case DISC_T_MEGACD: return "megacd";
	case DISC_T_PCECD:  return "pcecd";
	case DISC_T_NEOGEO: return "neogeocd";
	case DISC_T_MDPLUS: return "md";
	case DISC_T_SNES:   return "snes";
	}
	return 0;
}

int disc_capable_systems(const char **out, int max)
{
	if (!out || max <= 0) return 0;

	int n = 0;
	for (int t = DISC_T_NONE; t <= DISC_T_UNKNOWN; t++)
	{
		const char *id = disc_system_id(t);
		if (!id) continue;

		// Two disc types share the Mega Drive core, so de-duplicate.
		int seen = 0;
		for (int i = 0; i < n; i++) if (!strcmp(out[i], id)) seen = 1;
		if (seen) continue;

		out[n++] = id;
		if (n >= max) break;
	}

	return n;
}

/* ------------------------------------------------------------- the drive ---- */

static int dstate = DISC_ABSENT;
static int dtype = DISC_T_NONE;
static char dserial[DISC_SERIAL_LEN];
static char dlabel[DISC_LABEL_LEN];
static int ddirty = 0;
static int watching = 0;

int  disc_state() { return dstate; }
int  disc_type()  { return dtype; }
const char *disc_serial() { return dserial; }
const char *disc_label()  { return dlabel; }

int disc_take_dirty()
{
	int d = ddirty;
	ddirty = 0;
	return d;
}

/*
  What to put under the icon.

  The title table comes first, because it is the only layer that can produce a name a
  player recognises. A pressed disc has no filename, so the two things below it are
  the two things the disc itself carries: a volume label, which is whatever the
  mastering engineer typed and is sometimes the game and sometimes "PLAYSTATION", and
  a serial, which is exact and unreadable. "SLES-01506" is the right answer to the
  wrong question.

  Below the table, the order is unchanged - label, then serial, then the console's
  name - so a card with no table on it behaves exactly as it did before this existed.
  That is the whole contract: disc_title_for() returns 0 for a missing file, and 0
  falls straight through to what was here before.

  Both identifiers are offered to the table, serial first, because they are the only
  handle each system gives us: PlayStation discs carry a serial and PC Engine and Neo
  Geo discs do not, so for those the label *is* the key. Asking twice is free after
  the first frame - chome_titles.cpp caches both answers, misses included, which it
  has to because this function runs on every frame that draws the disc.
*/
const char *disc_display_name()
{
	const char *t = dserial[0] ? disc_title_for(dserial) : 0;
	if (!t && dlabel[0]) t = disc_title_for(dlabel);
	if (t) return t;

	if (dlabel[0]) return dlabel;
	if (dserial[0]) return dserial;
	return disc_type_name(dtype);
}

/*
  What to put in the *request*, which is not what to put on the screen - and the two had
  been the same string, which is the bug this fixes.

  disc_display_name() above prefers the volume label, correctly: a label is the closest
  thing to a human name a disc offers, and showing "SEGARALLY CHAMPIONSHIP" beats showing
  "MK-81088". But that same string was then sent to ScreenScraper as a rom name, and a
  volume label is not a rom name. jeuInfos.php matches romnom exactly, against filenames;
  no label was ever indexed as one, so the request could not match. It still cost the
  account a request, and a failed match is charged twice over - once to the day's total
  and once to the unmatched allowance, which is roughly a tenth the size and is the one
  that runs out. That is how a disc nobody could identify became the expensive kind of
  disc.

  So the order here is the opposite of the display's, and the bottom of it is nothing:

    the title, when the table resolved one from either identifier. An exact name, and
    the case the whole title table exists to produce.

    else the serial, which is exact and is at least a string the database could hold.

    else NOTHING, and this is the part that had to be written down. PC Engine CD discs
    have no ISO filesystem at all, so they have no label either and this was already the
    outcome. Neo Geo CD discs do have one, and reading fifteen of them is what settled
    it: "DD_CD", "B4CD", "CR2CD", "CD_DATA", "C205", "20111222_1507" and one flat
    "UNTITLED" - house codes, a mastering default and a timestamp. Sending those spends
    the scarce allowance to learn nothing, and "UNTITLED" is worse than nothing, because
    the artwork cache is keyed on this string and two different discs would share one
    picture.

  Returning 0 is therefore a real answer and callers must treat it as "do not ask",
  rather than falling back to something they happen to have.
*/
const char *disc_scrape_name()
{
	// 1. The offline table, by serial. Exact, free, and the best answer there is. This is
	//    the branch every PlayStation disc takes - the table is largely Sony serials.
	if (dserial[0])
	{
		const char *t = disc_title_for(dserial);
		if (t && t[0]) return t;
	}

	// 2. The table again, by whatever name the disc gave.
	if (dlabel[0])
	{
		const char *t = disc_title_for(dlabel);
		if (t && t[0]) return t;
	}

	/*
	  3. The disc's own name, unresolved - and this branch is why the order changed.

	  Teaching the helper to read Saturn product numbers (see disc_serial_for() above)
	  moved every Saturn disc into branch 1, where the table misses: it holds 12762
	  entries and seven of them begin "MK", so Sega product numbers are effectively not
	  in it. The old code then returned the bare serial as the name to search for, and
	  that is measurably the worst thing to send. Asking ScreenScraper for Saturn:

	      romnom "MK-81207"                 miss
	      romnom "SEGA RALLY CHAMPIONSHIP"  hit

	  over 37 discs, 30 of the header titles matched and not one returned the wrong game.
	  So a fix that only read the serial would have left Saturn art worse than before it,
	  which is the sort of thing that is obvious in a measurement and invisible in review.

	  Confined to the two Sega CD systems, because that is where the measurement was taken.
	  Neo Geo CD and PC Engine CD stay silent - see chome_disc.h: they carry no product code
	  at all, nobody has shown their volume labels are indexed, and a miss costs the scarce
	  allowance. Widening this to "any disc with any name" would spend that allowance on a
	  guess, which is the opposite of what the numbers above licence. It also broke the test
	  that guards their silence, which is the test doing its job.
	*/
	if (dlabel[0] && (dtype == DISC_T_SATURN || dtype == DISC_T_MEGACD)) return dlabel;

	/*
	  And NOT the bare serial, which this used to return as a last resort.

	  It was flagged as a product call and Dinofly took the narrow option: the serial is now
	  asked for as serialnum, an exact key, by the caller that has it - see disc_art_request()
	  and ss_query::serialnum. A serial as a *name* is fuzzy matched and does not merely miss,
	  it answers confidently wrong: "SLUS-00594" came back as "Beyblade Burst - Battle Zero",
	  a real cover for a real game that is not in the drive. Nothing downstream can detect
	  that, which is what makes it worse than no cover at all.

	  So this returns nothing, and "nothing" is a complete answer: the dialog draws the
	  generated disc face, which is honest about not knowing.
	*/
	return 0;
}

static int identify_pending = 0;

static void disc_forget()
{
	if (dstate != DISC_ABSENT || dtype != DISC_T_NONE) ddirty = 1;
	dstate = DISC_ABSENT;
	dtype = DISC_T_NONE;
	dserial[0] = 0;
	dlabel[0] = 0;
	identify_pending = 0;
}

/*
  The state machine, kept out of the ioctl path on purpose.

  The two-phase shape - "there is a disc" now, "it is a PlayStation disc" later - is
  the whole reason the spinning icon exists, so it is the last thing that should
  only be exercisable with a disc in a drive. disc_poll() below does nothing but
  read the drive and call these two; the harness calls them directly.
*/
void disc_ingest_present(int present)
{
	if (!present)
	{
		disc_forget();
		return;
	}

	if (dstate != DISC_ABSENT) return;      // already known about, not a new arrival

	/*
	  Deliberately does not identify here. The drive is almost certainly still
	  spinning up, and reading a sector now blocks the thread that draws for as long
	  as that takes - which is exactly the moment the player is waiting to see
	  something happen.
	*/
	dstate = DISC_SPINNING;
	dtype = DISC_T_NONE;
	dserial[0] = 0;
	dlabel[0] = 0;
	identify_pending = 1;
	ddirty = 1;
}

int disc_identify_due()
{
	return identify_pending;
}

void disc_ingest_identify(int lba0)
{
	if (!identify_pending) return;
	identify_pending = 0;

	dtype = disc_identify_at(lba0);
	dstate = (dtype == DISC_T_UNKNOWN) ? DISC_UNKNOWN : DISC_READY;

	// The disc's own title where it has one, the ISO volume id otherwise. Same order as
	// the helper, and that is not a coincidence to be maintained by hand - see below.
	if (!disc_title_at(dtype, lba0, dlabel, sizeof(dlabel)))
		disc_label_at(lba0, dlabel, sizeof(dlabel));

	// After the type, and given it: which identifier a disc carries is a fact about
	// which console pressed it. See disc_serial_for().
	disc_serial_for(dtype, lba0, dserial, sizeof(dserial));
	ddirty = 1;

	/*
	  This function and the identify block inside helper_main() are the same procedure
	  written twice - this one for the harness, that one for the drive - and the pair is
	  how a real bug shipped and stayed shipped.

	  This copy called disc_serial_for() from the day it was written. The helper called
	  disc_serial_at(), the PlayStation-only reader, so every physical Saturn and Mega CD
	  disc came out with no serial while the tested path was provably correct. The suite
	  could not fail: it was not exercising the code that runs on the device.

	  So when either half changes, the other one has to change in the same commit, and
	  neither is authority for the other. If a third caller ever appears, the answer is to
	  lift these four lines into one function both call rather than to write them again.
	*/
}

/* --------------------------------------------------- probing and backoff ---- */

/*
  Two small decisions pulled out of disc_watch_start() and disc_poll() so they can be
  tested without a device node, a fork(), or a wall clock: whether it is time to look
  for a drive again, and whether it is time to fork a replacement helper. Both are
  pure - the same three ints always give the same answer - which is what lets the
  harness drive "no drive at boot, one appears a few seconds later" and "a helper
  that keeps dying gets backed off" as ordinary checks instead of something that
  needs real hardware and real time to pass.
*/

// Sentinel for "never looked yet", distinct from every real clock value.
#define DISC_NEVER_PROBED (-1)

// Seconds between retries once no drive has been found. The probe itself is three
// open() calls that fail immediately when nothing is at the path - there is no seek
// or spin-up to make a fast retry expensive - so this floor is for the log, not the
// drive: "no optical drive" printed every frame would drown out everything else on
// the console. A few seconds is short enough that a USB drive enumerating a moment
// after the menu comes up is found well before anyone would think to reboot over it.
#define DISC_PROBE_RETRY_S 5

/*
  Whether disc_watch_start() should try opening a drive again right now.

    found       a drive is already known and being watched - always false once this
                is true, so the parent never opens a second fd racing its own helper.
    last_probe  DISC_NEVER_PROBED before the first attempt, else the time (same
                clock as `now`) of the previous one.
    now         the current time, same clock as `last_probe`.
*/
int disc_probe_due(int found, int last_probe, int now)
{
	if (found) return 0;
	if (last_probe == DISC_NEVER_PROBED) return 1;
	return (now - last_probe) >= DISC_PROBE_RETRY_S;
}

// A helper that dies inside this many seconds of its own fork is "instant" - too
// fast to have done any real work, so it is almost certainly failing the same way it
// just failed rather than hitting a fresh problem.
#define DISC_HELPER_QUICK_DEATH_S 2

// Backoff once two helpers in a row have died instantly. Long enough that a helper
// which can never open the device - wrong permissions, a drive gone between the
// probe and the fork - does not turn into a fork() bomb: one CPU doing nothing but
// forking and dying, forever, behind a UI whose log never repeats itself and so
// looks healthy.
#define DISC_HELPER_BACKOFF_S 10

/*
  Whether disc_poll() should fork a replacement helper right now.

    quick_deaths  consecutive helpers that died within DISC_HELPER_QUICK_DEATH_S
                  seconds of their own fork. 0 means either none has died yet or the
                  last one ran a normal while before it did.
    last_fork     the time the most recent fork was attempted.
    now           the current time.

  The first quick death still reforks at once - one bad fork is not a pattern, and a
  drive that was just found a moment ago is worth trying again immediately. Only a
  *second* consecutive quick death - the replacement dying just as fast - switches to
  the backoff above.
*/
int disc_refork_due(int quick_deaths, int last_fork, int now)
{
	if (quick_deaths <= 1) return 1;
	return (now - last_fork) >= DISC_HELPER_BACKOFF_S;
}

// See chome_disc.h. Above the split on purpose: this is the production decision, and both
// disc_poll()s below - the real one and the harness's - ask it rather than restating it.
int disc_release_due(int flag_on, int watching_now, int helper_alive)
{
	if (flag_on) return 0;
	return (watching_now || helper_alive) ? 1 : 0;
}

#ifdef CHOME_HOST_TEST

/*
  The harness drives the state machine directly rather than through a drive. Only
  the identification above is under test; the ioctl plumbing below is not something
  a host test can say anything true about.
*/
int  disc_watch_start() { watching = 1; return 1; }
void disc_watch_stop()  { watching = 0; disc_forget(); }
int  disc_watching()    { return watching; }

/*
  Everything the real disc_poll() does is a device access except one branch, and this is
  that branch: giving the drive back when classicui_disc goes off. Kept here rather than
  emptied out because the setting is now a row on a screen, so that transition is something
  a player can cause - and asking disc_release_due() rather than restating its test is what
  makes the assertion in the harness an assertion about the shipped decision.
*/
void disc_poll()
{
	if (disc_release_due(cfg.classicui_disc, watching, 0)) disc_watch_stop();
}
void disc_reset_reader() { reader = 0; reader_ctx = 0; }

#else

/*
  The drive is owned by a helper child. The parent never touches it.

  Two hardware findings forced this, in order:

  1. Identification inline on the draw thread froze the front-end. It issues up to
     ~30 sequential SCSI reads with multi-second timeouts, and on a drive that would
     not answer, the firmware sat in state D in blk_execute_rq for minutes.

  2. Moving only the *reads* into a child was not enough. With the child busy on the
     drive, the parent's own CDROM_DRIVE_STATUS ioctl blocked too - state D in
     sr_block_ioctl. Every ioctl on /dev/sr0 serialises behind whatever the drive is
     doing, so there is no such thing as a cheap status poll while a disc is being
     read.

  So the parent's only contact with the disc is reading a small file out of /tmp. The
  helper owns the fd, polls the status, identifies, and rewrites that file. A drive
  that wedges costs a stuck helper; the console keeps drawing.

  The helper is deliberately a process and not a thread: a thread stuck in an
  uninterruptible ioctl cannot be killed, and it would hold the same address space as
  the UI. A process can be abandoned.
*/

#define DISC_STATE_FILE "/tmp/classicui_disc_state"

/*
  Poll intervals, in seconds. Both are status queries; neither reads the disc. See the
  comment in helper_main() for why the settled one is as long as it is.
*/
// How many times to read a disc that came back without a name, and how long to wait
// between tries. Three reads a second apart covers a drive that is merely slow to come
// up to speed, without leaving a genuinely unreadable disc spinning in "Reading the
// disc" for longer than a person will wait.
#define DISC_ID_TRIES        3
#define DISC_ID_RETRY_S      1

#define DISC_POLL_EMPTY_S    1
#define DISC_POLL_SETTLED_S  30

static pid_t helper_pid = -1;

// When the current (or most recent) helper was forked, and the path it was forked
// onto - kept so a refork after a death does not have to re-probe /dev/sr0 et al.,
// and so disc_refork_due() has a clock to measure against.
static int helper_fork_t = 0;
static char dev_path[32] = {};

// Consecutive helpers that died within DISC_HELPER_QUICK_DEATH_S of their own fork.
// See disc_refork_due().
static int quick_deaths = 0;

// disc_watch_start()'s own retry state: the last time it looked for a drive and
// found none, and whether "no optical drive" has already been said once. See
// disc_probe_due().
static int last_probe_t = DISC_NEVER_PROBED;
static int no_drive_logged = 0;

// ------------------------------------------------------------------ the helper

static int helper_fd = -1;

static void quiet_the_drive(const char *dev)
{
	const char *name = strrchr(dev, '/');
	name = name ? name + 1 : dev;

	char path[128];
	FILE *f;

	// Readahead is wasted on sectors asked for one at a time, and the kernel's own
	// media polling fights ours for the drive. Both best-effort.
	snprintf(path, sizeof(path), "/sys/block/%s/queue/read_ahead_kb", name);
	if ((f = fopen(path, "w"))) { fputs("0", f); fclose(f); }

	snprintf(path, sizeof(path), "/sys/block/%s/events_poll_msecs", name);
	if ((f = fopen(path, "w"))) { fputs("-1", f); fclose(f); }
}

/*
  Reading sectors, using the kernel's own paths rather than raw SCSI.

  The first version issued SCSI READ CD (0xBE) through SG_IO, which is what the fork
  does and what every ripping tool does. On this drive - an HL-DT-ST DVDRAM GUD1N
  over USB - it **wedges**: the request sits in `blk_execute_rq` indefinitely and the
  `io.timeout` never rescues it, because the command never reaches the drive to time
  out against. Measured, not guessed: the helper stayed in state D for minutes while
  the disc sat there perfectly readable.

  Perfectly readable by other means, that is. On the same drive and the same disc:

    - CDROMREADTOCHDR / CDROMREADTOCENTRY  work
    - CDROMREADRAW (one raw 2352-byte sector) works
    - an ordinary read() at lba * 2048     works

  So that is what this uses. The cost is that CDROMREADRAW is CD-only and one sector
  at a time, which is irrelevant here - identification reads a few dozen sectors once
  - and would matter only for streaming playback, which is not this file's job.

  Keep SG_IO in mind if a drive ever refuses CDROMREADRAW; the two are alternatives
  and the fork carries both for what is presumably this reason.
*/

// One raw 2352-byte sector, via the kernel rather than SG_IO.
static int ioctl_read_raw(int lba, uint8_t *dst)
{
	union
	{
		struct cdrom_msf msf;
		uint8_t raw[DISC_RAW_SIZE];
	} req;

	// CDROMREADRAW addresses by MSF, and MSF counts from the 2-second pregap.
	int f = lba + 150;
	memset(&req, 0, sizeof(req));
	req.msf.cdmsf_min0 = (uint8_t)(f / (75 * 60));
	req.msf.cdmsf_sec0 = (uint8_t)((f / 75) % 60);
	req.msf.cdmsf_frame0 = (uint8_t)(f % 75);

	if (ioctl(helper_fd, CDROMREADRAW, &req) < 0) return -1;
	memcpy(dst, req.raw, DISC_RAW_SIZE);
	return 0;
}

/*
  The 2048-byte user area, read as a block device. pread rather than lseek+read so
  the fd has no shared position - the helper is the only reader, but a stateless read
  is one less thing to reason about.
*/
static int block_read_user(int lba, uint8_t *dst)
{
	ssize_t n = pread(helper_fd, dst, DISC_USER_SIZE, (off_t)lba * DISC_USER_SIZE);
	return (n == DISC_USER_SIZE) ? 0 : -1;
}

static int helper_reader(int lba, int mode, uint8_t *dst, void *ctx)
{
	(void)ctx;
	if (helper_fd < 0) return -1;

	if (mode == DISC_READ_USER) return block_read_user(lba, dst);
	return ioctl_read_raw(lba, dst);
}

/*
  Where the first data track starts, or -1 when the table of contents is all audio -
  which is how an audio CD is recognised before a sector is read.
*/
static int find_data_track()
{
	struct cdrom_tochdr hdr;
	if (ioctl(helper_fd, CDROMREADTOCHDR, &hdr) < 0) return -1;

	for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1; t++)
	{
		struct cdrom_tocentry e;
		memset(&e, 0, sizeof(e));
		e.cdte_track = (uint8_t)t;
		e.cdte_format = CDROM_LBA;

		if (ioctl(helper_fd, CDROMREADTOCENTRY, &e) < 0) continue;
		if (e.cdte_ctrl & CDROM_DATA_TRACK) return e.cdte_addr.lba;
	}

	return -1;
}

static void helper_write(int state, int type, const char *serial, const char *label)
{
	char tmp[80];
	snprintf(tmp, sizeof(tmp), "%s.new", DISC_STATE_FILE);

	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%d|%d|%s|%s\n", state, type, serial ? serial : "", label ? label : "");
	fclose(f);

	// Renamed into place so the parent never reads a half-written line.
	rename(tmp, DISC_STATE_FILE);
}

static void helper_main(const char *dev)
{
	helper_fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (helper_fd < 0) _exit(1);

	quiet_the_drive(dev);
	disc_set_reader(helper_reader, 0);

	int last = -1;
	pid_t parent = getppid();

	for (;;)
	{
		// Leave with the front-end rather than outliving it holding the drive.
		if (getppid() != parent) _exit(0);

		int st = ioctl(helper_fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);

		if (st != last)
		{
			last = st;

			if (st != CDS_DISC_OK)
			{
				helper_write(DISC_ABSENT, DISC_T_NONE, "", "");
			}
			else
			{
				// Say "there is a disc" before doing the slow part, so the front-end
				// can start its spinning icon while the drive is still seeking.
				helper_write(DISC_SPINNING, DISC_T_NONE, "", "");

				/*
				  Read it more than once when the first read came back nameless.

				  disc_serial_at() walks sectors 16 to 64 and silently skips any it
				  cannot read, so a drive still coming up to speed can return nothing
				  and still look like a completed answer. Combined with the once-per
				  -insertion rule above, that made a transient spin-up failure
				  permanent for as long as the disc stayed in the tray: the panel said
				  "PlayStation" and nothing else, for ever, while the very same disc
				  named itself correctly after a reboot because the reboot re-read it.

				  That is what the owner hit with disc 2 of a PAL Metal Gear Solid, and
				  it is the third bug of this shape in this file - the drive probe, the
				  dead helper, and now this. The lesson is the same each time: on a
				  spinning disc, one look is a sample, not an answer.

				  Only a *nameless* result is retried, and only a few times. A disc that
				  is genuinely unidentifiable must still settle on DISC_UNKNOWN quickly
				  rather than sit in "Reading the disc" for ever - a spinner that never
				  resolves is worse than a wrong answer. A disc that named itself on the
				  first read costs nothing: the loop runs once.
				*/
				int lba0 = 0, t = DISC_T_UNKNOWN;
				char ser[DISC_SERIAL_LEN] = {};
				char lbl[DISC_LABEL_LEN] = {};

				for (int try_n = 0; try_n < DISC_ID_TRIES; try_n++)
				{
					if (try_n) sleep(DISC_ID_RETRY_S);

					lba0 = find_data_track();
					t = disc_identify_at(lba0);

					ser[0] = 0;
					lbl[0] = 0;

					/*
					  disc_serial_for(), not disc_serial_at(). The difference is which
					  consoles get a serial at all.

					  disc_serial_at() is the PlayStation reader: it walks sectors 16..64
					  looking for Sony's publisher prefixes in a boot configuration file.
					  On a Saturn or a Mega CD disc it finds none - correctly, they are not
					  there - and writes an empty serial. So every physical Saturn and Mega
					  CD disc came out unidentified, while disc_saturn_serial_at() and
					  disc_megacd_serial_at() sat right there in this file, implemented and
					  commented down to the six malformed Mega CD headers they cope with,
					  and were never once called on the path that reads a real disc.

					  Measured rather than reasoned: a Sega Rally disc in the drive carries
					  "MK-81207" at the documented offset - dd off /dev/sr0 shows it - and
					  the state file this helper wrote said the serial was "".

					  Only this call site was wrong. disc_serial_for() is what the other one
					  (see above, in the rip path) has always used, which is why a rip names
					  a Saturn disc properly and the shelf did not.

					  Dispatched on `t` from the line above, and DISC_T_UNKNOWN still routes
					  to the PlayStation reader inside disc_serial_for(), so a disc that
					  named no console behaves exactly as it did before this change.
					*/
					disc_serial_for(t, lba0, ser, sizeof(ser));

					/*
					  The disc's own title first, the ISO volume id only if it has none.
					  See disc_title_at() for the measurement behind that order: on Saturn
					  the volume id is missing outright on seven of thirty-seven discs and
					  matches ScreenScraper on 23 where the header title matches 30.

					  Not the other way round even though the volume id is what shipped:
					  a Sega disc that fills in its own title is describing itself, and a
					  volume id is a filename that happens to be nearby.
					*/
					if (!disc_title_at(t, lba0, lbl, sizeof(lbl)))
						disc_label_at(lba0, lbl, sizeof(lbl));

					// Named, or nothing there to name: either way the answer is in.
					if (ser[0] || lbl[0] || t == DISC_T_UNKNOWN) break;

					printf("ClassicUI: disc read %d gave a %s with no name, reading again\n",
						try_n + 1, disc_type_name(t));
				}

				helper_write(t == DISC_T_UNKNOWN ? DISC_UNKNOWN : DISC_READY, t, ser, lbl);
			}
		}

		/*
		  How often to ask the drive again.

		  First, what this loop does *not* do, because I described it carelessly once and it
		  matters: the disc is read exactly once per insertion. Every sector access -
		  find_data_track, identify, serial, label - happens inside the state-change branch
		  above, which only runs when the status transitions. Once a disc is identified this
		  loop never touches its surface again. It does not re-spin the disc to keep the icon
		  turning; the icon is animation and knows nothing about the drive.

		  What remains is a status ioctl, which asks the drive's controller whether media is
		  present. That is not a disc read, but it is not free either: on some drives
		  TEST UNIT READY can provoke a spin-up to check the media, and there is no portable
		  way to know whether this drive is one of them.

		  Which leaves a genuine constraint rather than a bug: tray-open cannot be noticed
		  without somebody asking periodically. The kernel's own polling
		  (events_poll_msecs) is the same query on the same drive, just moved, and it is
		  disabled here anyway. So the choice is how often, and the honest position is "as
		  rarely as the interface tolerates":

		    disc present, identified   30s. Nothing is waiting on this. The only thing left
		                               to notice is the disc leaving, and a badge that
		                               lingers half a minute after an eject costs nothing -
		                               nothing acts on it, and re-identification happens on
		                               the transition back.
		    no disc                    1s. An insertion is something the player just did and
		                               is waiting to see acknowledged, and an empty drive
		                               has no disc to disturb.

		  If even the 30s query turns out to wake the drive on this hardware, the next step
		  is to stop entirely once identified and re-check only when the player opens the
		  disc prompt - at the cost of a badge that can be wrong until they look. That is a
		  product decision, not a technical one, and it is Dinofly's to make.
		*/
		int wait = (st == CDS_DISC_OK) ? DISC_POLL_SETTLED_S : DISC_POLL_EMPTY_S;

		sleep(wait);
	}
}

// ------------------------------------------------------------------ the parent

int disc_watching() { return watching; }

/*
  Fork a helper onto dev_path (already known to open) and record when, so
  disc_refork_due() has a clock to measure the next death against.

  A fork() failure is fed into the same quick_deaths counter as a helper that opens
  and immediately exits - to the caller both are "that attempt did not produce a
  running helper", and both should back off the same way rather than one of them
  retrying every frame forever.
*/
static int fork_helper()
{
	pid_t pid = fork();
	int now = (int)time(0);

	if (pid < 0)
	{
		quick_deaths++;
		helper_fork_t = now;
		helper_pid = -1;
		return 0;
	}

	if (!pid)
	{
		helper_main(dev_path);
		_exit(0);
	}

	helper_pid = pid;
	helper_fork_t = now;
	printf("ClassicUI: optical drive at %s, helper pid %d\n", dev_path, (int)pid);
	return 1;
}

int disc_watch_start()
{
	if (watching) return helper_pid > 0;
	if (!cfg.classicui_disc) return 0;

	int now = (int)time(0);
	if (!disc_probe_due(0, last_probe_t, now)) return 0;

	static const char *const paths[] = { "/dev/sr0", "/dev/cdrom", "/dev/sr1" };

	const char *dev = 0;
	for (size_t i = 0; !dev && i < sizeof(paths) / sizeof(paths[0]); i++)
	{
		// O_NONBLOCK: opening a drive with no disc in it otherwise hangs.
		int fd = open(paths[i], O_RDONLY | O_NONBLOCK);
		if (fd >= 0) { close(fd); dev = paths[i]; }
	}

	if (!dev)
	{
		// Looked, found nothing, and will look again in DISC_PROBE_RETRY_S rather than
		// never - see disc_probe_due(). watching stays 0 so disc_poll() keeps calling
		// back here every frame, but the retry timer - not this function being skipped
		// - is what keeps that cheap and the log quiet.
		last_probe_t = now;
		if (!no_drive_logged)
		{
			printf("ClassicUI: no optical drive\n");
			no_drive_logged = 1;
		}
		return 0;
	}

	unlink(DISC_STATE_FILE);
	snprintf(dev_path, sizeof(dev_path), "%s", dev);

	/*
	  There is a drive, so there will be discs, so there will be lookups. Settle whether
	  the card has a title table now rather than on the first frame after a serial turns
	  up - see disc_titles_preload().

	  Here, and not at start-up, is the whole point: this line is only reached once a
	  device node has opened, so a machine with no optical drive never touches the file.
	*/
	disc_titles_preload();

	// The device node itself is the thing that was "found" - watching latches here
	// and stays latched even if the fork below fails or the helper dies later; see
	// fork_helper() and disc_refork_due() for how those get retried without
	// re-probing paths that are already known good.
	watching = 1;
	quick_deaths = 0;
	return fork_helper();
}

void disc_watch_stop()
{
	watching = 0;

	if (helper_pid > 0)
	{
		/*
		  Killed and handed over, rather than killed and reaped here.

		  This used to be kill() followed by waitpid(WNOHANG), which never blocks - the
		  helper may be stuck in an ioctl, which is the whole reason it is a process - and
		  for that same reason never actually reaped either: a child that has just been
		  signalled has not died yet. The pid was then dropped, so nothing could try
		  again, and this runs before every disc launch and every rip.

		  It cannot be retried from disc_poll() either. The two callers that matter both
		  stop the poll: disc_launch() hands the drive to a core and chome_handle() then
		  keeps disc_poll() from running for as long as the core holds it, and the
		  classicui_disc row turning off is the one case disc_poll() returns early for.
		  See chome_proc.h.
		*/
		chome_child_stop(helper_pid, SIGKILL, 0);
		helper_pid = -1;
	}

	unlink(DISC_STATE_FILE);
	disc_reset_reader();
	disc_forget();
}

void disc_reset_reader()
{
	reader = 0;
	reader_ctx = 0;
}

/*
  The parent's whole involvement: has that little file changed, and if so what does it
  say. No device access, so this cannot block on the drive however wedged it is.

  Also where a dead helper is noticed and, subject to disc_refork_due()'s backoff,
  replaced. Neither is a device access either: waitpid(WNOHANG) asks the kernel about
  a process this one already owns, and fork() below re-runs the probe from the path
  already recorded in dev_path rather than re-opening /dev/sr0 et al.

  And where the flag going *away* is acted on, which is new and is what lets the setting
  be offered on a screen at all.

  This used to be a bare early return, which was correct while classicui_disc could only
  change by editing MiSTer.ini and rebooting: the flag was read once, at the value it
  would keep for the life of the process. Now that Options > More Settings can turn it off
  under a running front-end, a bare return would leave the helper process alive with
  /dev/sr0 open and nobody reading what it wrote - the badge frozen on whatever was last
  identified, the drive unavailable to anything else, and the feature reporting itself as
  off. That is the "switched off but still running" shape the front-end must not have; it
  is also what would have made OW_NOW on that row a lie in one direction only, which is
  the hardest kind to notice.

  Guarded rather than called unconditionally: disc_watch_stop() unlinks the state file and
  forgets the disc, and doing that every frame forever on a machine that has the feature
  off would be a syscall per frame for nothing.
*/
void disc_poll()
{
	if (!cfg.classicui_disc)
	{
		if (disc_release_due(cfg.classicui_disc, watching, helper_pid > 0)) disc_watch_stop();
		return;
	}

	if (!watching)
	{
		disc_watch_start();
	}
	else if (helper_pid > 0)
	{
		// Non-blocking for the same reason disc_watch_stop() reaps this way: a helper
		// stuck in an uninterruptible ioctl has not exited, so this never waits on one.
		int status = 0;
		if (waitpid(helper_pid, &status, WNOHANG) == helper_pid)
		{
			int now = (int)time(0);
			int ran = now - helper_fork_t;
			quick_deaths = (ran > DISC_HELPER_QUICK_DEATH_S) ? 0 : (quick_deaths + 1);
			helper_pid = -1;

			// The helper died with the drive; whatever it last wrote to the state file no
			// longer has anyone confirming it. Forget rather than leave a stale disc (or
			// worse, a stale identification) on screen with nothing watching to correct it.
			disc_forget();
		}
	}
	else if (disc_refork_due(quick_deaths, helper_fork_t, (int)time(0)))
	{
		fork_helper();
	}

	if (helper_pid <= 0) return;

	/*
	  Rate-limited, then compared by content - NOT by mtime.

	  This used to gate on st_mtime, which is **seconds** resolution on this filesystem.
	  The helper writes SPINNING and then READY, and when the disc is already spun up the
	  identification finishes inside the same second - so the second write had the same
	  mtime as the first and the front-end never saw it. The disc sat at SPINNING for
	  ever, which left the prompt with no Play row and made the disc unlaunchable. It
	  only ever worked when the drive was slow enough to push the two writes into
	  different seconds, which is why it looked intermittent.

	  Comparing the line itself cannot miss an update. The file is twenty bytes in tmpfs,
	  so the read is trivial; the counter is only there because this is called on every
	  pass of the draw loop and there is no point doing it thousands of times a second.
	*/
	static int skip = 0;
	if (++skip < 32) return;
	skip = 0;

	FILE *f = fopen(DISC_STATE_FILE, "r");
	if (!f) return;

	char line[160] = {};
	if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
	fclose(f);

	static char last_line[160] = {};
	if (!strcmp(line, last_line)) return;
	snprintf(last_line, sizeof(last_line), "%s", line);

	char *nl = strchr(line, '\n');
	if (nl) *nl = 0;

	char *f1 = strchr(line, '|');
	if (!f1) return;
	*f1++ = 0;
	char *f2 = strchr(f1, '|');
	if (!f2) return;
	*f2++ = 0;
	char *f3 = strchr(f2, '|');
	if (f3) *f3++ = 0;

	int st = atoi(line);
	int t = atoi(f1);

	if (st < DISC_ABSENT || st > DISC_UNKNOWN) return;
	if (t < DISC_T_NONE || t > DISC_T_UNKNOWN) t = DISC_T_UNKNOWN;

	dstate = st;
	dtype = t;
	snprintf(dserial, sizeof(dserial), "%s", f2);
	snprintf(dlabel, sizeof(dlabel), "%s", f3 ? f3 : "");
	ddirty = 1;

	printf("ClassicUI: disc state=%d %s", dstate, disc_type_name(dtype));
	if (dlabel[0]) printf(" \"%s\"", dlabel);
	if (dserial[0]) printf(" %s", dserial);
	printf("\n");
}

#endif
