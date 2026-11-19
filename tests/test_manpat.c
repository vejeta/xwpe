/* test_manpat.c -- unit test for the pattern Function Index looks for in
 * every folder of MANPATH (we_manpat.h).  Links we_path.c only; always runs.
 *
 * The code this replaced sized its buffer from the MANPATH folder alone and
 * wrote the section into it, so a section longer than the ten bytes of
 * slack ran past the heap block.  Each row builds the pattern in a buffer of
 * exactly the size it is given, with a guard behind it, and builds one that
 * fits and one that is a byte too long: the first is whole, the second is
 * refused with an empty buffer and the guard untouched.  Run it under
 * AddressSanitizer to see a write past the block.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../we_manpat.h"

#define GUARD 8
#define GUARD_BYTE 'Z'

static int fails;

static void chk_int(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

static void chk_str(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
		fails++;
	}
}

/* The pattern of @folder and @section in a heap block of @size bytes with a
   guard behind it; the guard must stay as it was. */
static void build(const char *what, const char *folder, const char *section,
		  size_t size, int want, const char *want_text)
{
	char *mem = malloc(size + GUARD);
	size_t i;
	int guard_ok = 1;

	if (!mem)
		exit(2);
	memset(mem, GUARD_BYTE, size + GUARD);
	chk_int(what, wpe_man_pattern(mem, size, folder, section), want);
	chk_str(what, mem, want_text);
	for (i = size; i < size + GUARD; i++)
		if (mem[i] != GUARD_BYTE)
			guard_ok = 0;
	chk_int("guard behind the buffer untouched", guard_ok, 1);
	free(mem);
}

static void usual_patterns(void)
{
	build("a folder and a section", "/usr/share/man", "1", 64, 0,
	      "/usr/share/man/man1/*");
	build("a section of two letters", "/usr/man", "3p", 64, 0,
	      "/usr/man/man3p/*");
	build("the root folder", "", "8", 64, 0, "/man8/*");
}

static void boundary_of_the_buffer(void)
{
	const char *folder = "/usr/share/man";
	const char *want = "/usr/share/man/man1/*";
	size_t whole = strlen(want) + 1;

	build("a buffer of exactly the pattern", folder, "1", whole, 0, want);
	build("a buffer a byte short", folder, "1", whole - 1, -1, "");
}

/* The row the old code failed: a one byte MANPATH folder and a section far
   past the ten bytes of slack the heap block had. */
static void long_section(void)
{
	char section[71];
	char want[128];

	memset(section, 'm', sizeof section - 1);
	section[sizeof section - 1] = '\0';
	snprintf(want, sizeof want, "x/man%s/*", section);
	build("a 70 byte section in a block sized for the folder", "x", section,
	      strlen("x") + 10, -1, "");
	build("the same section in room for it", "x", section, sizeof want, 0,
	      want);
}

static void missing_parts(void)
{
	build("no section", "/usr/man", NULL, 32, 0, "/usr/man/man/*");
	build("no folder", NULL, "1", 32, 0, "/man1/*");
}

int main(void)
{
	usual_patterns();
	boundary_of_the_buffer();
	long_section();
	missing_parts();
	if (fails) {
		printf("test_manpat: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_manpat: ok\n");
	return 0;
}
