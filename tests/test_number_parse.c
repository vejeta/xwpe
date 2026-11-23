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
#include "linebuf.h"
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

/* The line about keys a project's xwperc may not set, as the option reader
   gives it: its long and its short sentence (both empty: no such keys), and
   what the owner said on the bottom row after it. */
static char held[1024];          /* the long sentence */
static char held_short[1024];    /* the short one */
static char held_by_owner[1024];

static int room_columns = 78;    /* what the bottom row shows */

int e_status_room(void)
{
	return room_columns;
}

/* The same choice as the reader's: long, short, or the short one cut. */
int e_opt_ignored_line(char *buf, size_t n, int room)
{
	size_t width;

	buf[0] = '\0';
	if (!held[0])
		return E_OPT_IGNORED_NONE;
	snprintf(buf, n, "%s", held);
	if ((int)strlen(buf) <= room)
		return E_OPT_IGNORED_LONG;
	snprintf(buf, n, "%s", held_short);
	if ((int)strlen(buf) <= room)
		return E_OPT_IGNORED_SHORT;
	width = room > 3 ? (size_t)room - 3 : 0;
	snprintf(buf + width, n - width, "...");
	return E_OPT_IGNORED_CUT;
}

void e_status_say_until_key(FENSTER *f, const char *text)
{
	(void)f;
	snprintf(held_by_owner, sizeof held_by_owner, "%s", text);
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

/* What Save writes for one kind, as one line of text. */
static void written(ECNT *cn, const WpeNumberType *t, char *out, size_t n)
{
	FILE *fp = tmpfile();
	size_t got;

	out[0] = '\0';
	if (!fp)
		return;
	e_number_write(cn, t, fp);
	rewind(fp);
	got = fread(out, 1, n - 1, fp);
	out[got] = '\0';
	fclose(fp);
}

/* Five refused numbers on a row too narrow for their values: the keys alone,
   then the end cut when not even they fit, the held line's head always whole. */
static void narrow_rows(ECNT *cn)
{
	const char *keys = "xwperc not used: MaxColumn, Tab, MaxChanges, NumUndo, AutoIndent";

	chk_int("load MaxChanges 1000", e_number_load(cn, WPE_NUM(WPE_NUM_MAXCHG), "1000"), -1);
	held[0] = '\0';
	room_columns = 78;
	e_number_say_refused(cn);
	chk_str("the keys alone", held_by_owner, keys);
	room_columns = 40;
	e_number_say_refused(cn);
	chk_str("whole keys, the rest counted", held_by_owner, "xwperc not used: MaxColumn, Tab +3");

	snprintf(held, sizeof held, "keys line");
	snprintf(held_short, sizeof held_short, "keys");
	room_columns = 78;
	e_number_say_refused(cn);
	chk_str("the keys after the long keys line", held_by_owner,
	        "keys line; not used: MaxColumn, Tab, MaxChanges, NumUndo, AutoIndent");

	snprintf(held, sizeof held, "./xwperc may not set (only your own config may): AIBackend");
	snprintf(held_short, sizeof held_short, "./xwperc may not set: AIBackend");
	e_number_say_refused(cn);
	chk_str("the short keys line when the long one would not fit", held_by_owner,
	        "./xwperc may not set: AIBackend; not used: MaxColumn, Tab, MaxChanges +2");
	room_columns = 52;
	e_number_say_refused(cn);
	chk_str("only a count fits", held_by_owner, "./xwperc may not set: AIBackend; 5 numbers not used");
	room_columns = 50;
	e_number_say_refused(cn);
	chk_str("the numbers are left out", held_by_owner, "./xwperc may not set: AIBackend");
	room_columns = 20;
	e_number_say_refused(cn);
	chk_str("the keys line is cut from the right", held_by_owner, "./xwperc may not ...");
	held[0] = '\0';
	held_short[0] = '\0';
}

/* A DOS line end behind a number is not part of it. */
static void dos_line_end(ECNT *cn)
{
	char out[64];
	int v;

	chk_int("4 and a CR", wpe_number_parse("4\r", WPE_NUM(WPE_NUM_TAB), &v), WPE_NUMBER_OK);
	chk_int("the value", v, 4);
	chk_int("0 and a CR", e_number_load(cn, WPE_NUM(WPE_NUM_TAB), "0\r"), -1);
	written(cn, WPE_NUM(WPE_NUM_TAB), out, sizeof out);
	chk_str("the CR is not kept", out, "Tab : 0\n");
}

/* The option file: a number in range is used, any other is kept as the file
   wrote it, said once, and written back until the user stores another. */
static void the_file(void)
{
	ECNT cn;
	char out[128];
	int k;

	memset(&cn, 0, sizeof cn);
	e_number_defaults(&cn);

	chk_int("load Tab 3", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "3"), 0);
	chk_int("tabn is 3", cn.tabn, 3);
	written(&cn, WPE_NUM(WPE_NUM_TAB), out, sizeof out);
	chk_str("write Tab 3", out, "Tab : 3\n");

	chk_int("load Tab 0", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "0"), -1);
	chk_int("tabn stays 3", cn.tabn, 3);
	chk_int("load Tab text", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "8x"), -1);
	chk_int("load Tab past an int", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "99999999999"), -1);
	written(&cn, WPE_NUM(WPE_NUM_TAB), out, sizeof out);
	chk_str("write the refused text", out, "Tab : 99999999999\n");

	chk_int("a later good value", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "5"), 0);
	written(&cn, WPE_NUM(WPE_NUM_TAB), out, sizeof out);
	chk_str("write the good value again", out, "Tab : 5\n");

	chk_int("load NumUndo 500", e_number_load(&cn, WPE_NUM(WPE_NUM_NUMUNDO), "500"), -1);
	chk_int("numundo stays 10", cn.numundo, 10);
	chk_int("store the value in force is not a choice",
	        e_number_store(&cn, WPE_NUM(WPE_NUM_NUMUNDO), 10), 0);
	written(&cn, WPE_NUM(WPE_NUM_NUMUNDO), out, sizeof out);
	chk_str("still the file's text", out, "NumUndo : 500\n");
	chk_int("store 20", e_number_store(&cn, WPE_NUM(WPE_NUM_NUMUNDO), 20), 0);
	written(&cn, WPE_NUM(WPE_NUM_NUMUNDO), out, sizeof out);
	chk_str("the user's number is written", out, "NumUndo : 20\n");

	written(&cn, WPE_NUM(WPE_NUM_LINE), out, sizeof out);
	chk_str("Go to Line has no key", out, "");

	/* the start-up line: one, joined to what is held, naming each number */
	cn.f[0] = &window;
	held[0] = '\0';
	held_by_owner[0] = '\0';
	e_number_say_refused(&cn);
	chk_str("nothing refused, nothing said", held_by_owner, "");

	room_columns = 200;
	chk_int("load Tab 0 again", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "0"), -1);
	chk_int("load AutoIndent -1", e_number_load(&cn, WPE_NUM(WPE_NUM_AUTOINDENT), "-1"), -1);
	snprintf(held, sizeof held, "keys line");
	snprintf(held_short, sizeof held_short, "keys");
	e_number_say_refused(&cn);
	chk_str("the joined line", held_by_owner,
	        "keys line; xwperc: Tab : 0 is not 1 to 99, 5 used (the file keeps 0); "
	        "AutoIndent : -1 is not 0 to 99, 3 used (the file keeps -1)");

	/* a long file text in a sentence is cut where it ends and says so */
	chk_int("load Tab with 60 digits for the sentence",
	        e_number_load(&cn, WPE_NUM(WPE_NUM_TAB),
	                      "123456789012345678901234567890123456789012345678901234567890"), -1);
	e_number_say_refused(&cn);
	chk_str("a long text ends in the mark", held_by_owner,
	        "keys line; xwperc: Tab : 123456789012345678901... is not 1 to 99, 5 used "
	        "(the file keeps 123456789012345678901...); "
	        "AutoIndent : -1 is not 0 to 99, 3 used (the file keeps -1)");
	chk_int("load Tab 0 back", e_number_load(&cn, WPE_NUM(WPE_NUM_TAB), "0"), -1);

	/* a narrow row: the compact form names every key, with a short value */
	room_columns = 78;
	held[0] = '\0';
	held_short[0] = '\0';
	chk_int("load MaxColumn 0", e_number_load(&cn, WPE_NUM(WPE_NUM_MAXCOL), "0"), -1);
	chk_int("load NumUndo 100", e_number_load(&cn, WPE_NUM(WPE_NUM_NUMUNDO), "100"), -1);
	e_number_say_refused(&cn);
	chk_str("compact", held_by_owner,
	        "xwperc not used: MaxColumn 0, Tab 0, NumUndo 100, AutoIndent -1");
	chk_int("load Tab with 60 digits",
	        e_number_load(&cn, WPE_NUM(WPE_NUM_TAB),
	                      "123456789012345678901234567890123456789012345678901234567890"), -1);
	e_number_say_refused(&cn);
	chk_str("a long value is cut", held_by_owner,
	        "xwperc not used: MaxColumn 0, Tab 12345678..., NumUndo 100, AutoIndent -1");
	snprintf(held, sizeof held, "keys line");
	snprintf(held_short, sizeof held_short, "keys");
	e_number_say_refused(&cn);
	chk_str("after a held line", held_by_owner,
	        "keys line; not used: MaxColumn 0, Tab 12345678..., NumUndo 100, AutoIndent -1");

	narrow_rows(&cn);
	dos_line_end(&cn);

	e_number_defaults(&cn);
	for (k = 0; k < WPE_NUM_KINDS; k++) {
		if (WPE_NUM(k)->key == NULL)
			continue;
		written(&cn, WPE_NUM(k), out, sizeof out);
		snprintf(held, sizeof held, "%s : %d\n", WPE_NUM(k)->key, WPE_NUM(k)->fallback);
		chk_str("defaults clear the refused texts", out, held);
	}
}

int main(void)
{
	every_kind();
	the_file();
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
