/* test_menus.c -- unit test for the menu table (we_menus.c).
 *
 * Links we_menus.c and we_bindings.c only: no editor, no display.  The
 * functions the rows run and the builder's calls into the editor are left
 * unresolved on purpose; nothing here calls them.
 *
 * It proves the three bars equal the literals the editor drew before the
 * menus became data, how many rows every menu has in every mode, the filter
 * that decides a row, the rules of the table itself (hot letters, keys), and
 * that no Alt letter of the bar is bound to a key that would hide the menu.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "../edit.h"
#include "../we_menus.h"

/* What the editor supplies to the table's file. */
char e_we_sw;
char *e_hlp_str[40];
char *e_msg[40];

/* the table's file owns the bar the editor shows */
extern OPT opt[];
extern int e_mn_men;

static int fails;

static void fail(const char *what, int a, int b)
{
	if (fails < 60)
		printf("FAIL: %s: %d, %d\n", what, a, b);
	fails++;
}

static void failstr(const char *what, const char *got, const char *want)
{
	if (fails < 60)
		printf("FAIL: %s: got %s want %s\n", what, got, want);
	fails++;
}

/* HEAD's bars: we_main.c's static opt[] for the plain bar and the two
 * shuffles of e_ini_unix for the programming bars, written out. */
typedef struct {
	const char *t;
	int x;
	char s;
	int as;
} BarLiteral;

static const BarLiteral plain_bar[] = {
	{ "#", 3, '#', AltSYS }, { "File", 10, 'F', AltF }, { "Edit", 19, 'E', AltE },
	{ "Search", 28, 'S', AltS }, { "Block", 39, 'B', AltB }, { "Options", 49, 'O', AltO },
	{ "Window", 61, 'W', AltW }, { "Help", 72, 'H', AltH }
};
static const BarLiteral prog_bar[] = {
	{ "#", 2, '#', AltSYS }, { "File", 7, 'F', AltF }, { "Edit", 14, 'E', AltE },
	{ "Search", 21, 'S', AltS }, { "Block", 30, 'B', AltB }, { "Run", 38, 'R', AltR },
	{ "Project", 45, 'P', AltP }, { "Options", 55, 'O', AltO },
	{ "Window", 65, 'W', AltW }, { "Help", 74, 'H', AltH }
};
static const BarLiteral debug_bar[] = {
	{ "#", 2, '#', AltSYS }, { "File", 6, 'F', AltF }, { "Edit", 12, 'E', AltE },
	{ "Search", 18, 'S', AltS }, { "Block", 26, 'B', AltB }, { "Run", 34, 'R', AltR },
	{ "Debug", 40, 'D', AltD }, { "Project", 47, 'P', AltP }, { "Options", 56, 'O', AltO },
	{ "Window", 65, 'W', AltW }, { "Help", 74, 'H', AltH }
};

static void check_bar(const char *name, int composition, const BarLiteral *want, int n,
		      int gap)
{
	int i;
	char what[64];

	e_menu_bar_from_heads(composition);
	if (MENOPT != n)
		fail("MENOPT", MENOPT, n);
	if (e_mn_men != gap)
		fail("e_mn_men", e_mn_men, gap);
	for (i = 0; i < n; i++) {
		snprintf(what, sizeof what, "%s bar entry %d", name, i);
		if (strcmp(opt[i].t, want[i].t))
			failstr(what, opt[i].t, want[i].t);
		if (opt[i].x != want[i].x)
			fail(what, opt[i].x, want[i].x);
		if (opt[i].s != want[i].s)
			fail(what, opt[i].s, want[i].s);
		if (opt[i].as != want[i].as)
			fail(what, opt[i].as, want[i].as);
	}
}

/* The three bars equal the literals of the editor before the table: titles,
 * columns, plain letters, Alt letters, the number of entries and the gap the
 * mouse leaves left of a title. */
static void test_bars(void)
{
	check_bar("plain", WPE_BAR_PLAIN, plain_bar, 8, 3);
	check_bar("programming", WPE_BAR_PROG, prog_bar, 10, 2);
	check_bar("programming with debugger", WPE_BAR_PROG_DEBUG, debug_bar, 11, 1);
	if (wpe_menu_bar_composition(0) != WPE_BAR_PLAIN)
		fail("plain editor's bar", wpe_menu_bar_composition(0), WPE_BAR_PLAIN);
#ifdef DEBUGGER
	if (wpe_menu_bar_composition(1) != WPE_BAR_PROG_DEBUG)
		fail("programming editor's bar", wpe_menu_bar_composition(1), WPE_BAR_PROG_DEBUG);
#endif
}

static int rows_in(int menu, int style, int blocks, int prog, int xwin, int ai)
{
	size_t i, n;
	const WpeMenuRow *r = wpe_menu_rows(&n);
	WpeMenuFacts f;
	int count = 0;

	f.style = style;
	f.blocks = blocks;
	f.prog = prog;
	f.xwin = xwin;
	f.assistant = ai;
	for (i = 0; i < n; i++)
		if (r[i].menu == menu && wpe_menu_row_shown(&r[i], &f))
			count++;
	return count;
}

/* How many rows every menu had, in every mode, before the table. */
static void test_row_counts(void)
{
	struct { int menu; int count; int prog; int xwin; } want[] = {
		{ WPE_MENU_SYSTEM, 6, 0, 0 },
		{ WPE_MENU_FILE, 11, 0, 0 },
		{ WPE_MENU_EDIT, 7, 0, 0 },
		{ WPE_MENU_SEARCH, 4, 0, 0 },
		{ WPE_MENU_RUN, 12, 1, 0 },
		{ WPE_MENU_DEBUG, 16, 1, 0 },
		{ WPE_MENU_PROJECT, 6, 1, 0 },
		{ WPE_MENU_OPTIONS, 6, 0, 0 },    /* with the assistant */
		{ WPE_MENU_OPTIONS, 9, 1, 0 },
		{ WPE_MENU_WINDOW, 8, 0, 0 },     /* User Screen on a terminal */
		{ WPE_MENU_WINDOW, 7, 0, 1 },
		{ WPE_MENU_WINDOW, 11, 1, 0 },
		{ WPE_MENU_WINDOW, 10, 1, 1 },
		{ WPE_MENU_HELP, 8, 0, 0 },
	};
	size_t i;
	char what[64];

	for (i = 0; i < sizeof want / sizeof want[0]; i++) {
		int style;

		for (style = 0; style < WPE_STYLE_COUNT; style++) {
			int got = rows_in(want[i].menu, style, WPE_BLOCKS_ANCHOR, want[i].prog,
					  want[i].xwin, 1);

			snprintf(what, sizeof what, "rows of menu %d, style %d", want[i].menu, style);
			if (got != want[i].count)
				fail(what, got, want[i].count);
		}
	}
	/* no assistant: Options has one row less */
	if (rows_in(WPE_MENU_OPTIONS, WPE_STYLE_BORLAND, WPE_BLOCKS_ANCHOR, 0, 0, 0) != 5)
		fail("Options without the assistant", 0, 5);
}

/* The filter: Mark Word is the Modern style's and the Selection mode's, and
 * nothing else in the Block menu depends on either. */
static void test_filter(void)
{
	int style, blocks;
	const int modes[] = { WPE_BLOCKS_ANCHOR, WPE_BLOCKS_WORDSTAR, WPE_BLOCKS_SELECTION };

	for (style = 0; style < WPE_STYLE_COUNT; style++)
		for (blocks = 0; blocks < 3; blocks++) {
			int got = rows_in(WPE_MENU_BLOCK, style, modes[blocks], 1, 0, 1);
			int want = (style == WPE_STYLE_MODERN || modes[blocks] == WPE_BLOCKS_SELECTION) ? 16 : 15;

			if (got != want)
				fail("Block rows by style and block mode", style * 10 + blocks, got);
		}
}

static int hot_exception(const WpeMenuRow *r)
{
	/* Three rows of the editor before the table: their hot letter is not
	 * the letter the stem points at.  Kept as they were. */
	return !strcmp(r->stem, "Goto cursor") || !strcmp(r->stem, "File-Manager") ||
	       (r->menu == WPE_MENU_HELP && !strcmp(r->stem, "Next"));
}

/* The rules of the rows: the hot index lies in the stem and points at the
 * hot letter, and the letters of a menu are unique. */
static void test_row_rules(void)
{
	size_t i, j, n;
	const WpeMenuRow *r = wpe_menu_rows(&n);

	for (i = 0; i < n; i++) {
		if (r[i].hot < 0 || (size_t)r[i].hot >= strlen(r[i].stem))
			fail("hot index outside the stem", (int)i, r[i].hot);
		else if (!hot_exception(&r[i]) &&
			 toupper((unsigned char)r[i].stem[r[i].hot]) != toupper((unsigned char)r[i].letter))
			failstr("hot index not at the hot letter", r[i].stem, "the letter");
		for (j = i + 1; j < n; j++)
			if (r[j].menu == r[i].menu && r[j].letter == r[i].letter && r[i].letter &&
			    !(r[i].menu == WPE_MENU_HELP && r[j].letter == 'G'))
				failstr("two rows with one letter", r[i].stem, r[j].stem);
	}
}

/* A row that names an action has a menu key for it in at least one style:
 * else the key column is empty in both and the row should say NONE. */
static void test_rows_name_an_action_with_keys(void)
{
	size_t i, n;
	const WpeMenuRow *r = wpe_menu_rows(&n);
	char keys[96];

	for (i = 0; i < n; i++) {
		int style, found = 0;

		if (r[i].action == WPE_ACT_NONE)
			continue;
		for (style = 0; style < WPE_STYLE_COUNT; style++)
			found += wpe_binding_key_text(keys, sizeof keys, (WpeAction)r[i].action, style,
						      r[i].contexts, WPE_ROW_MENU, WPE_NAME_MENU, NULL, 0) > 0;
		/* Tile and Cascade name their action though neither style binds a
		 * key to it: a key added to the table shows beside them. */
		if (!found && strcmp(r[i].stem, "Tile") && strcmp(r[i].stem, "Cascade"))
			failstr("row names an action with no menu key in either style", r[i].stem, "a key");
	}
}

/* The Alt letter of a menu opens it only when no key table row takes it
 * first: a WINDOW, PROGRAM or DEBUG row would hide the menu, an EDIT row
 * would be dead.  Over the three bars and both styles. */
static void test_no_bar_alt_is_bound(void)
{
	size_t i, n;
	const WpeMenuHead *h = wpe_menu_heads(&n);
	int comp, style, ctx;
	char what[64];

	for (comp = 0; comp < WPE_BAR_COUNT; comp++)
		for (i = 0; i < n; i++) {
			if (!wpe_menu_head_shown(&h[i], comp))
				continue;
			for (style = 0; style < WPE_STYLE_COUNT; style++)
				for (ctx = 0; ctx < WPE_CTX_COUNT; ctx++) {
					if (!(WPE_CTXM_DIRECT & WPE_CTX_BIT(ctx)))
						continue;
					if (wpe_binding_action(h[i].alt, style, ctx) != WPE_ACT_NONE) {
						snprintf(what, sizeof what, "%s's Alt letter is a key of context %d",
							 h[i].title, ctx);
						fail(what, style, comp);
					}
				}
		}
}

int main(void)
{
	test_bars();
	test_row_counts();
	test_filter();
	test_row_rules();
	test_rows_name_an_action_with_keys();
	test_no_bar_alt_is_bound();
	if (fails) {
		printf("test_menus: %d failures\n", fails);
		return 1;
	}
	printf("test_menus: ok\n");
	return 0;
}
