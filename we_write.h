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
#include "we_seen.h"

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
 int    unsure;     /* the file could not be read after the write, so @lost is what the
                       write may have cleared, not what it was seen to clear */
 WpeModeOwners owners;  /* a bit dropped on purpose because the copy has another owner or group */
 mode_t asked;      /* the special bits the call asked for */
 int    unreached;  /* a change of mode by name: the name could not be reached, no call was made */
 int    replaced;   /* a change of mode by name: another file had taken the name since it was
                       read; that file was not the one whose bits were read */
 int    replaced_unsure; /* with replaced: the file was not proven to be the one named, and not proven
                       another either (no birth time): the sentence says "changed or replaced" */
 mode_t replaced_kind; /* with replaced: S_IFMT of what holds the name after the call (S_IFLNK
                       for a link); 0 when nothing does or it is not known.  It says what is
                       at the name now, not what the call reached: with changed set it is no
                       proof of what was changed. */
} WpeModeKept;

/** The file a window read, as the system knows it: one record of the owner of
    "is this the object I saw" (we_seen.h).  A window remembers it from the
    moment it read its file, so a Save writes into that file or asks, and never
    decides by looking at the name again; the judge says SAME, OTHER or UNSURE
    and no caller reads a number.  known == 0 for a window that read no file (a
    new one, a clipboard, a file that did not exist). */
typedef WpeSeen WpeFileIdentity;

/** What a name was, itself and not what it leads to, when the user was shown
    it: a record of the owner of "is this the object I saw" with the text of a
    link (we_seen.h).  Not known when no one looked (Ctrl-F9, a caller with
    no dialog). */
typedef WpeSeenName WpeNameSeen;

/** The argument that is the file the user was shown (the Attributes dialog,
    Ctrl-F9): a WpeSeen, or NULL.  The build accepts incompatible pointers, so
    a struct stat handed over in its place would compile and be compared by
    number; through this check it does not compile.  The calls of
    wpe_write_set_perm, wpe_write_set_perm_seen, wpe_write_add_perm and
    wpe_fileop_chmod_shown go through it. */
#define WPE_SHOWN_RECORD(p) _Generic((p), const WpeSeen *: (p), WpeSeen *: (p), void *: (p))

/** Why a file that is not proven to be the one the window read is refused
    with Save As as the way out and not asked about (the reason the sentence
    gives).  ASKABLE is no refusal: a plain file of the user's own, which the
    user is asked about. */
typedef enum {
 WPE_UNSURE_ASKABLE,
 WPE_UNSURE_SPECIAL_BITS,    /* it has a setuid, setgid or sticky bit */
 WPE_UNSURE_OTHER_OWNER,     /* it belongs to another user */
 WPE_UNSURE_NOT_REGULAR      /* it is not a regular file */
} WpeUnsureWhy;

/** What a caller saw at a name before it asked the user or decided to write:
    whether a file was there and, if so, which one (device and inode), how many
    names it had and whether it was a regular file.  The write owner holds the
    file it opens up against this, so a file put under the name afterwards (a
    swapped file, a hard link made in the meantime) is refused and never
    written into.  judged == 0 means no one looked and nothing is compared;
    judged == 1 with exists == 0 means the name was free, and the write must
    make the file or be refused. */
typedef struct {
 int     judged;
 int     exists;
 WpeSeen seen;       /* the look at the name itself, judged against the opened file */
 nlink_t nlink;
 int     regular;
} WpeWriteExpect;

typedef struct {
 WpeWriteStatus status;
 int err;                        /* errno of the failure, else 0 */
 int other_file;                 /* the name leads to another file than the one the
                                    window read: nothing was written, the save is
                                    refused (the file is not the user's own) */
 int unsure;                     /* with other_file, swapped or ask_overwrite: the file
                                    could not be proven to be another one, only not
                                    proven to be the one seen (its stamps moved and
                                    the system has no birth time to settle it).  The
                                    sentence says "changed or replaced", never "another
                                    file took the name" */
 WpeUnsureWhy unsure_why;        /* with other_file and unsure: why no question is asked */
 int changed_while_asked;        /* with other_file: the user had said Yes about one
                                    file, and by the time of the write the name led
                                    to yet another: the sentence says so */
 int not_regular;                /* a write with no one to ask found something that is
                                    not a regular file (a device, a terminal, a pipe)
                                    under the name: nothing was written */
 mode_t not_regular_kind;        /* with not_regular: S_IFMT of what was found (S_IFIFO,
                                    S_IFCHR, S_IFSOCK, ...); 0 when it is not known.
                                    Read from the file's own mode, never from an errno */
 int not_regular_tty;            /* with not_regular: it is a terminal (isatty on the
                                    opened file), said as "a terminal" */
 int planted;                    /* a copy under a name the editor chose (.ASV, .ESV, the
                                    post-edit copy): something that is not an old copy of
                                    ours (a link, another user's file, a folder) holds the
                                    name, so nothing was made and nothing written */
 int no_place;                   /* a file of the editor's own folder: there is no folder to
                                    put it in (no home folder, no run folder), so nothing
                                    was made and nothing written */
 int vanished;                   /* the caller looked at a file and it was removed before
                                    the open: nothing was made and nothing written */
 int swapped;                    /* the file the write reached is not the one the caller
                                    looked at (another file under the name, or a name
                                    that was free and is not): nothing was written */
 int gained_name;                /* the file has more names than when the caller looked:
                                    nothing was written */
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
 int replaced_by_link;           /* the open of a name that is never followed (WPE_WRITE_NOFOLLOW)
                                    failed and the name is a link: err is ELOOP, whatever
                                    the system called the refusal; nothing was created */
 int restore_err;                /* the write failed after the open and the old content
                                    is not back: the restore's errno, or ENOENT when
                                    no .bak was made (bak_path is then "") */
 int read_only;                  /* the window is read-only: nothing was tried */
 int not_a_file;                 /* the window has no file behind it (the assistant's
                                    composer, its pane, a report): nothing was tried,
                                    the sentence points to Save As */
 int kept;                       /* a copy went elsewhere: kept_path */
 char kept_path[WPE_PATH_SIZE];
 char bak_path[WPE_PATH_SIZE];   /* the .bak made in this call, else "" */
 char keep_name[WPE_PATH_SIZE];  /* the name the window keeps (Save As), else "" */
 WpeModeKept file_kept;          /* the special bits the saved file did not keep
                                    (lost == 0: all kept, or it had none).  The
                                    .bak is never given them, so it has no such
                                    record. */
 WpeModeKept file_stays;         /* the special bits a copy was to drop and, read back
                                    after the text, still has (lost) and why the call
                                    that was to drop them failed (err): a copy over a
                                    file the user does not own.  lost == 0: none */
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
 WPE_WRITE_THEN_USER_KEYS_STAY, /* "Your own settings (hook, engine command,
                                  endpoints, dial) are still in it; take them
                                  out before you share the file." */
 WPE_WRITE_THEN_NOTHING_MORE, /* nothing: the caller has no text of the user's
                                  to keep (the assistant's write: the file is as
                                  it was, and the sentence says so itself) */
 WPE_WRITE_THEN_PUT_BACK      /* a revert: it puts text back and says "Cannot put
                                  back", never advice about Save As */
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
void wpe_write_identity_take_fd(WpeFileIdentity *id, int fd);
void wpe_write_identity_forget(WpeFileIdentity *id);
int  wpe_write_identity_of_name(const char *dir, const char *name, WpeFileIdentity *id);
/* A window takes the identity a write reported (WpeWriteOutcome.written), and
   three questions about a look made now (we_seen.h): is this window's file
   proven to be that object (is), may it be that object (may_be: no proof
   against it, for finding the window of a file) and does the window know its
   file and fail to find it there (differs). */
void wpe_write_identity_set(WpeFileIdentity *id, const WpeFileIdentity *from);
int  wpe_write_identity_is(const WpeFileIdentity *id, const WpeSeen *now);
int  wpe_write_identity_may_be(const WpeFileIdentity *id, const WpeSeen *now);
int  wpe_write_identity_differs(const WpeFileIdentity *id, const WpeSeen *now);
/* May a file that took a window's name be written on a Yes: a plain file of the
   user's own with no special bit.  Anything else the owner refuses at once. */
int  wpe_write_replacement_may_be_asked(const char *path);

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
/* A window the editor writes itself (the assistant's composer and pane, a
   named report) has no file behind it: its title is a label, never a name.
   The owner refuses to write it under that title (a Save, a copy for the
   emergency or autosave), and the quit and close prompts do not ask about
   it.  A Save As to a name the user types writes there and makes the window
   that file's.  wpe_write_window_is_a_file says whether a window has a file. */
void wpe_write_window_set_not_a_file(struct FNST *f);
int  wpe_write_window_is_a_file(const struct FNST *f);
/* The special bits of the existing file @path, split by what a write onto it
   would do: @drops, the ones the copy will not keep, and @stays, the ones it
   cannot take off because the user does not own the file.  Both 0 when it is
   not a regular file, has none, or is the window's own file (given by its
   identity, or NULL for a block).  What the overwrite question names; a
   prediction by the rule of chmod, and the status row after the write says
   what happened. */
void wpe_write_overwrite_bits(const char *path, const WpeFileIdentity *own, mode_t *drops,
                              mode_t *stays);

/* A copy of a window's whole text under a name the editor chose beside the
   user's file (the autosave's .ASV, the emergency save's .ESV, the post-edit
   copy the hook works on).  An old copy of ours (a regular file of this user)
   is replaced; anything else under the name (a link, another user's file) is
   left alone and said (o->planted).  The new file is made with O_EXCL and
   O_NOFOLLOW, with the window's nine permission bits before its first byte,
   so a private file's copy is private from the start, and never given a
   special bit.  No .bak is made.  o->path is the file written. */
WPE_WRITE_RESULT int wpe_write_copy(struct FNST *f, const char *path, WpeWriteOutcome *o);
/* The copies a window itself made or read in this session: the .ASV its
   autosave wrote last, and the .ESV it was recovered from.  Only these are
   removed when the window closes; a copy left by an earlier session, or one
   the user made, is never taken for ours.  Forgotten after each allocation
   of a window (wpe_write_copies_forget). */
typedef struct {
 WpeFileIdentity asv;
 WpeFileIdentity esv;
} WpeWindowCopies;
void wpe_write_copies_forget(WpeWindowCopies *c);
/* Remove a copy when its window closes: only the file with identity @own (the
   one this window wrote or read this session), and only while it is still a
   regular file of this user.  A link at the name, another user's file, a file
   that took the name since, or an unknown @own leaves the name alone. */
void wpe_write_copy_remove(const char *path, const WpeFileIdentity *own);

/* Text that is not a window's rows. */
WPE_WRITE_RESULT int wpe_write_fd(int fd, const char *text, WpeWriteOutcome *o);
WPE_WRITE_RESULT int wpe_write_text(const char *path, const char *text, WpeWriteOutcome *o);

/* The editor's own folders and the files it keeps in them: the conversation,
   the before images and the recorded modes of a run, the lists git reads.
   A folder of the editor's own is one of four (the state folder, this
   run's private folder, the configuration folder, and the old ~/.xwpe the
   settings are moved out of); its base is resolved by
   name in we_places.c and followed, and only what is below it is judged.
   Nothing here takes a path: a caller names a kind, a folder below it and a
   file in it, so a name the editor did not judge cannot be written.

   The folders above the base are made 0700 if missing and never judged (a
   link there is the user's choice).  The base is opened as a folder,
   following a link, and judged on that descriptor: another user's folder or a
   file is refused, and one that others can read or write is made 0700.
   Below the base each component is made 0700 if missing and judged without
   following links: a link, another user's folder or a file is refused, and
   one that others can read or write is made 0700. */
typedef enum {
 WPE_OWN_STATE,     /* $XDG_STATE_HOME/xwpe (wpe_state_dir) */
 WPE_OWN_RUN,       /* the folder of this run (wpe_run_dir) */
 WPE_OWN_CONFIG,    /* $XDG_CONFIG_HOME/xwpe (wpe_config_dir) */
 WPE_OWN_LEGACY     /* <home>/.xwpe (wpe_legacy_dir), the migration's note */
} WpeOwnKind;

/* WPE_OWN_MAKE judges and makes what is missing; WPE_OWN_LOOK judges and
   tightens but never makes, so a reader never leaves a folder behind: a
   missing folder is o->err ENOENT with no sentence. */
typedef enum {
 WPE_OWN_MAKE,
 WPE_OWN_LOOK
} WpeOwnHow;

/* The path of a judged folder, for a caller that must name it to a child
   (git --pathspec-from-file, a mkstemp template).  It is an answer only:
   nothing accepts one back, so a path nobody judged cannot be written. */
typedef struct {
 char path[WPE_PATH_SIZE];
} WpeOwnDir;

/* Resolve and judge the folder of @kind, below it @sub (NULL, one component
   or two joined by '/'; no "." or ".."), as above.  Return 0 with d->path,
   or -1 with the outcome: o->no_place when there is no base at all (no home
   folder), o->planted with the component's path when something that is not
   the user's own folder is in the way (err EEXIST, ENOTDIR for a file), and
   o->err ENOENT for a LOOK at a folder that is not there. */
WPE_WRITE_RESULT int wpe_own_dir(WpeOwnKind kind, const char *sub, WpeOwnHow how,
                                 WpeOwnDir *d, WpeWriteOutcome *o);
/* A file of the editor's own: @leaf is one component (no '/', not "." or
   ".."; else EINVAL).  An old regular file of this user is replaced; anything
   else there (a link, another user's file, a folder) is left alone and said
   (o->planted).  The new file is made with O_EXCL and O_NOFOLLOW, 0600 from
   its first byte, so the inode written is one this call created.  @len bytes,
   NUL bytes included. */
WPE_WRITE_RESULT int wpe_write_own_bytes(WpeOwnKind kind, const char *sub, const char *leaf,
                                         const char *bytes, size_t len, WpeWriteOutcome *o);
/* Remove such a file: only a regular file of this user, else nothing. */
void wpe_write_own_remove(WpeOwnKind kind, const char *sub, const char *leaf);

/** wpe_write_save_text flag: a link at the end of @path is refused (the
    o->err is ELOOP, POSIX's word for O_NOFOLLOW on a link, and
    o->replaced_by_link is 1, on every system; callers read the flag), never
    followed.  For a name
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
   umask (WRITE_NEW_FILE_MODE), as every new file of the editor.

   With WPE_WRITE_NOFOLLOW the file is opened without waiting (a pipe with no
   reader is never waited for) and anything that is not a regular file is
   refused from its own mode (o->not_regular and o->not_regular_kind); a pipe,
   a socket or a device is never written.  @expect is what the caller saw at
   the name (NULL: nothing is compared): the file opened must be that file,
   with no more names than it had, and a name that was free must be made by
   this write; otherwise nothing is written (o->swapped, o->gained_name). */
WPE_WRITE_RESULT int wpe_write_save_text(const char *path, const char *text,
                                         const WpeFileIdentity *own, int flags,
                                         const WpeWriteExpect *expect, WpeWriteOutcome *o);
/* The same Save of a text, for a caller that can ask the user when the file
   the window read was replaced on disk by another plain file of the user's
   own (no setuid, setgid or sticky bit).  @asked NULL: the first pass, which
   writes nothing and sets o->ask_overwrite with the file in o->asked; the
   caller shows its question and, on a Yes, calls again with that identity in
   @asked, and the text is written as a copy (no special bit; the window takes
   the file as its own).  Any other file under the name, a program, another
   user's file or a file that is not a regular one, is refused as before. */
WPE_WRITE_RESULT int wpe_write_save_text_ask(const char *path, const char *text,
                                             const WpeFileIdentity *own, int flags,
                                             const WpeFileIdentity *asked,
                                             const WpeWriteExpect *expect, WpeWriteOutcome *o);
/** WPE_WRITE_NO_RECORDED_MODE as @recorded: no mode was recorded for the
    before image, so the file keeps the bits it has now. */
#define WPE_WRITE_NO_RECORDED_MODE ((mode_t)-1)
/* A revert: the before image's text (@len bytes, which may hold NUL bytes)
   put back with the twelve permission bits recorded with it (the nine before
   the text, the special ones after it is on disk), or, with
   WPE_WRITE_NO_RECORDED_MODE, with the bits the file has now: a file that
   only a command changed is never given another file's mode.  @path is a name
   the caller has already resolved; it is opened with WPE_WRITE_NOFOLLOW, so a
   link at its end is refused, and a pipe or a device is refused from its own
   mode.  @own and @expect as above.  A name that is not there is made as
   wpe_write_save_text makes it, with 0666 less the umask and then the
   recorded nine bits.  No .bak is made: the before image the text comes from
   is the copy. */
WPE_WRITE_RESULT int wpe_write_restore_text(const char *path, const char *text, size_t len,
                                            mode_t recorded, const WpeFileIdentity *own,
                                            const WpeWriteExpect *expect, WpeWriteOutcome *o);

/* A file that is printed line by line (a project list, the options): the
   stream is opened here and closed here, and the outcome of the close says
   whether the last byte reached the disk.  A caller that goes through
   fprintf in between never judges the stream itself. */
WPE_WRITE_RESULT FILE *wpe_write_stream_open(const char *path, WpeWriteOutcome *o);
/* The same with a flag (WPE_WRITE_NOFOLLOW, defined below): a link at the
   name is refused instead of followed.  For a name a folder the user did not
   make can choose (the ./xwperc of a cloned project, its project list). */
WPE_WRITE_RESULT FILE *wpe_write_stream_open_flags(const char *path, int flags,
                                                   WpeWriteOutcome *o);
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
   record the dialog took of the file it showed (a WpeSeen, NULL when none),
   add_perm the record of the file the window read (NULL when none): only that
   file, as wpe_seen_judge says, is changed.  add_perm refreshes that record
   when it succeeds, because the change moved the file's status-change time.
   When another file has taken the name, k->replaced says so and the errno is
   not 0. */
int  wpe_write_sync_before_bits(int fd, mode_t mode, int durable);
int  wpe_write_set_special_bits(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_mode_fd(int fd, mode_t mode, WpeModeKept *k);
int  wpe_write_set_perm(const char *path, const WpeSeen *shown, mode_t nine, WpeModeKept *k);
/* The same for a dialog that took wpe_write_name_seen of the name when it
   opened (NULL: not known, and then a link at the name is never claimed to
   have taken it). */
void wpe_write_name_seen(const char *path, WpeNameSeen *seen);
int  wpe_write_set_perm_seen(const char *path, const WpeSeen *shown, const WpeNameSeen *seen,
                             mode_t nine, WpeModeKept *k);
int  wpe_write_add_perm(const char *path, WpeFileIdentity *own, mode_t add, WpeModeKept *k);
#ifndef WPE_WRITE_DEFINING_SHOWN
# define wpe_write_set_perm(path, shown, nine, k) \
   wpe_write_set_perm((path), WPE_SHOWN_RECORD(shown), (nine), (k))
# define wpe_write_set_perm_seen(path, shown, seen, nine, k) \
   wpe_write_set_perm_seen((path), WPE_SHOWN_RECORD(shown), (seen), (nine), (k))
# define wpe_write_add_perm(path, own, add, k) \
   wpe_write_add_perm((path), WPE_SHOWN_RECORD(own), (add), (k))
#endif
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
/* What a save that worked could not keep, as a clause with no capital and no
   full stop ("the setgid bit was not kept: Operation not permitted"); 0 and
   "" when it kept everything. */
int  wpe_write_written_clause(char *out, size_t n, const WpeWriteOutcome *o);

#endif /* WE_WRITE_H */
