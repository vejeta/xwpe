/* we_path.c -- the writers of a path into fixed storage (see we_path.h). */
#include "we_path.h"

#include <stdlib.h>
#include <string.h>

#define PATH_SEP '/'

/**
 * wpe_path_copy - Copy a path whole into a buffer, or not at all.
 * @out/@n: the buffer and its size; @src: the path.
 *
 * Lets a site keep a path in fixed storage without ever holding a prefix of
 * it: a cut path names another file.
 * Return: 0, or -1 with @out empty when @src and its terminator do not fit.
 */
int wpe_path_copy(char *out, size_t n, const char *src)
{
 size_t len;

 if (!out || !n)
  return -1;
 out[0] = '\0';
 if (!src)
  return -1;
 len = strlen(src);
 if (len >= n)
  return -1;
 memcpy(out, src, len + 1);
 return 0;
}

/**
 * wpe_path_join - Write "dir/name" into a buffer, or nothing.
 * @out/@n: the buffer and its size; @dir: a folder with or without its
 * trailing separator, or NULL or "" for none; @name: the file's name.
 *
 * A folder and a name that each fit can still be too long joined; the caller
 * then tells the user, it does not open a shorter file.
 * Return: 0, or -1 with @out empty when the result does not fit.
 */
int wpe_path_join(char *out, size_t n, const char *dir, const char *name)
{
 size_t dlen, nlen, sep, total;

 if (!out || !n)
  return -1;
 out[0] = '\0';
 if (!name)
  return -1;
 dlen = dir ? strlen(dir) : 0;
 nlen = strlen(name);
 sep = (dlen > 0 && dir[dlen - 1] != PATH_SEP) ? 1 : 0;
 total = dlen + sep + nlen;
 if (total >= n)
  return -1;
 if (dlen > 0)
  memcpy(out, dir, dlen);
 if (sep)
  out[dlen] = PATH_SEP;
 memcpy(out + dlen + sep, name, nlen + 1);
 return 0;
}

/**
 * wpe_path_cat - Add a suffix to the path in a buffer, or empty the buffer.
 * @out/@n: the buffer holding a path, and its size; @tail: ".o", "/xwperc".
 * Return: 0, or -1 with @out empty when the result does not fit.
 */
int wpe_path_cat(char *out, size_t n, const char *tail)
{
 size_t len, tlen;

 if (!out || !n)
  return -1;
 if (!tail)
  return -1;
 len = strnlen(out, n);
 tlen = strlen(tail);
 if (len >= n || len + tlen >= n)
 {
  out[0] = '\0';
  return -1;
 }
 memcpy(out + len, tail, tlen + 1);
 return 0;
}

/**
 * wpe_path_dup - Keep a path on the heap at its real length.
 * Return: the copy, or NULL when out of memory or @src is NULL.
 */
char *wpe_path_dup(const char *src)
{
 size_t len;
 char *p;

 if (!src)
  return NULL;
 len = strlen(src);
 p = malloc(len + 1);
 if (p)
  memcpy(p, src, len + 1);
 return p;
}

/**
 * wpe_path_join_dup - "dir/name" on the heap at its real length.
 * Return: the path, or NULL when out of memory or @name is NULL.
 */
char *wpe_path_join_dup(const char *dir, const char *name)
{
 size_t dlen, nlen, sep;
 char *p;

 if (!name)
  return NULL;
 dlen = dir ? strlen(dir) : 0;
 nlen = strlen(name);
 sep = (dlen > 0 && dir[dlen - 1] != PATH_SEP) ? 1 : 0;
 p = malloc(dlen + sep + nlen + 1);
 if (!p)
  return NULL;
 if (dlen > 0)
  memcpy(p, dir, dlen);
 if (sep)
  p[dlen] = PATH_SEP;
 memcpy(p + dlen + sep, name, nlen + 1);
 return p;
}
