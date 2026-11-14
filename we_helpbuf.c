/* we_helpbuf.c -- the writers of a help or Info token into storage (see
   we_helpbuf.h). */
#include "we_helpbuf.h"
#include "we_utf8.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* How many bytes of @src, within @srclen, come before a stop byte or a NUL.
   @end receives the pointer to that byte (or src + srclen). */
static size_t help_token_length(const char *src, size_t srclen,
                                const char *stops, const char **end)
{
 size_t i;

 for (i = 0; i < srclen; i++)
 {
  if (src[i] == '\0')
   break;
  if (stops && strchr(stops, src[i]))
   break;
 }
 if (end)
  *end = src + i;
 return(i);
}

/* How many of the @len bytes of @src a buffer with room for @cap bytes keeps
   without splitting a character: the glyph that covers byte @cap starts at
   or before it, so all of that glyph is dropped, not half of it. */
static size_t help_keep_whole_glyphs(const char *src, size_t len, size_t cap)
{
 int glyph_start;

 if (len <= cap)
  return(len);
 if (len > (size_t) INT_MAX)
  len = INT_MAX;
 glyph_start = e_utf8_glyph_floor((const unsigned char *) src, (int) len,
                                  (int) cap);
 return((size_t) glyph_start);
}

size_t wpe_help_token(char *out, size_t n, const char *src, size_t srclen,
                      const char *stops, const char **end, int *cut)
{
 size_t len, keep;

 len = help_token_length(src, srclen, stops, end);
 if (n == 0)
 {
  if (cut)
   *cut = len > 0;
  return(0);
 }
 keep = help_keep_whole_glyphs(src, len, n - 1);
 memcpy(out, src, keep);
 out[keep] = '\0';
 if (cut)
  *cut = keep < len;
 return(keep);
}

char *wpe_help_token_dup(const char *src, size_t srclen, const char *stops,
                         const char **end)
{
 size_t len = help_token_length(src, srclen, stops, end);
 char *copy = malloc(len + 1);

 if (!copy)
  return(NULL);
 memcpy(copy, src, len);
 copy[len] = '\0';
 return(copy);
}

size_t wpe_help_lcpy(char *out, size_t n, const char *src)
{
 return(wpe_help_token(out, n, src, strlen(src), NULL, NULL, NULL));
}

size_t wpe_help_lcat(char *out, size_t n, const char *src)
{
 size_t len;

 if (n == 0)
  return(0);
 len = strnlen(out, n);
 if (len == n)
 {
  len = n - 1;
  out[len] = '\0';
 }
 return(len + wpe_help_lcpy(out + len, n - len, src));
}

int wpe_help_line_insert(char *line, size_t n, int at, const char *ins,
                         int inslen)
{
 size_t len;

 if (n == 0 || at < 0 || inslen < 0)
  return(-1);
 len = strnlen(line, n);
 if (len == n || (size_t) at > len)
  return(-1);
 if (len + (size_t) inslen + 1 > n)
  return(-1);
 memmove(line + at + inslen, line + at, len - at + 1);
 memcpy(line + at, ins, inslen);
 return(0);
}
