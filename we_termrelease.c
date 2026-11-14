/* we_termrelease.c -- the string a signal handler writes to give the terminal
   back.  See we_termrelease.h; no curses headers, so tests/test_termrelease.c
   links it alone. */

#include <string.h>

#include "we_termrelease.h"
#include "unixkeys.h"   /* only #defines: the strings the editor's own exit writes */

/* The pieces that are the editor's own, written by every exit. */
#define RELEASE_KEYS_OFF  E_T_KEYS_OFF_STR
#define RELEASE_MODES_OFF E_T_MOUSE_PASTE_OFF_STR
#define RELEASE_POINTER   E_T_POINTER_SHAPE_RESET_STR

/* terminfo's marker for a capability that exists but is no string. */
#define RELEASE_ABSENT ((const char *)-1)

/* terminfo's padding: "$<" digits, ".", digits, "*" or "/", ">". */
#define RELEASE_PAD_OPEN "$<"
#define RELEASE_PAD_CLOSE '>'

/**
 * release_is_present - Is @s a string capability the terminal has?
 */
static int release_is_present(const char *s)
{
 return(s != NULL && s != RELEASE_ABSENT && s[0] != '\0');
}

/**
 * release_append - Put @s in the buffer if it fits whole.
 * @buf: the buffer.
 * @size: its room, one byte of which stays free for a closing NUL.
 * @len: the length so far.
 * Return: the new length.
 */
static size_t release_append(char *buf, size_t size, size_t len, const char *s)
{
 size_t n = strlen(s);

 if (len + n + 1 > size)
  return(len);
 memcpy(buf + len, s, n);
 return(len + n);
}

/**
 * release_append_capability - Put a terminfo string in the buffer, without
 * its padding marks, if it is present and fits whole.
 * Return: the new length.
 */
static size_t release_append_capability(char *buf, size_t size, size_t len,
                                        const char *cap)
{
 char plain[E_T_RELEASE_MAX];
 size_t n = 0;
 const char *p;

 if (!release_is_present(cap))
  return(len);
 for (p = cap; *p != '\0' && n + 1 < sizeof plain; )
 {
  if (strncmp(p, RELEASE_PAD_OPEN, 2) == 0)
  {
   const char *close = strchr(p, RELEASE_PAD_CLOSE);

   if (close != NULL)
   {
    p = close + 1;
    continue;
   }
  }
  plain[n++] = *p++;
 }
 if (*p != '\0')
  return(len);   /* longer than any release piece: leave it out whole */
 plain[n] = '\0';
 return(release_append(buf, size, len, plain));
}

size_t e_t_release_string_build(char *buf, size_t size, int console,
                                const char *sgr0, const char *cnorm,
                                const char *rmkx, const char *rmcup)
{
 size_t len = 0;

 if (buf == NULL || size == 0)
  return(0);
 if (!console)
  len = release_append(buf, size, len, RELEASE_KEYS_OFF);
 len = release_append(buf, size, len, RELEASE_MODES_OFF);
 len = release_append(buf, size, len, RELEASE_POINTER);
 len = release_append_capability(buf, size, len, sgr0);
 len = release_append_capability(buf, size, len, cnorm);
 len = release_append_capability(buf, size, len, rmkx);
 len = release_append_capability(buf, size, len, rmcup);
 buf[len] = '\0';
 return(len);
}
