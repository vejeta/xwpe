/* we_write.h -- the one owner of "write the user's text, and what happens
   when the write fails".
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   A write of the user's text has one of three outcomes, every caller reads
   it, and nothing a caller does afterwards assumes WRITTEN unless it is.  A
   failed write of the user's own file leaves the file as it was when the
   backup copy was made and put back; otherwise the sentence says the file
   may be cut short and names the backup.  The user is told once, by this
   owner, in one sentence with the path and the reason.  The companion
   .bak copy is made here, private, and counted as made only when every
   byte reached the disk. */

#ifndef WE_WRITE_H
#define WE_WRITE_H

#include <stddef.h>
#include "we_path.h"

/* A caller tests the status, never a number: WRITTEN is 0, so `!= WRITTEN`
   reads "the text is not on disk" and a window that has nothing to write
   (a file manager, no window found) is WRITTEN, not FAILED, so Close still
   closes it. */
typedef enum {
 WPE_WRITE_WRITTEN,     /* the file holds the text, closed and flushed; also
                           "nothing to write", with path[0] == '\0' */
 WPE_WRITE_DECLINED,    /* the user said No to a question the write asked */
 WPE_WRITE_FAILED       /* err and path say why and where */
} WpeWriteStatus;

typedef struct {
 WpeWriteStatus status;
 int err;                        /* errno of the failure, else 0 */
 char path[WPE_PATH_SIZE];       /* where the text went, or the file that refused it */
 int backup_err;                 /* the .bak could not be opened; the write went on */
 int backup_stopped;             /* the .bak was opened and not completed: the save was refused */
 int restore_err;                /* the write failed after the open and the old content
                                    is not back: the restore's errno, or ENOENT when
                                    no .bak was made (bak_path is then "") */
 int read_only;                  /* the window is read-only: nothing was tried */
 int kept;                       /* a copy went elsewhere: kept_path */
 char kept_path[WPE_PATH_SIZE];
 char bak_path[WPE_PATH_SIZE];   /* the .bak made in this call, else "" */
 char keep_name[WPE_PATH_SIZE];  /* the name the window keeps (Save As), else "" */
} WpeWriteOutcome;

/* What the user is told after the failure, beside the path and the reason. */
typedef enum {
 WPE_WRITE_THEN_KEPT,        /* "Your text is still in the window." */
 WPE_WRITE_THEN_NAME_KEPT,   /* "The window keeps its name t.c." */
 WPE_WRITE_THEN_NO_BUILD,    /* "The build was not started." */
 WPE_WRITE_THEN_NO_PRINT     /* "Nothing was printed." */
} WpeWriteThen;

/* Whoever drops the outcome of a write has lost the news that the text is
   not on disk. */
#define WPE_WRITE_RESULT __attribute__((warn_unused_result))

/* The room for the sentence the owner builds: a path twice, a reason and
   the follow-up. */
#define WPE_WRITE_SENTENCE_MAX (2 * WPE_PATH_SIZE + 256)

struct FNST;

/* The user's own file. */
WPE_WRITE_RESULT int wpe_write_file(struct FNST *f, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_range(struct FNST *f, const char *path,
                                     int xa, int ya, int xe, int ye,
                                     int backup, WpeWriteOutcome *o);
/* The window saved and the user told: what F2 does.  @keep_name is the name a
   failed Save As leaves the window under, or NULL. */
int wpe_write_save(struct FNST *f, WpeWriteThen then, const char *keep_name);

/* Text that is not a window's rows. */
WPE_WRITE_RESULT int wpe_write_fd(int fd, const char *text, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o);

/* The one message. */
void wpe_write_say(struct FNST *f, const WpeWriteOutcome *o, WpeWriteThen then);
int  wpe_write_sentence(char *out, size_t n, const WpeWriteOutcome *o, WpeWriteThen then);

#endif /* WE_WRITE_H */
