/* we_helpbuf.h -- the one owner of a help or Info token taken into storage.

   The Help viewer and the Info reader pull a link name, a header, a node
   name or a file name out of a row of text.  Info files come from the
   system and from the user, so a token can be longer than the author of the
   reader guessed.  Every such copy goes through the writers below: a token
   or line is written whole when it fits and cut at a UTF-8 character
   boundary when it does not, is always terminated, and never writes past the
   size it is given.  A name that is a key (a link, a node, a file) is never
   used cut: the caller asks for it whole (wpe_help_token_dup) or refuses
   when the cut flag says it did not fit.

   Plain C: libc and the UTF-8 rule of we_utf8.h, so a unit test links it
   alone. */
#ifndef WE_HELPBUF_H
#define WE_HELPBUF_H

#include <stddef.h>

/* The line length the readers take from a file: e_i_fgets reads at most this
   many bytes including the terminator. */
#define WPE_HELP_LINE_MAX  256
/* A line buffer with headroom for the markup bytes the Info reader inserts
   and for the terminator. */
#define WPE_HELP_LINE_SIZE (WPE_HELP_LINE_MAX + 8)

/**
 * wpe_help_token - Copy the token at the start of @src into @out[@n].
 * @src/@srclen: the text and the bytes of it that may be read; @stops: the
 * bytes that end the token (NULL or "" for none; a NUL always ends it);
 * @end: receives the stop byte found within @srclen, or @src + @srclen, or
 * the NUL, whatever the size of @out (may be NULL); @cut: receives 1 when
 * the token did not fit whole (may be NULL).
 *
 * A token that does not fit is cut at a character boundary, so no half of a
 * multi-byte character is ever kept.  @n of 0 writes nothing; @n of 1 writes
 * the terminator only.
 * Return: the bytes written, without the terminator.
 */
size_t wpe_help_token(char *out, size_t n, const char *src, size_t srclen,
                      const char *stops, const char **end, int *cut);

/**
 * wpe_help_token_dup - The token at the start of @src, whole, in malloc'd
 * storage the caller frees.
 *
 * For a name that is a key: a node, a link, a file name.  @src, @srclen,
 * @stops and @end are as for wpe_help_token.
 * Return: the token, or NULL when memory is short.
 */
char *wpe_help_token_dup(const char *src, size_t srclen, const char *stops,
                         const char **end);

/**
 * wpe_help_lcpy - Copy all of @src into @out[@n], cut at a character
 * boundary, terminated.  Return: the bytes written.
 */
size_t wpe_help_lcpy(char *out, size_t n, const char *src);

/**
 * wpe_help_lcat - Append @src after the string in @out[@n], same guarantee.
 * Return: the length of @out after the append.
 */
size_t wpe_help_lcat(char *out, size_t n, const char *src);

/**
 * wpe_help_line_insert - Insert @inslen bytes of @ins at byte offset @at of
 * the NUL-terminated line in @line[@n], shifting the tail right.
 *
 * Refuses when the result with its terminator would not fit, so a line the
 * reader marks up stays readable plain text instead of overflowing.
 * Return: 0 inserted, -1 refused with @line unchanged.
 */
int wpe_help_line_insert(char *line, size_t n, int at, const char *ins,
                         int inslen);

#endif
