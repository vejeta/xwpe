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
 */
static int e_number_is_blank(char c)
{
 return c == ' ' || c == '\t';
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

/**
 * e_number_defaults - Give the editor its five numbers before any file is read.
 * @cn: the editor's state.
 *
 * Every number starts at its default, inside its range, so a start with no
 * option file runs on numbers the editor can divide by and size rows with.
 */
void e_number_defaults(ECNT *cn)
{
 int k;

 for (k = 0; k < WPE_NUM_KINDS; k++)
 {
  int *slot = e_number_slot(cn, WPE_NUM(k));

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
 * take does not raise an error for a number the user never touched.
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
 return 0;
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
