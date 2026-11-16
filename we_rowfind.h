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

/* Whether the owner can run the engine under a UTF-8 ctype of its own
   (newlocale and uselocale, POSIX 2008; NetBSD has no uselocale).  0 or 1.
   The owner and its test read this one decision and never the HAVE_ names. */
#if defined(HAVE_NEWLOCALE) && defined(HAVE_USELOCALE)
#define WPE_ROWFIND_HAS_LOCALES 1
#else
#define WPE_ROWFIND_HAS_LOCALES 0
#endif

/* Whether this system's regular expression engine survives a word-boundary
   escape (\b \B \< \>) beside an alternation (WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP)
   and beside anything at all (WPE_HAS_REGEX_SAFE_BOUNDARY), as configure
   measured by running the shapes in a child: NetBSD 10's faults in mbrtowc on
   `\B(a|b)[ab]` over a row with a multibyte character.  Where the engine is
   not safe the owner refuses the expression and says so, so Find never hands
   it to the engine.  WPE_ROWFIND_UNSAFE_BOUNDARY_GROUP and
   WPE_ROWFIND_UNSAFE_BOUNDARY force the refusal (the second forces both, as
   a failed control of the probe does), so a test can build it on a system
   whose engine is safe.  The owner and its test read these two
   decisions and never the WPE_HAS_ names. */
#if WPE_HAS_REGEX_SAFE_BOUNDARY_GROUP && !defined(WPE_ROWFIND_UNSAFE_BOUNDARY_GROUP) && \
    !defined(WPE_ROWFIND_UNSAFE_BOUNDARY)
#define WPE_ROWFIND_BOUNDARY_GROUP_SAFE 1
#else
#define WPE_ROWFIND_BOUNDARY_GROUP_SAFE 0
#endif
#if WPE_HAS_REGEX_SAFE_BOUNDARY && !defined(WPE_ROWFIND_UNSAFE_BOUNDARY)
#define WPE_ROWFIND_BOUNDARY_SAFE 1
#else
#define WPE_ROWFIND_BOUNDARY_SAFE 0
#endif

/* What wpe_rowfind_refuses answers: 0 the text may go to the engine. */
#define WPE_ROWFIND_REFUSES_GROUP  1   /* an escape beside an alternation */
#define WPE_ROWFIND_REFUSES_ESCAPE 2   /* any word-boundary escape */

/* The option bits of FIND.sw that decide how a search is compiled. */
#define FIND_CASE_SENSITIVE 128
#define FIND_WHOLE_WORDS     64
#define FIND_REGULAR         32
#define FIND_COMPILE_BITS (FIND_CASE_SENSITIVE | FIND_WHOLE_WORDS | FIND_REGULAR)

/** WPE_ROWFIND_CFLAGS - The regcomp flags the owner compiles a search with.
 * @bits: FIND.sw (only the compile bits are read).
 *
 * Find with Case sensitive off keeps a regular expression's meaning, and a
 * regular expression gets the library's flag for the word escapes where
 * configure names one (REG_GNU on NetBSD, REG_ENHANCED on macOS).  The owner
 * compiles with this and so does anything that must compile the same
 * search, so that a test's reference cannot drift from what Find runs.
 * Needs <regex.h> and config.h. */
#define WPE_ROWFIND_CFLAGS(bits) \
 (REG_EXTENDED | (((bits) & FIND_CASE_SENSITIVE) ? 0 : REG_ICASE) | \
  (((bits) & FIND_REGULAR) ? WPE_REGEX_WORD_ESCAPES : 0))

/* What a row is in its line (a line longer than Max. Columns is several
   rows, and ^ and $ belong to the line, not to the row). */
#define WPE_ROW_STARTS_LINE  1u   /* the first row, or the row before ends with a line break */
#define WPE_ROW_ENDS_LINE    2u   /* the row ends with a line break, or is the last row */

/* How the search runs. */
#define WPE_ROWFIND_BACKWARD   4u /* the hit that starts last, with eo <= to */
#define WPE_ROWFIND_AFTER_HIT  8u /* the search goes on from a hit: no zero-width hit where it starts */

/* The caller's contract for @row: the byte at row[row_len] must exist and be
   readable.  The C library's engine may look at the byte behind the range it
   is given (NetBSD 10 does), and the owner never reads past it.  The editor's
   rows have one (the line break after them), Grep's line buffer a zero. */
int wpe_rowfind_row(const FIND *fd, const unsigned char *row, int row_len,
                    unsigned kind, int from, int to, unsigned how,
                    WpeRowHit *hit);
const char *wpe_rowfind_error(void);

/* Would the owner refuse this Find text for the engine's measured safety?
   0, WPE_ROWFIND_REFUSES_GROUP or WPE_ROWFIND_REFUSES_ESCAPE.  The test's
   reference asks the same function, so it cannot restate the rule.  The
   second form takes the two decisions, for a test that checks the rule on a
   system where both are safe. */
int wpe_rowfind_refuses(const char *text, unsigned sw);
int wpe_rowfind_refuses_when(const char *text, unsigned sw, int group_safe, int boundary_safe);
void wpe_rowfind_release(void);

/* Where the last search left the cursor, so that the next one knows it goes
   on from a hit (zero-width hits are found once per place). */
void wpe_rowfind_text_place(int *y, int *x, int row_len, unsigned kind);
void wpe_rowfind_note_resume(int y, int x, int backward);
int  wpe_rowfind_resumes_at(int y, int x, int backward);
void wpe_rowfind_forget_resume(void);

#endif
