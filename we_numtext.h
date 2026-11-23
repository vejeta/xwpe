/* we_numtext.h -- the numbers the editor shows in places of their own: the
   cursor's line and column, and the counts of a replace.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   The builders are static inline and write through the formatted-text owner
   (we_fmt.h), so the editor and its C test run the same statements without
   linking the editor.  A count is always shown in full, whatever the size of
   the file: the place for it is as large as the type. */

#ifndef WE_NUMTEXT_H
#define WE_NUMTEXT_H

#include "we_fmt.h"

/* The box's third line when a replacement ran out of memory. */
#define E_REPLACE_SAY_NOMEM "Out of memory: the replacement was not made"

/* The bytes of "line:column" for any two ints: both numbers, the colon and
   the blanks of the field, and the NUL. */
#define E_LINECOL_ROOM (2 * WPE_FMT_INT_ROOM + 2)

/* The bytes of a replace report with a third line of at most @reason_room
   bytes (its NUL counts). */
#define E_REPLACE_REPORT_ROOM(reason_room) \
  (sizeof "Found \nReplaced \n" + 2 * WPE_FMT_INT_ROOM + (reason_room))

/**
 * e_linecol_text - The cursor's line and column as the window's bottom frame
 * shows them, whole at any size of file.
 * Return: WPE_FMT_OK, or a refusal with @out "".
 */
static inline WPE_FMT_MUST_READ int e_linecol_text(char *out, size_t n, int line, int col)
{
 return(wpe_fmt_whole(out, n, "%5d:%-4d", line, col));
}

/**
 * e_replace_report_text - What Search > Replace found and replaced, and why
 * any were not; @reason is NULL or the third line.  The counts are never cut:
 * the box shows them in full or not at all.
 * Return: WPE_FMT_OK, or a refusal with @out "".
 */
static inline WPE_FMT_MUST_READ int e_replace_report_text(char *out, size_t n, int found,
                                                          int replaced, const char *reason)
{
 if (reason)
  return(wpe_fmt_whole(out, n, "Found %d\nReplaced %d\n%s", found, replaced, reason));
 return(wpe_fmt_whole(out, n, "Found %d\nReplaced %d", found, replaced));
}

#endif
