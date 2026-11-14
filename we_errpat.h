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
