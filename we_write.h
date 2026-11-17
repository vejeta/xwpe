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

/* Who owned a copy and who owned its original, when a copy was not given a
   setuid or setgid bit because they differ.  other_owner: the user differs
   (setuid and setgid are both dropped); other_group: the user is the same and
   the group differs (setgid is dropped).  The ids are the two that were
   compared. */
typedef struct {
 mode_t dropped;      /* the set-ID bits left off for this reason; 0 = none */
 int   other_owner;
 int   other_group;
 uid_t uid_copy, uid_src;
 gid_t gid_copy, gid_src;
} WpeModeOwners;

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
 WpeModeOwners owners;  /* a bit dropped on purpose because the copy has another owner or group */
 mode_t asked;      /* the special bits the call asked for */
 int    unreached;  /* a change of mode by name: the name could not be reached, no call was made */
 int    replaced;   /* a change of mode by name: another file had taken the name since it was
                       read; that file was not the one whose bits were read */
} WpeModeKept;

/** The file a window read, as the system knows it: device and inode, which
    stay the same under any name, hard link or path.  A window remembers it
    from the moment it read its file, so a Save writes into that file or
    asks, and never decides by looking at the name again.  known == 0 for a
    window that read no file (a new one, a clipboard, a file that did not
    exist). */
typedef struct {
 dev_t dev;
 ino_t ino;
 int   known;
} WpeFileIdentity;

typedef struct {
 WpeWriteStatus status;
 int err;                        /* errno of the failure, else 0 */
 int other_file;                 /* the name leads to another file than the one the
                                    window read: nothing was written, the save is
                                    refused (the file is not the user's own) */
 int changed_while_asked;        /* with other_file: the user had said Yes about one
                                    file, and by the time of the write the name led
                                    to yet another: the sentence says so */
 int not_regular;                /* a write with no one to ask found something that is
                                    not a regular file (a device, a terminal, a pipe)
                                    under the name: nothing was written */
 mode_t file_off;               /* a failed write left no backup to put back: the
                                    special bits the file had and, read back from
                                    the file, no longer has (0: none, or unknown) */
 int ask_overwrite;              /* a plain Save found another file under the name and
                                    wrote nothing: the caller asks the user (owner-internal
                                    between the two passes of a Save) */
 WpeFileIdentity asked;          /* with ask_overwrite: the file the user is asked about */
 WpeFileIdentity written;        /* the file that holds the text after WRITTEN, new
                                    or old: the window's file from now on */
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
struct stat;

/* The identity of the file a window read (FENSTER.file_id).  Taken from the
   descriptor that read it (take), forgotten for a window that has no file
   (forget), or by name for a window that read a copy of its file (of_name:
   the recovery, which read the .ESV). */
void wpe_write_identity_take(WpeFileIdentity *id, const struct stat *st);
void wpe_write_identity_forget(WpeFileIdentity *id);
int  wpe_write_identity_of_name(const char *dir, const char *name, WpeFileIdentity *id);

/* The user's own file. */
WPE_WRITE_RESULT int wpe_write_file(struct FNST *f, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_range(struct FNST *f, const char *path,
                                     int xa, int ya, int xe, int ye,
                                     int backup, WpeWriteOutcome *o);
/* The window saved and the user told: what F2 does.  @keep_name is the name
   the window had before a Save As, which a failed Save As leaves it under;
   NULL for a plain Save.  A plain Save writes into the file the window read
   (f->file_id); when the name now leads to another file it asks or refuses. */
int wpe_write_save(struct FNST *f, WpeWriteThen then, const char *keep_name);
/* The special bits that a write onto the existing file @path would not keep
   (0: none, not a regular file, or the window's own file, given by its
   identity, or NULL for a block): what the overwrite question names. */
mode_t wpe_write_overwrite_drops(const char *path, const WpeFileIdentity *own);

/* Text that is not a window's rows. */
WPE_WRITE_RESULT int wpe_write_fd(int fd, const char *text, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o);

/** wpe_write_save_text flag: a link at the end of @path is refused (the
    system's refusal of a link: ELOOP on Linux), never followed.  For a name
    the caller has already resolved and judged, so that a link put there
    afterwards cannot lead the write elsewhere. */
#define WPE_WRITE_NOFOLLOW 1

/* A whole text written into a file of the user's, in every respect as a Save
   writes it: the old file is copied to its .bak first, the write never
   truncates before the old bits are read, the file's own setuid, setgid and
   sticky bits are put back after the text is on disk, and a bit that could
   not be kept is named in the outcome.  For what the assistant writes into
   the user's files.  @own is the identity of the window open on that file,
   or NULL when none is (then the file at the name is the file); a window
   whose file is not the one at the name is refused, with nothing written:
   this face has no one to ask, and a caller that can show the question asks
   first, as Save does.  A name that is not there is made with mode 0666 less the
   umask (WRITE_NEW_FILE_MODE), as every new file of the editor. */
WPE_WRITE_RESULT int wpe_write_save_text(const char *path, const char *text,
                                         const WpeFileIdentity *own, int flags,
                                         WpeWriteOutcome *o);
/* A revert: the before image's text put back with the twelve permission bits
   recorded with it (the nine before the text, the special ones after it is on
   disk).  @path is a name the caller has already resolved; it is opened with
   WPE_WRITE_NOFOLLOW, so a link at its end is refused.  @own as above.  A
   name that is not there is made as wpe_write_save_text makes it, with
   0666 less the umask and then the recorded nine bits.  No .bak is made: the
   before image the text comes from is the copy. */
WPE_WRITE_RESULT int wpe_write_restore_text(const char *path, const char *text, mode_t recorded,
                                            const WpeFileIdentity *own, WpeWriteOutcome *o);

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
   open file or folder.  set_perm and add_perm are for a name the user gave
   (the Attributes dialog, Ctrl-F9): the file is opened once, never through a
   link that took the name since, and the mode is read from and given to that
   very file, so its special bits are the ones it had.  set_perm replaces the
   nine permission bits; add_perm adds bits to the twelve.  set_perm takes the
   stat the dialog showed (NULL when none), add_perm the identity of the file
   the window read (NULL when none): only that file, by device and inode, is
   changed.  When another file has taken the name, k->replaced says
   so and the errno is not 0. */
struct stat;
int  wpe_write_sync_before_bits(int fd, mode_t mode, int durable);
int  wpe_write_set_special_bits(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_mode_fd(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_perm(const char *path, const struct stat *shown, mode_t nine, WpeModeKept *k);
int  wpe_write_add_perm(const char *path, const WpeFileIdentity *own, mode_t add, WpeModeKept *k);
int  wpe_write_caller_in_group(gid_t gid);
void wpe_write_kept_merge(WpeModeKept *to, const WpeModeKept *from);
/* "setgid bit", "setuid and setgid bits"; and why they were not kept. */
void wpe_write_bits_words(char *out, size_t n, mode_t bits);
void wpe_write_bits_reason(char *out, size_t n, const WpeModeKept *k);
void wpe_write_guard_enter(void);
void wpe_write_guard_leave(void);

/* The one message. */
void wpe_write_say(struct FNST *f, const WpeWriteOutcome *o, WpeWriteThen then);
int  wpe_write_sentence(char *out, size_t n, const WpeWriteOutcome *o, WpeWriteThen then);

#endif /* WE_WRITE_H */
