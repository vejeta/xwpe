/* we_path.h -- how long a path or a file name the editor holds may be.

   A folder the editor stands in and a file it opens are bounded by the
   system (getcwd and open refuse longer ones with ENAMETOOLONG), so a buffer
   of WPE_PATH_SIZE holds any path the editor can reach.  A longer value can
   only come from a file or the environment, and names something the system
   cannot open.  The debugger's command length (we_debreq.h) multiplies this
   number.

   Plain C: libc headers only (<limits.h>, <stddef.h>, <stdio.h>,
   <sys/types.h>, <sys/stat.h>), no curses and no editor header, so a unit
   test links it alone. */
#ifndef WE_PATH_H
#define WE_PATH_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "we_fmt.h"

#define WPE_PATH_MAX  4096          /* PATH_MAX of Linux; the BSDs' and macOS's 1024 are within it */
#define WPE_NAME_MAX   511           /* the largest NAME_MAX of the systems built on: NetBSD's; Linux, FreeBSD, OpenBSD and macOS have 255 */
#define WPE_PATH_SIZE  WPE_PATH_MAX  /* room for a path and its terminator: a path is under PATH_MAX bytes */
#define WPE_NAME_SIZE  (WPE_NAME_MAX + 1)

/* What a dialog field over a path or a name buffer may take.  The buffer
   holds its terminator, so a field of WPE_PATH_MAX over a WPE_PATH_SIZE
   buffer runs one byte past it: a field takes the buffer's size minus one. */
#define WPE_PATH_FIELD_MAX (WPE_PATH_SIZE - 1)
#define WPE_NAME_FIELD_MAX WPE_NAME_MAX

#if defined(PATH_MAX) && PATH_MAX > WPE_PATH_MAX
#error "PATH_MAX is longer than WPE_PATH_MAX: a buffer could not hold a path of this system"
#endif
#if defined(NAME_MAX) && NAME_MAX > WPE_NAME_MAX
#error "NAME_MAX is longer than WPE_NAME_MAX: a buffer could not hold a name of this system"
#endif

/* The longest path this system takes, for a test that builds a path near the
   limit, and the number the refusal sentence prints
   (wpe_path_say_too_long).  PATH_MAX is 4096 on Linux and 1024 on macOS and the BSDs, and
   absent on GNU/Hurd, which has no limit; there the editor's own bound
   stands.  The editor's buffers are WPE_PATH_SIZE; this is the system's
   number, which decides when the system itself answers "File name too
   long".  Its owner is this header, as the other path numbers' is. */
#ifdef PATH_MAX
#define WPE_PLATFORM_PATH_MAX PATH_MAX
#else
#define WPE_PLATFORM_PATH_MAX WPE_PATH_MAX
#endif

/* The longest file name this system takes: NAME_MAX where the system declares
   it, the editor's own bound where it does not.  The number a refused name's
   sentence prints. */
#ifdef NAME_MAX
#define WPE_PLATFORM_NAME_MAX NAME_MAX
#else
#define WPE_PLATFORM_NAME_MAX WPE_NAME_MAX
#endif

/* A refusal nobody reads leaves @out empty and lets "" reach chdir, fopen or
   a command, so the result of the writers below must be looked at. */
#ifdef __GNUC__
#define WPE_PATH_RESULT __attribute__((warn_unused_result))
#else
#define WPE_PATH_RESULT
#endif

/* What a site says when a value does not fit; @what names the value ("file",
   "folder", "include folder") and the number is the system's, which the
   helpers below fill in.  No site formats this itself. */
#define WPE_PATH_TOO_LONG_FMT "The %s path is longer than the system allows (%d bytes)."
/* Room for that sentence with its words and number filled in. */
#define WPE_PATH_MSG_MAX 160

/** wpe_path_say_too_long - The sentence a user reads when the editor refuses
 *  a path; @what names it ("project", "include folder").
 *
 *  The number is the system's own limit, because every path the editor
 *  refuses is longer than the editor's room, which is never smaller than the
 *  system's: the user is told the bound their system really has (1024 on
 *  macOS and the BSDs), not the editor's 4096.
 *  @out holds @n bytes, WPE_PATH_MSG_MAX is enough. */
static inline void wpe_path_say_too_long(char *out, size_t n, const char *what)
{
  (void)wpe_fmt_shown(out, n, WPE_PATH_TOO_LONG_FMT, what, (int)WPE_PLATFORM_PATH_MAX);
}

/** wpe_path_say_name_too_long - The same sentence for one file name, with
 *  the system's limit for a name (255 on most systems), not the editor's. */
static inline void wpe_path_say_name_too_long(char *out, size_t n, const char *what)
{
  (void)wpe_fmt_shown(out, n, WPE_PATH_TOO_LONG_FMT, what, (int)WPE_PLATFORM_NAME_MAX);
}

/* The only writers of a path into fixed storage.  Each returns 0, or -1 and
   leaves @out empty when the result would not fit: a path is whole or it is
   nothing, never a prefix of itself. */
int wpe_path_copy(char *out, size_t n, const char *src) WPE_PATH_RESULT;
/* @dir, one separator unless @dir ends in one or is empty, @name. */
int wpe_path_join(char *out, size_t n, const char *dir, const char *name) WPE_PATH_RESULT;
/* @out already holds a path; @tail (".o", "/xwperc") is added to it.  On a
   refusal @out is left empty. */
int wpe_path_cat(char *out, size_t n, const char *tail) WPE_PATH_RESULT;

/* The one place a program is looked up on a PATH.  Each entry of the ':' list
   (an empty one is the current folder, as the shell reads it) is joined with
   @name; an entry whose path would not fit @out is skipped, never cut into
   the path of some other folder.  Returns 0 with the path of the first
   executable found in @out, -1 with @out empty when there is none, and
   WPE_PATH_SEARCH_NO_LIST when @list is NULL. */
#define WPE_PATH_SEARCH_NO_LIST (-2)
int wpe_path_search_in(const char *list, const char *name, char *out, size_t n);
/* The same on the PATH of the environment. */
int wpe_path_search(const char *name, char *out, size_t n);

/* A path kept past the call lives on the heap at its real length.  NULL when
   out of memory. */
char *wpe_path_dup(const char *src);
char *wpe_path_join_dup(const char *dir, const char *name);

/* The words of a refused read.  The sentence has one shape, "Cannot open
   <what> <path>: <reason>.", and "<What> not found: <path>." only for a name
   that does not exist.  @what is an ASCII noun in lowercase ("file",
   "project file", "options file"); the first letter is raised for the
   not-found shape.  This is the one place that spells such a sentence:
   we_read.c maps its outcomes onto these and holds no word of its own. */

/** What a refusal found by the editor itself is called (not an errno). */
typedef enum {
 WPE_PATH_REASON_LINK = 1,    /* a link holds the name and links are not followed */
 WPE_PATH_REASON_OTHER,       /* another file took the name */
 WPE_PATH_REASON_CHANGED,     /* not proven to be the file that was looked at */
 WPE_PATH_REASON_NO_WRITER,   /* a pipe nobody is writing to */
 WPE_PATH_REASON_NOT_FOLDER,  /* a folder was asked for and something else is there */
 WPE_PATH_REASON_DANGLING     /* a link holds the name and what it leads to is not there */
} WpePathWhy;

/* "not found" for ENOENT, the system's own words for any other errno.  The
   one place in the editor that may say "not found" about a path. */
const char *wpe_path_refused_reason(int err);
/* The sentence for a refusal of the system.  A sentence longer than @out ends
   in "...".  Returns the length written, never a length only wanted. */
int wpe_path_say_refused(char *out, size_t n, const char *what, const char *path, int err);
/* The same sentence at its real length on the heap (free it), for a caller
   with a path longer than any array.  NULL when out of memory. */
char *wpe_path_say_refused_dup(const char *what, const char *path, int err);
/* The same sentence with a reason the editor found itself.  Never the
   not-found shape. */
int wpe_path_say_refused_because(char *out, size_t n, const char *what,
                                 const char *path, const char *reason);
char *wpe_path_say_refused_because_dup(const char *what, const char *path, const char *reason);
/* What a thing that is not a regular file is called: "it is a pipe", "it is a
   socket", "it is a device", "it is a folder"; a descriptor that was opened
   and is a terminal says "it is a terminal".  A kind found by a look cannot
   say terminal: only isatty on a descriptor can. */
const char *wpe_path_kind_reason(mode_t kind, int tty);
/* "it is larger than 1 MiB", from a bound in bytes.  Returns the length written. */
int wpe_path_too_large_reason(char *out, size_t n, size_t bound);
/* The fixed words of the refusals the editor finds itself. */
const char *wpe_path_reason_of(WpePathWhy why);

/* Room for any such sentence over a path of the editor's own size. */
#define WPE_PATH_REFUSED_MAX (WPE_PATH_SIZE + WPE_PATH_MSG_MAX)

#endif
