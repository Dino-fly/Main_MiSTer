/*
  Grabs the front-end's framebuffer off the device, so a test can look at what it drew.

  Usage: fbgrab <out.raw> [buffer]     buffer 0 or 1; default: both, as out.raw.0/.1

  Why this exists rather than `dd if=/dev/mem`: a plain read() of /dev/mem fails with
  EFAULT here, so the frame has to be mmap'd. Geometry comes from the module's own
  parameters rather than being assumed - the canvas follows the video mode, and on this
  machine analog output makes it 320x240 rather than the 1280x720 a capture at a desk
  would suggest.

  Output is raw BGRA at width*height*4, no header; convert on the host.

  The trap this does NOT solve: the front-end double-buffers, and `mode` does not
  reliably name the buffer that was just drawn. Grab both and use the one that differs
  from the previous grab - a capture identical to the last one is the first thing to
  distrust, not evidence that a keypress was ignored.

  Build: arm-none-linux-gnueabihf-gcc -O1 -static -o fbgrab fbgrab.c
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define FB_BASE   0x22000000UL
#define FB_STRIDE (1920UL * 1080UL * 4UL)   // spacing between buffers, not this mode's pitch

static int param(const char *name, int def)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/module/MiSTer_fb/parameters/%s", name);
	FILE *f = fopen(path, "r");
	if (!f) return def;
	int v = def;
	if (fscanf(f, "%d", &v) != 1) v = def;
	fclose(f);
	return v;
}

static int grab(int fd, int idx, int w, int h, int stride, const char *out)
{
	unsigned long off = FB_BASE + FB_STRIDE * (unsigned long)idx;
	size_t len = (size_t)stride * (size_t)h;

	// mmap wants a page-aligned offset; keep the remainder and skip it in the copy.
	unsigned long page = (unsigned long)sysconf(_SC_PAGESIZE);
	unsigned long base = off & ~(page - 1);
	size_t slack = (size_t)(off - base);

	void *m = mmap(0, len + slack, PROT_READ, MAP_SHARED, fd, (off_t)base);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }

	FILE *o = fopen(out, "wb");
	if (!o) { perror(out); munmap(m, len + slack); return 1; }

	const unsigned char *p = (const unsigned char *)m + slack;
	for (int y = 0; y < h; y++) fwrite(p + (size_t)y * stride, 4, (size_t)w, o);

	fclose(o);
	munmap(m, len + slack);
	printf("%s: buffer %d, %dx%d\n", out, idx, w, h);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: fbgrab <out.raw> [buffer]\n"); return 2; }

	int w = param("width", 320), h = param("height", 240), stride = param("stride", w * 4);

	int fd = open("/dev/mem", O_RDONLY);
	if (fd < 0) { perror("/dev/mem"); return 1; }

	int rc = 0;
	if (argc >= 3) rc = grab(fd, atoi(argv[2]), w, h, stride, argv[1]);
	else
	{
		char p[512];
		snprintf(p, sizeof(p), "%s.0", argv[1]); rc |= grab(fd, 0, w, h, stride, p);
		snprintf(p, sizeof(p), "%s.1", argv[1]); rc |= grab(fd, 1, w, h, stride, p);
	}

	close(fd);
	return rc;
}
