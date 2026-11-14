/* we_rowfind.h -- the one owner of a search over a row.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Find, Replace, Change All and Grep ask this owner where the Find text
   occurs on a row; none of them runs a regular expression engine or decides
   what a word, a character, a case or an empty hit is.  The owner needs
   only libc and the UTF-8 rule of we_utf8.h, so its C test links it alone.

   Where the C library has no REG_STARTEND (musl) the engine matches a copy
   of the row from where each call starts, so a \b, \B, \< or \> at that
   place is judged without the byte before it: at the cursor, after each
   rejected match and at the start of each backward window. */

#ifndef WE_ROWFIND_H
#define WE_ROWFIND_H

#include "edit.h"

/** A hit on a row: byte columns of the row, both on a character start or at
 *  the row's length.  eo == so is a zero-width hit: a position. */
typedef struct {
 int so;
 int eo;
} WpeRowHit;

/* What a row is in its line (a line longer than Max. Columns is several
   rows, and ^ and $ belong to the line, not to the row). */
#define WPE_ROW_STARTS_LINE  1u   /* the first row, or the row before ends with a line break */
#define WPE_ROW_ENDS_LINE    2u   /* the row ends with a line break, or is the last row */

/* How the search runs. */
#define WPE_ROWFIND_BACKWARD   4u /* the hit that starts last, with eo <= to */
#define WPE_ROWFIND_AFTER_HIT  8u /* the search goes on from a hit: no zero-width hit where it starts */

int wpe_rowfind_row(const FIND *fd, const unsigned char *row, int row_len,
                    unsigned kind, int from, int to, unsigned how,
                    WpeRowHit *hit);
const char *wpe_rowfind_error(void);
void wpe_rowfind_release(void);

/* Where the last search left the cursor, so that the next one knows it goes
   on from a hit (zero-width hits are found once per place). */
void wpe_rowfind_text_place(int *y, int *x, int row_len, unsigned kind);
void wpe_rowfind_note_resume(int y, int x, int backward);
int  wpe_rowfind_resumes_at(int y, int x, int backward);
void wpe_rowfind_forget_resume(void);

#endif
