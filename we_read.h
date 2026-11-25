/* we_read.h -- the one owner of "a file is read by a name the editor did not choose".

   A name a user typed, a folder held, a model wrote or a server sent can lead
   to a pipe nobody writes to, a terminal, a device that never ends, a socket,
   or a file of any size.  A plain fopen of it waits for ever, takes the keys
   the user is typing, or fills the memory.  This is the one place that opens
   such a name: it opens without waiting and without taking a terminal, judges
   the object on the descriptor before one byte is read, bounds what comes
   out, and hands the caller the identity of what it read.  A refusal has a
   kind and the kind has one sentence, which is we_path.c's: this file holds
   no word the user reads.

   The caller says what it wants with a WPE_READ_ASK_* form, never by filling
   WpeReadAsk by hand (tests/check-read-owner.sh keeps it so).

   Plain C: libc, we_seen.h and we_path.h only, no curses and no editor type,
   so a unit test links it alone, as it does we_seen.c. */
#ifndef WE_READ_H
#define WE_READ_H

#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "we_path.h"
#include "we_seen.h"

#ifdef __GNUC__
#define WPE_READ_RESULT __attribute__((warn_unused_result))
#else
#define WPE_READ_RESULT
#endif

/** How the last part of the name is treated. */
typedef enum {
 WPE_READ_NAMED,            /* a link the user typed is followed */
 WPE_READ_NAMED_NOFOLLOW    /* a link at the name is refused and named */
} WpeReadHow;

/** What may be under the name.  A caller that names nothing gets FILE. */
#define WPE_READ_KIND_FILE 0x1u   /* a regular file */
#define WPE_READ_KIND_PIPE 0x2u   /* a FIFO: handed back still non-blocking (the pipe load only) */

/** What happens to a file larger than the bound. */
typedef enum {
 WPE_READ_WHOLE_OR_REFUSE,  /* refused whole; nothing is read (options, project, before image) */
 WPE_READ_CUT               /* read up to the bound; the outcome says cut (tool output) */
} WpeReadOver;

#define WPE_READ_UNBOUNDED   ((size_t)-1)
#define WPE_READ_CONFIG_MAX  ((size_t)1 << 20)    /* 1 MiB: an options or configuration file */
#define WPE_READ_PROJECT_MAX ((size_t)16 << 20)   /* 16 MiB: a project file */

/** What a caller asks for.  Built with a WPE_READ_ASK_* form. */
typedef struct {
 const char    *what;     /* the noun of the sentence: "options file", "header" ... ASCII */
 WpeReadHow     how;
 unsigned       kinds;
 size_t         bound;
 WpeReadOver    over;
 const WpeSeen *expect;   /* the look the caller judged, or NULL: nothing is compared */
 int            dirfd;    /* AT_FDCWD, or a folder held open (the walk's, the names owner's) */
} WpeReadAsk;

/* The forms.  Each says what the caller reads in one word; the bound and the
   link policy come with it. */
#define WPE_READ_ASK_CONFIG(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, WPE_READ_CONFIG_MAX, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
#define WPE_READ_ASK_FOLDER_CONFIG(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED_NOFOLLOW, WPE_READ_KIND_FILE, WPE_READ_CONFIG_MAX, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
#define WPE_READ_ASK_PROJECT(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, WPE_READ_PROJECT_MAX, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A file the user or the installation named, read whole up to a bound the
   caller names; refused whole over it (a key file, an installed table, a
   migrated file, a language server's file). */
#define WPE_READ_ASK_NAMED_BOUNDED(what, max) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, (max), \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A file read to a bound the caller names and then cut there, the outcome
   saying so: a file that may be longer than the room the caller has (a
   start-up file a box shows in part). */
#define WPE_READ_ASK_NAMED_CUT(what, max) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, (max), \
                WPE_READ_CUT, NULL, AT_FDCWD })
/* A file judged and then read by the caller from the descriptor, to a bound
   of its own (a header, a section), a link at the name refused. */
#define WPE_READ_ASK_SCAN_NOFOLLOW(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED_NOFOLLOW, WPE_READ_KIND_FILE, WPE_READ_UNBOUNDED, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A folder to list: a link at its name is followed (the user's own folder may
   be one).  The folder face ignores the kinds and the bound of a file; they
   carry the same values as the other forms only so the record is complete. */
#define WPE_READ_ASK_FOLDER(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, WPE_READ_CONFIG_MAX, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A folder met while walking another: a link at its name is refused, never
   entered (the same note on the kinds and the bound). */
#define WPE_READ_ASK_FOLDER_ENTRY(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED_NOFOLLOW, WPE_READ_KIND_FILE, WPE_READ_CONFIG_MAX, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A reader with constant memory (a line at a time): the file is not held. */
#define WPE_READ_ASK_SCAN(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, WPE_READ_UNBOUNDED, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* The user's own open of a file: no bound, the user asked for it. */
#define WPE_READ_ASK_LOAD(what) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED, WPE_READ_KIND_FILE, WPE_READ_UNBOUNDED, \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* A name a model or a server chose, judged against the look the caller took. */
#define WPE_READ_ASK_JUDGED(what, max, over, seen) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED_NOFOLLOW, WPE_READ_KIND_FILE, (max), (over), (seen), AT_FDCWD })
/* A file the editor or a folder the user opened named, no look taken first, a
   link at the name refused: refused whole over the bound (a list git wrote) ... */
#define WPE_READ_ASK_FOLDER_BOUNDED(what, max) \
 ((WpeReadAsk){ (what), WPE_READ_NAMED_NOFOLLOW, WPE_READ_KIND_FILE, (max), \
                WPE_READ_WHOLE_OR_REFUSE, NULL, AT_FDCWD })
/* The same form, read below a folder the caller holds open. */
#define WPE_READ_ASK_AT(form, dirfd) wpe_read_ask_at((form), (dirfd))
/* A project judged at its first read, read again against that look. */
#define WPE_READ_ASK_PROJECT_SEEN(what, seen) wpe_read_ask_seen(WPE_READ_ASK_PROJECT(what), (seen))

/** wpe_read_ask_at - A form with the folder its name is relative to. */
static inline WpeReadAsk wpe_read_ask_at(WpeReadAsk a, int dirfd)
{
 a.dirfd = dirfd;
 return a;
}

/** wpe_read_ask_seen - A form that must read the object the look found. */
static inline WpeReadAsk wpe_read_ask_seen(WpeReadAsk a, const WpeSeen *seen)
{
 a.expect = seen;
 return a;
}

/** Why nothing was read. */
typedef enum {
 WPE_READ_DONE,
 WPE_READ_REFUSED_BY_SYSTEM,  /* err holds the errno */
 WPE_READ_NOT_A_FILE,         /* kind and tty say what it is */
 WPE_READ_IS_A_LINK,          /* NOFOLLOW, and a link holds the name */
 WPE_READ_TOO_LARGE,          /* over a bound that refuses; bound says which */
 WPE_READ_OTHER_FILE,         /* expect given: not the object looked at; unsure says how sure */
 WPE_READ_NO_WRITER,          /* a pipe nobody writes to (the pipe load only) */
 WPE_READ_NO_MEMORY
} WpeReadWhy;

typedef struct {
 WpeReadWhy why;
 int      err;         /* with REFUSED_BY_SYSTEM */
 mode_t   kind;        /* with NOT_A_FILE: S_IFMT of what was found */
 int      tty;         /* with NOT_A_FILE: a descriptor was opened and it is a terminal */
 int      want_folder; /* with NOT_A_FILE: a folder was asked, something else was found */
 int      unsure;      /* with OTHER_FILE: not proven another, only not proven the one seen */
 size_t   bound;       /* with TOO_LARGE or cut */
 int      cut;         /* WPE_READ_CUT stopped the read */
 WpeSeen  seen;        /* the object read; known 0 after a refusal before the open */
 off_t    size;        /* its size when judged */
 time_t   mtime;       /* for the header check, which compared a second stat by name */
 mode_t   mode;        /* its st_mode */
 uid_t    uid;         /* its owner, for a store that is believed only when it is the user's */
} WpeReadOutcome;

/* The descriptor face: judged, no longer non-blocking (a pipe asked for by
   kind stays non-blocking).  For a reader with its own loop: the copy, the
   load, Find in files.  Returns the descriptor or -1. */
WPE_READ_RESULT int wpe_read_open(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o);

/* The name face: the same judgement by the name alone, no open.  For a gate
   that must refuse before it changes anything (a project about to replace
   the open one), and for "is this still a file I may read" questions.  A
   FIFO is refused by its kind, never opened.  Returns 0 or -1. */
WPE_READ_RESULT int wpe_read_name_ok(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o);

/* The stream face, for the fgets and getline readers.  A bound that refuses
   gives a stream over the bytes already read whole into memory, so the reader
   never sees more than the bound, never a byte the file does not hold and
   never a file that grew.  UNBOUNDED gives a stream on the descriptor.  Both
   are closed by fclose, and the first owns what it reads from, so no caller
   holds a second pointer to release: the bytes themselves, or on a system
   with no memory stream call a temporary file that goes with the stream. */
WPE_READ_RESULT FILE *wpe_read_stream_open(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o);

/* The text face: the whole file as malloc'd bytes, NUL-ended; @len is the
   count and the bytes may hold NUL.  NULL on a refusal. */
WPE_READ_RESULT char *wpe_read_text(const char *path, const WpeReadAsk *ask, size_t *len, WpeReadOutcome *o);

/* The folder face: the one way to list a folder.  Opened O_DIRECTORY, judged
   on the descriptor, listed with fdopendir. */
WPE_READ_RESULT DIR *wpe_read_folder(const char *path, const WpeReadAsk *ask, WpeReadOutcome *o);

/* What an entry of that folder is, asked of the folder's descriptor and the
   entry's name, never of a joined path.  A link is reported as a link.
   Returns 0 or -1. */
WPE_READ_RESULT int wpe_read_entry_look(DIR *d, const char *name, WpeSeen *s);

/* What an entry leads to: a link is looked at as the file it points to.  Fails
   when the link's target is gone; ask wpe_read_entry_look for the link then.
   Returns 0 or -1. */
WPE_READ_RESULT int wpe_read_entry_look_target(DIR *d, const char *name, WpeSeen *s);

/* The kind of an entry (S_IFMT bits: S_IFREG, S_IFDIR, S_IFLNK, S_IFIFO ...),
   asked the same way, 0 when it cannot be looked at. */
mode_t wpe_read_entry_kind(DIR *d, const char *name);

/* A refusal that came from the system and not from the owner's judgement
   (a getcwd that failed, an errno a caller already holds): the outcome a
   reporter takes. */
void wpe_read_outcome_errno(WpeReadOutcome *o, int err);

/* Was the file refused only because it does not exist? */
int wpe_read_outcome_missing(const WpeReadOutcome *o);

/* The reason a refusal gets, as the words owner spells it ("it is a pipe").
   Returns the length written. */
int wpe_read_reason(char *out, size_t n, const WpeReadOutcome *o);

/* The whole sentence of a refusal: "Cannot open <what> <path>: <reason>.",
   or "<What> not found: <path>." for a missing name.  A sentence longer than
   @out ends in "...".  Returns the length written, never a length only
   wanted. */
int wpe_read_say(char *out, size_t n, const char *what, const char *path, const WpeReadOutcome *o);

/* The same sentence at its real length on the heap (free it), for a path
   longer than any array.  NULL when out of memory. */
char *wpe_read_say_dup(const char *what, const char *path, const WpeReadOutcome *o);

#endif
