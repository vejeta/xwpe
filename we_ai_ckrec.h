/* we_ai_ckrec.h -- what the assistant's checkpoint knows about each path.

   A checkpoint is taken before a run and read again when the run is over.
   Both moments are kept here, in one record, and the record is the only code
   that says a path was created, removed, moved, replaced, stayed, or that
   nobody can say.  A name that is missing from a list has four possible
   causes (it was not there, the walk was refused, the walk was cut, or the
   object is there under another name); every cause is kept, so a refusal or a
   bound after the run is on record like one before it, and "the review saw
   everything" is said only when nothing was left unseen.

   A name is judged by the object behind it (we_seen.h), never by its spelling
   alone: an object the checkpoint saw is not called new under another name.

   Plain C: libc, we_read.h, we_seen.h and we_path.h only, no curses and no
   editor type, so a unit test links it alone, as it does we_read.c. */
#ifndef WE_AI_CKREC_H
#define WE_AI_CKREC_H

#include <stddef.h>
#include <sys/types.h>

#include "we_path.h"
#include "we_read.h"
#include "we_seen.h"

#ifdef __GNUC__
#define WPE_CK_RESULT __attribute__((warn_unused_result))
#else
#define WPE_CK_RESULT
#endif

/** The most files a walk lists on one side.  On the "then" side every file
    counts; on the "now" side only a name the checkpoint did not know counts,
    so the files a run makes are found in a workspace that already held the
    bound. */
#define WPE_CK_WALK_MAX 2000
/** The room the row text of a verdict needs: Messages is 38 columns wide in
    the default layout and a row also carries a path and a line number. */
#define WPE_CK_ROW_MAX 28
/** How many notes a review says in the pane; the others are in the trace. */
#define WPE_CK_NOTES_SHOWN 6
/** The room for one sentence of the owner. */
#define WPE_CK_SENTENCE_MAX 1800
/** The image number of a verdict that has no before image. */
#define WPE_CK_NO_IMAGE (-1)

typedef struct WpeCkRecord WpeCkRecord;

/** The two moments: the checkpoint, and the review or the revert. */
typedef enum { WPE_CK_THEN, WPE_CK_NOW } WpeCkSide;

/** Why a place was not reached.  WPE_CK_WHY_NONE is "it was reached". */
typedef enum {
 WPE_CK_WHY_NONE,
 WPE_CK_WHY_REFUSED,        /* the folder or the file could not be opened */
 WPE_CK_WHY_BOUND,          /* the walk stopped at its bound */
 WPE_CK_WHY_NOT_LISTED,     /* a place the walk does not read by design */
 WPE_CK_WHY_ROOT_REPLACED,  /* the workspace folder is not the one held */
 WPE_CK_WHY_TOOL,           /* git could not give its list */
 WPE_CK_WHY_IMAGE_LOST      /* the before image cannot be read back */
} WpeCkWhy;

/** What the checkpoint or the review says about one path. */
typedef enum {
 WPE_CK_STAYED,            /* there then, there now; an edit is the caller's diff */
 WPE_CK_CREATED,           /* proven: the run made this object under this name */
 WPE_CK_REMOVED,           /* held then, absent now: r puts it back */
 WPE_CK_REMOVED_NO_COPY,   /* listed then, absent now: said, not put back */
 WPE_CK_MOVED,             /* an object the checkpoint saw, under another name now */
 WPE_CK_MOVED_HIDDEN,      /* the path is where it was; the object is under a hidden name now (from) */
 WPE_CK_LINKED,            /* an object the checkpoint saw, and its old name still holds it */
 WPE_CK_MAYBE_COPY,        /* new object as large as a name the run removed with no copy */
 WPE_CK_REPLACED,          /* listed then, another object under the name now */
 WPE_CK_NOT_READABLE_NOW,  /* held then, refused now: r puts the image back */
 WPE_CK_CANNOT_SAY         /* why says which place was not reached */
} WpeCkVerdictKind;

/** How a removed file's folder stands now: what r can do for it. */
typedef enum {
 WPE_CK_FOLDER_THERE,      /* the folder is there */
 WPE_CK_FOLDER_GONE,       /* the folder is not there */
 WPE_CK_FOLDER_IS_A_FILE   /* something that is not a folder is in its place */
} WpeCkFolderNow;

/** The verdict on one path. */
typedef struct {
 WpeCkVerdictKind kind;
 WpeCkWhy         why;          /* CANNOT_SAY and NOT_READABLE_NOW */
 const char      *path;         /* the name now (the name then, for a removal) */
 const char      *from;         /* MOVED, LINKED, MAYBE_COPY: the name then, else NULL */
 int              image;        /* the before image's number, or WPE_CK_NO_IMAGE */
 int              is_folder;    /* the verdict is about a folder */
 int              under_folder; /* MOVED: its folder moved too, and that verdict acts for it */
 WpeCkVerdictKind object;       /* REPLACED: what the object now at the name is (CREATED,
                                   MOVED, LINKED, MAYBE_COPY or CANNOT_SAY) */
 WpeCkFolderNow   folder_now;   /* REMOVED: where its folder stands */
 WpeReadOutcome   read;         /* the read owner's refusal, for the reason */
} WpeCkVerdict;

/** One place that was not reached, with the sentence that says so. */
typedef struct {
 WpeCkSide side;
 WpeCkWhy  why;
 char     *where;                    /* the folder or file, or the workspace */
 char      reason[WPE_PATH_MSG_MAX]; /* the read owner's words, or the tool's */
 int       count;                    /* BOUND: how many files the walk listed */
} WpeCkNote;

/** What the walk needs from its caller: nothing else reaches the editor. */
typedef struct {
 const char *skip;          /* the editor's state folder, resolved, or NULL */
 int         bound;         /* files per side before a walk stops; 0 is WPE_CK_WALK_MAX */
 void      (*trace)(const char *line);  /* a line for the run's trace, or NULL */
 /* A test seam: called before the walk enters the subfolders of a folder,
    so a test can swap one for a link at that moment.  The editor passes the
    hook that does nothing unless the test names it. */
 void      (*before_enter)(void);
} WpeCkAsk;

WpeCkRecord *wpe_ckrec_new(const WpeCkAsk *ask);
/* The workspace folder the caller holds open and checks itself; every other
   watched root is checked after the run. */
void         wpe_ckrec_hold_root(WpeCkRecord *r, const char *root);
/* The folder the next walks leave out (the editor's state folder), or NULL. */
void         wpe_ckrec_set_skip(WpeCkRecord *r, const char *skip);
void         wpe_ckrec_free(WpeCkRecord *r);

/* Fill one side: every watched folder is given to the walk. */
void wpe_ckrec_walk(WpeCkRecord *r, WpeCkSide side, const char *folder);

/* The checkpoint's before image of a file, read whole. */
void wpe_ckrec_then_image(WpeCkRecord *r, const char *path, int image, const WpeSeen *seen);
/* A name the checkpoint asked for and did not read: absent, or refused. */
void wpe_ckrec_then_asked(WpeCkRecord *r, const char *path, const WpeReadOutcome *o);

/* A tool's list (git's): each name is looked at, a folder the tool listed to
   the end is complete, a folder it ignored is a place nobody read. */
void wpe_ckrec_tool_begin(WpeCkRecord *r, WpeCkSide side, const char *folder);
void wpe_ckrec_listed(WpeCkRecord *r, WpeCkSide side, const char *path);
/* The same, for a file git ignores: it is present and known, and never a row. */
void wpe_ckrec_listed_ignored(WpeCkRecord *r, WpeCkSide side, const char *path);
void wpe_ckrec_complete(WpeCkRecord *r, WpeCkSide side, const char *folder);
void wpe_ckrec_unread(WpeCkRecord *r, WpeCkSide side, const char *folder);

/* The checkpoint starts again (a try that failed leaves nothing behind). */
void wpe_ckrec_then_begin(WpeCkRecord *r);
/* The review starts again: forget what the last "now" saw. */
void wpe_ckrec_now_begin(WpeCkRecord *r);
/* A place the "now" side could not reach, and why. */
void wpe_ckrec_unreached(WpeCkRecord *r, WpeCkSide side, WpeCkWhy why, const char *where,
                         const WpeReadOutcome *o);
/* A before image that cannot be read back. */
void wpe_ckrec_image_lost(WpeCkRecord *r, const char *path, const WpeReadOutcome *o);

/* A held file as it is now.  Returns its bytes; on NULL the verdict says
   removed or not readable. */
WPE_CK_RESULT char *wpe_ckrec_now_text(WpeCkRecord *r, int image, size_t *len, WpeCkVerdict *v);

/* Judge every path.  The verdicts are the record's own, sorted by path, and
   stay valid until the next fill.  Returns how many. */
WPE_CK_RESULT int wpe_ckrec_judge_all(WpeCkRecord *r, const WpeCkVerdict **v);
/* The verdict of one path, from the last judge.  Returns 1 when there is one. */
WPE_CK_RESULT int wpe_ckrec_verdict_of(WpeCkRecord *r, const char *path, WpeCkVerdict *v);
/* May a revert take this name away?  Only for a CREATED file, judged now and
   still the object the review judged. */
WPE_CK_RESULT int wpe_ckrec_may_remove(WpeCkRecord *r, const char *path, WpeCkVerdict *v);
/* May a revert put the checkpoint's copy back?  Gives the image's number. */
WPE_CK_RESULT int wpe_ckrec_may_restore(WpeCkRecord *r, const char *path, int *image);
/* How many places were left unseen: notes of both moments and paths nobody
   can say anything about. */
WPE_CK_RESULT int wpe_ckrec_unseen(WpeCkRecord *r);
/* 1 when nothing was left unseen. */
WPE_CK_RESULT int wpe_ckrec_whole(WpeCkRecord *r);
/* Is this a kind of verdict r leaves alone and says so (moved, linked, replaced ...)? */
int wpe_ckrec_kind_is_kept(WpeCkVerdictKind kind);
/* Is the path of a verdict of this kind not there now (so Alt-T has nothing to open)? */
int wpe_ckrec_kind_is_gone(WpeCkVerdictKind kind);

/* What was not reached, each with its cause.  Returns the count. */
int wpe_ckrec_notes(const WpeCkRecord *r, WpeCkSide side, const WpeCkNote **notes);

/* The words: one function each, so no other file spells a word of them. */
const char *wpe_ckrec_row_text(const WpeCkVerdict *v);
int wpe_ckrec_sentence(char *out, size_t n, const WpeCkVerdict *v);
int wpe_ckrec_note_sentence(char *out, size_t n, const WpeCkNote *note);
int wpe_ckrec_note_trace(char *out, size_t n, const WpeCkNote *note, const char *stage);
int wpe_ckrec_more_notes(char *out, size_t n, int more);
int wpe_ckrec_more_sentences(char *out, size_t n, int more);
int wpe_ckrec_header(char *out, size_t n, int changes, int unseen);
int wpe_ckrec_unseen_line(char *out, size_t n, int unseen);
int wpe_ckrec_last_line(char *out, size_t n, int failed, int kept, int unseen);
int wpe_ckrec_kept_question(char *out, size_t n, WpeCkVerdictKind kind, const char *shown);

#endif
