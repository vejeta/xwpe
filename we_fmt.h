/* we_fmt.h -- the one owner of a formatted text written into storage.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Every text the editor builds with a format (a status row, a command for the
   debugger, a request for a server, a line and column) goes through here, so
   one place decides what happens when the text does not fit its place:

     whole   A number, a path, a command, a key or a protocol line.  A text
             that does not fit is not written at all: the storage is "" and
             the call says WPE_FMT_NO_ROOM, so a command is never run cut
             and a file is never opened by a prefix of its name.
     shown   A sentence a person reads (a status row, a message, a label).
             A text that does not fit is cut where a character ends and ends
             in "...", so the reader sees it was cut and never a half letter.
     block   A piece of a request or a prompt with a budget.  It is added
             whole or not at all; it never carries a mark into a request and
             the next block still gets its turn.
     heap    A text with no known bound.  It is as long as it is.

   The module is plain C, the C library and the UTF-8 rule of we_utf8.h, so a
   unit links it alone.  It is the only file of the editor that calls
   vsnprintf. */

#ifndef WE_FMT_H
#define WE_FMT_H

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>

#define WPE_FMT_OK          0
/* A whole text did not fit: the storage holds "". */
#define WPE_FMT_NO_ROOM   (-1)
/* The C library refused the format or its values: the storage holds "". */
#define WPE_FMT_FAILED    (-2)
/* A heap text could not grow. */
#define WPE_FMT_NO_MEMORY (-3)

/* The one mark of a cut sentence. */
#define WPE_FMT_CUT_MARK "..."

/* The most bytes a formatted text may take: one system's vsnprintf refuses
   more than an int holds, so every system gets the same limit.  A unit may
   lower it with -DWPE_FMT_HEAP_LIMIT=n to reach the refusal. */
#ifndef WPE_FMT_HEAP_LIMIT
#define WPE_FMT_HEAP_LIMIT ((size_t) INT_MAX)
#endif

/* The bytes the decimal text of any int, long or size_t takes, sign
   included, without the NUL. */
#define WPE_FMT_DIGITS(type) ((sizeof(type) * CHAR_BIT - 1) * 302 / 1000 + 2)
#define WPE_FMT_INT_ROOM   WPE_FMT_DIGITS(int)
#define WPE_FMT_LONG_ROOM  WPE_FMT_DIGITS(long)
#define WPE_FMT_SIZE_ROOM  WPE_FMT_DIGITS(size_t)

#if defined(__GNUC__)
#define WPE_FMT_PRINTF(fmt_at, args_at) \
  __attribute__((format(printf, fmt_at, args_at)))
/* A result the caller must read: a refused text is never used as if it
   were whole. */
#define WPE_FMT_MUST_READ __attribute__((warn_unused_result))
/* The size of an array, and a compile error for a pointer. */
#define WPE_FMT_ARRAY_SIZE(arr) \
  (sizeof(arr) + 0 * sizeof(char[1 - 2 * \
   __builtin_types_compatible_p(__typeof__(arr), __typeof__(&(arr)[0]))]))
#else
#define WPE_FMT_PRINTF(fmt_at, args_at)
#define WPE_FMT_MUST_READ
#define WPE_FMT_ARRAY_SIZE(arr) sizeof(arr)
#endif

/**
 * wpe_fmt_whole - Write a text that must be whole into @out of @n bytes.
 * Used for a command, a path, a key or a protocol line: when it does not fit
 * nothing is written, @out is "" and the caller refuses the action with its
 * own sentence, instead of acting on a prefix.
 * Return: WPE_FMT_OK, WPE_FMT_NO_ROOM or WPE_FMT_FAILED.
 */
int wpe_fmt_whole(char *out, size_t n, const char *fmt, ...)
  WPE_FMT_PRINTF(3, 4) WPE_FMT_MUST_READ;
int wpe_fmt_vwhole(char *out, size_t n, const char *fmt, va_list ap)
  WPE_FMT_MUST_READ;

/**
 * wpe_fmt_shown - Write a sentence a person reads into @out of @n bytes.
 * A sentence that does not fit is cut where a character ends and ends in
 * "...", so the reader never sees half a letter and can tell it was cut.
 * Return: 0 when whole, 1 when cut and marked, WPE_FMT_FAILED.
 */
int wpe_fmt_shown(char *out, size_t n, const char *fmt, ...)
  WPE_FMT_PRINTF(3, 4);
int wpe_fmt_vshown(char *out, size_t n, const char *fmt, va_list ap);

/**
 * wpe_fmt_shown_tail - Write the end of @s into @out of @n bytes when all of
 * it does not fit: "..." and then the last characters, the first byte of the
 * first one a character start.  Used for a path the reader needs the file
 * name of.  A text that fits is written whole.
 * Return: 0 when whole, 1 when cut.
 */
int wpe_fmt_shown_tail(char *out, size_t n, const char *s);

/**
 * wpe_fmt_dup - A text of no known bound, in memory of its own (free it).
 * Return: the text, or NULL when memory or the format failed.
 */
char *wpe_fmt_dup(const char *fmt, ...) WPE_FMT_PRINTF(1, 2);
char *wpe_fmt_vdup(const char *fmt, va_list ap);

/* The array faces: the size is the array's own, and a pointer given where an
   array was meant does not compile. */
#define WPE_FMT_WHOLE(arr, ...) \
  wpe_fmt_whole((arr), WPE_FMT_ARRAY_SIZE(arr), __VA_ARGS__)
#define WPE_FMT_SHOWN(arr, ...) \
  wpe_fmt_shown((arr), WPE_FMT_ARRAY_SIZE(arr), __VA_ARGS__)

/* A text built from pieces. */
typedef struct
{
 char *out;
 size_t n;
 size_t len;
 int kind;
 int state;
} WpeFmtText;

/**
 * wpe_fmt_begin_whole, wpe_fmt_begin_shown, wpe_fmt_begin_block,
 * wpe_fmt_begin_heap - Start a text built from pieces in @out of @n bytes
 * (heap: no storage given).  The kind says what a piece that does not fit
 * becomes, as the table at the top of this header.
 */
void wpe_fmt_begin_whole(WpeFmtText *t, char *out, size_t n);
void wpe_fmt_begin_shown(WpeFmtText *t, char *out, size_t n);
void wpe_fmt_begin_block(WpeFmtText *t, char *out, size_t n);
void wpe_fmt_begin_heap(WpeFmtText *t);

/**
 * wpe_fmt_resume_shown, wpe_fmt_resume_whole - Go on with a text that is
 * already in @out of @n bytes: the pieces added next follow what is there.
 * Used to end a sentence the caller was handed with a hint, in the room the
 * caller was given.  A text already longer than @n - 1 bytes counts as full,
 * and a shown text that already ends in the cut mark counts as cut and
 * closed: nothing is added after the mark.
 */
void wpe_fmt_resume_shown(WpeFmtText *t, char *out, size_t n);
void wpe_fmt_resume_whole(WpeFmtText *t, char *out, size_t n);

/**
 * wpe_fmt_add - Add one formatted piece.  After a piece that did not fit
 * nothing more is added: a text never has a gap with later pieces behind it.
 */
void wpe_fmt_add(WpeFmtText *t, const char *fmt, ...) WPE_FMT_PRINTF(2, 3);
void wpe_fmt_vadd(WpeFmtText *t, const char *fmt, va_list ap);

/**
 * wpe_fmt_add_block - Add one piece of a request whole, or not at all.
 * The piece is never cut and never marked, and the text stays open for the
 * next block: a prompt that has no room for the open files still gets its
 * current file.
 * Return: 1 when the piece was added, 0 when it did not fit (or the text
 * already failed).
 */
int wpe_fmt_add_block(WpeFmtText *t, const char *fmt, ...) WPE_FMT_PRINTF(2, 3);

/**
 * wpe_fmt_add_bytes - Add @len bytes of @bytes as they are.
 */
void wpe_fmt_add_bytes(WpeFmtText *t, const char *bytes, size_t len);

/**
 * wpe_fmt_add_shown - Add the sentence @s in a place of at most @room bytes:
 * a longer one is cut where a character ends and ends in "...".  Used for a
 * name or a message inside a longer sentence that has its own limit for it.
 */
void wpe_fmt_add_shown(WpeFmtText *t, size_t room, const char *s);

/**
 * wpe_fmt_room - The bytes a fixed text can still take (0 once it failed).
 */
size_t wpe_fmt_room(const WpeFmtText *t);

/**
 * wpe_fmt_len - The bytes the text holds so far (a caller that must know
 * whether anything was added, or where the next piece starts, asks here and
 * never reads the structure).
 */
size_t wpe_fmt_len(const WpeFmtText *t);

/**
 * wpe_fmt_failed - Has a piece been refused or the memory run out?  A shown
 * text that was cut and marked has not failed: it is closed and complete.
 */
int wpe_fmt_failed(const WpeFmtText *t);

/**
 * wpe_fmt_end - Finish a fixed text: WPE_FMT_OK; WPE_FMT_NO_ROOM with ""
 * (whole); 1 when cut and marked (shown); WPE_FMT_FAILED.  @len, when not
 * NULL, receives the bytes written.  Asked of a heap text it is refused with
 * WPE_FMT_FAILED.
 */
int wpe_fmt_end(WpeFmtText *t, size_t *len) WPE_FMT_MUST_READ;

/**
 * wpe_fmt_end_heap - Finish a heap text and hand over its memory (free it).
 * Return: the text, or NULL when it failed.  Asked of a fixed text it is
 * refused with NULL.
 */
char *wpe_fmt_end_heap(WpeFmtText *t, size_t *len);

/**
 * wpe_fmt_cut_len - How many of the first @max bytes of @s can stay without
 * splitting a character.  The precision of a piece that is an exact slice.
 */
int wpe_fmt_cut_len(const char *s, size_t max);

#endif
