/* we_fileop.h -- the one owner of "copy, move, link or remove a user's
   file, and what happens when it fails".
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   A copy, move, link or removal either completes whole or leaves the source
   and any existing destination as they were, and the outcome says which.
   The destination is made new, with the source's permissions before it
   holds a byte, under a temporary name in its own folder, and reaches its
   name by one rename: a link at the destination is replaced as a name and
   never written through, a copy that stops leaves no half file, and a copy
   counts as done only when every write and the close returned 0.  A Move
   across file systems removes its source only after the copy is whole and
   on disk.  The names are this file's; the bytes go through the write
   owner (we_write.h).

   Plain C: no drawing and no key reads.  The questions go through a
   callback; the File Manager (we_fl_unix.c) fills it with its dialog and
   tells the user what the outcome says. */

#ifndef WE_FILEOP_H
#define WE_FILEOP_H

#include <stddef.h>
#include <sys/types.h>
#include "we_path.h"
#include "we_write.h"

/** The folder of the files the user removed, under the home folder. */
#define WPE_FILEOP_WASTEBASKET ".wastebasket"

/** A caller tests the status, never a number. */
typedef enum {
 WPE_FILEOP_DONE,       /* complete */
 WPE_FILEOP_DECLINED,   /* the policy or the user said no; nothing changed */
 WPE_FILEOP_FAILED      /* step and err say why; the sentence says what is left */
} WpeFileOpStatus;

typedef enum {
 WPE_FILEOP_COPY, WPE_FILEOP_MOVE, WPE_FILEOP_LINK, WPE_FILEOP_REMOVE,
 WPE_FILEOP_MKDIR, WPE_FILEOP_RMDIR, WPE_FILEOP_CHMOD
} WpeFileOpKind;

/** The step that stopped; the sentence names it. */
typedef enum {
 WPE_FILEOP_STEP_NONE,
 WPE_FILEOP_STEP_SOURCE,        /* opening or reading the source */
 WPE_FILEOP_STEP_CREATE,        /* making the copy in the destination's folder */
 WPE_FILEOP_STEP_WRITE,         /* the bytes of the copy */
 WPE_FILEOP_STEP_CLOSE,         /* closing the copy, or making its name durable */
 WPE_FILEOP_STEP_MODE,          /* the permissions */
 WPE_FILEOP_STEP_REPLACE,       /* the old destination could not go to the wastebasket */
 WPE_FILEOP_STEP_RENAME,        /* the final rename into place */
 WPE_FILEOP_STEP_REMOVE_SOURCE, /* a Move's source after a whole copy */
 WPE_FILEOP_STEP_LINK,
 WPE_FILEOP_STEP_MKDIR,
 WPE_FILEOP_STEP_RMDIR,
 WPE_FILEOP_STEP_REMOVE,        /* the removal itself */
 WPE_FILEOP_STEP_WASTEBASKET,   /* the wastebasket cannot be used */
 WPE_FILEOP_STEP_EXISTS,        /* DECLINED by "Don't overwrite": said on the status row */
 WPE_FILEOP_STEP_SAME_FILE,
 WPE_FILEOP_STEP_NOT_A_FILE,
 WPE_FILEOP_STEP_NOT_A_FOLDER,  /* a folder is wanted and the path, or an entry inside the destination, is something else, a link included */
 WPE_FILEOP_STEP_INSIDE,        /* a folder to be put inside itself */
 WPE_FILEOP_STEP_TOO_DEEP,      /* a tree deeper than WPE_FILEOP_MAX_DEPTH folders */
 WPE_FILEOP_STEP_CHANGED,       /* an entry changed between being judged and being used: the walk stopped, the counts are said */
 WPE_FILEOP_STEP_STOPPED,       /* a folder operation the user ended: the count is said */
 WPE_FILEOP_STEP_CANCELLED      /* the user answered Esc: no sentence */
} WpeFileOpStep;

/** What a copy could not keep: the special bits (setuid, setgid, sticky) the
    system did not let this user keep on what the operation made.  The
    operation is complete without them and the sentence says so.  A folder
    sums the entries it concerns and names the first. */
typedef struct {
 mode_t bits_lost;                 /* the bits missing on the first entry named in bits_path */
 int    bits_err;                  /* errno of the refusal, 0 for a silent drop */
 gid_t  bits_gid;                  /* that entry's group */
 int    bits_in_group;             /* the user is in that group */
 WpeModeOwners bits_owners;        /* the copy had another owner or group than its original */
 long   bits_lost_count;           /* entries with a bit not kept; 0 = nothing to say */
 char   bits_path[WPE_PATH_SIZE];  /* the first such entry */
} WpeFileOpBitsNote;

typedef struct {
 WpeFileOpStatus status;
 WpeFileOpKind   kind;
 WpeFileOpStep   step;
 int             err;                      /* errno, else 0 */
 int             err_link;                 /* a Link: errno of the hard link tried first, else 0 */
 char            src[WPE_PATH_SIZE];
 char            dst[WPE_PATH_SIZE];       /* where it went: inside a folder named as the destination */
 char            bad[WPE_PATH_SIZE];       /* the path that is not a file (STEP_NOT_A_FILE) */
 char            aside[WPE_PATH_SIZE];     /* where a replaced destination went, else "" */
 int             src_kept;                 /* the source is as it was */
 int             dst_kept;                 /* an existing destination is as it was */
 int             replaced_by_link;         /* STEP_CHANGED of a chmod: a link now holds the name */
 int             replaced_unsure;          /* STEP_CHANGED of a chmod: not proven to be the file shown, nor another */
 long            done, skipped, total;     /* a folder: entries done, left alone by the policy, all */
 WpeFileOpBitsNote note;                   /* the special bits the system did not keep, said after a DONE copy */
} WpeFileOpOutcome;

typedef enum { WPE_REPLACE_ASK, WPE_REPLACE_ALWAYS, WPE_REPLACE_NEVER } WpeReplaceRule;
typedef enum { WPE_REMOVE_WASTEBASKET, WPE_REMOVE_ASK, WPE_REMOVE_NOW } WpeRemoveRule;

/** What the question's callback answers. */
#define WPE_FILEOP_ANSWER_NO   0
#define WPE_FILEOP_ANSWER_YES  1
#define WPE_FILEOP_ANSWER_STOP 2

/** The policy by value: an operation inside a folder builds its own and
    never touches the user's options. */
typedef struct {
 WpeReplaceRule replace;
 WpeRemoveRule  remove;
 int            hardlink_first;            /* a Link tries a hard link before a symbolic one */
 int            recursive;                 /* a folder operation goes into subfolders; Remove always does */
 int          (*ask)(void *ctx, const char *question);  /* an answer above; NULL answers no */
 int          (*progress)(void *ctx, const char *verb, const char *src, const char *dst);
                                           /* before each entry of a folder: 1 stops; NULL never */
 void          *ctx;
} WpeFileOpPolicy;

/** The deepest tree a folder operation takes: deeper is refused before
    anything is changed, and said.  A Move that is one rename (a free name
    on the same file system) walks nothing and has no depth. */
#define WPE_FILEOP_MAX_DEPTH 15

/** Whoever drops the outcome of a file operation has lost the news that it
    did not happen. */
#define WPE_FILEOP_RESULT __attribute__((warn_unused_result))

/** The room for the sentence: three paths, a fourth for the bits note, a
    reason and the follow-up. */
#define WPE_FILEOP_SENTENCE_MAX (4 * WPE_PATH_SIZE + 320)

/* Each returns 0 for DONE and -1 otherwise; the outcome says which. */
WPE_FILEOP_RESULT int wpe_fileop_copy(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_move(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_link(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_remove(const char *path, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_mkdir(const char *path, mode_t mode, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_rmdir(const char *path, WpeFileOpOutcome *o);
/* A folder, whole: every entry, hidden ones and those the panel's pattern
   hides included.  The two names given are the only paths resolved; below
   them every entry is addressed by its folder's descriptor and its name, so
   a link, wherever it sits and whenever it appears, is a name and never a
   place: copied, moved, linked or removed as a name, never entered and
   never written through.  A link inside the destination where the source
   has a folder is refused (NOT_A_FOLDER, the walk stops with its counts);
   a file onto a link there is replaced as a name under the replace rule.  A
   source that changes under the walk gives CHANGED with the counts.  An
   existing destination folder is merged into, entry by entry, under the
   policy; permissions go on a folder only when the operation made it.  A
   policy that left entries alone gives DECLINED with the counts; a stop or
   a failure says how far it got. */
WPE_FILEOP_RESULT int wpe_fileop_copy_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_move_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
WPE_FILEOP_RESULT int wpe_fileop_link_tree(const char *src, const char *dst, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);
/* The tree and the folder itself, for good: the policy's Remove rule says
   whether each file is asked about (ASK) or not; it never goes to the
   wastebasket (a Move of the folder does that). */
WPE_FILEOP_RESULT int wpe_fileop_remove_tree(const char *path, const WpeFileOpPolicy *p, WpeFileOpOutcome *o);

/* perm: the nine permission bits; the file's setuid, setgid and sticky bits are
   kept when the system lets this user keep them, and the outcome's note names
   the one it did not.  The nine bits being refused is a failure. */
WPE_FILEOP_RESULT int wpe_fileop_chmod(const char *path, mode_t perm, WpeFileOpOutcome *o);
/* The same for the Attributes dialog: @shown is the record the dialog took of
   the file it showed (wpe_seen_look_name, a link followed); only that file,
   as the judge of we_seen.c says, is changed, however long the dialog stayed
   open.  Another file under the name is "replaced" and nothing is changed.
   @seen is what the name itself was then (wpe_write_name_seen), so that a
   link the dialog was opened on is not called a link that took the name;
   NULL when not known.  A call that passes anything but a WpeSeen (or NULL)
   as @shown does not compile (WPE_SHOWN_RECORD, we_write.h). */
WPE_FILEOP_RESULT int wpe_fileop_chmod_shown(const char *path, const WpeSeen *shown, const WpeNameSeen *seen,
                                             mode_t perm, WpeFileOpOutcome *o);
#ifndef WPE_FILEOP_DEFINING_SHOWN
# define wpe_fileop_chmod_shown(path, shown, seen, perm, o) \
   wpe_fileop_chmod_shown((path), WPE_SHOWN_RECORD(shown), (seen), (perm), (o))
#endif
/* A name moved where none is: a folder's rename, which rename(2) would let
   replace an empty folder that appeared in between. */
WPE_FILEOP_RESULT int wpe_fileop_rename_noreplace(const char *src, const char *dst, WpeFileOpOutcome *o);

/* The wastebasket: $HOME/.wastebasket, made 0700, opened without following a
   link and checked to be ours; a folder of ours that is open to others is
   closed to them. */
WPE_FILEOP_RESULT int wpe_fileop_wastebasket(char *out, size_t n, WpeFileOpOutcome *o);
/* A name for @path inside the wastebasket that is free: `name`, `name.1`, ... */
WPE_FILEOP_RESULT int wpe_fileop_wastebasket_slot(const char *path, char *out, size_t n, WpeFileOpOutcome *o);
/* @path is the wastebasket or lies inside it. */
int wpe_fileop_in_wastebasket(const char *path);

/** The mode a copy may be given from who owns it and who owned its original:
    setuid needs the same owner, setgid the same owner and group.  Pure, so a
    test can give it any pair of owners; the copy itself decides through it. */
mode_t wpe_fileop_copy_mode(mode_t wanted, uid_t src_uid, gid_t src_gid, uid_t copy_uid, gid_t copy_gid, WpeModeKept *dropped);

/* The one sentence, from the outcome alone.  Returns the length, 0 when
   there is nothing to say (DONE with every special bit kept, a question
   answered no, Esc).  A DONE copy that could not keep a special bit says
   which bit, where and why; a folder stopped or failed after such an entry
   adds the same note. */
int wpe_fileop_sentence(char *out, size_t n, const WpeFileOpOutcome *o);

#endif /* WE_FILEOP_H */
