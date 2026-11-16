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
#include <limits.h>
#include <stdio.h>
#include <string.h>

#undef PATH_MAX
#define PATH_MAX 1024
#undef NAME_MAX
#define NAME_MAX 255

#include "../we_path.h"

static int fails;

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

	if (fails) {
		printf("test_path_say: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_path_say: ok\n");
	return 0;
}
