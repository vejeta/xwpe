/* test_debpath.c -- unit test for the file names a debugger is started on.
 * Links we_debpath.c only; no editor, no curses -- always runs (no skip).
 *
 * A debugger started on a cut file name ends at once and the user is told
 * nothing useful, so a path that does not fit the start buffer is refused
 * whole and the editor says the path is too long.  No file system lets a
 * path that long be opened in the editor, so the refusal is checked here, at
 * the exact edge of the buffer, together with the bytes past it, which must
 * stay untouched.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../we_debpath.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Bytes after the destination that must never be written. */
#define GUARD     16
#define GUARD_BYTE 'G'

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* A destination of n bytes followed by a guard that must stay intact. */
static char area[PATH_MAX * 2 + GUARD];

static char *dest(size_t n)
{
	memset(area, 'x', sizeof area);
	memset(area + n, GUARD_BYTE, GUARD);
	return area;
}

static int guard_intact(size_t n)
{
	size_t k;

	for (k = 0; k < GUARD; k++)
		if (area[n + k] != GUARD_BYTE)
			return 0;
	return 1;
}

/* A path of exactly len bytes: '/' every 100 bytes, so no component comes
   near NAME_MAX, as on a real file system. */
static const char *path_of(size_t len)
{
	static char p[PATH_MAX * 2];
	size_t k;

	for (k = 0; k < len; k++)
		p[k] = (k % 100 == 0) ? '/' : 'd';
	p[len] = '\0';
	return p;
}

static void test_copy_that_fits_is_whole(void)
{
	const char *src = path_of(PATH_MAX - 1);
	char *out = dest(PATH_MAX);

	chk("a path of PATH_MAX-1 bytes fits", e_d_copy_arg(out, PATH_MAX, src), 0);
	chk("it is copied whole", strcmp(out, src), 0);
	chk("nothing past the buffer", guard_intact(PATH_MAX), 1);
}

static void test_copy_one_byte_too_long_is_refused(void)
{
	const char *src = path_of(PATH_MAX);
	char *out = dest(PATH_MAX);

	chk("a path of PATH_MAX bytes is refused",
	    e_d_copy_arg(out, PATH_MAX, src), -1);
	chk("the refused copy leaves an empty name", out[0], '\0');
	chk("nothing past the buffer", guard_intact(PATH_MAX), 1);
}

static void test_copy_into_a_small_buffer(void)
{
	char *out = dest(4);

	chk("three bytes fit in four", e_d_copy_arg(out, 4, "a.c"), 0);
	chk("copied", strcmp(out, "a.c"), 0);
	out = dest(4);
	chk("four bytes do not", e_d_copy_arg(out, 4, "ab.c"), -1);
	chk("refused leaves empty", out[0], '\0');
	chk("nothing past the small buffer", guard_intact(4), 1);
}

static void test_full_path_that_fits_is_whole(void)
{
	/* dir + '/' + name is exactly PATH_MAX - 1 bytes */
	const char *name = "p.py";
	size_t dlen = PATH_MAX - 1 - 1 - strlen(name);
	char dir[PATH_MAX];
	char want[PATH_MAX];
	char *out;

	memcpy(dir, path_of(dlen), dlen + 1);
	snprintf(want, sizeof want, "%s/%s", dir, name);
	out = dest(PATH_MAX);
	chk("dir/name of PATH_MAX-1 bytes fits",
	    e_d_debuggee_path(out, PATH_MAX, dir, name), 0);
	chk("it is built whole", strcmp(out, want), 0);
	chk("nothing past the buffer", guard_intact(PATH_MAX), 1);
}

static void test_full_path_one_byte_too_long_is_refused(void)
{
	const char *name = "p.py";
	size_t dlen = PATH_MAX - 1 - strlen(name);   /* one byte more */
	char dir[PATH_MAX];
	char *out;

	memcpy(dir, path_of(dlen), dlen + 1);
	out = dest(PATH_MAX);
	chk("dir/name of PATH_MAX bytes is refused",
	    e_d_debuggee_path(out, PATH_MAX, dir, name), -1);
	chk("the refused path is empty, not a cut one", out[0], '\0');
	chk("nothing past the buffer", guard_intact(PATH_MAX), 1);
}

static void test_a_directory_that_fits_alone_is_not_enough(void)
{
	/* The directory fits; only the name added to it does not. */
	char *out = dest(8);

	chk("dir fits, dir/name does not",
	    e_d_debuggee_path(out, 8, "/a/b", "long.py"), -1);
	chk("refused leaves empty", out[0], '\0');
	chk("nothing past the buffer", guard_intact(8), 1);
}

int main(void)
{
	test_copy_that_fits_is_whole();
	test_copy_one_byte_too_long_is_refused();
	test_copy_into_a_small_buffer();
	test_full_path_that_fits_is_whole();
	test_full_path_one_byte_too_long_is_refused();
	test_a_directory_that_fits_alone_is_not_enough();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("test_debpath: all checks passed\n");
	return 0;
}
