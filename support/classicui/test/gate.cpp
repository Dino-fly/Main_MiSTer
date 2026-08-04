/*
  One property, checked in the only configuration that can check it: that a build
  with no devid cannot make a ScreenScraper request.

  Why this is a separate binary. The main harness compiles chome_ss.cpp *with* a
  dummy CLASSICUI_SS_DEVID, because otherwise ss_build_url() refuses and the URL
  builder is unreachable dead code that no test could cover. But that means the
  main harness can say nothing at all about the configuration we actually ship,
  which is the one where the macro is undefined. Same source file, compiled the
  shipped way, linked against nothing but a cfg definition.

  If this ever prints a URL, a public build is one ini setting away from talking to
  an API it has no credential for, under a devid that is not ours.
*/

#include <stdio.h>
#include <string.h>

#include "../chome_ss.h"
#include "../../../cfg.h"

cfg_t cfg;

static int fails = 0;

static void check(int cond, const char *what)
{
	if (cond) printf("  ok    %s\n", what);
	else { printf("  FAIL  %s\n", what); fails++; }
}

int main()
{
	printf("== the shipped configuration: no devid compiled in ==\n");

	memset(&cfg, 0, sizeof(cfg));

	check(ss_available() == 0, "ss_available() is false with no devid");

	// Everything a user could turn on, turned on.
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, "derek");
	strcpy(cfg.classicui_ss_pass, "hunter2");

	check(ss_enabled() == 0, "and no amount of configuration enables it");

	ss_query q;
	memset(&q, 0, sizeof(q));
	q.systemeid = "57";
	q.romnom = "Destruction Derby (USA).cue";
	q.romtaille = 1234567;
	q.md5 = "d41d8cd98f00b204e9800998ecf8427e";

	char url[1024];
	memset(url, 'x', sizeof(url));

	check(ss_build_url(&q, 0, url, sizeof(url)) == 0, "a fully valid query still builds no URL");
	check(url[0] == 0, "and the buffer is left empty rather than half-written");

	// The parser and the picker are pure and stay usable - only the network side is
	// gated - so a reply captured by hand can still be examined in this build.
	check(ss_system_id("psx", "x.cue") != 0, "the system table still answers");

	printf("%s\n", fails ? "GATE FAILED" : "gate holds");
	return fails ? 1 : 0;
}
