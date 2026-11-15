/* we_errpat.h -- the one owner of a compiler's message pattern.
 *
 * A message pattern (the MessageString of a language) says which rows of
 * a compiler's output are errors and where in them the file, the line and
 * the column are.  This module reads the pattern text once into a checked
 * pattern.  A text the editor cannot read safely is refused with a reason
 * the user can act on; a text it reads in a surprising way is accepted with
 * a note saying how it was read.  Plain C over char, libc only, so a unit
 * test links it alone.
 */
#ifndef WE_ERRPAT_H
#define WE_ERRPAT_H

/** A message spans at most this many rows; rows after them are ignored. */
#define WPE_ERRPAT_ROWS 4
/** A diagnostic fits one status row. */
#define WPE_ERRPAT_DIAG 96
/** Room for the notes of one pattern, joined by "; ". */
#define WPE_ERRPAT_NOTES 256

typedef struct WpeErrPat WpeErrPat;

/**
 * wpe_errpat_compile - Read a MessageString into a checked pattern.
 * @text: the pattern as F9 sees it, rows joined by a line end.
 * @diag: where the reason goes when @text is refused.
 *
 * The user gains a reason instead of silence: a pattern cut inside a
 * ${...} token, a [...] class or a backslash is refused and the reason names
 * the token, so the fix is one edit in the Compiler dialog.  Any other text
 * is accepted; what the editor reads differently from what was written (an
 * unknown variable, rows past the fourth) is told by wpe_errpat_notes().
 * Return: a pattern to free with wpe_errpat_free(), or NULL with @diag set.
 */
WpeErrPat *wpe_errpat_compile(const char *text, char diag[WPE_ERRPAT_DIAG]);

/** Free a pattern; NULL is allowed. */
void wpe_errpat_free(WpeErrPat *p);

/**
 * wpe_errpat_names_file - Does the text carry ${FILE}?
 * Without it the file compiled is the file of every error row.
 */
int wpe_errpat_names_file(const WpeErrPat *p);

/** The number of rows the pattern has, 1..WPE_ERRPAT_ROWS. */
int wpe_errpat_row_count(const WpeErrPat *p);

/** The notes compile made, joined by "; ", or "" when there are none. */
const char *wpe_errpat_notes(const WpeErrPat *p);

/** How many notes compile made. */
int wpe_errpat_note_count(const WpeErrPat *p);

/**
 * wpe_errpat_row - Row @i of the pattern, its warning tests taken out.
 * Return: the row text, or NULL when @i is not a row.
 */
const char *wpe_errpat_row(const WpeErrPat *p, int i);

/** How many warning tests (${?N:text} and ${?*:text}) the pattern has. */
int wpe_errpat_marker_count(const WpeErrPat *p);

/** The text of warning test @i, or NULL. */
const char *wpe_errpat_marker_text(const WpeErrPat *p, int i);

/** The column warning test @i must start at, -1 for anywhere in the row. */
int wpe_errpat_marker_column(const WpeErrPat *p, int i);

/**
 * wpe_errpat_literal_end - Where the literal run that starts at @n ends.
 * @a: a pattern row, terminated;
 * @n: where the scan starts, 0 <= @n <= strlen(@a).  A piece of a pattern
 * starts its scan after its own first byte, so a backslash that is the first
 * byte of a piece escapes nothing: "\\" at the start of a row is a backslash
 * that takes the next one with it and then the terminator;
 * @cut: set to 1 when the run ends on a backslash with nothing after it.
 *
 * The one scan that decides where a run of plain bytes stops: at an
 * unescaped '*', '?', '[' or "${".  Compile uses it to find every shape
 * the matcher would run off the end of a row on, and the matcher uses the
 * same scan, so the two cannot disagree.
 * Return: the index of the byte that stops the run (the terminator when the
 * row ends first).
 */
int wpe_errpat_literal_end(const char *a, int n, int *cut);

/** What an entry of the error list is: an error stops the build, a warning
 * is counted and marked. */
#define WPE_ERRPAT_ERROR 0
#define WPE_ERRPAT_WARNING 1

/** A Messages row as the buffer holds it: @s the text and its line end,
 * terminated; @len the length of the text without the line end. */
typedef struct
{
  const char *s;
  int len;
} WpeErrRow;

/**
 * One entry the pattern made.  @file and @srch are the caller's to free.
 * @row is the first Messages row of the message, @text_row the row whose text
 * and kind the entry shows (the last row of a message that spans several).
 * @file is the whole text ${FILE} delimited; the caller decides which part
 * of it names a file.  @x is the column, 0 when the pattern names none.
 * @srch is the text the entry looks for in the source line, or NULL.
 * @kind is WPE_ERRPAT_ERROR or WPE_ERRPAT_WARNING.  @stops is 1 when the
 * entry fails the build, 0 for a warning the build survives.
 */
typedef struct
{
  int row, text_row, line, x, kind, stops;
  char *file, *srch;
} WpeErrHit;

typedef struct WpeErrWalk WpeErrWalk;

/**
 * wpe_errpat_walk_begin - Begin one pass over the Messages rows.
 * @p: a compiled pattern, which must outlive the walk;
 * @rows, @n: the rows of Messages, as the buffer holds them;
 * @source: the file compiled, which is the file of every entry when the
 * pattern names none (may be NULL);
 * @stop_at_warnings: 1 when a warning must fail the build too.
 *
 * Sizes the scratch once from the longest row, so one pass over a long
 * build output allocates once, however long a path the compiler printed.
 * Return: the walk, or NULL when memory is short.
 */
WpeErrWalk *wpe_errpat_walk_begin(const WpeErrPat *p, const WpeErrRow *rows,
                                  int n, const char *source,
                                  int stop_at_warnings);

/**
 * wpe_errpat_walk_from - Match the message that starts at row @start.
 * @hits: room for at least n - @start entries;
 * @nhits: out, how many entries were written.
 *
 * The user gains one entry for each error the compiler printed, with its
 * file, line and column, and the walk never stands still: the return value
 * is the last row the message used, always >= @start and < n, so the next
 * message is looked for at the row after it.  A message that runs out of
 * rows, or a row that costs more than WPE_ERRPAT_BUDGET steps to match,
 * simply does not match.
 * Return: the last row used.
 */
int wpe_errpat_walk_from(WpeErrWalk *w, int start, WpeErrHit *hits, int *nhits);

/**
 * wpe_errpat_walk_costly_row - The first Messages row that was too costly.
 * Return: its index, or -1 when every row was matched within the budget.
 */
int wpe_errpat_walk_costly_row(const WpeErrWalk *w);

/** The most steps any row has taken so far (for tests that size the budget). */
long wpe_errpat_walk_peak_steps(const WpeErrWalk *w);

/** Free a walk; NULL is allowed. */
void wpe_errpat_walk_end(WpeErrWalk *w);

/**
 * WPE_ERRPAT_BUDGET - Steps one row may take to match before it counts as
 * not matching.  A pattern that backtracks without end therefore costs the
 * user one unrecognised row, not a frozen editor.  The built-in patterns
 * take under 300,000 steps on the longest real compiler rows measured, and
 * under 100 on ordinary ones.
 */
#define WPE_ERRPAT_BUDGET 1000000L

/**
 * WPE_ERRPAT_DEPTH - How many candidate positions of a '*' one row may nest
 * before it counts as not matching, exactly as if it had spent the budget.
 * The matcher tries a '*' at each position of the row in turn, one call
 * deeper each time, so a single row of more than about 10,000 repeated
 * separators (about 20 KB of text) is not recognised: no entry is made from
 * it and wpe_errpat_walk_costly_row() names it.  Without the cap such a row
 * matched below about 60,000 repeats and crashed the editor above.
 */
#define WPE_ERRPAT_DEPTH 10000

/**
 * wpe_errpat_unchecked - Cut @text into rows and refuse nothing.
 *
 * For tests only: it gives the matcher a pattern that compile would have
 * refused, to prove the matcher alone never reads outside a row.  A pattern
 * cut in a warning test ends at the cut.
 * Return: a pattern to free with wpe_errpat_free(), or NULL when memory is
 * short.
 */
WpeErrPat *wpe_errpat_unchecked(const char *text);

/**
 * wpe_errpat_file_token - The text a ${FILE} token delimited.
 * @out: room for strlen(@s) + 1 bytes;
 * @s: the text between the pattern's delimiters.
 *
 * The whole token is kept, blanks included, so a row naming "my file.c"
 * jumps into that file.  Only the blanks around the token and the quotes
 * some compilers put around a name are taken off.
 */
void wpe_errpat_file_token(char *out, const char *s);

#endif
