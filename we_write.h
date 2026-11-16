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
#include <stdio.h>
#include <sys/types.h>
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

/* What a change of mode could not keep.  The editor gives a file every
   permission bit the system takes and names the special bit (setuid, setgid,
   sticky) it does not, instead of failing the copy or losing the bit in
   silence.  A bit the system refuses and a bit it drops are told apart by
   @err, and said alike. */
typedef struct {
 mode_t lost;       /* the special bits asked for and not on the file afterwards */
 int    err;        /* errno of the call that refused them; 0 when a call returned 0
                       and the bit was absent anyway */
 gid_t  gid;        /* the file's group, read back */
 int    in_group;   /* the caller is in that group */
 int    changed;    /* a call succeeded: a failure after it is not "nothing was changed" */
} WpeModeKept;

typedef struct {
 WpeWriteStatus status;
 int err;                        /* errno of the failure, else 0 */
 char path[WPE_PATH_SIZE];       /* where the text went, or the file that refused it */
 int backup_err;                 /* the .bak could not be opened; the write went on */
 int backup_stopped;             /* the .bak was opened and not completed: the save was refused */
 int replaced;                   /* the name led to another file by the time the backup read
                                    it: nothing was written, the save is refused */
 int restore_err;                /* the write failed after the open and the old content
                                    is not back: the restore's errno, or ENOENT when
                                    no .bak was made (bak_path is then "") */
 int read_only;                  /* the window is read-only: nothing was tried */
 int kept;                       /* a copy went elsewhere: kept_path */
 char kept_path[WPE_PATH_SIZE];
 char bak_path[WPE_PATH_SIZE];   /* the .bak made in this call, else "" */
 char keep_name[WPE_PATH_SIZE];  /* the name the window keeps (Save As), else "" */
 WpeModeKept file_kept;          /* the special bits the saved file did not keep
                                    (lost == 0: all kept, or it had none).  The
                                    .bak is never given them, so it has no such
                                    record. */
 WpeModeKept restore_kept;       /* the same for a restore by write-back after a
                                    failed write: the old content is back and
                                    these bits could not be put with it (lost == 0
                                    when restored by rename, when the restore
                                    failed, or when the file had none) */
} WpeWriteOutcome;

/* What the user is told after the failure, beside the path and the reason. */
typedef enum {
 WPE_WRITE_THEN_KEPT,        /* "Your text is still in the window." */
 WPE_WRITE_THEN_NAME_KEPT,   /* "The window keeps its name t.c." */
 WPE_WRITE_THEN_NO_BUILD,    /* "The build was not started." */
 WPE_WRITE_THEN_NO_PRINT,    /* "Nothing was printed." */
 WPE_WRITE_THEN_NO_PROJECT,  /* "No project was created." */
 WPE_WRITE_THEN_PROJECT_KEPT,/* "The project list is still in its window." */
 WPE_WRITE_THEN_NO_OPTIONS,  /* "The options were not saved." */
 WPE_WRITE_THEN_USER_KEYS_STAY /* "Your own settings (hook, engine command,
                                  endpoints, dial) are still in it; take them
                                  out before you share the file." */
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
/* The window saved and the user told: what F2 does.  @keep_dir and @keep_name
   are the folder and name the window had before a Save As, which a failed
   Save As leaves it under and which say whether the target is the window's
   own file; NULL and NULL for a plain Save. */
int wpe_write_save(struct FNST *f, WpeWriteThen then, const char *keep_dir,
                   const char *keep_name);
/* The special bits that a write onto the existing file @path would not keep
   (0: none, not a regular file, or the window's own file, given by folder and
   name, or NULL and NULL for a block): what the overwrite question names. */
mode_t wpe_write_overwrite_drops(const char *path, const char *own_dir, const char *own_name);

/* Text that is not a window's rows. */
WPE_WRITE_RESULT int wpe_write_fd(int fd, const char *text, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o);

/* A file that is printed line by line (a project list, the options): the
   stream is opened here and closed here, and the outcome of the close says
   whether the last byte reached the disk.  A caller that goes through
   fprintf in between never judges the stream itself. */
WPE_WRITE_RESULT FILE *wpe_write_stream_open(const char *path, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_stream_close(FILE *fp, WpeWriteOutcome *o);

/* The bytes of a copy, for the owner of names (we_fileop.c): the one byte
   loop, a private new file in a folder, the special bits put on after the
   data, and the file size limit as an error. */
int  wpe_write_copy_fd(int from, int to, int *read_failed);
int  wpe_write_create_private(int dirfd, const char *name, mode_t mode, int *fd);
/* The one place the editor changes a file's permissions.  Each returns 0 when
   the nine bits are on, whatever happened to the special bits (@k says), and
   the errno of a real failure otherwise.  set_special_bits is the copy's:
   no call at all when the mode has no special bit; sync_before_bits is what
   a caller does first, so the text is on disk before the bits are (0 when
   there is no bit to give, else the sync's errno, and then the bits are not
   set).  set_mode_fd is for an
   open file or folder, set_mode_path for a name (the Attributes dialog). */
int  wpe_write_sync_before_bits(int fd, mode_t mode, int durable);
int  wpe_write_set_special_bits(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_mode_fd(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_mode_path(const char *path, mode_t mode, WpeModeKept *k);
int  wpe_write_caller_in_group(gid_t gid);
/* "setgid bit", "setuid and setgid bits"; and why they were not kept. */
void wpe_write_bits_words(char *out, size_t n, mode_t bits);
void wpe_write_bits_reason(char *out, size_t n, const WpeModeKept *k);
void wpe_write_guard_enter(void);
void wpe_write_guard_leave(void);

/* The one message. */
void wpe_write_say(struct FNST *f, const WpeWriteOutcome *o, WpeWriteThen then);
int  wpe_write_sentence(char *out, size_t n, const WpeWriteOutcome *o, WpeWriteThen then);

#endif /* WE_WRITE_H */
