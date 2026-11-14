/* we_number.h -- the one owner of a number the user or a file gives the
   editor.

   Each number has a type: its name as a dialog shows it, its key in xwperc,
   the range the editor can work with, its default, and whether a field
   saturates a value that is too large.  The editor divides by Tabstops, sizes
   every row with Max. Columns and reallocates with Auto Ind. Col., so a value
   outside the range is a crash or lost text, never a preference.  Nobody but
   we_number.c parses the digits of an option, writes one of the five editor
   numbers, or says that a value was refused.

   This part is plain C: a unit test uses the table and the parser without the
   editor.  The editor's side (the store, the defaults, the field's accept)
   is declared in edit.h. */
#ifndef WE_NUMBER_H
#define WE_NUMBER_H

#include <stddef.h>

#ifdef __GNUC__
#define WPE_NUMBER_RESULT __attribute__((warn_unused_result))
#else
#define WPE_NUMBER_RESULT
#endif

typedef enum {
 WPE_NUM_MAXCOL, WPE_NUM_TAB, WPE_NUM_MAXCHG, WPE_NUM_NUMUNDO,
 WPE_NUM_AUTOINDENT, WPE_NUM_LINE, WPE_NUM_KINDS
} WpeNumberKind;

#define WPE_NUM_REFUSE   0   /* a typed value above max is refused and said */
#define WPE_NUM_SATURATE 1   /* a typed value above max becomes max (Go to Line) */

/* Room for the digits of the widest field (an int has ten) and the end. */
#define WPE_NUM_TEXT_SIZE 16

typedef struct {
 WpeNumberKind kind;
 const char *label;    /* as the dialog names it: "Tabstops" */
 const char *key;      /* as xwperc writes it: "Tab"; NULL when no file carries it */
 int min, max;         /* inclusive; max also sets the field's places */
 int fallback;         /* the built-in default */
 int above_max;        /* WPE_NUM_REFUSE or WPE_NUM_SATURATE, for the field only */
} WpeNumberType;

extern const WpeNumberType WPE_NUM_TYPES[WPE_NUM_KINDS];   /* we_number.c, the one table */
#define WPE_NUM(k) (&WPE_NUM_TYPES[k])

#define WPE_NUMBER_OK            0
#define WPE_NUMBER_NOT_A_NUMBER  1   /* no digit, or text after the digits */
#define WPE_NUMBER_RANGE         2   /* a whole number outside [min, max], overflow included */

/**
 * wpe_number_parse - Read a whole text as one number of a type.
 * @text: blanks, an optional '-', one or more digits, blanks, the end.
 * @t: the type whose range the number must be in.
 * @out: written only when the answer is WPE_NUMBER_OK.
 *
 * Anything else is WPE_NUMBER_NOT_A_NUMBER, so "8x" is not 8 and "" is not 0.
 * A number that does not fit an int, or is outside the type's range, is
 * WPE_NUMBER_RANGE.  Saturation is not decided here: a value in a file is
 * never cut to fit.
 */
int wpe_number_parse(const char *text, const WpeNumberType *t, int *out) WPE_NUMBER_RESULT;

/**
 * wpe_number_range_words - The sentence that says what a number may be.
 * @buf: where to write it; @n: its size.
 * @t: the type; @text: the text that was refused, or NULL.
 *
 * " Tabstops: 1 to 99 (not 0)": the one sentence every refusal uses, so the
 * user reads the range where he typed the number.  "(not 0)" is added when
 * the refused text was zero.
 */
void wpe_number_range_words(char *buf, size_t n, const WpeNumberType *t, const char *text);

#endif
