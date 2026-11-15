/* we_number.c                                            */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */
/*
   The one owner of a number the user or a file gives the editor: the table
   of the numbers' types, the parser that reads a text whole, the one writer
   of the five editor numbers, and the one way a dialog field's digits become
   its value.  See we_number.h.
*/

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit.h"

/* Width of the sentence that names a range: label, two numbers, the rest. */
#define E_NUMBER_WORDS_MAX 96

/* The start-up line about numbers the option file gave and the editor did not
   take: how much of a file's text it shows, how it starts, how its parts are
   joined, and room for it with the line it is joined to. */
#define E_NUMBER_SHOWN_MAX 24
#define E_NUMBER_LINE_LEAD "xwperc: "
#define E_NUMBER_LINE_JOIN "; "
#define E_NUMBER_SAY_MAX   1536

/* The compact form for a narrow row: what introduces the keys alone, after
   a line already held, and when even the first does not fit; how the keys
   are joined; how much of a value shows and what marks the cut. */
#define E_NUMBER_COMPACT_LEAD "xwperc: not used, defaults kept, file unchanged: "
#define E_NUMBER_SHORT_LEAD   "xwperc not used: "
#define E_NUMBER_JOIN_LEAD    "not used: "
#define E_NUMBER_COUNT_LEAD   "xwperc: "
#define E_NUMBER_COMPACT_JOIN ", "
#define E_NUMBER_COMPACT_VALUE_MAX 8
#define E_NUMBER_CUT_MARK     "..."

/*
   The one table.  The range is what the editor can work with: Tabstops is a
   divisor, Max. Columns sizes every row (maxcol + 2 bytes, so 9999 costs
   1 GB for a 100 000 line file), Max. Changes is added to one, Auto Ind.
   Col. is a count of blanks, and Go to Line is a row number of an int.
*/
const WpeNumberType WPE_NUM_TYPES[WPE_NUM_KINDS] = {
 { WPE_NUM_MAXCOL,     "Max. Columns",     "MaxColumn",  4, 9999, MAXCOLUM, WPE_NUM_REFUSE },
 { WPE_NUM_TAB,        "Tabstops",         "Tab",        1,   99,        8, WPE_NUM_REFUSE },
 { WPE_NUM_MAXCHG,     "MaX. Changes",     "MaxChanges", 0,  999,      999, WPE_NUM_REFUSE },
 { WPE_NUM_NUMUNDO,    "Num. Undo",        "NumUndo",    0,   99,       10, WPE_NUM_REFUSE },
 { WPE_NUM_AUTOINDENT, "Auto Ind. Col.",   "AutoIndent", 0,   99,        3, WPE_NUM_REFUSE },
 { WPE_NUM_LINE,       "Goto Line Number", NULL,         0, INT_MAX,     0, WPE_NUM_SATURATE },
};

/**
 * e_number_is_blank - Is @c a blank a number may have around its digits?
 *
 * A carriage return counts: a file saved with DOS line ends gives
 * "Tab : 4" a CR behind the 4, and the number is still 4.  The reader keeps
 * the CR for text options, so the number owns this case.
 */
static int e_number_is_blank(char c)
{
 return c == ' ' || c == '\t' || c == '\r';
}

/**
 * e_number_is_digit - Is @c one of 0 to 9?  The locale has no say in it.
 */
static int e_number_is_digit(char c)
{
 return c >= '0' && c <= '9';
}

/**
 * e_number_scan - Find where the number of a text starts, if the whole text
 * is one number.
 * @text: the text to read.
 *
 * Return: the position of the sign or first digit, or NULL when the text is
 * not blanks, an optional '-', digits and blanks.
 */
static const char *e_number_scan(const char *text)
{
 const char *p = text;
 const char *start;

 if (!text)
  return NULL;
 while (e_number_is_blank(*p))
  p++;
 start = p;
 if (*p == '-')
  p++;
 if (!e_number_is_digit(*p))
  return NULL;
 while (e_number_is_digit(*p))
  p++;
 while (e_number_is_blank(*p))
  p++;
 return *p == '\0' ? start : NULL;
}

/**
 * e_number_text_is_negative - Does the number in @text carry a minus sign?
 *
 * A field types digits only, so this is false for every field's text; the
 * saturation of a too large value must never turn a negative one into max.
 */
static int e_number_text_is_negative(const char *text)
{
 const char *start = e_number_scan(text);

 return start != NULL && *start == '-';
}

int wpe_number_parse(const char *text, const WpeNumberType *t, int *out)
{
 const char *start = e_number_scan(text);
 long value;

 if (!start)
  return WPE_NUMBER_NOT_A_NUMBER;
 errno = 0;
 value = strtol(start, NULL, 10);
 if (errno == ERANGE || value < INT_MIN || value > INT_MAX)
  return WPE_NUMBER_RANGE;
 if (value < t->min || value > t->max)
  return WPE_NUMBER_RANGE;
 *out = (int)value;
 return WPE_NUMBER_OK;
}

/**
 * e_number_text_is_zero - Is @text the number 0, however many zeros?
 */
static int e_number_text_is_zero(const char *text)
{
 const char *start = e_number_scan(text);

 if (!start)
  return 0;
 if (*start == '-')
  start++;
 while (*start == '0')
  start++;
 return e_number_is_blank(*start) || *start == '\0';
}

void wpe_number_range_words(char *buf, size_t n, const WpeNumberType *t, const char *text)
{
 snprintf(buf, n, " %s: %d to %d%s", t->label, t->min, t->max,
          e_number_text_is_zero(text) ? " (not 0)" : "");
}

/**
 * e_number_slot - Where the editor keeps the number of a type.
 * @cn: the editor's state; @t: the type.
 *
 * The one place that knows which field of the editor's state a kind lives
 * in.  Return: the field, or NULL for a kind that has none (Go to Line).
 */
static int *e_number_slot(ECNT *cn, const WpeNumberType *t)
{
 switch (t->kind)
 {
  case WPE_NUM_MAXCOL:     return &cn->maxcol;
  case WPE_NUM_TAB:        return &cn->tabn;
  case WPE_NUM_MAXCHG:     return &cn->maxchg;
  case WPE_NUM_NUMUNDO:    return &cn->numundo;
  case WPE_NUM_AUTOINDENT: return &cn->autoindent;
  default:                 return NULL;
 }
}

/* What the option file gave for a kind and the owner did not take: the file's
   own text, kept so that a save writes it back unchanged until the user
   stores another number for that kind; and whether the start-up line still
   has to name it. */
static char *e_number_refused_text[WPE_NUM_KINDS];
static int e_number_refused_noted[WPE_NUM_KINDS];

/**
 * e_number_forget_refused - Drop the file text kept for a kind.
 * @t: the type.
 */
static void e_number_forget_refused(const WpeNumberType *t)
{
 WpeFree(e_number_refused_text[t->kind]);
 e_number_refused_text[t->kind] = NULL;
 e_number_refused_noted[t->kind] = 0;
}

/**
 * e_number_defaults - Give the editor its five numbers before any file is read.
 * @cn: the editor's state.
 *
 * Every number starts at its default, inside its range, so a start with no
 * option file runs on numbers the editor can divide by and size rows with.
 * No refused file text is kept from before.
 */
void e_number_defaults(ECNT *cn)
{
 int k;

 for (k = 0; k < WPE_NUM_KINDS; k++)
 {
  int *slot = e_number_slot(cn, WPE_NUM(k));

  e_number_forget_refused(WPE_NUM(k));
  if (slot)
   *slot = WPE_NUM(k)->fallback;
 }
}

/**
 * e_number_store - The one writer of the five editor numbers.
 * @cn: the editor's state; @t: the type of the number; @value: the new value.
 *
 * Options > Editor and Beautify end here, so the editor never holds a number
 * outside its range: a value outside it is refused whatever the caller
 * believed.  A value equal to the one in force is left alone and is not a
 * refusal, so Ok on a box whose number came from a file the owner would not
 * take does not raise an error for a number the user never touched.  A new
 * value is the user's own choice: the text the option file held for that
 * number, and the editor refused, is no longer kept for Save to write back.
 * Return: 0, or -1 when the value is refused or the type has no slot.
 */
int e_number_store(ECNT *cn, const WpeNumberType *t, int value)
{
 int *slot = e_number_slot(cn, t);

 if (!slot)
  return -1;
 if (value == *slot)
  return 0;            /* what is in force is kept, whatever its range */
 if (value < t->min || value > t->max)
  return -1;
 *slot = value;
 e_number_forget_refused(t);
 return 0;
}

/**
 * e_number_note_refused - Keep what the option file gave for a number it may not have.
 * @t: the type; @text: the file's text for it.
 *
 * Only the latest file read counts for a number, as it does for a good one.
 */
static void e_number_note_refused(const WpeNumberType *t, const char *text)
{
 size_t len;

 e_number_forget_refused(t);
 e_number_refused_text[t->kind] = WpeStrdup(text);
 len = strlen(e_number_refused_text[t->kind]);
 while (len > 0 && e_number_refused_text[t->kind][len - 1] == '\r')
  e_number_refused_text[t->kind][--len] = '\0';   /* a DOS line end is not the user's value */
 e_number_refused_noted[t->kind] = 1;
}

/**
 * e_number_load - Take a number of the option file.
 * @cn: the editor's state; @t: the type of the key; @value: the file's text.
 *
 * A number the editor can work with is used.  Any other (Tab : 0, a text,
 * more digits than an int holds) leaves the number in force as it is, so a
 * mistake in the file cannot stop the editor, and the file's text is kept:
 * the start-up line names it (e_number_say_refused) and Save writes it back
 * as it was (e_number_write), so the file is never rewritten behind the user.
 * Return: 0 when the number was used, -1 when it was refused.
 */
int e_number_load(ECNT *cn, const WpeNumberType *t, const char *value)
{
 int *slot = e_number_slot(cn, t);
 int number;

 if (!slot || wpe_number_parse(value, t, &number) != WPE_NUMBER_OK)
 {
  e_number_note_refused(t, value);
  return -1;
 }
 *slot = number;
 e_number_forget_refused(t);
 return 0;
}

/**
 * e_number_write - Write one of the five numbers of the option file.
 * @cn: the editor's state; @t: the type; @fp: the option file being written.
 *
 * The one writer of these keys, for Save, the save at quit and the editor's
 * own saves alike.  While the file's own text for a number is kept, because
 * the editor refused it and the user has not chosen another, that text is
 * written back, so what the user wrote is not replaced by a default he never
 * asked for.  Otherwise the number in force is written.
 */
void e_number_write(ECNT *cn, const WpeNumberType *t, FILE *fp)
{
 int *slot = e_number_slot(cn, t);

 if (!t->key || !slot)
  return;
 if (e_number_refused_text[t->kind])
  fprintf(fp, "%s : %s\n", t->key, e_number_refused_text[t->kind]);
 else
  fprintf(fp, "%s : %d\n", t->key, *slot);
}

/**
 * e_number_refusal_words - The words about one refused number, for the start-up line.
 * @buf: where to write them; @n: its size.
 * @cn: the editor's state; @t: the type.
 *
 * "Tab : 0 is not 1 to 99, 8 used (the file keeps 0)": what the file said,
 * what it may be, what the editor uses instead, and that the file is left
 * alone.  A long file text is cut so the line stays one line.
 */
static void e_number_refusal_words(char *buf, size_t n, ECNT *cn, const WpeNumberType *t)
{
 const char *text = e_number_refused_text[t->kind];

 snprintf(buf, n, "%s : %.*s is not %d to %d, %d used (the file keeps %.*s)",
          t->key, E_NUMBER_SHOWN_MAX, text, t->min, t->max,
          *e_number_slot(cn, t), E_NUMBER_SHOWN_MAX, text);
}

/**
 * e_number_refusals_line - Add the words about every number the file refused.
 * @line: the line so far (what is already held on the bottom row, or "");
 * @n: its size; @cn: the editor's state.
 */
static void e_number_refusals_line(char *line, size_t n, ECNT *cn)
{
 char words[E_NUMBER_WORDS_MAX + 2 * E_NUMBER_SHOWN_MAX];
 const char *lead = E_NUMBER_LINE_LEAD;
 int k;

 for (k = 0; k < WPE_NUM_KINDS; k++)
 {
  size_t used = strlen(line);

  if (!e_number_refused_noted[k])
   continue;
  e_number_refusal_words(words, sizeof words, cn, WPE_NUM(k));
  snprintf(line + used, n - used, "%s%s%s", used ? E_NUMBER_LINE_JOIN : "", lead, words);
  lead = "";
 }
}

/**
 * e_number_short_value - A file's text for a number, cut so a key name shows.
 * @buf: where to write it; @n: its size; @text: the refused text.
 *
 * A 60 digit value would push the key names out of an 80 column row; its
 * first digits tell the user which value it was.
 */
static void e_number_short_value(char *buf, size_t n, const char *text)
{
 if (strlen(text) <= E_NUMBER_COMPACT_VALUE_MAX)
  snprintf(buf, n, "%s", text);
 else
  snprintf(buf, n, "%.*s%s", E_NUMBER_COMPACT_VALUE_MAX, text, E_NUMBER_CUT_MARK);
}

/** e_number_noted_count - How many numbers of the file the start-up line has to name. */
static int e_number_noted_count(void)
{
 int k, count = 0;

 for (k = 0; k < WPE_NUM_KINDS; k++)
  count += e_number_refused_noted[k] != 0;
 return count;
}

/**
 * e_number_compact_line - Add every refused key, once, with its value or not.
 * @line: the line so far; @n: its size; @lead: what introduces the keys;
 * @with_values: 1 to give each key its value, short; @limit: how many keys
 * to name, the others being counted ("+2").
 *
 * "xwperc not used: MaxColumn 0, NumUndo 100", or with no values
 * "xwperc not used: MaxColumn, NumUndo", or "xwperc not used: MaxColumn +1":
 * for a row too narrow for a sentence per number.  A key name is never cut.
 */
static void e_number_compact_line(char *line, size_t n, const char *lead, int with_values,
                                  int limit)
{
 char value[E_NUMBER_COMPACT_VALUE_MAX + sizeof E_NUMBER_CUT_MARK];
 int k, shown = 0, total = e_number_noted_count();

 snprintf(line + strlen(line), n - strlen(line), "%s", lead);
 for (k = 0; k < WPE_NUM_KINDS && shown < limit; k++)
 {
  if (!e_number_refused_noted[k])
   continue;
  snprintf(line + strlen(line), n - strlen(line), "%s%s",
           shown ? E_NUMBER_COMPACT_JOIN : "", WPE_NUM(k)->key);
  if (with_values)
  {
   e_number_short_value(value, sizeof value, e_number_refused_text[k]);
   snprintf(line + strlen(line), n - strlen(line), " %s", value);
  }
  shown++;
 }
 if (shown < total)
  snprintf(line + strlen(line), n - strlen(line), " +%d", total - shown);
}

/**
 * e_number_count_line - Say how many numbers were not used, without naming them.
 * @line: receives it; @n: its size; @lead: what introduces it ("" or "xwperc: ").
 *
 * The last resort of a row too narrow for even one whole key name.
 */
static void e_number_count_line(char *line, size_t n, const char *lead)
{
 int total = e_number_noted_count();

 snprintf(line, n, "%s%d %s not used", lead, total, total == 1 ? "number" : "numbers");
}

/**
 * e_number_candidate - One form of the numbers part, the longest first.
 * @line: receives it; @n: its size; @cn: the editor's state; @joined: 1 when
 * it follows the keys line; @step: 0 sentences, 1 and 2 keys with values,
 * then the keys alone with one key fewer each step, then the count.
 * Return: 0 when @step is past the last form.
 */
static int e_number_candidate(char *line, size_t n, ECNT *cn, int joined, int step)
{
 int total = e_number_noted_count();
 const char *lead = joined ? E_NUMBER_JOIN_LEAD : E_NUMBER_COMPACT_LEAD;

 line[0] = '\0';
 if (step == 0)
  e_number_refusals_line(line, n, cn);
 else if (step == 1)
  e_number_compact_line(line, n, lead, 1, total);
 else if (step == 2)
  e_number_compact_line(line, n, joined ? E_NUMBER_JOIN_LEAD : E_NUMBER_SHORT_LEAD, 1, total);
 else if (step < 3 + total)
  e_number_compact_line(line, n, joined ? E_NUMBER_JOIN_LEAD : E_NUMBER_SHORT_LEAD, 0,
                        total - (step - 3));
 else if (step == 3 + total)
  e_number_count_line(line, n, joined ? "" : E_NUMBER_COUNT_LEAD);
 else
  return 0;
 return 1;
}

/**
 * e_number_join_after_keys - Put the numbers part behind the keys line when both fit.
 * @line: receives the whole line; @n: its size; @numbers: the numbers part;
 * @room: the row's columns.
 *
 * The keys line is owned by the option reader, which gives its long sentence
 * when it fits and its short one when it must.  Return: 1 when both fit.
 */
static int e_number_join_after_keys(char *line, size_t n, const char *numbers, int room)
{
 char keys[E_NUMBER_SAY_MAX];
 int avail = room - (int)strlen(E_NUMBER_LINE_JOIN) - (int)strlen(numbers);
 int kind;

 if (avail <= 0)
  return 0;
 kind = e_opt_ignored_line(keys, sizeof keys, avail);
 if (kind != E_OPT_IGNORED_LONG && kind != E_OPT_IGNORED_SHORT)
  return 0;
 snprintf(line, n, "%s%s%s", keys, E_NUMBER_LINE_JOIN, numbers);
 return 1;
}

/**
 * e_number_fit_line - Choose the longest form of the line that fits the row.
 * @line: receives it; @n: its size; @cn: the editor's state.
 *
 * The numbers part is the whole sentence for each number when it fits, else
 * the keys with their values, else the keys alone, else as many whole keys
 * as fit and "+N" for the rest, else only how many.  When a project's
 * xwperc also named keys it may not set, that line comes first: its long
 * sentence if both fit, else its short one.  When not even the count fits
 * behind it, the numbers part is left out and the keys line is cut from the
 * right: its head is never cut, and no key name of a number is cut either.
 */
static void e_number_fit_line(char *line, size_t n, ECNT *cn)
{
 char numbers[E_NUMBER_SAY_MAX];
 int room = e_status_room();
 int joined = e_opt_ignored_line(line, n, room) != E_OPT_IGNORED_NONE;
 int step;

 for (step = 0; e_number_candidate(numbers, sizeof numbers, cn, joined, step); step++)
 {
  if (!joined && (int)strlen(numbers) <= room)
  {
   snprintf(line, n, "%s", numbers);
   return;
  }
  if (joined && e_number_join_after_keys(line, n, numbers, room))
   return;
 }
 if (!joined)
  e_number_count_line(line, n, E_NUMBER_COUNT_LEAD);   /* nothing fits: the row cuts it */
}

/**
 * e_number_say_refused - Tell the user which numbers of the option file were not used.
 * @cn: the editor's state.
 *
 * At start, a number the file gives that the editor cannot work with is not
 * used, and without a word the user would not know why the editor differs
 * from his file.  One line names each, joined to the line about keys a
 * project's file may not set when there is one, and stays on the bottom row
 * until the next key.  A narrow screen gets the compact form, which still
 * names every key.
 */
void e_number_say_refused(ECNT *cn)
{
 char line[E_NUMBER_SAY_MAX];

 if (e_number_noted_count() == 0 || !cn || !cn->f[0])
  return;
 e_number_fit_line(line, sizeof line, cn);
 e_status_say_until_key(cn->f[0], line);
}

/**
 * e_number_field_init - Make a dialog field hold a number of a type.
 * @n: the field; @type: its type; @value: the number it starts with.
 *
 * The field's places come from the type's largest number, so a box shows
 * every number the editor accepts for it whole.
 */
void e_number_field_init(W_O_NUMSTR *n, const WpeNumberType *type, int value)
{
 n->type = type;
 n->nw = WpeNumberOfPlaces(type->max);
 n->num = value;
}

/**
 * e_number_say_range - Say on the status row what a field's number may be.
 * @f: the window the dialog was opened from; @t: the field's type;
 * @text: what the user typed.
 *
 * The user typed a number the editor cannot use; without a word the field
 * would just not let go of the focus.
 */
static void e_number_say_range(FENSTER *f, const WpeNumberType *t, const char *text)
{
 char words[E_NUMBER_WORDS_MAX];

 if (!f)
  return;
 wpe_number_range_words(words, sizeof words, t, text);
 e_status_say(f, words);
}

/**
 * e_number_saturate - Make a field hold the largest number its type allows.
 * @n: the field; @text: its digits, rewritten to match.
 *
 * Go to Line takes any run of digits and a number past the end goes to the
 * last line, so a run too long for an int is the largest int, shown as such.
 */
static void e_number_saturate(W_O_NUMSTR *n, char *text)
{
 n->num = n->type->max;
 WpeNumberToString(n->num, n->nw, text);
}

/**
 * e_number_field_accept - The one way a field's digits become its number.
 * @n: the field; @text: the digits the user typed, rewritten when saturated.
 * @f: the window the dialog was opened from, for the status row.
 *
 * A number in the field's range is taken.  One that is too large for a
 * field that saturates (Go to Line) becomes the largest number, shown as
 * such.  Any other is refused: the range is said on the bottom row and the
 * field keeps the last number it held, so the box can never be closed on a
 * number the editor cannot use.
 * Return: 0 when the field holds the number, -1 when it was refused.
 */
int e_number_field_accept(W_O_NUMSTR *n, char *text, FENSTER *f)
{
 int value;
 int rc = wpe_number_parse(text, n->type, &value);

 if (rc == WPE_NUMBER_OK)
 {
  n->num = value;
  return 0;
 }
 if (rc == WPE_NUMBER_RANGE && n->type->above_max == WPE_NUM_SATURATE &&
     !e_number_text_is_negative(text))
 {
  e_number_saturate(n, text);
  return 0;
 }
 e_number_say_range(f, n->type, text);
 return -1;
}
