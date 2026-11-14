/* test_number_parse.c -- unit test for the owner of a number the user or a
 * file gives the editor (we_number.c).
 *
 * Links we_number.c and WeString.c only: no editor, no display, always runs.
 * The status row the owner speaks on is a stub that keeps the last words.
 *
 * Covers the parser (a text is one whole number or it is refused, overflow
 * included), the range of each kind at min-1, min, max and max+1, the
 * sentence that names a range, that the parser never saturates, and the
 * editor's side: the store (a value equal to the one in force is left
 * alone), the defaults, and a field's accept (Go to Line saturates, the
 * others refuse and say the range).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../edit.h"

static int fails;
static char said[128];
static FENSTER window;      /* the stub never reads it */

/* The status row, as the owner sees it. */
void e_status_say(FENSTER *f, const char *text)
{
	(void)f;
	snprintf(said, sizeof said, "%s", text);
}

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

/* @text read as a number of @t: the answer, and the number when it is OK. */
static void parse(const char *what, const char *text, const WpeNumberType *t,
		  int want, int want_value)
{
	int value = -12345;
	char name[160];
	int got;

	snprintf(name, sizeof name, "%s: \"%s\" as %s", what, text, t->label);
	got = wpe_number_parse(text, t, &value);
	chk_int(name, got, want);
	if (want == WPE_NUMBER_OK)
		chk_int(name, value, want_value);
	else
		chk_int(name, value, -12345);     /* written only on OK */
}

/* The texts that are not numbers, whatever the range. */
static void not_numbers(const WpeNumberType *t)
{
	parse("empty", "", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("blank", " ", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("letter", "x", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("trailing text", "8x", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("leading plus", "+8", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("sign alone", "-", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("two numbers", "8 9", t, WPE_NUMBER_NOT_A_NUMBER, 0);
	parse("null", NULL, t, WPE_NUMBER_NOT_A_NUMBER, 0);
}

/* The edges of the range of a type. */
static void edges(const WpeNumberType *t)
{
	char text[32];

	if (t->min > 0) {
		snprintf(text, sizeof text, "%d", t->min - 1);
		parse("min-1", text, t, WPE_NUMBER_RANGE, 0);
	}
	snprintf(text, sizeof text, "%d", t->min);
	parse("min", text, t, WPE_NUMBER_OK, t->min);
	snprintf(text, sizeof text, "%d", t->max);
	parse("max", text, t, WPE_NUMBER_OK, t->max);
	if (t->max < INT_MAX) {
		snprintf(text, sizeof text, "%d", t->max + 1);
		parse("max+1", text, t, WPE_NUMBER_RANGE, 0);
	}
}

/* The numbers every parse has an answer for, on every type. */
static void overflow(const WpeNumberType *t)
{
	parse("ten digits past int", "99999999999", t, WPE_NUMBER_RANGE, 0);
	parse("negative past int", "-99999999999", t, WPE_NUMBER_RANGE, 0);
	parse("long past long", "99999999999999999999999", t, WPE_NUMBER_RANGE, 0);
	parse("INT_MIN", "-2147483648", t, WPE_NUMBER_RANGE, 0);
	parse("minus zero", "-0", t, t->min == 0 ? WPE_NUMBER_OK : WPE_NUMBER_RANGE, 0);
	parse("blanks around", " 8 ", t, t->max >= 8 && t->min <= 8 ? WPE_NUMBER_OK : WPE_NUMBER_RANGE, 8);
}

static void every_kind(void)
{
	int k;

	for (k = 0; k < WPE_NUM_KINDS; k++) {
		const WpeNumberType *t = WPE_NUM(k);

		chk_int("table kind", t->kind, k);
		not_numbers(t);
		edges(t);
		overflow(t);
	}
	parse("INT_MAX on Go to Line", "2147483647", WPE_NUM(WPE_NUM_LINE), WPE_NUMBER_OK, INT_MAX);
	parse("INT_MAX+1 on Go to Line, not saturated by the parser",
	      "2147483648", WPE_NUM(WPE_NUM_LINE), WPE_NUMBER_RANGE, 0);
	parse("zero on Tabstops", "0", WPE_NUM(WPE_NUM_TAB), WPE_NUMBER_RANGE, 0);
	parse("zero on Max. Changes", "0", WPE_NUM(WPE_NUM_MAXCHG), WPE_NUMBER_OK, 0);
	parse("zero on Num. Undo", "0", WPE_NUM(WPE_NUM_NUMUNDO), WPE_NUMBER_OK, 0);
}

static void the_table(void)
{
	chk_int("Max. Columns min", WPE_NUM(WPE_NUM_MAXCOL)->min, 4);
	chk_int("Max. Columns max", WPE_NUM(WPE_NUM_MAXCOL)->max, 9999);
	chk_int("Tabstops min", WPE_NUM(WPE_NUM_TAB)->min, 1);
	chk_int("Tabstops max", WPE_NUM(WPE_NUM_TAB)->max, 99);
	chk_int("Max. Changes max", WPE_NUM(WPE_NUM_MAXCHG)->max, 999);
	chk_int("Num. Undo max", WPE_NUM(WPE_NUM_NUMUNDO)->max, 99);
	chk_int("Auto Ind. Col. max", WPE_NUM(WPE_NUM_AUTOINDENT)->max, 99);
	chk_int("Go to Line max", WPE_NUM(WPE_NUM_LINE)->max, INT_MAX);
	chk_int("only Go to Line saturates", WPE_NUM(WPE_NUM_LINE)->above_max, WPE_NUM_SATURATE);
	chk_int("Tabstops refuses", WPE_NUM(WPE_NUM_TAB)->above_max, WPE_NUM_REFUSE);
	chk_int("Go to Line has no file key", WPE_NUM(WPE_NUM_LINE)->key == NULL, 1);
	chk_str("Tabstops key", WPE_NUM(WPE_NUM_TAB)->key, "Tab");
}

static void range_words(void)
{
	char buf[96];

	wpe_number_range_words(buf, sizeof buf, WPE_NUM(WPE_NUM_TAB), "0");
	chk_str("Tabstops 0", buf, " Tabstops: 1 to 99 (not 0)");
	wpe_number_range_words(buf, sizeof buf, WPE_NUM(WPE_NUM_TAB), "100");
	chk_str("Tabstops 100", buf, " Tabstops: 1 to 99");
	wpe_number_range_words(buf, sizeof buf, WPE_NUM(WPE_NUM_MAXCOL), NULL);
	chk_str("Max. Columns", buf, " Max. Columns: 4 to 9999");
}

static void the_store(void)
{
	ECNT cn;

	memset(&cn, 0, sizeof cn);
	e_number_defaults(&cn);
	chk_int("default maxcol", cn.maxcol, MAXCOLUM);
	chk_int("default tabn", cn.tabn, 8);
	chk_int("default maxchg", cn.maxchg, 999);
	chk_int("default numundo", cn.numundo, 10);
	chk_int("default autoindent", cn.autoindent, 3);

	chk_int("store 4", e_number_store(&cn, WPE_NUM(WPE_NUM_TAB), 4), 0);
	chk_int("tabn is 4", cn.tabn, 4);
	chk_int("store 0 on Tabstops is refused", e_number_store(&cn, WPE_NUM(WPE_NUM_TAB), 0), -1);
	chk_int("tabn still 4", cn.tabn, 4);
	chk_int("store 100 on Tabstops is refused", e_number_store(&cn, WPE_NUM(WPE_NUM_TAB), 100), -1);
	chk_int("store -3 on Auto Ind. Col. is refused", e_number_store(&cn, WPE_NUM(WPE_NUM_AUTOINDENT), -3), -1);
	chk_int("autoindent still 3", cn.autoindent, 3);
	chk_int("store 9999 on Max. Columns", e_number_store(&cn, WPE_NUM(WPE_NUM_MAXCOL), 9999), 0);
	chk_int("store 3 on Max. Columns is refused", e_number_store(&cn, WPE_NUM(WPE_NUM_MAXCOL), 3), -1);
	chk_int("Go to Line has no slot", e_number_store(&cn, WPE_NUM(WPE_NUM_LINE), 5), -1);

	/* A value equal to the one in force is not a write and is not refused,
	   whatever its range: Ok on a box whose file gave a number the owner
	   would not take must not raise an error for it. */
	cn.numundo = 500;
	chk_int("store equal, out of range", e_number_store(&cn, WPE_NUM(WPE_NUM_NUMUNDO), 500), 0);
	chk_int("numundo untouched", cn.numundo, 500);
	chk_int("store another from there", e_number_store(&cn, WPE_NUM(WPE_NUM_NUMUNDO), 20), 0);
	chk_int("numundo is 20", cn.numundo, 20);
}

static void a_field(W_O_NUMSTR *n, const WpeNumberType *type, int value)
{
	memset(n, 0, sizeof *n);
	e_number_field_init(n, type, value);
}

static void the_field(void)
{
	W_O_NUMSTR n;
	char text[WPE_NUM_TEXT_SIZE];

	a_field(&n, WPE_NUM(WPE_NUM_TAB), 8);
	chk_int("Tabstops has two places", n.nw, 2);
	a_field(&n, WPE_NUM(WPE_NUM_MAXCOL), 120);
	chk_int("Max. Columns has four places", n.nw, 4);
	a_field(&n, WPE_NUM(WPE_NUM_LINE), 3);
	chk_int("Go to Line has ten places", n.nw, 10);
	chk_int("the field holds what it was given", n.num, 3);

	a_field(&n, WPE_NUM(WPE_NUM_TAB), 8);
	said[0] = '\0';
	snprintf(text, sizeof text, " 0");
	chk_int("0 on Tabstops is refused", e_number_field_accept(&n, text, NULL), -1);
	chk_int("the field keeps 8", n.num, 8);
	chk_str("the text is left as typed", text, " 0");

	/* a field of a window says the range; with no window it stays silent */
	said[0] = '\0';
	chk_int("again, with a window", e_number_field_accept(&n, text, &window), -1);
	chk_str("the words", said, " Tabstops: 1 to 99 (not 0)");

	snprintf(text, sizeof text, "12");
	chk_int("12 on Tabstops", e_number_field_accept(&n, text, NULL), 0);
	chk_int("the field holds 12", n.num, 12);

	snprintf(text, sizeof text, "  ");
	chk_int("blank on Tabstops is refused", e_number_field_accept(&n, text, NULL), -1);
	chk_int("the field keeps 12", n.num, 12);

	a_field(&n, WPE_NUM(WPE_NUM_LINE), 1);
	snprintf(text, sizeof text, "3456789012");
	chk_int("ten digits past int on Go to Line", e_number_field_accept(&n, text, NULL), 0);
	chk_int("saturated", n.num, INT_MAX);
	chk_str("the text is rewritten", text, "2147483647");
	snprintf(text, sizeof text, "9999999999");
	chk_int("9999999999 on Go to Line", e_number_field_accept(&n, text, NULL), 0);
	chk_int("saturated again", n.num, INT_MAX);
	snprintf(text, sizeof text, "         0");
	chk_int("0 on Go to Line", e_number_field_accept(&n, text, NULL), 0);
	chk_int("the field holds 0", n.num, 0);
	snprintf(text, sizeof text, "        -5");
	chk_int("-5 on Go to Line is not saturated", e_number_field_accept(&n, text, NULL), -1);
}

int main(void)
{
	every_kind();
	the_table();
	range_words();
	the_store();
	the_field();
	if (fails) {
		printf("test_number_parse: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_number_parse: all passed\n");
	return 0;
}
