/* test_menurows.c -- unit test for where a dropdown's highlight goes next.
 * Links we_menurows.c only; no editor, no curses -- always runs (no skip).
 *
 * The rule (wrap at both ends, step over a divider, open on the first row the
 * user can pick) was proven before only through the pyte suite, which starts
 * a whole editor and drives keys through a pseudo-terminal: seconds per case.
 * Here every case costs microseconds, including the edges no menu in the
 * editor has today: a divider first or last, two dividers in a row, a menu
 * with nothing to pick.
 */
#include <stdio.h>
#include <string.h>

#include "../we_menurows.h"

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* A menu written as a string: '-' is a divider, any other character a row
   the user can pick.  The Alt-G menu is "AECFGB-YNS". */
static int pick(const void *rows, int i)
{
	return ((const char *)rows)[i] != '-';
}

static int count_of(const char *rows)
{
	return (int)strlen(rows);
}

static void test_down_and_up_step_over_the_divider(void)
{
	const char *m = "AECFGB-YNS";
	int n = count_of(m);

	chk("Down from Build & fix lands on Permissions", wpe_menu_step_row(m, n, 5, 1, pick), 7);
	chk("Up from Permissions lands on Build & fix", wpe_menu_step_row(m, n, 7, -1, pick), 5);
	chk("Down between two ordinary rows", wpe_menu_step_row(m, n, 0, 1, pick), 1);
}

static void test_both_ends_wrap(void)
{
	const char *m = "AECFGB-YNS";
	int n = count_of(m);

	chk("Down from the last row wraps to the first", wpe_menu_step_row(m, n, 9, 1, pick), 0);
	chk("Up from the first row wraps to the last", wpe_menu_step_row(m, n, 0, -1, pick), 9);
}

static void test_open_home_and_end(void)
{
	const char *m = "AECFGB-YNS";
	int n = count_of(m);

	chk("the highlight opens on the first row", wpe_menu_first_row(m, n, pick), 0);
	chk("End reaches the last row", wpe_menu_last_row(m, n, pick), 9);
}

static void test_divider_at_either_end(void)
{
	chk("a leading divider: open on the row after it", wpe_menu_first_row("-AB", 3, pick), 1);
	chk("a trailing divider: End stops before it", wpe_menu_last_row("AB-", 3, pick), 1);
	chk("Down from the last real row wraps past a trailing divider",
	    wpe_menu_step_row("AB-", 3, 1, 1, pick), 0);
	chk("Up from the first real row wraps past a leading divider",
	    wpe_menu_step_row("-AB", 3, 1, -1, pick), 2);
}

static void test_two_dividers_in_a_row(void)
{
	chk("Down crosses two dividers", wpe_menu_step_row("A--B", 4, 0, 1, pick), 3);
	chk("Up crosses two dividers", wpe_menu_step_row("A--B", 4, 3, -1, pick), 0);
}

static void test_nothing_to_pick(void)
{
	chk("a menu of dividers keeps the highlight where it is",
	    wpe_menu_step_row("---", 3, 1, 1, pick), 1);
	chk("first row of an empty menu", wpe_menu_first_row("", 0, pick), 0);
	chk("last row of an empty menu", wpe_menu_last_row("", 0, pick), 0);
}

static void test_one_row(void)
{
	chk("Down in a one-row menu stays on it", wpe_menu_step_row("A", 1, 0, 1, pick), 0);
	chk("Up in a one-row menu stays on it", wpe_menu_step_row("A", 1, 0, -1, pick), 0);
}

int main(void)
{
	test_down_and_up_step_over_the_divider();
	test_both_ends_wrap();
	test_open_home_and_end();
	test_divider_at_either_end();
	test_two_dividers_in_a_row();
	test_nothing_to_pick();
	test_one_row();
	if (fails) {
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("test_menurows: all checks passed\n");
	return 0;
}
