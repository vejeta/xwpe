#ifndef WPE_MENUROWS_H
#define WPE_MENUROWS_H

/* Moving the highlight in a dropdown menu.  Kept in its own file, with no
   editor headers, so the rule can be unit tested directly: we_menue.c draws
   and reads keys, and does not link on its own. */

/**
 * wpe_menu_row_pick - Whether the highlight may rest on row @i.
 * @rows: the menu's rows, as the caller stores them.
 * @i:    a row index.
 *
 * Supplied by the caller: the editor answers from its menu rows (a divider
 * has no hotkey), a test answers from a plain array.
 */
typedef int (*wpe_menu_row_pick)(const void *rows, int i);

/**
 * wpe_menu_step_row - The next row the highlight can rest on.
 * @rows:  the rows, passed through to @pick.
 * @count: how many rows the menu has, dividers included.
 * @from:  the row the highlight is on now.
 * @step:  +1 for Down, -1 for Up.
 * @pick:  says whether a row can be picked.
 *
 * Wraps at both ends, so Down from the last row reaches the first and Up from
 * the first reaches the last; a divider met on the way is stepped over, so
 * the user never sees the highlight on a line that runs nothing.
 *
 * Return: the new row, or @from when no row can be picked at all.
 */
int wpe_menu_step_row(const void *rows, int count, int from, int step,
                      wpe_menu_row_pick pick);

/**
 * wpe_menu_first_row - Where the highlight goes when the menu opens or on Home.
 * Return: the first row that can be picked (0 when none can).
 */
int wpe_menu_first_row(const void *rows, int count, wpe_menu_row_pick pick);

/**
 * wpe_menu_last_row - Where the highlight goes on End.
 * Return: the last row that can be picked (0 when none can).
 */
int wpe_menu_last_row(const void *rows, int count, wpe_menu_row_pick pick);

#endif
