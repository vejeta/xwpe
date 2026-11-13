/* test_fieldtake.c -- unit test for the one rule that takes a dialog field's
 * text into a caller's buffer (we_fieldtake.h).  Header only; always runs.
 *
 * The text goes in whole or not at all: never past the buffer (the strcpy
 * this replaces) and never cut to fit (the strncpy it would be replaced by).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_fieldtake.h"

#define CANARY 'Z'
#define GUARD  8     /* bytes past the buffer that must stay untouched */

static int fails;

static void chk_int(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
		fails++;
	}
}

/* A field text of @len bytes taken into a buffer of @size bytes, with a
   guard behind the buffer to see a write past it. */
static void take(const char *what, size_t len, size_t size, int want)
{
	char *txt = malloc(len + 1);
	char *mem = malloc(size + GUARD);
	size_t i;
	int guard_ok = 1;

	if (!txt || !mem)
		exit(2);
	memset(txt, 'a', len);
	txt[len] = '\0';
	memset(mem, CANARY, size + GUARD);
	chk_int(what, wpe_field_take(txt, mem, size), want);
	for (i = size; i < size + GUARD; i++)
		if (mem[i] != CANARY)
			guard_ok = 0;
	chk_int("nothing written past the buffer", guard_ok, 1);
	if (want == 0)
		chk_int("the text arrives whole", (long)strlen(mem), (long)len);
	else
		chk_int("a refused text leaves the buffer empty", mem[0], 0);
	free(txt);
	free(mem);
}

int main(void)
{
	char one[1];

	take("a text one byte under the room fits", 79, 80, 0);
	take("a text of exactly the room less the terminator", 79, 80, 0);
	take("a text of the buffer's size is refused", 80, 80, -1);
	take("the 128 byte field into an 80 byte buffer is refused", 128, 80, -1);
	take("the 128 byte field into a 129 byte buffer fits", 128, 129, 0);
	take("the 128 byte field into a 128 byte buffer is refused", 128, 128, -1);
	take("the empty text fits", 0, 1, 0);
	chk_int("NULL text is refused", wpe_field_take(NULL, one, sizeof one), -1);
	chk_int("a NULL buffer is refused", wpe_field_take("a", NULL, 4), -1);
	chk_int("a zero size is refused", wpe_field_take("a", one, 0), -1);
	if (fails) {
		printf("test_fieldtake: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_fieldtake: ok\n");
	return 0;
}
