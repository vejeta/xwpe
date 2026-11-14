/* test_path.c -- unit test for the owner of how long a path the editor holds
 * may be (we_path.c).  Links we_path.c only; always runs.
 *
 * A path is whole or it is nothing: every writer returns -1 and leaves the
 * buffer empty when the result would not fit, at the byte where it stops
 * fitting.  A folder and a name that each fit can be too long joined.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* NetBSD's NAME_MAX is 511.  Say so before the owner's header is read, so
   its guard is tried against the longest name any system builds on, whatever
   system this runs on. */
#undef NAME_MAX
#define NAME_MAX 511

#include "../we_path.h"

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
	if (!got || strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what,
		       got ? got : "(null)", want);
		fails++;
	}
}

/* @len copies of @c, as a heap string. */
static char *run(char c, size_t len)
{
	char *s = malloc(len + 1);

	if (!s)
		exit(2);
	memset(s, c, len);
	s[len] = '\0';
	return s;
}

static void test_limits(void)
{
#ifdef PATH_MAX
	chk_int("WPE_PATH_MAX >= PATH_MAX", WPE_PATH_MAX >= PATH_MAX, 1);
#endif
#ifdef NAME_MAX
	chk_int("WPE_NAME_MAX >= NAME_MAX", WPE_NAME_MAX >= NAME_MAX, 1);
#endif
	chk_int("a path field leaves room for the terminator",
		WPE_PATH_FIELD_MAX, WPE_PATH_SIZE - 1);
	chk_int("a name field leaves room for the terminator",
		WPE_NAME_FIELD_MAX, WPE_NAME_SIZE - 1);
}

static void test_copy(void)
{
	char out[WPE_PATH_SIZE];
	char *fits = run('a', WPE_PATH_SIZE - 1);
	char *over = run('a', WPE_PATH_SIZE);

	chk_int("copy of SIZE-1 bytes fits", wpe_path_copy(out, sizeof out, fits), 0);
	chk_int("copy keeps every byte", (long)strlen(out), WPE_PATH_SIZE - 1);
	memset(out, 'x', sizeof out);
	chk_int("copy of SIZE bytes is refused", wpe_path_copy(out, sizeof out, over), -1);
	chk_int("a refused copy leaves the buffer empty", out[0], 0);
	chk_int("NULL source is refused", wpe_path_copy(out, sizeof out, NULL), -1);
	chk_int("zero-size buffer is refused", wpe_path_copy(out, 0, "a"), -1);
	chk_int("empty source fits", wpe_path_copy(out, sizeof out, ""), 0);
	chk_str("empty source is empty", out, "");
	free(fits);
	free(over);
}

static void test_join(void)
{
	char out[WPE_PATH_SIZE];
	char small[8];

	chk_int("join /a b", wpe_path_join(out, sizeof out, "/a", "b"), 0);
	chk_str("one separator added", out, "/a/b");
	chk_int("join /a/ b", wpe_path_join(out, sizeof out, "/a/", "b"), 0);
	chk_str("no second separator", out, "/a/b");
	chk_int("join empty folder", wpe_path_join(out, sizeof out, "", "b"), 0);
	chk_str("empty folder gives the name", out, "b");
	chk_int("join NULL folder", wpe_path_join(out, sizeof out, NULL, "b"), 0);
	chk_str("NULL folder gives the name", out, "b");
	chk_int("NULL name is refused", wpe_path_join(out, sizeof out, "/a", NULL), -1);

	/* "/a/bcd" is 6 bytes: 7 with the terminator. */
	chk_int("join fits exactly", wpe_path_join(small, 7, "/a", "bcd"), 0);
	chk_str("exact fit is whole", small, "/a/bcd");
	chk_int("join one byte short is refused", wpe_path_join(small, 6, "/a", "bcd"), -1);
	chk_int("a refused join leaves the buffer empty", small[0], 0);
}

/* Each piece fits; together they do not.  An editor in a 4000 byte folder
   can open a 200 byte name there (the system takes it relative), but the
   joined path is longer than it allows. */
static void test_join_of_two_that_fit(void)
{
	char out[WPE_PATH_SIZE];
	char *dir = run('d', 4000);
	char *name = run('n', 200);

	chk_int("a 4000 byte folder fits", 4000 < WPE_PATH_SIZE, 1);
	chk_int("a 200 byte name fits", 200 < WPE_NAME_SIZE, 1);
	memset(out, 'x', sizeof out);
	chk_int("folder 4000 + name 200 is refused", wpe_path_join(out, sizeof out, dir, name), -1);
	chk_int("and leaves the buffer empty", out[0], 0);
	free(dir);
	free(name);
}

static void test_cat(void)
{
	char out[8];

	chk_int("copy ab", wpe_path_copy(out, sizeof out, "ab"), 0);
	chk_int("cat .o", wpe_path_cat(out, sizeof out, ".o"), 0);
	chk_str("suffix added", out, "ab.o");
	chk_int("cat to the exact fit", wpe_path_cat(out, sizeof out, "xyz"), 0);
	chk_str("exact fit is whole", out, "ab.oxyz");
	chk_int("cat one byte over is refused", wpe_path_cat(out, sizeof out, "q"), -1);
	chk_int("a refused cat leaves the buffer empty", out[0], 0);
	chk_int("cat of NULL is refused", wpe_path_cat(out, sizeof out, NULL), -1);
}

static void test_dup(void)
{
	char *long_path = run('p', WPE_PATH_SIZE + 500);
	char *d = wpe_path_dup(long_path);
	char *j;

	chk_int("dup keeps a path of any length", d != NULL && strcmp(d, long_path) == 0, 1);
	free(d);
	free(long_path);
	chk_int("dup of NULL is NULL", wpe_path_dup(NULL) == NULL, 1);

	j = wpe_path_join_dup("/a", "b");
	chk_str("join_dup /a b", j, "/a/b");
	free(j);
	j = wpe_path_join_dup("/a/", "b");
	chk_str("join_dup /a/ b", j, "/a/b");
	free(j);
	j = wpe_path_join_dup("", "b");
	chk_str("join_dup empty folder", j, "b");
	free(j);
	j = wpe_path_join_dup(NULL, "b");
	chk_str("join_dup NULL folder", j, "b");
	free(j);
	chk_int("join_dup NULL name is NULL", wpe_path_join_dup("/a", NULL) == NULL, 1);
}

/* @scenario SC-path-debug-09 */
/* A program is looked up on a PATH whose entries the editor does not control.
   An entry too long for the system, or for the answer's buffer, is skipped:
   it cannot overrun the lookup and it cannot name another folder's file. */
#define SEARCH_TOOL      "wpetool"
#define SEARCH_LONG      300      /* over the 255 bytes the lookup once held */
#define SEARCH_HUGE      5000     /* over any path the system holds */
#define SEARCH_SMALL_OUT 5        /* an answer buffer no path of the test fits */
#define SEARCH_CUT_TAIL  "zz"     /* what follows the slashes of a long entry */
#define SEARCH_PLAIN     "wpeplain" /* a file of the folder that is not a program */
static void test_search(void)
{
	char dir[] = "/var/tmp/test_path.XXXXXX";
	char plain[sizeof dir + sizeof SEARCH_PLAIN + 2];
	char tool[sizeof dir + sizeof SEARCH_TOOL + 2];
	char want[sizeof tool], out[WPE_PATH_SIZE], small[SEARCH_SMALL_OUT];
	char *list, *far;
	size_t n;
	FILE *fp;

	if (!mkdtemp(dir)) {
		printf("FAIL: no scratch folder for the search rows\n");
		fails++;
		return;
	}
	snprintf(tool, sizeof tool, "%s/%s", dir, SEARCH_TOOL);
	snprintf(want, sizeof want, "%s", tool);
	if (!(fp = fopen(tool, "w")) || fclose(fp) != 0 || chmod(tool, 0755) != 0) {
		printf("FAIL: cannot make %s\n", tool);
		fails++;
		rmdir(dir);
		return;
	}
	list = malloc(SEARCH_HUGE + strlen(dir) + 8);
	far = run('a', SEARCH_HUGE);
	if (!list || !far)
		exit(2);

	snprintf(list, SEARCH_HUGE + strlen(dir) + 8, "%s:/xwpe-none:%s", dir, dir);
	chk_int("search: found in the first entry", wpe_path_search_in(list, SEARCH_TOOL, out, sizeof out), 0);
	chk_str("search: its path", out, want);

	n = snprintf(list, SEARCH_HUGE + strlen(dir) + 8, "/%.*s:%s", SEARCH_LONG, far, dir);
	chk_int("search: an entry of 300 bytes is passed over", wpe_path_search_in(list, SEARCH_TOOL, out, sizeof out), 0);
	chk_str("search: the path found after it", out, want);
	chk_int("search: the list was built", (long)n > SEARCH_LONG, 1);

	snprintf(list, SEARCH_HUGE + strlen(dir) + 8, "/%.*s:%s", SEARCH_HUGE, far, dir);
	chk_int("search: an entry no system holds is passed over", wpe_path_search_in(list, SEARCH_TOOL, out, sizeof out), 0);
	chk_str("search: the path found after the huge entry", out, want);

	snprintf(list, SEARCH_HUGE + strlen(dir) + 8, "/%.*s", SEARCH_HUGE, far);
	chk_int("search: nothing but the huge entry finds nothing",
	        wpe_path_search_in(list, SEARCH_TOOL, out, sizeof out), -1);
	chk_int("search: and leaves the answer empty", out[0], 0);

	chk_int("search: an answer buffer too small is not cut into another file",
	        wpe_path_search_in(dir, SEARCH_TOOL, small, sizeof small), -1);
	chk_int("search: it is left empty", small[0], 0);
	chk_int("search: the name too long for the buffer",
	        wpe_path_search_in(dir, far, out, sizeof out), -1);

	chk_int("search: a name that is not there", wpe_path_search_in(dir, "wpe-nothing", out, sizeof out), -1);
	chk_int("search: and leaves no candidate behind", out[0], 0);
	chk_int("search: no list", wpe_path_search_in(NULL, SEARCH_TOOL, out, sizeof out), WPE_PATH_SEARCH_NO_LIST);

	/* The folder followed by slashes up to past any path the system holds:
	   cut where a buffer ends, the entry would be the folder itself. */
	n = strlen(dir);
	memcpy(list, dir, n);
	memset(list + n, '/', SEARCH_HUGE - n);
	snprintf(list + SEARCH_HUGE, strlen(dir) + 8, "%s", SEARCH_CUT_TAIL);
	chk_int("search: an entry whose cut is a folder holding the program is not cut",
	        wpe_path_search_in(list, SEARCH_TOOL, out, sizeof out), -1);
	chk_int("search: and names no file", out[0], 0);

	snprintf(plain, sizeof plain, "%s/%s", dir, SEARCH_PLAIN);
	if ((fp = fopen(plain, "w")) && fclose(fp) == 0 && chmod(plain, 0644) == 0)
		chk_int("search: a file that is not executable is not a program",
		        wpe_path_search_in(dir, SEARCH_PLAIN, out, sizeof out), -1);
	else {
		printf("FAIL: cannot make %s\n", plain);
		fails++;
	}
	unlink(plain);

	if (chdir(dir) == 0) {
		chk_int("search: an empty entry is the current folder",
		        wpe_path_search_in("/xwpe-none:", SEARCH_TOOL, out, sizeof out), 0);
		chk_str("search: its path", out, "./" SEARCH_TOOL);
		chk_int("search: an empty list is the current folder",
		        wpe_path_search_in("", SEARCH_TOOL, out, sizeof out), 0);
		if (chdir("/") != 0)
			exit(2);
	}
	unlink(tool);
	rmdir(dir);
	free(list);
	free(far);
}

int main(void)
{
	test_limits();
	test_copy();
	test_join();
	test_join_of_two_that_fit();
	test_cat();
	test_dup();
	test_search();
	if (fails) {
		printf("test_path: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_path: ok\n");
	return 0;
}
