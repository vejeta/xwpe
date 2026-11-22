/* we_path.c -- the writers of a path into fixed storage (see we_path.h). */
#include "we_path.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_SEP '/'

#define WPE_PATH_KIB ((size_t)1 << 10)
#define WPE_PATH_MIB ((size_t)1 << 20)

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
 * wpe_path_entry_join - Name @command as it would sit in one PATH entry.
 * @entry/@len: the entry, which is not terminated; @out/@n: receive
 * "<entry>/<command>" ("./<command>" for an empty entry).
 * Return: 0, or -1 with @out empty when the entry or the path is too long.
 */
static int wpe_path_entry_join(const char *entry, size_t len, const char *command,
                               char *out, size_t n)
{
 char dir[WPE_PATH_SIZE];

 if (len == 0)
  return wpe_path_join(out, n, ".", command);
 if (len >= sizeof dir)
 {
  if (n)
   out[0] = '\0';
  return -1;
 }
 memcpy(dir, entry, len);
 dir[len] = '\0';
 return wpe_path_join(out, n, dir, command);
}

/**
 * wpe_path_search_in - Find the executable @name in a ':' list of folders.
 * @list: a PATH; @name: a program's name; @out/@n: receive its path.
 *
 * Lets the editor offer a debugger, a compiler or a language server only
 * when it is installed, on a PATH it does not control: an entry too long for
 * the system to hold is skipped, so it can neither overrun a buffer nor
 * point at some other folder.
 * Return: 0 with the path in @out; -1 with @out empty when not found;
 * WPE_PATH_SEARCH_NO_LIST when @list is NULL.
 */
int wpe_path_search_in(const char *list, const char *name, char *out, size_t n)
{
 const char *entry, *end;

 if (out && n)
  out[0] = '\0';
 if (!list)
  return WPE_PATH_SEARCH_NO_LIST;
 if (!name || !out || !n)
  return -1;
 for (entry = list; ; entry = end + 1)
 {
  end = strchr(entry, ':');
  if (!end)
   end = entry + strlen(entry);
  if (wpe_path_entry_join(entry, (size_t)(end - entry), name, out, n) == 0 &&
      access(out, X_OK) == 0)
   return 0;
  if (!*end)
   break;
 }
 out[0] = '\0';
 return -1;
}

/**
 * wpe_path_search - Find the executable @name on the environment's PATH.
 * Return: as wpe_path_search_in.
 */
int wpe_path_search(const char *name, char *out, size_t n)
{
 return wpe_path_search_in(getenv("PATH"), name, out, n);
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

/**
 * wpe_path_refused_reason - What the user reads as the reason a path was refused.
 * @err: the errno of the refusal.
 *
 * Lets the user look for the real cause: "not found" means a typo, anything
 * else (a folder closed to reading, a loop of links) is the system's own
 * words, so nobody searches for a typo that is not there.
 * Return: "not found" for ENOENT, else strerror(@err).
 */
const char *wpe_path_refused_reason(int err)
{
 if (err == ENOENT)
  return "not found";
 return strerror(err);
}

/**
 * wpe_path_say_refused_because - "Cannot open <what> <path>: <reason>."
 * @out/@n: the buffer; @what: an ASCII noun; @path; @reason: the words.
 * Return: the length the sentence needs, as snprintf.
 */
int wpe_path_say_refused_because(char *out, size_t n, const char *what,
                                 const char *path, const char *reason)
{
 return snprintf(out, n, "Cannot open %s %s: %s.", what, path, reason);
}

/**
 * wpe_path_say_not_found - "<What> not found: <path>." with the first letter raised.
 * @out/@n: the buffer; @what: an ASCII noun; @path.
 * Return: the length the sentence needs, as snprintf.
 *
 * Only an ASCII a to z is raised: a byte above 127 is left alone, because a
 * locale's toupper would change it.
 */
static int wpe_path_say_not_found(char *out, size_t n, const char *what, const char *path)
{
 char first = what[0];

 if (first >= 'a' && first <= 'z')
  first = (char)(first - 'a' + 'A');
 return snprintf(out, n, "%c%s not found: %s.", first, what[0] ? what + 1 : what, path);
}

/**
 * wpe_path_say_refused - The sentence for a path the system refused.
 * @out/@n: the buffer; @what: an ASCII noun in lowercase; @path; @err: errno.
 *
 * The user reads the path and the system's reason, and "not found" only when
 * the path does not exist.
 * Return: the length the whole sentence needs, as snprintf.
 */
int wpe_path_say_refused(char *out, size_t n, const char *what, const char *path, int err)
{
 if (err == ENOENT)
  return wpe_path_say_not_found(out, n, what, path);
 return wpe_path_say_refused_because(out, n, what, path, wpe_path_refused_reason(err));
}

/**
 * wpe_path_kind_reason - What a thing that is not a regular file is called.
 * @kind: S_IFMT of what was found; @tty: a descriptor was opened and is a terminal.
 * Return: the reason, never NULL.
 */
const char *wpe_path_kind_reason(mode_t kind, int tty)
{
 if (tty)
  return "it is a terminal";
 switch (kind & S_IFMT)
 {
 case S_IFIFO:
  return "it is a pipe";
 case S_IFSOCK:
  return "it is a socket";
 case S_IFCHR:
 case S_IFBLK:
  return "it is a device";
 case S_IFDIR:
  return "it is a folder";
 default:
  return "it is not a regular file";
 }
}

/**
 * wpe_path_too_large_reason - "it is larger than 1 MiB", from a bound in bytes.
 * @out/@n: the buffer; @bound: the bound in bytes.
 * Return: the length the reason needs, as snprintf.
 */
int wpe_path_too_large_reason(char *out, size_t n, size_t bound)
{
 if (bound >= WPE_PATH_MIB && bound % WPE_PATH_MIB == 0)
  return snprintf(out, n, "it is larger than %lu MiB", (unsigned long)(bound / WPE_PATH_MIB));
 if (bound >= WPE_PATH_KIB && bound % WPE_PATH_KIB == 0)
  return snprintf(out, n, "it is larger than %lu KiB", (unsigned long)(bound / WPE_PATH_KIB));
 return snprintf(out, n, "it is larger than %lu bytes", (unsigned long)bound);
}

/**
 * wpe_path_reason_of - The fixed words of a refusal the editor found itself.
 * @why: which one.
 * Return: the reason, never NULL.
 */
const char *wpe_path_reason_of(WpePathWhy why)
{
 switch (why)
 {
 case WPE_PATH_REASON_LINK:
  return "it is a link";
 case WPE_PATH_REASON_OTHER:
  return "another file took the name";
 case WPE_PATH_REASON_CHANGED:
  return "it changed or was replaced";
 case WPE_PATH_REASON_NO_WRITER:
  return "nothing is writing to the pipe";
 case WPE_PATH_REASON_NOT_FOLDER:
  return "it is not a folder";
 }
 return "it cannot be read";
}
