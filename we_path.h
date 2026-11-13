/* we_path.h -- how long a path or a file name the editor holds may be.

   A folder the editor stands in and a file it opens are bounded by the
   system (getcwd and open refuse longer ones with ENAMETOOLONG), so a buffer
   of WPE_PATH_SIZE holds any path the editor can reach.  A longer value can
   only come from a file or the environment, and names something the system
   cannot open.  The debugger's command length (we_debreq.h) multiplies this
   number.

   Plain C: <limits.h> and <stddef.h> only, no curses and no
   editor header, so a unit test links it alone. */
#ifndef WE_PATH_H
#define WE_PATH_H

#include <limits.h>
#include <stddef.h>

#define WPE_PATH_MAX   4096          /* PATH_MAX of Linux; the BSDs' and macOS's 1024 are within it */
#define WPE_NAME_MAX   255           /* NAME_MAX of Linux, the BSDs and macOS */
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

/* A refusal nobody reads leaves @out empty and lets "" reach chdir, fopen or
   a command, so the result of the writers below must be looked at. */
#ifdef __GNUC__
#define WPE_PATH_RESULT __attribute__((warn_unused_result))
#else
#define WPE_PATH_RESULT
#endif

/* What a site says when a value does not fit; @what names the value ("file",
   "folder", "include folder") and the number is WPE_PATH_MAX. */
#define WPE_PATH_TOO_LONG_FMT "The %s path is longer than the system allows (%d bytes)."
/* Room for that sentence with its words and number filled in. */
#define WPE_PATH_MSG_MAX 160

/* The only writers of a path into fixed storage.  Each returns 0, or -1 and
   leaves @out empty when the result would not fit: a path is whole or it is
   nothing, never a prefix of itself. */
int wpe_path_copy(char *out, size_t n, const char *src) WPE_PATH_RESULT;
/* @dir, one separator unless @dir ends in one or is empty, @name. */
int wpe_path_join(char *out, size_t n, const char *dir, const char *name) WPE_PATH_RESULT;
/* @out already holds a path; @tail (".o", "/xwperc") is added to it.  On a
   refusal @out is left empty. */
int wpe_path_cat(char *out, size_t n, const char *tail) WPE_PATH_RESULT;

/* A path kept past the call lives on the heap at its real length.  NULL when
   out of memory. */
char *wpe_path_dup(const char *src);
char *wpe_path_join_dup(const char *dir, const char *name);

#endif
