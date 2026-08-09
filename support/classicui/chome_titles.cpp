#include <stdio.h>
#include <string.h>

#include "chome_titles.h"
#include "../../file_io.h"          // getRootDir()

/*
  See chome_titles.h for what the file is and why it is a file at all. This is the
  search over it, and the two decisions worth defending are the format and the fact
  that the file is never held open.

  ------------------------------------------------- why sorted text, not binary ---

  The alternative considered - and the one that looks better on paper - is a packed
  index: a header, a sorted array of fixed-width records (key plus a uint32 offset),
  and a string blob, exactly the shape chome_lib.cpp's index.bin already uses. It
  addresses record N arithmetically, so a lookup is a clean log2(n) seeks with no
  scanning for line boundaries, and it is about 25% smaller.

  Sorted text wins anyway, on four counts that all point the same way:

    - The keys are not fixed width. A PlayStation serial normalises to 9 or 10
      characters, a Sega product code to 10-12, and a PC Engine or Neo Geo disc has
      no serial at all - its key is the volume label, up to 40. Padding every record
      to 40 to keep the arithmetic costs more than the newlines ever could, and a
      variable-width binary record needs an offset table, at which point the format
      is complicated and no longer smaller.

    - A player can fix it. The one thing that will definitely happen is a disc whose
      title comes out wrong or missing, and with a text file the answer is "add a
      line". With a binary index the answer is "re-run a Python script over a DAT you
      no longer have", and nobody does that. Cheats and classicui_systems.txt are
      both hand-editable for the same reason.

    - It diffs, greps and truncates visibly. A truncated text file is a file with a
      short last line; a truncated binary index is a file whose offsets point past
      the end, which is a class of bug that has to be defended against rather than
      seen.

    - The measured cost of the difference is nothing. Both formats are ~10 seeks
      over ~650 KB, once per disc insertion. Choosing the faster of two operations
      that each happen once when somebody puts a disc in a drive is not engineering.

  Size, for the record: Redump lists on the order of 12,000 PlayStation discs, plus
  roughly 600 Mega CD, 900 PC Engine CD and 250 Neo Geo CD. At a 10-character key, a
  tab, a ~34-character title and a newline that is about 46 bytes a row, so ~640 KB
  for all four systems. One erase block on the card, and small enough that the
  "should this be per-system?" question does not need answering yet.

  ---------------------------------------------------- why nothing stays open ---

  A core switch re-execs the binary, so anything held across one is lost anyway;
  and a lookup happens when a disc arrives, which is a human-scale event. Holding a
  descriptor on the card for the life of the process to save an open() that happens
  twice per disc would be a real cost - the card is also being read by whatever core
  is running - for no gain.

  What *is* kept is the answers, and the verdict on the file. See the cache below.
*/

#define TDB_FILE  "classicui/disctitles.txt"

// Exact, including the version: a file that says 2 is a file this build does not
// understand, and refusing it is better than guessing at a layout that changed.
#define TDB_MAGIC "#classicui-disctitles 1"

/*
  Long enough for any line the generator writes - a 40-character key, a tab, a
  63-character title and a CRLF - with room for a hand-typed line that is longer.
  A line that overruns it is read as two lines, neither of which parses, so it
  becomes a row that never matches rather than a buffer that overflows.
*/
#define TDB_LINE_MAX 256

// The normalised key. DISC_LABEL_LEN (40) is the longest thing a caller can hand
// us and normalising only ever removes characters, so this cannot truncate in
// practice; it is sized above the ceiling rather than at it.
#define TDB_KEY_MAX 48

/*
  Where the binary search stops halving and starts reading. One 4 KB window is
  ~85 rows, and scanning them linearly costs a single buffered read - fewer bytes
  off the card than the three or four extra seeks that narrowing further would take,
  and much easier to be sure is correct.
*/
#define TDB_WINDOW 4096

/*
  A hard ceiling on probes, and it is not belt-and-braces.

  The search narrows by locating the next line boundary at or after the midpoint,
  which converges in log2(size / TDB_WINDOW) steps - twelve for a 16 MB file - as
  long as the file has line boundaries. A file that claims our magic and then
  contains megabytes with no newline in it converges one line-buffer at a time
  instead, which is thousands of seeks: not a crash, not an infinite loop, but a
  visibly frozen front-end, which is the same thing to the player. So the search is
  allowed a budget it cannot exceed on well-formed input, and giving up is a miss.
*/
#define TDB_PROBE_MAX 64

// Explicit rather than left to BUFSIZ, so the worst-case footprint of this module
// is a number that can be stated: this buffer, plus the cache below, plus one
// TDB_LINE_MAX scratch line on the stack.
#define TDB_IO_BUF 4096

/*
  How many answers to remember.

  disc_display_name() is called on every frame that draws the disc, and it asks
  about up to two keys - the serial and the volume label - because a PC Engine CD
  has only the second. Misses are cached alongside hits, which is the point: a disc
  that is not in the table is the *common* case, and caching only the hits would
  leave exactly that disc re-reading the card sixty times a second.

  Four rather than two so that a caller which also asks about something else - the
  disc prompt, redesigned, may well want the title beside the serial - does not
  start evicting the entries the draw loop depends on.
*/
#define TDB_CACHE 4

#define TDB_VERDICT_UNKNOWN 0      // not looked for yet
#define TDB_VERDICT_GOOD    1      // opened, and the magic was ours
#define TDB_VERDICT_NONE    2      // absent, unreadable, or not ours. Do not look again.

static int  tdb_verdict = TDB_VERDICT_UNKNOWN;
static char tdb_io[TDB_IO_BUF];

struct tdb_entry
{
	char key[TDB_KEY_MAX];         // normalised; empty means the slot is unused
	char title[DISC_TITLE_LEN];    // empty means "asked, and there is no title"
};

static tdb_entry tdb_cache[TDB_CACHE];
static int tdb_next = 0;           // round-robin victim

void disc_titles_forget()
{
	tdb_verdict = TDB_VERDICT_UNKNOWN;
	memset(tdb_cache, 0, sizeof(tdb_cache));
	tdb_next = 0;
}

/* ------------------------------------------------------------ normalising --- */

/*
  The canonical form of a key: upper case, A-Z and 0-9 only.

  tools/disctitles.py does exactly this to every serial it writes, and the file is
  sorted by strcmp over the result. All three - this function, that script's
  normalise(), and the sort order - are one agreement, and breaking any one of them
  silently turns the table into a table of misses. Which is why the file carries a
  version in its magic line: a change here is a change to the format.
*/
static int tdb_normalise(const char *in, char *out, int outsz)
{
	int n = 0;

	if (!in || !out || outsz <= 0) return 0;

	for (; *in && n < outsz - 1; in++)
	{
		unsigned char c = (unsigned char)*in;
		if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = (char)c;
	}

	out[n] = 0;
	return n;
}

/* ------------------------------------------------------------- the search --- */

/*
  Split a line in place. Returns the title, or 0 when this is not one of our rows -
  the magic line, a blank line, a comment, a line the generator wrote longer than
  TDB_LINE_MAX, or the tail of one.

  The key must start with an alphanumeric, which is what keeps a stray '#' line
  carrying a tab from being taken for a row and compared against.
*/
static char *tdb_split(char *line, char **key)
{
	char *p = line + strlen(line);
	while (p > line && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;   // the file may be CRLF

	char *tab = strchr(line, '\t');
	if (!tab || tab == line) return 0;
	*tab = 0;

	unsigned char c = (unsigned char)line[0];
	if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;

	if (!tab[1]) return 0;

	*key = line;
	return tab + 1;
}

/*
  Read the line starting at `off`. Returns how many bytes it occupied, so the caller
  can step to the next one, or 0 at end of file.

  A line longer than the buffer comes back cut short, and the count reflects what was
  consumed rather than the whole line - so the remainder is presented as another
  line. Both halves fail tdb_split(), which is the intended outcome: an over-long row
  is unmatchable, not dangerous.
*/
static long tdb_read_line(FILE *f, long off, char *buf, int bufsz)
{
	if (fseek(f, off, SEEK_SET)) return 0;
	if (!fgets(buf, bufsz, f)) return 0;

	long end = ftell(f);
	if (end <= off) return 0;

	return end - off;
}

/*
  The offset of the first line that *starts* at or after `off`, or -1 when there is
  none. At the top of the file that is the file itself; anywhere else it means
  discarding whatever partial line `off` landed inside.

  Note what this deliberately does not do: it does not try to find the start of the
  line containing `off` by reading backwards. Every caller below is written so that
  skipping over that line is safe - see the search's own comment.
*/
static long tdb_next_line(FILE *f, long off, char *scratch, int scratchsz)
{
	if (off <= 0) return 0;

	long n = tdb_read_line(f, off, scratch, scratchsz);
	if (!n) return -1;

	return off + n;
}

/*
  Binary search, and the invariant is the thing to read twice.

  [lo, hi) is the byte range that may still contain the answer, and lo is always
  either 0 or the exact start of a row. Each step takes the midpoint, walks forward
  to the next row boundary at `p`, and compares that row's key:

    row < key   every row starting before p+len is also < key, so lo = p + len
    row > key   the answer is before p, so hi = p
    no row      nothing starts in [mid, hi), so hi = mid

  Moving `hi` to `p` rather than to `mid` is what makes skipping the partial line at
  `mid` safe: that line starts before p, so it stays inside the range either way.
  Moving hi to mid would drop it, and it would drop it in precisely the case where
  mid happened to fall on a row boundary - a bug that shows up on some keys and not
  others, which is the worst kind to go looking for later.

  It converges because p is at most one line past mid, so the range shrinks by
  roughly half each step until it is smaller than TDB_WINDOW; the last stretch is
  read and scanned. TDB_PROBE_MAX is what happens when the file has no lines to find
  boundaries in.
*/
static const char *tdb_search(FILE *f, long size, const char *key, char *out, int outsz)
{
	char line[TDB_LINE_MAX];
	char scratch[TDB_LINE_MAX];

	long lo = 0, hi = size;
	int probes = 0;

	while (hi - lo > TDB_WINDOW)
	{
		if (++probes > TDB_PROBE_MAX) return 0;

		long mid = lo + (hi - lo) / 2;
		long p = tdb_next_line(f, mid, scratch, sizeof(scratch));
		if (p < 0 || p >= hi) { hi = mid; continue; }

		long len = tdb_read_line(f, p, line, sizeof(line));
		if (!len) { hi = mid; continue; }

		char *k = 0;
		char *title = tdb_split(line, &k);

		// An unreadable row leaves no way to tell which half to keep, so take the
		// half that cannot cost us correctness on a *well-formed* file: the answer,
		// if there is one, is at worst missed rather than wrong.
		int c = title ? strcmp(k, key) : 1;

		if (!c)
		{
			snprintf(out, (size_t)outsz, "%s", title);
			return out;
		}

		if (c < 0)
		{
			lo = p + len;
			if (lo > hi) lo = hi;   // that row ran past hi, so no row starts below it
		}
		else hi = p;
	}

	// The window. Rows are sorted, so a key that sorts past ours ends it.
	for (long off = lo; off < hi; )
	{
		long len = tdb_read_line(f, off, line, sizeof(line));
		if (!len) break;
		off += len;

		char *k = 0;
		char *title = tdb_split(line, &k);
		if (!title) continue;       // the magic line, a comment, a torn row: skip it

		int c = strcmp(k, key);
		if (c > 0) break;
		if (!c)
		{
			snprintf(out, (size_t)outsz, "%s", title);
			return out;
		}
	}

	return 0;
}

/* --------------------------------------------------------------- the file --- */

/*
  Open the table, or decide once and for all that there is not one.

  The two failures are reported differently on purpose. A missing file is the state
  of every card that has not had one put on it, so it says nothing at all - a log
  line per boot for a feature nobody opted into is noise, and noise in a log is how
  real messages get missed. A file that is *there* and is not ours is somebody's
  mistake and worth one line, once.
*/
static FILE *tdb_open(long *size)
{
	if (tdb_verdict == TDB_VERDICT_NONE) return 0;

	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", getRootDir(), TDB_FILE);

	FILE *f = fopen(path, "rb");
	if (!f)
	{
		tdb_verdict = TDB_VERDICT_NONE;
		return 0;
	}

	// One static buffer, and nothing here nests or keeps a handle, so it cannot be
	// in use by a second stream.
	setvbuf(f, tdb_io, _IOFBF, sizeof(tdb_io));

	char line[TDB_LINE_MAX];
	if (!fgets(line, sizeof(line), f))
	{
		// Zero bytes. Empty is a legitimate way to say "I have no titles".
		fclose(f);
		tdb_verdict = TDB_VERDICT_NONE;
		return 0;
	}

	char *p = line + strlen(line);
	while (p > line && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;

	if (strcmp(line, TDB_MAGIC))
	{
		fclose(f);
		tdb_verdict = TDB_VERDICT_NONE;
		printf("ClassicUI: %s is not a disc title table (expected \"%s\")\n", path, TDB_MAGIC);
		return 0;
	}

	if (fseek(f, 0, SEEK_END)) { fclose(f); tdb_verdict = TDB_VERDICT_NONE; return 0; }

	long n = ftell(f);
	if (n <= 0) { fclose(f); tdb_verdict = TDB_VERDICT_NONE; return 0; }

	if (tdb_verdict != TDB_VERDICT_GOOD) printf("ClassicUI: disc titles from %s (%ld bytes)\n", path, n);
	tdb_verdict = TDB_VERDICT_GOOD;

	*size = n;
	return f;
}

/*
  Ask the question early, so the answer is not being discovered on the frame it is
  wanted. See chome_titles.h for why this is worth an open() and what it deliberately
  does not do - which is cache anything, because there is no key yet to cache against.
*/
void disc_titles_preload()
{
	long size = 0;
	FILE *f = tdb_open(&size);
	if (f) fclose(f);
}

/* ------------------------------------------------------------- the lookup --- */

const char *disc_title_for(const char *key)
{
	char norm[TDB_KEY_MAX];
	if (!tdb_normalise(key, norm, sizeof(norm))) return 0;

	for (int i = 0; i < TDB_CACHE; i++)
	{
		if (strcmp(tdb_cache[i].key, norm)) continue;
		return tdb_cache[i].title[0] ? tdb_cache[i].title : 0;
	}

	// Remembered before the answer is known, so that the miss is remembered too.
	tdb_entry *e = &tdb_cache[tdb_next];
	tdb_next = (tdb_next + 1) % TDB_CACHE;
	snprintf(e->key, sizeof(e->key), "%s", norm);
	e->title[0] = 0;

	long size = 0;
	FILE *f = tdb_open(&size);
	if (!f) return 0;

	const char *t = tdb_search(f, size, norm, e->title, sizeof(e->title));
	fclose(f);

	return t;
}
