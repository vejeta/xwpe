/* test_path_say.c -- unit test for the sentence a refused path gets
 * (we_path.h: wpe_path_say_too_long, wpe_path_say_name_too_long).
 *
 * A user told that a path is longer than "the system allows" must be told the
 * system's number, not the editor's room: 1024 on macOS and the BSDs, 255 for
 * a name.  This system's numbers are whatever it has, so the test says them
 * before the owner's header is read, and the sentence is then checked against
 * numbers that differ from the editor's own (WPE_PATH_MAX 4096, WPE_NAME_MAX
 * 511) on every system the test runs on.  Kept apart from test_path.c, whose
 * checks read this system's real PATH_MAX.
 */
#include <errno.h>
#include <limits.h>
#include "linebuf.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#undef PATH_MAX
#define PATH_MAX 1024
#undef NAME_MAX
#define NAME_MAX 255

#include "../we_path.h"

static int fails;

static void chk_eq(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: \"%s\", want \"%s\"\n", what, got, want);
		fails++;
	}
}

static void chk_num(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: %ld, want %ld\n", what, got, want);
		fails++;
	}
}

/* The sentence of a refused read, one row per kind of refusal the owner of
   reading names: the shape is "Cannot open <what> <path>: <reason>." and
   "<What> not found: <path>." for a missing name only. */
static void refused_sentences(void)
{
	char b[WPE_PATH_REFUSED_MAX];
	char tight[16];
	char longpath[WPE_PATH_SIZE + 8];
	int need;

	wpe_path_say_refused(b, sizeof b, "file", "/x/a", ENOENT);
	chk_eq("missing", b, "File not found: /x/a.");
	wpe_path_say_refused(b, sizeof b, "project file", "/w/p.prj", ENOENT);
	chk_eq("missing project", b, "Project file not found: /w/p.prj.");
	wpe_path_say_refused(b, sizeof b, "file", "/x/a", EACCES);
	chk_eq("denied", b, "Cannot open file /x/a: Permission denied.");
	wpe_path_say_refused(b, sizeof b, "file", "/x/a/b", ENOTDIR);
	chk_eq("not a folder in the path", b, "Cannot open file /x/a/b: Not a directory.");
	wpe_path_say_refused(b, sizeof b, "file", "/x/loop", ELOOP);
	chk_eq("link loop", b, "Cannot open file /x/loop: Too many levels of symbolic links.");
	chk_eq("the reason of a missing name", wpe_path_refused_reason(ENOENT), "not found");
	chk_eq("the reason of another", wpe_path_refused_reason(EACCES), "Permission denied");
	/* the first letter is raised for ASCII a to z only */
	wpe_path_say_refused(b, sizeof b, "\xc3\xa9" "x", "/x/a", ENOENT);
	chk_eq("a byte above 127 is left alone", b, "\xc3\xa9" "x not found: /x/a.");
	wpe_path_say_refused(b, sizeof b, "Folder", "/x/a", ENOENT);
	chk_eq("a capital stays", b, "Folder not found: /x/a.");
	/* a path longer than a buffer: the length is what the whole sentence needs */
	memset(longpath, 'p', sizeof longpath - 1);
	longpath[sizeof longpath - 1] = '\0';
	need = wpe_path_say_refused(tight, sizeof tight, "file", longpath, EACCES);
	chk_num("the needed length of a long sentence", need,
	        (long)strlen("Cannot open file : Permission denied.") + (long)strlen(longpath));
	chk_num("a tight buffer is cut, never run past", (long)strlen(tight), (long)sizeof tight - 1);
}

/* One row per line of the owner's table of kinds. */
static void kind_sentences(void)
{
	char b[WPE_PATH_REFUSED_MAX];

	wpe_path_say_refused_because(b, sizeof b, "project file", "/w/p.prj",
	                             wpe_path_kind_reason(S_IFIFO, 0));
	chk_eq("a pipe", b, "Cannot open project file /w/p.prj: it is a pipe.");
	chk_eq("a socket", wpe_path_kind_reason(S_IFSOCK, 0), "it is a socket");
	chk_eq("a character device", wpe_path_kind_reason(S_IFCHR, 0), "it is a device");
	chk_eq("a block device", wpe_path_kind_reason(S_IFBLK, 0), "it is a device");
	chk_eq("a folder", wpe_path_kind_reason(S_IFDIR, 0), "it is a folder");
	chk_eq("a terminal, found on a descriptor", wpe_path_kind_reason(S_IFCHR, 1), "it is a terminal");
	chk_eq("a link", wpe_path_reason_of(WPE_PATH_REASON_LINK), "it is a link");
	chk_eq("another file", wpe_path_reason_of(WPE_PATH_REASON_OTHER), "another file took the name");
	chk_eq("a changed file", wpe_path_reason_of(WPE_PATH_REASON_CHANGED), "it changed or was replaced");
	chk_eq("no writer", wpe_path_reason_of(WPE_PATH_REASON_NO_WRITER), "nothing is writing to the pipe");
	chk_eq("not a folder", wpe_path_reason_of(WPE_PATH_REASON_NOT_FOLDER), "it is not a folder");
	wpe_path_too_large_reason(b, sizeof b, (size_t)1 << 20);
	chk_eq("a megabyte", b, "it is larger than 1 MiB");
	wpe_path_too_large_reason(b, sizeof b, (size_t)16 << 20);
	chk_eq("sixteen megabytes", b, "it is larger than 16 MiB");
	wpe_path_too_large_reason(b, sizeof b, 4096);
	chk_eq("four kilobytes", b, "it is larger than 4 KiB");
	wpe_path_too_large_reason(b, sizeof b, 100);
	chk_eq("a hundred bytes", b, "it is larger than 100 bytes");
}

static void chk_has(const char *what, const char *got, const char *want)
{
	if (!strstr(got, want)) {
		printf("FAIL: %s: \"%s\" does not hold \"%s\"\n", what, got, want);
		fails++;
	}
}

static void chk_lacks(const char *what, const char *got, const char *unwanted)
{
	if (strstr(got, unwanted)) {
		printf("FAIL: %s: \"%s\" holds \"%s\"\n", what, got, unwanted);
		fails++;
	}
}

int main(void)
{
	char b[WPE_PATH_MSG_MAX];

	wpe_path_say_too_long(b, sizeof b, "project");
	chk_has("path sentence number", b, "(1024 bytes)");
	chk_has("path sentence names the value", b, "The project path");
	chk_lacks("path sentence is not the editor's room", b, "4096");

	wpe_path_say_name_too_long(b, sizeof b, "file name");
	chk_has("name sentence number", b, "(255 bytes)");
	chk_has("name sentence names the value", b, "The file name path");
	chk_lacks("name sentence is not the editor's room", b, "511");

	/* the sentence is cut by the caller's size, never run past it */
	{
		char small[16];

		wpe_path_say_too_long(small, sizeof small, "project");
		if (strlen(small) != sizeof small - 1) {
			printf("FAIL: a small buffer is not filled to its end\n");
			fails++;
		}
	}

	refused_sentences();
	kind_sentences();

	if (fails) {
		printf("test_path_say: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_path_say: ok\n");
	return 0;
}
