/* we_optval.c -- the one owner of an option value's stored form.
 * See we_optval.h for the form and what it guarantees.  Libc only.
 */
#include <stdlib.h>
#include <string.h>

#include "we_optval.h"

#define ESCAPE '\\'

/** The byte that follows a backslash in the stored form of @c, or 0 when @c
    is stored as it is. */
static char optval_escape_letter(char c)
{
 if (c == '\n')
  return 'n';
 if (c == ESCAPE)
  return ESCAPE;
 return 0;
}

/** The byte a backslash followed by @letter stands for, or 0 when the pair
    is not an escape (the backslash then stands for itself). */
static char optval_unescape_letter(char letter)
{
 if (letter == 'n')
  return '\n';
 if (letter == 'r')
  return '\r';
 if (letter == ESCAPE)
  return ESCAPE;
 return 0;
}

size_t wpe_optval_encoded_size(size_t n)
{
 return 2 * n + 1;
}

size_t wpe_optval_encode(char *out, size_t cap, const char *in, size_t n)
{
 size_t i, o = 0;
 char letter;

 if (cap == 0)
  return 0;
 for (i = 0; i < n; i++)
 {
  letter = optval_escape_letter(in[i]);
  if (letter)
  {
   if (o + 2 >= cap)
    break;
   out[o++] = ESCAPE;
   out[o++] = letter;
  }
  else
  {
   if (o + 1 >= cap)
    break;
   out[o++] = in[i];
  }
 }
 out[o] = '\0';
 return o;
}

size_t wpe_optval_decode(char *out, size_t cap, const char *in, size_t n)
{
 size_t i = 0, o = 0;
 char byte;

 if (cap == 0)
  return 0;
 while (i < n && o + 1 < cap)
 {
  if (in[i] != ESCAPE)
  {
   out[o++] = in[i++];
   continue;
  }
  /* A backslash that ends the value stands for itself. */
  byte = (i + 1 < n) ? optval_unescape_letter(in[i + 1]) : 0;
  if (byte)
  {
   out[o++] = byte;
   i += 2;
  }
  else
  {
   out[o++] = ESCAPE;
   i++;
  }
 }
 out[o] = '\0';
 return o;
}

char *WpeStringToValue(const char *str)
{
 size_t n = strlen(str), cap = wpe_optval_encoded_size(n);
 char *answer = malloc(cap);

 if (answer)
  wpe_optval_encode(answer, cap, str, n);
 return answer;
}

char *WpeValueToString(const char *value)
{
 size_t n = strlen(value);
 char *answer = malloc(n + 1);

 if (answer)
  wpe_optval_decode(answer, n + 1, value, n);
 return answer;
}
