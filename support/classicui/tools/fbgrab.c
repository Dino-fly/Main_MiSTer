/*
  Grabs the front-end's framebuffer off the device, so a test can look at what it drew.

  Usage: fbgrab <out.raw> [buffer [w h stride]]
                                       buffer 0, 1 or 2; default: 0 and 1, as
                                       out.raw.0/.1. Geometry defaults to the module's
                                       parameters and can be overridden - see below.

  Why this exists rather than `dd if=/dev/mem`: a plain read() of /dev/mem fails with
  EFAULT here, so the frame has to be mmap'd. Geometry comes from the module's own
  parameters rather than being assumed - the canvas follows the video mode, and on this
  machine analog output makes it 320x240 rather than the 1280x720 a capture at a desk
  would suggest.

  WHY THE GEOMETRY CAN BE OVERRIDDEN, AND WHEN YOU MUST

  The module parameters describe the video *mode*, and the front-end does not always
  draw a frame that size. With the analog takeover on it draws buffer 1 at 320x240 and
  the parameters agree. With the takeover off it draws buffer 2 at half the mode -
  640x360 inside a 1280x720 mode - and the parameters still say 1280x720. Reading
  640x360 worth of pixels as 1280x720 does not fail: it produces a plausible-looking
  smear of the frame repeated four times across, which reads as a rendering bug in the
  front-end rather than as a capture that asked for the wrong rectangle. It cost an hour
  once. The firmware's own log line is the authority:

    video: mode now 1280x720, fb 2 at 640x360, takeover=0
                               ^ buffer     ^ what to pass here

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
	if (argc < 2) { fprintf(stderr, "usage: fbgrab <out.raw> [buffer [w h stride]]\n"); return 2; }

	int w = param("width", 320), h = param("height", 240), stride = param("stride", w * 4);

	// An explicit rectangle wins over the module's, for the reason in the header. The
	// stride is separate from the width because a half-size frame keeps the mode's
	// stride: 640 pixels of content on a 5120-byte row, the rest of it untouched.
	if (argc >= 6)
	{
		w = atoi(argv[3]);
		h = atoi(argv[4]);
		stride = atoi(argv[5]);
		if (w <= 0 || h <= 0 || stride < w * 4)
		{
			fprintf(stderr, "fbgrab: %dx%d stride %d is not a frame\n", w, h, stride);
			return 2;
		}
	}

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
