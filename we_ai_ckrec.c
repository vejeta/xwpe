/* we_ai_ckrec.c -- the checkpoint's record of what it knows about each path.

   See we_ai_ckrec.h.  One record holds both moments, the checkpoint (then)
   and the review or the revert (now).  The same walk fills both, so a
   refusal or a bound after the run is on record like one before it, and a
   verdict is made from the two states and from the identity of the object,
   never from "the name is not in the before list". */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "we_ai_ckrec.h"
#include "we_fmt.h"

/** The noun of the read owner's sentences for what the walk opens. */
#define CK_READ_WHAT "file"
/** The most a held file is read back for the review (as the before image). */
#define CK_TEXT_MAX 4000000
/** The step the entry arrays grow by. */
#define CK_GROW_FIRST 64

/** What is known about one name at one moment.  A new entry knows nothing. */
typedef enum { CK_UNKNOWN, CK_ABSENT, CK_LISTED, CK_HELD } CkState;

/** How far a folder was read. */
typedef enum { CK_F_NONE, CK_F_LISTED, CK_F_CUT, CK_F_REFUSED, CK_F_ABSENT, CK_F_UNREAD } CkFolder;

typedef struct {
 char    *path;
 CkState  state;
 WpeCkWhy why;            /* UNKNOWN: why the name was not reached */
 int      is_folder;
 CkFolder folder;         /* a folder: how far it was read */
 int      hidden;         /* a source of identity only: never a row */
 int      ignored;        /* a file git ignores: present and known, never a row */
 int      root_link;      /* a watched root: it was a link at the checkpoint */
 int      claimed_visible;/* the judge: a listed name now holds this object */
 const char *hidden_to;   /* the judge: a hidden name now holds this object */
 int      image;          /* HELD: the before image's number */
 int      claimed;        /* the judge: an object seen under this name was found elsewhere */
 int      claimed_proven; /* the judge: a listed name proved the object is this one; once set, kept */
 WpeSeen  seen;
} CkEntry;

/** One moment's names, in the order they came, and the same names in path order. */
typedef struct {
 CkEntry *e;
 int      n, cap;
 int     *order;          /* indexes into e, sorted by path */
 int      counted;        /* files that count against the bound */
 int      hidden_counted;
 int      hidden_noted;   /* the hidden cap note was made */
 int      bound_noted;    /* the bound note was made */
 WpeCkWhy cause;          /* why nothing at all was reached, when that is so */
} CkSide;

struct WpeCkRecord {
 WpeCkAsk      ask;
 char         *skip;
 CkSide        side[2];
 char         *tool_root[2]; /* the folder a tool listed, so a listed name's folders are known */
 char         *held_root;    /* the workspace folder: the caller holds it open and checks it itself */
 WpeCkNote    *notes[2];    /* the places not reached, one list per side */
 int           nnotes[2], notes_cap[2];
 WpeCkVerdict *v;
 int           nv;
 int           judged;
 int           unsaid;     /* paths nobody can say anything about */
};

/* ---- the spelling of a path ---------------------------------------------- */

/**
 * ckrec_spell - The one spelling of a path in the record.
 * @in: a path as a caller has it; @out/@sz: the result.
 * Return: 0, or -1 when it does not fit.
 *
 * Runs of "/" fold into one and a trailing "/" goes, so "dir//name" and
 * "dir/name" are one entry and the same file is never in the record twice.
 */
static int ckrec_spell(const char *in, char *out, size_t sz)
{
 size_t w = 0;
 int slash = 0;
 for (; *in; in++) {
  if (*in == '/' && slash) continue;
  if (w + 1 >= sz) return -1;
  out[w++] = *in;
  slash = *in == '/';
 }
 while (w > 1 && out[w - 1] == '/') w--;
 out[w] = '\0';
 return 0;
}

/**
 * ckrec_parent - Cut a spelled path down to its folder.
 * @p: the path, changed in place.
 * Return: 1 when a folder is left, 0 at the top.
 */
static int ckrec_parent(char *p)
{
 char *sl = strrchr(p, '/');
 if (!sl || sl == p) return 0;
 *sl = '\0';
 return 1;
}

/** ckrec_strdup - A copy of a string, NULL when memory ran out (the record never aborts). */
static char *ckrec_strdup(const char *s)
{
 size_t n = strlen(s) + 1;
 char *d = malloc(n);
 if (d) memcpy(d, s, n);
 return d;
}

/* ---- one side's entries ---------------------------------------------------- */

static CkSide *ck_side(WpeCkRecord *r, WpeCkSide s) { return &r->side[s]; }

/**
 * ck_locate - Find a path among a side's entries by bisection.
 * @s: the side; @path: spelled; @pos: receives where it would go in `order`.
 * Return: the entry's index, or -1.
 */
static int ck_locate(const CkSide *s, const char *path, int *pos)
{
 int lo = 0, hi = s->n - 1;
 while (lo <= hi) {
  int mid = (lo + hi) / 2, c = strcmp(path, s->e[s->order[mid]].path);
  if (c == 0) { if (pos) *pos = mid; return s->order[mid]; }
  if (c < 0) hi = mid - 1; else lo = mid + 1;
 }
 if (pos) *pos = lo;
 return -1;
}

/** ck_find - The entry of a spelled path on one side, or NULL: how every rule asks "what is known of this name". */
static CkEntry *ck_find(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 CkSide *s = ck_side(r, side);
 int i = ck_locate(s, path, NULL);
 return i < 0 ? NULL : &s->e[i];
}

/** ck_grow - Make room for one more entry on a side; -1 when memory ran out. */
static int ck_grow(CkSide *s)
{
 CkEntry *ne;
 int *no, cap;
 if (s->n < s->cap) return 0;
 cap = s->cap ? s->cap * 2 : CK_GROW_FIRST;
 ne = realloc(s->e, (size_t)cap * sizeof *ne);
 if (!ne) return -1;
 s->e = ne;
 no = realloc(s->order, (size_t)cap * sizeof *no);
 if (!no) return -1;
 s->order = no;
 s->cap = cap;
 return 0;
}

/**
 * ck_put - Make an entry for a path, or find the one that is there.
 * @r: the record; @side: which moment; @path: spelled; @created: receives 1
 * when the entry is new.
 * Return: the entry, NULL when memory ran out.
 */
static CkEntry *ck_put(WpeCkRecord *r, WpeCkSide side, const char *path, int *created)
{
 CkSide *s = ck_side(r, side);
 CkEntry *e;
 int pos, i = ck_locate(s, path, &pos);
 if (created) *created = 0;
 if (i >= 0) return &s->e[i];
 if (ck_grow(s) != 0) return NULL;
 e = &s->e[s->n];
 memset(e, 0, sizeof *e);
 e->path = ckrec_strdup(path);
 if (!e->path) return NULL;
 e->image = WPE_CK_NO_IMAGE;
 memmove(&s->order[pos + 1], &s->order[pos], (size_t)(s->n - pos) * sizeof *s->order);
 s->order[pos] = s->n;
 s->n++;
 if (created) *created = 1;
 return e;
}

/** ck_side_free - Forget everything one side saw (a new checkpoint, or a review that starts again). */
static void ck_side_free(CkSide *s)
{
 int i;
 for (i = 0; i < s->n; i++) free(s->e[i].path);
 free(s->e);
 free(s->order);
 memset(s, 0, sizeof *s);
}

/** Is a state one that holds an object the checkpoint looked at? */
static int ck_seen_state(const CkEntry *e)
{
 return e->state == CK_HELD || e->state == CK_LISTED;
}

/** Are two looks the same object, as far as anyone can prove it is another? */
static int ck_not_other(const CkEntry *a, const CkEntry *b)
{
 if (!wpe_seen_known(&a->seen) || !wpe_seen_known(&b->seen)) return 1;
 return wpe_seen_judge(&a->seen, &b->seen) != WPE_SEEN_OTHER;
}

/* ---- notes ---------------------------------------------------------------- */

/** ck_say_trace - One line for the run's trace, through the caller's sink (none in a unit test). */
static void ck_say_trace(const WpeCkRecord *r, const char *fmt, const char *arg)
{
 char line[WPE_PATH_SIZE + 120];
 if (!r->ask.trace) return;
 WPE_FMT_SHOWN(line, fmt, arg);
 r->ask.trace(line);
}

/**
 * ck_note - Keep one place that was not reached.
 * @r: the record; @side: which moment; @why: the cause; @where: the place;
 * @reason: the words of the refusal, or NULL; @count: for a bound, the files.
 */
static void ck_note(WpeCkRecord *r, WpeCkSide side, WpeCkWhy why, const char *where,
                    const char *reason, int count)
{
 WpeCkNote *n;
 if (r->nnotes[side] == r->notes_cap[side]) {
  int cap = r->notes_cap[side] ? r->notes_cap[side] * 2 : 8;
  WpeCkNote *nn = realloc(r->notes[side], (size_t)cap * sizeof *nn);
  if (!nn) return;
  r->notes[side] = nn;
  r->notes_cap[side] = cap;
 }
 n = &r->notes[side][r->nnotes[side]];
 memset(n, 0, sizeof *n);
 n->where = ckrec_strdup(where ? where : "");
 if (!n->where) return;
 n->side = side;
 n->why = why;
 n->count = count;
 if (reason) WPE_FMT_SHOWN(n->reason, "%s", reason);
 r->nnotes[side]++;
}

/** ck_note_refusal - A note for a place the system or the read owner refused, with their words as the reason. */
static void ck_note_refusal(WpeCkRecord *r, WpeCkSide side, const char *where, const WpeReadOutcome *o)
{
 char reason[WPE_PATH_MSG_MAX];
 (void)wpe_read_reason(reason, sizeof reason, o);
 ck_note(r, side, WPE_CK_WHY_REFUSED, where, reason, 0);
}

/* ---- the walk -------------------------------------------------------------- */

/** ck_bound - How many files a walk lists on one side: the caller's bound, or WPE_CK_WALK_MAX. */
static int ck_bound(const WpeCkRecord *r)
{
 return r->ask.bound > 0 ? r->ask.bound : WPE_CK_WALK_MAX;
}

/** The name of an entry below a folder, spelled. */
static int ck_join(const char *dir, const char *name, char *out, size_t sz)
{
 int at_top = dir[0] == '/' && dir[1] == '\0';
 if (wpe_fmt_whole(out, sz, "%s/%s", at_top ? "" : dir, name) != WPE_FMT_OK) return -1;
 return 0;
}

/** Does the now walk go into this folder?  Not into one the checkpoint could not read. */
static int ck_now_may_enter(WpeCkRecord *r, const char *path)
{
 CkEntry *t = ck_find(r, WPE_CK_THEN, path);
 if (!t || !t->is_folder) return 1;
 return t->folder == CK_F_LISTED || t->folder == CK_F_ABSENT;
}

/** Did the checkpoint know this name, as a file or a folder it looked at? */
static int ck_then_knows(WpeCkRecord *r, const char *path)
{
 CkEntry *t = ck_find(r, WPE_CK_THEN, path);
 return t && ck_seen_state(t);
}

/**
 * ck_add_entry - Note one entry a folder listed.
 * @r/@side: the record and the moment; @path: spelled; @seen: its look;
 * @hidden: a source of identity only.
 * Return: the entry, NULL when memory ran out.
 */
static CkEntry *ck_add_entry(WpeCkRecord *r, WpeCkSide side, const char *path, const WpeSeen *seen,
                             int hidden)
{
 int created = 0;
 CkEntry *e = ck_put(r, side, path, &created);
 if (!e) return NULL;
 if (e->state == CK_HELD) return e;
 e->state = CK_LISTED;
 e->why = WPE_CK_WHY_NONE;
 e->seen = *seen;
 e->is_folder = wpe_seen_kind(seen) == S_IFDIR;
 e->hidden = hidden;
 if (created) r->judged = 0;
 return e;
}

/** ck_set_folder - Say how far a folder was read, so the judge knows what its silence means. */
static void ck_set_folder(WpeCkRecord *r, WpeCkSide side, const char *dir, CkFolder how)
{
 CkEntry *e = ck_find(r, side, dir);
 if (e) e->folder = how;
}

/**
 * ck_folder_refused - Keep a folder the walk could not open.
 * @dir: the folder; @o: the read owner's refusal; @below: met while walking.
 *
 * A folder that is not there is absent, not unreached; anything else the
 * system or the owner refused is a note, and the folder's files are
 * "nobody can say".
 */
static void ck_folder_refused(WpeCkRecord *r, WpeCkSide side, const char *dir, const WpeReadOutcome *o,
                              int below)
{
 if (below && o->why == WPE_READ_IS_A_LINK)
  ck_say_trace(r, "walk refused a folder that became a link: %s", dir);
 if (wpe_read_outcome_missing(o)) { ck_set_folder(r, side, dir, CK_F_ABSENT); return; }
 ck_set_folder(r, side, dir, CK_F_REFUSED);
 ck_note_refusal(r, side, dir, o);
}

/** Does the walk leave this folder out (the editor's own state folder)? */
static int ck_is_skipped(const WpeCkRecord *r, const char *path)
{
 char real[WPE_PATH_SIZE];
 return r->skip && realpath(path, real) && !strcmp(real, r->skip);
}

/** The bound was reached: say it once per side. */
static void ck_bound_reached(WpeCkRecord *r, WpeCkSide side, const char *root)
{
 CkSide *s = ck_side(r, side);
 if (s->bound_noted) return;
 s->bound_noted = 1;
 ck_note(r, side, WPE_CK_WHY_BOUND, root, NULL, s->counted);
}

static int ck_walk_dir(WpeCkRecord *r, WpeCkSide side, const char *dir, const char *root, int below);

/**
 * ck_hidden_cap_reached - Say once that hidden entries beyond the cap were not recorded.
 * @side: the moment; @path: the hidden entry that did not fit.
 *
 * A hidden entry only tells the judge which object a new name is; leaving
 * some out can make a renamed hidden file look new, so the review says so.
 */
static void ck_hidden_cap_reached(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 CkSide *s = ck_side(r, side);
 char dir[WPE_PATH_SIZE];
 if (s->hidden_noted) return;
 s->hidden_noted = 1;
 /* a path too long for the array is noted as no place: the note still says
    that hidden entries were left out */
 if (WPE_FMT_WHOLE(dir, "%s", path) == WPE_FMT_OK)
  (void)ckrec_parent(dir);
 ck_note(r, side, WPE_CK_WHY_NOT_LISTED, dir, NULL, s->hidden_counted);
}

/** One entry of a folder the walk reads: a hidden one is kept as a source and never entered. */
static void ck_walk_hidden(WpeCkRecord *r, WpeCkSide side, const char *path, const WpeSeen *seen)
{
 CkSide *s = ck_side(r, side);
 CkEntry *e;
 if (s->hidden_counted >= ck_bound(r)) {
  ck_hidden_cap_reached(r, side, path);
  return;
 }
 e = ck_add_entry(r, side, path, seen, 1);
 if (!e) return;
 s->hidden_counted++;
 if (e->is_folder) e->folder = CK_F_UNREAD;
}

/** Does this entry count against the bound?  On the now side only a new name does. */
static int ck_counts(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 return side == WPE_CK_THEN || !ck_then_knows(r, path);
}

/** The step the queue of subfolders grows by. */
#define CK_SUBS_FIRST 8

/**
 * ck_subs_push - Queue a subfolder to be entered once its folder's entries are read.
 * @subs/@nsubs/@scap: the queue; @path: the subfolder.
 */
static void ck_subs_push(char ***subs, int *nsubs, int *scap, const char *path)
{
 char **ns;
 if (*nsubs >= *scap) {
  int cap = *scap ? *scap * 2 : CK_SUBS_FIRST;
  ns = realloc(*subs, (size_t)cap * sizeof **subs);
  if (!ns) return;
  *subs = ns;
  *scap = cap;
 }
 (*subs)[*nsubs] = ckrec_strdup(path);
 if ((*subs)[*nsubs]) (*nsubs)++;
}

/** A file, a link or a folder of a folder being read: list it, and queue a folder. */
static void ck_walk_entry(WpeCkRecord *r, WpeCkSide side, const char *path, const WpeSeen *seen,
                          char ***subs, int *nsubs, int *scap)
{
 mode_t kind = wpe_seen_kind(seen);
 CkSide *s = ck_side(r, side);
 if (kind == S_IFDIR) {
  if (ck_is_skipped(r, path)) return;
  if (!ck_add_entry(r, side, path, seen, 0)) return;
  ck_subs_push(subs, nsubs, scap, path);
  return;
 }
 if (kind != S_IFREG && kind != S_IFLNK) return;
 if (ck_counts(r, side, path)) s->counted++;
 (void)ck_add_entry(r, side, path, seen, 0);
}

/** Go into the subfolders of a folder, or mark them cut when the walk stopped. */
static int ck_walk_subs(WpeCkRecord *r, WpeCkSide side, const char *root, char **subs, int nsubs,
                        int stopped)
{
 int k;
 if (nsubs > 0 && r->ask.before_enter) r->ask.before_enter();
 for (k = 0; k < nsubs; k++) {
  if (stopped) { ck_set_folder(r, side, subs[k], CK_F_CUT); continue; }
  if (side == WPE_CK_NOW && !ck_now_may_enter(r, subs[k])) {
   ck_set_folder(r, side, subs[k], CK_F_UNREAD);
   continue;
  }
  if (ck_walk_dir(r, side, subs[k], root, 1)) stopped = 1;
 }
 return stopped;
}

/**
 * ck_walk_dir - List a folder and everything below it, as far as the bound allows.
 * @dir: spelled; @root: the watched folder the walk began at; @below: met
 * while walking (a link at its name is refused, never entered).
 * Return: 1 when the bound stopped the walk.
 *
 * What the user gets: a file the run makes in a deeper folder is found; a
 * folder that cannot be read is on record with the reason, and so is one the
 * bound cut.  A folder's own entries are read before any subfolder, so a
 * deep tree spends the bound below it and not the files beside the open one.
 */
static int ck_walk_dir(WpeCkRecord *r, WpeCkSide side, const char *dir, const char *root, int below)
{
 WpeReadAsk ask = below ? WPE_READ_ASK_FOLDER_ENTRY(CK_READ_WHAT) : WPE_READ_ASK_FOLDER(CK_READ_WHAT);
 WpeReadOutcome ro;
 DIR *d = wpe_read_folder(dir, &ask, &ro);
 struct dirent *de;
 char **subs = NULL;
 int nsubs = 0, scap = 0, stopped = 0, k;
 if (!d) { ck_folder_refused(r, side, dir, &ro, below); return 0; }
 while ((de = readdir(d))) {
  char path[WPE_PATH_SIZE];
  WpeSeen seen;
  if (ck_side(r, side)->counted >= ck_bound(r)) { stopped = 1; break; }
  if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
  if (wpe_read_entry_look(d, de->d_name, &seen) != 0) continue;
  if (ck_join(dir, de->d_name, path, sizeof path) != 0) continue;
  if (de->d_name[0] == '.') { ck_walk_hidden(r, side, path, &seen); continue; }
  ck_walk_entry(r, side, path, &seen, &subs, &nsubs, &scap);
 }
 closedir(d);
 ck_set_folder(r, side, dir, stopped ? CK_F_CUT : CK_F_LISTED);
 if (stopped) ck_bound_reached(r, side, root);
 stopped = ck_walk_subs(r, side, root, subs, nsubs, stopped);
 for (k = 0; k < nsubs; k++) free(subs[k]);
 free(subs);
 return stopped;
}

/**
 * ck_unlooked - Keep a name whose look failed as a place nobody reached.
 * @e: its entry; @path: the name; @err: the errno the look gave.
 *
 * A record with no look is no identity: it must never stand for an object, so
 * the entry is UNKNOWN and the review says why.
 */
static void ck_unlooked(WpeCkRecord *r, WpeCkSide side, CkEntry *e, const char *path, int err)
{
 WpeReadOutcome o;
 wpe_read_outcome_errno(&o, err);
 e->state = CK_UNKNOWN;
 e->why = WPE_CK_WHY_REFUSED;
 ck_note_refusal(r, side, path, &o);
}

/**
 * ck_look_folder_entry - Make the entry of a folder, with its look.
 * @path: spelled; @follow: look through a link at the name; @how: how far the
 * folder was read (CK_F_NONE until a walk sets it).
 * Return: the entry, NULL when memory ran out.  A failed look gives an UNKNOWN
 * entry with a note, never a LISTED one with an empty look.
 */
static CkEntry *ck_look_folder_entry(WpeCkRecord *r, WpeCkSide side, const char *path, int follow,
                                     CkFolder how)
{
 WpeSeen seen;
 CkEntry *e;
 if (wpe_seen_look_name(path, follow, &seen) != 0) {
  int err = errno;
  e = ck_put(r, side, path, NULL);
  if (!e) return NULL;
  e->is_folder = 1;
  e->folder = how;
  if (!ck_seen_state(e)) ck_unlooked(r, side, e, path, err);
  return e;
 }
 e = ck_add_entry(r, side, path, &seen, 0);
 if (!e) return NULL;
 e->is_folder = 1;
 e->folder = how;
 return e;
}

/** ck_root_is_link - Is a watched root a link at this moment? */
static int ck_root_is_link(const char *root)
{
 struct stat st;
 return lstat(root, &st) == 0 && S_ISLNK(st.st_mode);
}

/**
 * ck_root_still_the_one - After the run, is a watched root the folder the checkpoint walked?
 * @root: spelled.
 * Return: 0 when a link, or another folder, took its place; 1 otherwise.
 *
 * What the user gets: a watched folder outside the workspace that the run
 * replaced by a link is not walked as if it were the one the checkpoint saw,
 * so a revert never takes away a file of whatever the link leads to.
 */
static int ck_root_still_the_one(WpeCkRecord *r, const char *root)
{
 CkEntry *then = ck_find(r, WPE_CK_THEN, root);
 WpeSeen seen;
 if (r->held_root && !strcmp(r->held_root, root)) return 1;
 if (!then || !then->is_folder) return 1;
 if (ck_root_is_link(root) != then->root_link) return 0;
 if (!wpe_seen_known(&then->seen) || wpe_seen_look_name(root, 1, &seen) != 0) return 1;
 return wpe_seen_judge(&then->seen, &seen) != WPE_SEEN_OTHER;
}

/**
 * wpe_ckrec_walk - Fill one side from a watched folder.
 * @r: the record; @side: the checkpoint or the review; @folder: the root.
 *
 * What the user gets: the same walk before and after the run, so a refusal or
 * a bound met after the run is on record like one before it.  A folder the
 * scope names is opened as the user wrote it (a link is followed); the folders
 * met below it are not followed.  Nothing is returned: every outcome is a
 * note or an entry.
 */
void wpe_ckrec_walk(WpeCkRecord *r, WpeCkSide side, const char *folder)
{
 char root[WPE_PATH_SIZE];
 CkEntry *e;
 if (ckrec_spell(folder, root, sizeof root) != 0) return;
 if (side == WPE_CK_NOW && !ck_root_still_the_one(r, root)) {
  wpe_ckrec_unreached(r, WPE_CK_NOW, WPE_CK_WHY_ROOT_REPLACED, root, NULL);
  return;
 }
 e = ck_look_folder_entry(r, side, root, 1, CK_F_NONE);
 if (e && side == WPE_CK_THEN) e->root_link = ck_root_is_link(root);
 (void)ck_walk_dir(r, side, root, root, 0);
}

/* ---- the other ways a side is filled --------------------------------------- */

/**
 * wpe_ckrec_hold_root - Name the watched folder the caller holds open and checks itself.
 * @r: the record; @root: the workspace folder, as it is walked.
 *
 * Every other watched root is checked here after the run: one that a link or
 * another folder took the place of is not walked.  The workspace folder is
 * held by descriptor by the caller, whose check also lets a workspace reached
 * through the user's own link keep working when that link is retargeted.
 */
void wpe_ckrec_hold_root(WpeCkRecord *r, const char *root)
{
 char p[WPE_PATH_SIZE];
 free(r->held_root);
 r->held_root = ckrec_spell(root, p, sizeof p) == 0 ? ckrec_strdup(p) : NULL;
}

/**
 * wpe_ckrec_set_skip - Name the one folder the walks leave out, or none.
 * @r: the record; @skip: the editor's own state folder, resolved, or NULL.
 *
 * The session files and the checkpoint's copies the editor writes there are
 * its bookkeeping, never the run's changes.  The folder may be made after the
 * record was, so the caller names it before each walk.
 */
void wpe_ckrec_set_skip(WpeCkRecord *r, const char *skip)
{
 free(r->skip);
 r->skip = skip ? ckrec_strdup(skip) : NULL;
 r->ask.skip = r->skip;
}

/**
 * wpe_ckrec_new - Make the record of one checkpoint.
 * @ask: the bound, the trace sink and the test seam the walks use (copied).
 * Return: the record, NULL when memory ran out (the checkpoint is then not taken).
 */
WpeCkRecord *wpe_ckrec_new(const WpeCkAsk *ask)
{
 WpeCkRecord *r = calloc(1, sizeof *r);
 if (!r) return NULL;
 r->ask = *ask;
 if (ask->skip) {
  r->skip = ckrec_strdup(ask->skip);
  r->ask.skip = r->skip;
 }
 return r;
}

/** ck_free_notes - Forget the places one side could not reach. */
static void ck_free_notes(WpeCkRecord *r, WpeCkSide side)
{
 int i;
 for (i = 0; i < r->nnotes[side]; i++) free(r->notes[side][i].where);
 free(r->notes[side]);
 r->notes[side] = NULL;
 r->nnotes[side] = r->notes_cap[side] = 0;
}

/** ck_drop_verdicts - Forget the last judge, so the next question judges again. */
static void ck_drop_verdicts(WpeCkRecord *r)
{
 free(r->v);
 r->v = NULL;
 r->nv = 0;
 r->judged = 0;
 r->unsaid = 0;
}

/**
 * wpe_ckrec_free - Let go of a record: both moments, the notes and the last verdicts.
 * @r: the record, or NULL.
 */
void wpe_ckrec_free(WpeCkRecord *r)
{
 if (!r) return;
 ck_side_free(&r->side[WPE_CK_THEN]);
 ck_side_free(&r->side[WPE_CK_NOW]);
 ck_free_notes(r, WPE_CK_THEN);
 ck_free_notes(r, WPE_CK_NOW);
 free(r->v);
 free(r->skip);
 free(r->held_root);
 free(r->tool_root[WPE_CK_THEN]);
 free(r->tool_root[WPE_CK_NOW]);
 free(r);
}

/**
 * wpe_ckrec_then_image - Record a file the checkpoint holds a before image of.
 * @r: the record; @path: the file; @image: the number of its image; @seen: the
 * look the image was read under.
 */
void wpe_ckrec_then_image(WpeCkRecord *r, const char *path, int image, const WpeSeen *seen)
{
 char p[WPE_PATH_SIZE];
 CkEntry *e;
 if (ckrec_spell(path, p, sizeof p) != 0) return;
 e = ck_put(r, WPE_CK_THEN, p, NULL);
 if (!e) return;
 e->state = CK_HELD;
 e->why = WPE_CK_WHY_NONE;
 e->image = image;
 e->seen = *seen;
 e->is_folder = 0;
 r->judged = 0;
}

/**
 * wpe_ckrec_then_asked - Record a name the checkpoint asked for and did not read.
 * @o: the refusal: a name that is not there is absent, any other refusal is
 * a place nobody reached.
 */
void wpe_ckrec_then_asked(WpeCkRecord *r, const char *path, const WpeReadOutcome *o)
{
 char p[WPE_PATH_SIZE];
 CkEntry *e;
 if (ckrec_spell(path, p, sizeof p) != 0) return;
 e = ck_put(r, WPE_CK_THEN, p, NULL);
 if (!e || ck_seen_state(e)) return;
 if (wpe_read_outcome_missing(o)) { e->state = CK_ABSENT; return; }
 e->state = CK_UNKNOWN;
 e->why = WPE_CK_WHY_REFUSED;
 r->judged = 0;
}

/**
 * wpe_ckrec_tool_begin - A tool is about to list a folder: remember which one.
 * @side: the moment; @folder: the folder the tool lists.
 *
 * So that each name it lists also records the folders it stands in below
 * @folder, with their looks: a folder the run renames is the same object
 * under its new name, and the files in it are judged through it.
 */
void wpe_ckrec_tool_begin(WpeCkRecord *r, WpeCkSide side, const char *folder)
{
 char p[WPE_PATH_SIZE];
 if (ckrec_spell(folder, p, sizeof p) != 0) return;
 free(r->tool_root[side]);
 r->tool_root[side] = ckrec_strdup(p);
}

/**
 * ck_add_tool_parents - Record the folders a listed name stands in, up to the tool's folder.
 * @side: the moment; @path: the listed name, spelled.
 *
 * A folder the tool lists a name in is read to the end: a name it did not list
 * is not there.
 */
static void ck_add_tool_parents(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 char anc[WPE_PATH_SIZE];
 const char *root = r->tool_root[side];
 size_t rl;
 if (!root) return;
 rl = strlen(root);
 if (WPE_FMT_WHOLE(anc, "%s", path) != WPE_FMT_OK) return;
 while (ckrec_parent(anc) && strlen(anc) > rl && !strncmp(anc, root, rl)) {
  if (ck_find(r, side, anc)) return;
  if (!ck_look_folder_entry(r, side, anc, 0, CK_F_LISTED)) return;
 }
}

/** ck_listed_unlooked - A listed name that cannot be looked at: UNKNOWN, with its note. */
static void ck_listed_unlooked(WpeCkRecord *r, WpeCkSide side, const char *path, int err)
{
 CkEntry *e = ck_put(r, side, path, NULL);
 if (e) ck_unlooked(r, side, e, path, err);
}

/**
 * wpe_ckrec_listed - Record a name a tool listed, looked at where it stands.
 * @side: the moment; @path: the name.
 *
 * A name with no look would make every new file look like it: the look is the
 * identity.  A look that fails keeps the name as a place nobody reached.
 */
void wpe_ckrec_listed(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 char p[WPE_PATH_SIZE];
 WpeSeen seen;
 CkEntry *e;
 const char *leaf;
 if (ckrec_spell(path, p, sizeof p) != 0) return;
 leaf = strrchr(p, '/');
 leaf = leaf ? leaf + 1 : p;
 if (wpe_seen_look_name(p, 0, &seen) != 0) {
  ck_listed_unlooked(r, side, p, errno);
  return;
 }
 (void)ck_add_entry(r, side, p, &seen, leaf[0] == '.');
 ck_add_tool_parents(r, side, p);
}

/** wpe_ckrec_listed_ignored - A file git ignores: present and known, but never a row. */
void wpe_ckrec_listed_ignored(WpeCkRecord *r, WpeCkSide side, const char *path)
{
 char p[WPE_PATH_SIZE];
 CkEntry *e;
 wpe_ckrec_listed(r, side, path);
 if (ckrec_spell(path, p, sizeof p) != 0) return;
 e = ck_find(r, side, p);
 if (e) e->ignored = 1;
}

/** wpe_ckrec_complete - A tool listed this folder to its end: what it did not list is not there. */
void wpe_ckrec_complete(WpeCkRecord *r, WpeCkSide side, const char *folder)
{
 char p[WPE_PATH_SIZE];
 if (ckrec_spell(folder, p, sizeof p) != 0) return;
 (void)ck_look_folder_entry(r, side, p, 1, CK_F_LISTED);
}

/** wpe_ckrec_unread - A folder a tool ignored: nobody read what is in it. */
void wpe_ckrec_unread(WpeCkRecord *r, WpeCkSide side, const char *folder)
{
 char p[WPE_PATH_SIZE];
 if (ckrec_spell(folder, p, sizeof p) != 0) return;
 if (!ck_look_folder_entry(r, side, p, 0, CK_F_UNREAD)) return;
 ck_add_tool_parents(r, side, p);
}

/** wpe_ckrec_then_begin - The checkpoint starts again: forget what a failed try recorded. */
void wpe_ckrec_then_begin(WpeCkRecord *r)
{
 ck_side_free(&r->side[WPE_CK_THEN]);
 ck_free_notes(r, WPE_CK_THEN);
 free(r->tool_root[WPE_CK_THEN]);
 r->tool_root[WPE_CK_THEN] = NULL;
 ck_drop_verdicts(r);
}

/** wpe_ckrec_now_begin - The review starts again: forget what the last look at "now" found. */
void wpe_ckrec_now_begin(WpeCkRecord *r)
{
 ck_side_free(&r->side[WPE_CK_NOW]);
 ck_free_notes(r, WPE_CK_NOW);
 free(r->tool_root[WPE_CK_NOW]);
 r->tool_root[WPE_CK_NOW] = NULL;
 ck_drop_verdicts(r);
}

/**
 * wpe_ckrec_unreached - Keep a place a side could not reach, and why.
 * @side: the moment; @why: the cause; @where: the place; @o: the read
 * owner's refusal, or NULL (a tool's failure has its own words in @where).
 */
void wpe_ckrec_unreached(WpeCkRecord *r, WpeCkSide side, WpeCkWhy why, const char *where,
                         const WpeReadOutcome *o)
{
 char reason[WPE_PATH_MSG_MAX];
 reason[0] = '\0';
 if (o) (void)wpe_read_reason(reason, sizeof reason, o);
 ck_side(r, side)->cause = why;
 ck_note(r, side, why, where, reason, 0);
}

/** wpe_ckrec_image_lost - A before image the checkpoint cannot read back. */
void wpe_ckrec_image_lost(WpeCkRecord *r, const char *path, const WpeReadOutcome *o)
{
 wpe_ckrec_unreached(r, WPE_CK_THEN, WPE_CK_WHY_IMAGE_LOST, path, o);
}

/* ---- the judge ------------------------------------------------------------- */

/** What the rules decided about one name present now. */
typedef struct {
 WpeCkVerdictKind kind;
 WpeCkVerdictKind object;     /* REPLACED: what the object now at the name is */
 WpeCkWhy         why;
 CkEntry         *from;
} CkCall;

/** Is the name present now?  An entry the now side asked for and found absent is not. */
static int ck_present_now(WpeCkRecord *r, const char *path)
{
 CkEntry *n = ck_find(r, WPE_CK_NOW, path);
 return n && ck_seen_state(n);
}

/** Is the object seen under this then name gone from that name now? */
static int ck_absent_now(WpeCkRecord *r, const CkEntry *t)
{
 CkEntry *n = ck_find(r, WPE_CK_NOW, t->path);
 if (!n || !ck_seen_state(n)) return 1;
 return !ck_not_other(t, n);
}

/**
 * ck_known_object - Find the then entry that is the object this name holds now.
 * @now: an entry present now; @absent: receives 1 when that then name no
 * longer holds the object.
 * Return: the then entry, NULL when the checkpoint saw no such object.
 *
 * The choice is the same on every system: a name that no longer holds the
 * object first, then the first in path order.  An UNSURE judge counts as
 * known (safe: the file is kept and said).  A record with no look is never
 * the object.
 */
static CkEntry *ck_known_object(WpeCkRecord *r, const CkEntry *now, int *absent)
{
 CkSide *s = ck_side(r, WPE_CK_THEN);
 CkEntry *best = NULL;
 int best_absent = 0, i;
 if (!wpe_seen_known(&now->seen)) return NULL;
 for (i = 0; i < s->n; i++) {
  CkEntry *t = &s->e[s->order[i]];
  int abs;
  if (!ck_seen_state(t) || !wpe_seen_known(&t->seen)) continue;
  if (t->is_folder != now->is_folder || !strcmp(t->path, now->path)) continue;
  if (wpe_seen_judge(&t->seen, &now->seen) == WPE_SEEN_OTHER) continue;
  abs = ck_absent_now(r, t);
  if (!best || (abs && !best_absent)) { best = t; best_absent = abs; }
 }
 *absent = best_absent;
 return best;
}

/** What a folder's state says about a name that the folder did not list. */
typedef enum { CK_UNDER_ABSENT, CK_UNDER_MOVED, CK_UNDER_UNSAID } CkUnder;

/** The answer for a name below a folder the checkpoint knew, by how far it read it. */
static CkUnder ck_under_state(const CkEntry *folder, WpeCkWhy *why)
{
 switch (folder->folder) {
 case CK_F_LISTED:  return CK_UNDER_ABSENT;
 case CK_F_ABSENT:  return CK_UNDER_ABSENT;
 case CK_F_CUT:     *why = WPE_CK_WHY_BOUND;      return CK_UNDER_UNSAID;
 case CK_F_REFUSED: *why = WPE_CK_WHY_REFUSED;    return CK_UNDER_UNSAID;
 case CK_F_UNREAD:  *why = WPE_CK_WHY_NOT_LISTED; return CK_UNDER_UNSAID;
 case CK_F_NONE:    break;
 }
 *why = WPE_CK_WHY_NOT_LISTED;
 return CK_UNDER_UNSAID;
}

/**
 * ck_under_moved - A name below a folder that moved: was it there, under the old folder?
 * @t: the then folder that is now @anc; @anc: the folder now; @path: the name.
 */
static CkUnder ck_under_moved(WpeCkRecord *r, const CkEntry *t, const char *anc, const char *path,
                              const CkEntry **from, WpeCkWhy *why)
{
 char old[WPE_PATH_SIZE];
 CkEntry *e;
 if (ck_join(t->path, path + strlen(anc) + 1, old, sizeof old) != 0) return CK_UNDER_UNSAID;
 e = ck_find(r, WPE_CK_THEN, old);
 if (e && ck_seen_state(e)) { *from = e; return CK_UNDER_MOVED; }
 return ck_under_state(t, why);
}

/**
 * ck_judge_under - Was a name that is not in the checkpoint's list there then, below its folders?
 * @path: spelled; @from/@why: receive the then entry, or the cause.
 *
 * Goes up the folders of the name as they are now.  The nearest one that the
 * checkpoint knew (by identity, or by name when it asked and found it
 * absent) decides: read to the end, the name was absent; cut, refused or
 * never read, nobody can say.  A folder that moved is looked for under its
 * old name.
 */
static CkUnder ck_judge_under(WpeCkRecord *r, const char *path, const CkEntry **from, WpeCkWhy *why)
{
 char anc[WPE_PATH_SIZE];
 if (WPE_FMT_WHOLE(anc, "%s", path) != WPE_FMT_OK) {
  *why = WPE_CK_WHY_NOT_LISTED;
  return CK_UNDER_UNSAID;
 }
 while (ckrec_parent(anc)) {
  CkEntry *t = ck_find(r, WPE_CK_THEN, anc), *n = ck_find(r, WPE_CK_NOW, anc);
  int absent;
  if (t && t->is_folder) {
   /* only a folder read to the end can say a name was not in it: one that was
      cut, refused or never read keeps its cause whatever it holds now */
   if (t->folder == CK_F_LISTED && n && !ck_not_other(t, n)) return CK_UNDER_ABSENT;
   return ck_under_state(t, why);
  }
  if (!t && n && n->is_folder) {
   CkEntry *m = ck_known_object(r, n, &absent);
   if (m && m->is_folder) return ck_under_moved(r, m, anc, path, from, why);
  }
 }
 *why = WPE_CK_WHY_NOT_LISTED;
 return CK_UNDER_UNSAID;
}

/** Rules two and three for a present object that is not what the checkpoint listed under its name. */
static void ck_judge_object(WpeCkRecord *r, const CkEntry *now, CkCall *c)
{
 const CkEntry *from = NULL;
 int absent = 0;
 CkEntry *known = ck_known_object(r, now, &absent);
 if (known) {
  c->kind = absent ? WPE_CK_MOVED : WPE_CK_LINKED;
  c->from = known;
  return;
 }
 switch (ck_judge_under(r, now->path, &from, &c->why)) {
 case CK_UNDER_MOVED:  c->kind = WPE_CK_MOVED; c->from = (CkEntry *)from; return;
 case CK_UNDER_ABSENT: c->kind = WPE_CK_CREATED; return;
 case CK_UNDER_UNSAID: c->kind = WPE_CK_CANNOT_SAY; return;
 }
}

/** Rule one and then the rest, for one name that is present now. */
static void ck_judge_present(WpeCkRecord *r, const CkEntry *now, CkCall *c)
{
 CkEntry *then = ck_find(r, WPE_CK_THEN, now->path);
 memset(c, 0, sizeof *c);
 if (then && then->state == CK_UNKNOWN) { c->kind = WPE_CK_CANNOT_SAY; c->why = then->why; return; }
 if (then && ck_seen_state(then)) {
  int same_kind = then->is_folder == now->is_folder;
  if (then->state == CK_HELD || (same_kind && ck_not_other(then, now))) { c->kind = WPE_CK_STAYED; return; }
  ck_judge_object(r, now, c);
  c->object = c->kind;
  c->kind = WPE_CK_REPLACED;
  return;
 }
 ck_judge_object(r, now, c);
}

/**
 * ck_now_reached - Did the now side read the folder this name would be in?
 * @path: spelled; @why: receives the cause when it did not.
 */
static int ck_now_reached(WpeCkRecord *r, const char *path, WpeCkWhy *why)
{
 char anc[WPE_PATH_SIZE];
 if (WPE_FMT_WHOLE(anc, "%s", path) != WPE_FMT_OK) {
  *why = WPE_CK_WHY_NOT_LISTED;
  return 0;
 }
 while (ckrec_parent(anc)) {
  CkEntry *n = ck_find(r, WPE_CK_NOW, anc);
  if (!n || !n->is_folder) continue;
  switch (n->folder) {
  case CK_F_LISTED: return 1;
  case CK_F_ABSENT: return 1;
  case CK_F_CUT:     *why = WPE_CK_WHY_BOUND;      return 0;
  case CK_F_REFUSED: *why = WPE_CK_WHY_REFUSED;    return 0;
  case CK_F_UNREAD:  *why = WPE_CK_WHY_NOT_LISTED; return 0;
  case CK_F_NONE:    break;
  }
 }
 *why = ck_side(r, WPE_CK_NOW)->cause != WPE_CK_WHY_NONE ? ck_side(r, WPE_CK_NOW)->cause
                                                       : WPE_CK_WHY_NOT_LISTED;
 return 0;
}

/**
 * ck_maybe_copy - A new object as large as a removed name the checkpoint holds no copy of.
 * @now: a CREATED file; @from: receives that name.
 *
 * The rename the identity cannot see (across file systems, or on one with no
 * stable numbers) leaves a new object of the old size.  A different size
 * proves the object is not a byte copy of it.  Only a way to keep a file, never
 * a proof that the file is new.
 */
static CkEntry *ck_maybe_copy(WpeCkRecord *r, const CkEntry *now)
{
 CkSide *s = ck_side(r, WPE_CK_THEN);
 WpeCkWhy why;
 int i;
 for (i = 0; i < s->n; i++) {
  CkEntry *t = &s->e[s->order[i]];
  if (t->state != CK_LISTED || t->is_folder || t->claimed) continue;
  if (!wpe_seen_known(&t->seen) || wpe_seen_size(&t->seen) != wpe_seen_size(&now->seen)) continue;
  if (!ck_absent_now(r, t) || !ck_now_reached(r, t->path, &why)) continue;
  return t;
 }
 return NULL;
}

/** Where the folder a name was in stands now. */
static WpeCkFolderNow ck_folder_now(const char *path)
{
 char anc[WPE_PATH_SIZE];
 WpeSeen seen;
 int direct = 1;
 /* a path too long to look at is a folder nobody can show to be there */
 if (WPE_FMT_WHOLE(anc, "%s", path) != WPE_FMT_OK) return WPE_CK_FOLDER_GONE;
 while (ckrec_parent(anc)) {
  if (wpe_seen_look_name(anc, 1, &seen) == 0) {
   if (wpe_seen_kind(&seen) != S_IFDIR) return WPE_CK_FOLDER_IS_A_FILE;
   return direct ? WPE_CK_FOLDER_THERE : WPE_CK_FOLDER_GONE;
  }
  if (errno == ENOTDIR) return WPE_CK_FOLDER_IS_A_FILE;
  direct = 0;
 }
 return WPE_CK_FOLDER_GONE;
}

/** Fill the part of a verdict every kind shares. */
static void ck_verdict_base(WpeCkVerdict *v, WpeCkVerdictKind kind, const CkEntry *e)
{
 memset(v, 0, sizeof *v);
 v->kind = kind;
 v->path = e->path;
 v->image = e->state == CK_HELD ? e->image : WPE_CK_NO_IMAGE;
 v->is_folder = e->is_folder;
}

/**
 * ck_unsaid_without_note - Is this a path nobody can say anything about, in a place no note names?
 * @v: a verdict.
 *
 * A path in a folder that was refused, cut or replaced is covered by that
 * folder's note, which says it once for every path in it.  A place the walk
 * does not read by design (below a hidden or an ignored folder) has no note,
 * so each path found there is counted as unseen and has its own sentence.
 */
static int ck_unsaid_without_note(const WpeCkVerdict *v)
{
 return v->kind == WPE_CK_CANNOT_SAY && v->why == WPE_CK_WHY_NOT_LISTED;
}

/** ck_push_verdict - Add one verdict to the list being built; -1 when memory ran out. */
static int ck_push_verdict(WpeCkRecord *r, int *cap, const WpeCkVerdict *v)
{
 if (r->nv == *cap) {
  int nc = *cap ? *cap * 2 : CK_GROW_FIRST;
  WpeCkVerdict *nv = realloc(r->v, (size_t)nc * sizeof *nv);
  if (!nv) return -1;
  r->v = nv;
  *cap = nc;
 }
 r->v[r->nv++] = *v;
 if (ck_unsaid_without_note(v)) r->unsaid++;
 return 0;
}

/** The verdict of a present name that is not STAYED. */
static void ck_verdict_of_call(const CkCall *c, const CkEntry *now, WpeCkVerdict *v)
{
 ck_verdict_base(v, c->kind, now);
 v->why = c->why;
 v->from = c->from ? c->from->path : NULL;
 v->object = c->object;
}

/**
 * ck_claim - A name now holds the object the checkpoint saw under @then.
 * @then: the entry of the old name; @now: the entry that holds the object.
 *
 * The old name is then not "removed": it is moved, or it is a hidden name the
 * review never lists (the old name then gets its own verdict).  Kept with it
 * is how sure that is, which is what the user gains: a claim the identity
 * proved is the object itself under a new name, and one it could not prove may
 * be a number the file system gave back to a file the run made.
 *
 * Several names can claim one old name: a second name of the same object, a
 * hidden name beside a visible one, or a name below a folder that moved, which
 * the review reads as a move of the same old name and proves nothing about.
 * The proof is then gathered and never overwritten, so what the user gets is
 * the surest answer those names can give, and a last claimer that proved
 * nothing cannot take away the proof an earlier one had.  A hidden claimer is
 * left out of the proof because it is not the name that answers for the old
 * one: the old name goes silent only for a name the review lists, so only that
 * name's proof is an answer.
 */
static void ck_claim(CkEntry *then, const CkEntry *now)
{
 then->claimed = 1;
 if (now->hidden) {
  if (!then->hidden_to) then->hidden_to = now->path;
  return;
 }
 then->claimed_visible = 1;
 if (wpe_seen_judge(&then->seen, &now->seen) == WPE_SEEN_SAME) then->claimed_proven = 1;
}

/**
 * ck_judge_now_names - The first pass: every name present now.
 * @calls: one per now entry, in the order of `order`.
 *
 * Rule 2 marks the then names whose object was found elsewhere, so that
 * rule 4 and the removals know which names are not removed but moved.
 */
static void ck_judge_now_names(WpeCkRecord *r, CkCall *calls)
{
 CkSide *s = ck_side(r, WPE_CK_NOW);
 int i;
 for (i = 0; i < s->n; i++) {
  CkEntry *e = &s->e[s->order[i]];
  CkCall *c = &calls[i];
  memset(c, 0, sizeof *c);
  if (!ck_seen_state(e)) { c->kind = WPE_CK_STAYED; continue; }
  ck_judge_present(r, e, c);
  if (c->from && c->kind != WPE_CK_CREATED) ck_claim(c->from, e);
 }
 for (i = 0; i < s->n; i++) {
  CkEntry *e = &s->e[s->order[i]];
  CkCall *c = &calls[i];
  CkEntry *t;
  if (c->kind != WPE_CK_CREATED || e->is_folder) continue;
  t = ck_maybe_copy(r, e);
  if (t) { c->kind = WPE_CK_MAYBE_COPY; c->from = t; }
 }
}

/** Does the folder verdict of @e's folder say the folder moved, so it acts for the file? */
static int ck_under_moved_folder(WpeCkRecord *r, const CkEntry *e, const CkCall *calls)
{
 char par[WPE_PATH_SIZE];
 int pos;
 if (WPE_FMT_WHOLE(par, "%s", e->path) != WPE_FMT_OK) return 0;
 if (!ckrec_parent(par)) return 0;
 if (ck_locate(ck_side(r, WPE_CK_NOW), par, &pos) < 0) return 0;
 return calls[pos].kind == WPE_CK_MOVED;
}

/** Emit the verdicts of the names present now. */
static int ck_emit_present(WpeCkRecord *r, const CkCall *calls, int *cap)
{
 CkSide *s = ck_side(r, WPE_CK_NOW);
 int i;
 for (i = 0; i < s->n; i++) {
  CkEntry *e = &s->e[s->order[i]];
  const CkCall *c = &calls[i];
  WpeCkVerdict v;
  if (e->hidden || e->ignored || !ck_seen_state(e) || c->kind == WPE_CK_STAYED) continue;
  ck_verdict_of_call(c, e, &v);
  if (c->kind == WPE_CK_MOVED && !e->is_folder) v.under_folder = ck_under_moved_folder(r, e, calls);
  if (ck_push_verdict(r, cap, &v) != 0) return -1;
 }
 return 0;
}

/**
 * ck_under_replaced_root - Is a path below a watched root that the run replaced?
 * @path: spelled.
 *
 * What the user gets: a file whose folder a link or another folder took the
 * place of is not the file the checkpoint copied, so it is never read, diffed
 * or written over, even though a file by that name is there now.  The
 * workspace folder is not judged here: the caller holds it open and its own
 * revert refuses to write or remove through a replaced one, and says why.
 */
static int ck_under_replaced_root(const WpeCkRecord *r, const char *path)
{
 int i;
 for (i = 0; i < r->nnotes[WPE_CK_NOW]; i++) {
  const WpeCkNote *n = &r->notes[WPE_CK_NOW][i];
  size_t l = strlen(n->where);
  if (n->why != WPE_CK_WHY_ROOT_REPLACED) continue;
  if (r->held_root && !strcmp(r->held_root, n->where)) continue;   /* the caller's own check refuses it */
  if (!strncmp(path, n->where, l) && path[l] == '/') return 1;
 }
 return 0;
}

/**
 * ck_held_verdict - What became of a file the checkpoint holds an image of.
 * @t: the then entry.
 *
 * Asked by name when the walk did not list it, so it does not depend on the
 * walk: gone is REMOVED, refused is NOT_READABLE_NOW, there is STAYED (the
 * caller diffs it against the image).
 */
static void ck_held_verdict(WpeCkRecord *r, const CkEntry *t, WpeCkVerdict *v)
{
 WpeSeen seen;
 ck_verdict_base(v, WPE_CK_STAYED, t);
 if (ck_under_replaced_root(r, t->path)) {
  v->kind = WPE_CK_CANNOT_SAY;
  v->why = WPE_CK_WHY_ROOT_REPLACED;
  return;
 }
 if (ck_present_now(r, t->path)) return;
 if (wpe_seen_look_name(t->path, 0, &seen) == 0) return;
 if (errno == ENOENT || errno == ENOTDIR) {
  v->kind = WPE_CK_REMOVED;
  v->folder_now = ck_folder_now(t->path);
  return;
 }
 v->kind = WPE_CK_NOT_READABLE_NOW;
 v->why = WPE_CK_WHY_REFUSED;
 wpe_read_outcome_errno(&v->read, errno);
}

/** What became of a listed name that is not present now: removed, or nobody can say. */
static void ck_gone_verdict(WpeCkRecord *r, const CkEntry *t, WpeCkVerdict *v)
{
 WpeCkWhy why = WPE_CK_WHY_NONE;
 ck_verdict_base(v, WPE_CK_REMOVED_NO_COPY, t);
 if (!ck_now_reached(r, t->path, &why)) {
  v->kind = WPE_CK_CANNOT_SAY;
  v->why = why;
  return;
 }
 if (ck_side(r, WPE_CK_NOW)->bound_noted) {
  v->kind = WPE_CK_CANNOT_SAY;
  v->why = WPE_CK_WHY_BOUND;
  return;
 }
 if (t->is_folder) v->kind = WPE_CK_REMOVED;
}

/** ck_hidden_verdict - A listed name whose object is now under a hidden name: a row for the old name. */
static void ck_hidden_verdict(const CkEntry *t, WpeCkVerdict *v)
{
 ck_verdict_base(v, WPE_CK_MOVED_HIDDEN, t);
 v->from = t->hidden_to;
}

/**
 * ck_claim_stands - Does a claim answer for this name, so the name needs no verdict of its own?
 * @r: the record; @t: the then entry another name now holds the object of.
 *
 * What the user gets: a name the checkpoint listed is never silent.  A claim
 * the identity proved is the object itself under a new name, and it stands
 * whatever the review did.  A claim the identity could not prove, in a review
 * the bound cut, may be a number the file system gave back to a file the run
 * made: the review then never read far enough to tell a move from a removal,
 * so the name is answered by the rule for a name that is not there now, which
 * says CANNOT_SAY with the bound's cause.  A review read to its end keeps the
 * move, which is the direction that keeps a renamed file alive.
 * Return: 1 when the claim answers for the name, 0 when the name needs its own answer.
 */
static int ck_claim_stands(WpeCkRecord *r, const CkEntry *t)
{
 if (t->claimed_proven) return 1;
 return !ck_side(r, WPE_CK_NOW)->bound_noted;
}

/** Emit what became of the names the checkpoint knew that are not present now. */
static int ck_emit_then_names(WpeCkRecord *r, int *cap)
{
 CkSide *s = ck_side(r, WPE_CK_THEN);
 int i;
 for (i = 0; i < s->n; i++) {
  CkEntry *t = &s->e[s->order[i]];
  WpeCkVerdict v;
  if (!ck_seen_state(t)) continue;
  if (t->state == CK_HELD) ck_held_verdict(r, t, &v);
  else if (t->hidden || t->ignored || ck_present_now(r, t->path)) continue;
  else if (t->claimed && t->claimed_visible && ck_claim_stands(r, t)) continue;
  else if (t->claimed && !t->claimed_visible) ck_hidden_verdict(t, &v);
  else ck_gone_verdict(r, t, &v);
  if (ck_push_verdict(r, cap, &v) != 0) return -1;
 }
 return 0;
}

/** ck_verdict_cmp - Order verdicts by path, so the review lists them the same way on every system. */
static int ck_verdict_cmp(const void *a, const void *b)
{
 return strcmp(((const WpeCkVerdict *)a)->path, ((const WpeCkVerdict *)b)->path);
}

/**
 * wpe_ckrec_judge_all - Judge every path the two moments know.
 * @r: the record; @v: receives the verdicts, sorted by path.
 * Return: how many verdicts.
 *
 * What the user gets: one answer per path, from the object and from how far
 * each side read, in the same order on every system.  A name the checkpoint
 * held an image of is always answered, so a removed or unreadable file is a
 * row and not a silence.
 */
int wpe_ckrec_judge_all(WpeCkRecord *r, const WpeCkVerdict **v)
{
 CkSide *now = ck_side(r, WPE_CK_NOW), *then = ck_side(r, WPE_CK_THEN);
 CkCall *calls;
 int cap = 0, i;
 if (r->judged) { *v = r->v; return r->nv; }
 ck_drop_verdicts(r);
 for (i = 0; i < then->n; i++) {
  then->e[i].claimed = 0;
  then->e[i].claimed_visible = 0;
  then->e[i].claimed_proven = 0;
  then->e[i].hidden_to = NULL;
 }
 calls = calloc((size_t)now->n + 1, sizeof *calls);
 if (!calls) { *v = NULL; return 0; }
 ck_judge_now_names(r, calls);
 if (ck_emit_present(r, calls, &cap) != 0 || ck_emit_then_names(r, &cap) != 0) {
  free(calls);
  ck_drop_verdicts(r);
  *v = NULL;
  return 0;
 }
 free(calls);
 if (r->nv > 1) qsort(r->v, (size_t)r->nv, sizeof *r->v, ck_verdict_cmp);
 r->judged = 1;
 *v = r->v;
 return r->nv;
}

/** wpe_ckrec_verdict_of - The verdict of one path, from the last judge. */
int wpe_ckrec_verdict_of(WpeCkRecord *r, const char *path, WpeCkVerdict *v)
{
 const WpeCkVerdict *all;
 char p[WPE_PATH_SIZE];
 int n, lo, hi;
 if (ckrec_spell(path, p, sizeof p) != 0) return 0;
 n = wpe_ckrec_judge_all(r, &all);
 lo = 0; hi = n - 1;
 while (lo <= hi) {
  int mid = (lo + hi) / 2, c = strcmp(p, all[mid].path);
  if (c == 0) { *v = all[mid]; return 1; }
  if (c < 0) hi = mid - 1; else lo = mid + 1;
 }
 return 0;
}

/**
 * wpe_ckrec_may_remove - May a revert take this name away?
 * @path: the file; @v: receives its verdict.
 *
 * What the user gets: a file is taken away only when the record proved the run
 * made it, and a fresh look at the name still finds the object the review
 * judged.  A file that was there, moved, linked, replaced, or that nobody
 * can say anything about stays.  "Not in the before list" is not a verdict.
 * Return: 1 when it may be taken away.
 */
int wpe_ckrec_may_remove(WpeCkRecord *r, const char *path, WpeCkVerdict *v)
{
 char p[WPE_PATH_SIZE];
 CkEntry *now;
 WpeSeen fresh;
 if (ckrec_spell(path, p, sizeof p) != 0) return 0;
 if (!wpe_ckrec_verdict_of(r, p, v)) return 0;
 if (v->kind != WPE_CK_CREATED || v->is_folder) return 0;
 now = ck_find(r, WPE_CK_NOW, p);
 if (!now || wpe_seen_look_name(p, 0, &fresh) != 0) return 0;
 return wpe_seen_judge(&now->seen, &fresh) == WPE_SEEN_SAME;
}

/** wpe_ckrec_may_restore - May a revert put a copy back at this name?  Gives the image's number. */
int wpe_ckrec_may_restore(WpeCkRecord *r, const char *path, int *image)
{
 char p[WPE_PATH_SIZE];
 CkEntry *t;
 if (ckrec_spell(path, p, sizeof p) != 0) return 0;
 t = ck_find(r, WPE_CK_THEN, p);
 if (!t || t->state != CK_HELD || ck_under_replaced_root(r, p)) return 0;
 *image = t->image;
 return 1;
}

/**
 * wpe_ckrec_unseen - How many places the review could not see.
 * Return: the notes of both moments and the paths nobody can say anything
 * about.  Zero is the only value that lets the review say it saw everything.
 */
int wpe_ckrec_unseen(WpeCkRecord *r)
{
 const WpeCkVerdict *all;
 if (wpe_ckrec_judge_all(r, &all) < 0) return 0;
 return r->nnotes[WPE_CK_THEN] + r->nnotes[WPE_CK_NOW] + r->unsaid;
}

/**
 * wpe_ckrec_whole - Did the review see everything?
 * Return: 1 only when no place was left unseen: the one condition for saying
 * "ALL changes reverted" or "no files changed".
 */
int wpe_ckrec_whole(WpeCkRecord *r)
{
 return wpe_ckrec_unseen(r) == 0;
}

/**
 * wpe_ckrec_kind_is_kept - Is this a kind of verdict that r leaves where the run put it?
 * @kind: a verdict's kind.
 *
 * What the user gets: a file the run moved, linked, replaced or removed with no
 * copy to put back is kept and said, never overwritten or taken away by a
 * guess.  The review counts these separately from files it could not put back.
 */
int wpe_ckrec_kind_is_kept(WpeCkVerdictKind kind)
{
 switch (kind) {
 case WPE_CK_MOVED: case WPE_CK_MOVED_HIDDEN: case WPE_CK_LINKED: case WPE_CK_MAYBE_COPY:
 case WPE_CK_REPLACED: case WPE_CK_REMOVED_NO_COPY:
  return 1;
 case WPE_CK_STAYED: case WPE_CK_CREATED: case WPE_CK_REMOVED: case WPE_CK_NOT_READABLE_NOW:
 case WPE_CK_CANNOT_SAY:
  return 0;
 }
 return 0;
}

/**
 * wpe_ckrec_notes - The places one side could not reach.
 * @side: the moment; @notes: receives the record's own list.
 * Return: how many.  The notes are the only way to read what was left unseen,
 * so nothing a fill could not do can be thrown away by its caller.
 */
/**
 * wpe_ckrec_kind_is_gone - Is the path of a verdict of this kind not there now?
 * @kind: a verdict's kind.
 *
 * What the user gets: a row about a file that is gone (removed, or moved to a
 * hidden name) is in Messages but never in the list Alt-T steps, so a jump
 * does not open an empty window under the old name.
 */
int wpe_ckrec_kind_is_gone(WpeCkVerdictKind kind)
{
 switch (kind) {
 case WPE_CK_REMOVED: case WPE_CK_REMOVED_NO_COPY: case WPE_CK_MOVED_HIDDEN:
  return 1;
 case WPE_CK_STAYED: case WPE_CK_CREATED: case WPE_CK_MOVED: case WPE_CK_LINKED:
 case WPE_CK_MAYBE_COPY: case WPE_CK_REPLACED: case WPE_CK_NOT_READABLE_NOW:
 case WPE_CK_CANNOT_SAY:
  return 0;
 }
 return 0;
}

int wpe_ckrec_notes(const WpeCkRecord *r, WpeCkSide side, const WpeCkNote **notes)
{
 *notes = r->notes[side];
 return r->nnotes[side];
}

/* ---- a held file as it is now ---------------------------------------------- */

/** The then entry that holds image number @image. */
static CkEntry *ck_find_image(WpeCkRecord *r, int image)
{
 CkSide *s = ck_side(r, WPE_CK_THEN);
 int i;
 for (i = 0; i < s->n; i++)
  if (s->e[i].state == CK_HELD && s->e[i].image == image) return &s->e[i];
 return NULL;
}

/** A held file that could not be read: gone, or refused. */
static void ck_now_failed(const CkEntry *t, const WpeReadOutcome *o, WpeCkVerdict *v)
{
 ck_verdict_base(v, WPE_CK_NOT_READABLE_NOW, t);
 v->why = WPE_CK_WHY_REFUSED;
 v->read = *o;
 if (wpe_read_outcome_missing(o) || (o->why == WPE_READ_REFUSED_BY_SYSTEM && o->err == ENOTDIR)) {
  v->kind = WPE_CK_REMOVED;
  v->why = WPE_CK_WHY_NONE;
  v->folder_now = ck_folder_now(t->path);
 }
}

/**
 * wpe_ckrec_now_text - A file the checkpoint holds an image of, as it is now.
 * @image: the image's number; @len: receives the byte count; @v: receives the verdict.
 * Return: the bytes, or NULL with @v saying removed or not readable.
 *
 * The one reader of "now" for such a file, so a file the run removed or made
 * unreadable is told as that and never as "unchanged".
 */
char *wpe_ckrec_now_text(WpeCkRecord *r, int image, size_t *len, WpeCkVerdict *v)
{
 CkEntry *t = ck_find_image(r, image);
 WpeSeen named;
 WpeReadAsk ask;
 WpeReadOutcome o;
 char *text;
 *len = 0;
 if (!t) {
  memset(v, 0, sizeof *v);
  v->kind = WPE_CK_CANNOT_SAY;
  v->image = image;
  return NULL;
 }
 if (ck_under_replaced_root(r, t->path)) {
  ck_verdict_base(v, WPE_CK_CANNOT_SAY, t);
  v->why = WPE_CK_WHY_ROOT_REPLACED;
  return NULL;
 }
 if (wpe_seen_look_name(t->path, 0, &named) != 0) {
  wpe_read_outcome_errno(&o, errno);
  ck_now_failed(t, &o, v);
  return NULL;
 }
 ask = WPE_READ_ASK_JUDGED(CK_READ_WHAT, CK_TEXT_MAX, WPE_READ_CUT, &named);
 text = wpe_read_text(t->path, &ask, len, &o);
 if (!text) { ck_now_failed(t, &o, v); return NULL; }
 ck_verdict_base(v, WPE_CK_STAYED, t);
 return text;
}

/* ---- the words ------------------------------------------------------------- */

/**
 * wpe_ckrec_row_text - The short words after "path:line:" in the Messages window for a verdict.
 * @v: the verdict.
 * Return: the text, NULL for a verdict that has no row.  Short enough for the
 * 38 columns the docked pane leaves.
 */
const char *wpe_ckrec_row_text(const WpeCkVerdict *v)
{
 switch (v->kind) {
 case WPE_CK_STAYED:           return NULL;
 case WPE_CK_CREATED:          return "[AI] new file";
 case WPE_CK_REMOVED:          return "[AI] removed";
 case WPE_CK_REMOVED_NO_COPY:  return "[AI] removed, no copy";
 case WPE_CK_MOVED:            return "[AI] moved here";
 case WPE_CK_MOVED_HIDDEN:     return "[AI] now hidden";
 case WPE_CK_LINKED:           return "[AI] second name";
 case WPE_CK_MAYBE_COPY:       return "[AI] new, kept";
 case WPE_CK_REPLACED:         return "[AI] replaced, no copy";
 case WPE_CK_NOT_READABLE_NOW: return "[AI] not readable now";
 case WPE_CK_CANNOT_SAY:       return NULL;
 }
 return NULL;
}

/** The cause of a path nobody can say anything about, in words. */
static const char *ck_why_words(WpeCkWhy why)
{
 switch (why) {
 case WPE_CK_WHY_NONE:          return "it was not reached";
 case WPE_CK_WHY_REFUSED:       return "its folder could not be read";
 case WPE_CK_WHY_BOUND:         return "the walk stopped before it";
 case WPE_CK_WHY_NOT_LISTED:    return "nobody reads that place";
 case WPE_CK_WHY_ROOT_REPLACED: return "the workspace folder was replaced";
 case WPE_CK_WHY_TOOL:          return "git could not list it";
 case WPE_CK_WHY_IMAGE_LOST:    return "its copy cannot be read";
 }
 return "it was not reached";
}

/**
 * ck_say - Write one sentence of the review into the caller's room.
 * @out/@n: the room; @fmt: the sentence.
 * Return: the bytes written, 0 when nothing could be said.
 *
 * What the user gets: a path or a reason too long for the pane's line is cut
 * where a character ends and ends in "...", so a sentence never shows half a
 * letter and never runs past its room.
 */
static int ck_say(char *out, size_t n, const char *fmt, ...) WPE_FMT_PRINTF(3, 4);
static int ck_say(char *out, size_t n, const char *fmt, ...)
{
 va_list ap;
 int said;
 if (n == 0) return 0;
 va_start(ap, fmt);
 said = wpe_fmt_vshown(out, n, fmt, ap);
 va_end(ap);
 if (said == WPE_FMT_FAILED) out[0] = '\0';
 return (int)strlen(out);
}

/** ck_sentence_removed - The pane sentence for a removed file, never promising a copy back where its folder is gone. */
static int ck_sentence_removed(char *out, size_t n, const WpeCkVerdict *v)
{
 switch (v->folder_now) {
 case WPE_CK_FOLDER_THERE:
  return ck_say(out, n,"removed by the run: %s. r puts it back.", v->path);
 case WPE_CK_FOLDER_GONE:
  return ck_say(out, n,"removed by the run: %s, and its folder is gone too. r cannot put it back.",
                  v->path);
 case WPE_CK_FOLDER_IS_A_FILE:
  return ck_say(out, n,"removed by the run: %s, and its folder is now a file. r cannot put it back.",
                  v->path);
 }
 return 0;
}

/** ck_sentence_unreadable - The pane sentence for a held file that cannot be read now, with the system's reason. */
static int ck_sentence_unreadable(char *out, size_t n, const WpeCkVerdict *v)
{
 char reason[WPE_PATH_MSG_MAX];
 (void)wpe_read_reason(reason, sizeof reason, &v->read);
 return ck_say(out, n,"cannot be read now: %s: %s. Whether the run changed it is not known. "
                 "r puts the checkpoint's copy back.", v->path, reason);
}

/**
 * wpe_ckrec_sentence - The sentence the pane says for one verdict.
 * @out/@n: the buffer; @v: the verdict.
 * Return: the bytes written (cut with "..." when the room is short); 0 for a
 * verdict that needs no sentence.
 */
int wpe_ckrec_sentence(char *out, size_t n, const WpeCkVerdict *v)
{
 const char *from = v->from ? v->from : "";
 if (v->is_folder && v->kind != WPE_CK_MOVED) {
  if (n) out[0] = '\0';
  return 0;
 }
 switch (v->kind) {
 case WPE_CK_STAYED:  case WPE_CK_CREATED:
  if (n) out[0] = '\0';
  return 0;
 case WPE_CK_REMOVED:          return ck_sentence_removed(out, n, v);
 case WPE_CK_REMOVED_NO_COPY:
  return ck_say(out, n,"removed by the run: %s. The checkpoint holds no copy of it, so r cannot "
                  "put it back.", v->path);
 case WPE_CK_MOVED:
  return ck_say(out, n,"moved by the run: %s is now %s. r leaves it where it is.", from, v->path);
 case WPE_CK_MOVED_HIDDEN:
  return ck_say(out, n,"moved by the run: %s is now %s, a hidden name the review does not list. "
                  "r leaves it where it is.", v->path, from);
 case WPE_CK_LINKED:
  return ck_say(out, n,"%s is a second name of %s, added by the run. r leaves it.", v->path, from);
 case WPE_CK_MAYBE_COPY:
  return ck_say(out, n,"%s is new, and it is as large as %s, which the run removed and the "
                  "checkpoint holds no copy of. r keeps it.", v->path, from);
 case WPE_CK_REPLACED:
  return ck_say(out, n,"%s was replaced by another file, and the checkpoint holds no copy of the "
                  "old one. r leaves it.", v->path);
 case WPE_CK_NOT_READABLE_NOW: return ck_sentence_unreadable(out, n, v);
 case WPE_CK_CANNOT_SAY:
  if (!ck_unsaid_without_note(v)) {      /* the folder's note says it once for every path in it */
   if (n) out[0] = '\0';
   return 0;
  }
  return ck_say(out, n,"cannot say what the run did to %s: %s.", v->path, ck_why_words(v->why));
 }
 return 0;
}

/**
 * wpe_ckrec_note_sentence - The sentence the pane says for one place that was not reached.
 * @out/@n: the buffer; @note: the note.
 * Return: the bytes written (cut with "..." when the room is short).
 */
int wpe_ckrec_note_sentence(char *out, size_t n, const WpeCkNote *note)
{
 int then = note->side == WPE_CK_THEN;
 switch (note->why) {
 case WPE_CK_WHY_NONE:
  break;
 case WPE_CK_WHY_REFUSED:
  if (then)
   return ck_say(out, n,"not watched: %s: %s. What the run makes or removes in it is not listed, "
                   "and r keeps it.", note->where, note->reason);
  return ck_say(out, n,"not seen after the run: %s: %s. What the run made or removed in it is not "
                  "listed, and r keeps it.", note->where, note->reason);
 case WPE_CK_WHY_BOUND:
  if (then)
   return ck_say(out, n,"the snapshot watches %d files under %s; a file the run creates beyond "
                   "them is unseen", note->count, note->where);
  return ck_say(out, n,"the review saw %d new files under %s and stopped; more new files are not "
                  "listed, and r keeps them", note->count, note->where);
 case WPE_CK_WHY_NOT_LISTED:
  return ck_say(out, n,"not read: %s. What the run did in it is not listed, and r keeps it.",
                  note->where);
 case WPE_CK_WHY_ROOT_REPLACED:
  return ck_say(out, n,"the workspace folder was replaced since the checkpoint; nothing in it is "
                  "listed, restored or taken away");
 case WPE_CK_WHY_TOOL:
  return ck_say(out, n,"git could not list %s after the run. New files are not listed, and r "
                  "keeps them.", note->where);
 case WPE_CK_WHY_IMAGE_LOST:
  return ck_say(out, n,"the checkpoint's copy of %s cannot be read: %s. r cannot put it back.",
                  note->where, note->reason);
 }
 return ck_say(out, n,"not seen: %s", note->where);
}

/** The cause of a note, as one word of the trace. */
static const char *ck_why_word(WpeCkWhy why)
{
 switch (why) {
 case WPE_CK_WHY_NONE:          return "none";
 case WPE_CK_WHY_REFUSED:       return "refused";
 case WPE_CK_WHY_BOUND:         return "bound";
 case WPE_CK_WHY_NOT_LISTED:    return "unread";
 case WPE_CK_WHY_ROOT_REPLACED: return "root";
 case WPE_CK_WHY_TOOL:          return "tool";
 case WPE_CK_WHY_IMAGE_LOST:    return "image";
 }
 return "none";
}

/**
 * wpe_ckrec_note_trace - The trace line of one note.
 * @stage: "checkpoint" or "changeset", the first word of the line.
 */
int wpe_ckrec_note_trace(char *out, size_t n, const WpeCkNote *note, const char *stage)
{
 if (note->why == WPE_CK_WHY_BOUND)
  return ck_say(out, n,"%s note bound seen=%d dir=%s", stage, note->count, note->where);
 return ck_say(out, n,"%s note %s %s", stage, ck_why_word(note->why), note->where);
}

/** wpe_ckrec_more_notes - The pane line that stands for the notes not shown. */
int wpe_ckrec_more_notes(char *out, size_t n, int more)
{
 return ck_say(out, n,"and %d more not seen (all of them are in the trace)", more);
}

/**
 * wpe_ckrec_header - The line the review opens with.
 * @changes: the rows; @unseen: the places not seen.
 */
int wpe_ckrec_header(char *out, size_t n, int changes, int unseen)
{
 if (changes == 0 && unseen == 0)
  return ck_say(out, n,"=== AI changeset: no files changed ===");
 if (changes == 0)
  return ck_say(out, n,"=== AI changeset: no change seen, %d not seen ===", unseen);
 if (unseen == 0)
  return ck_say(out, n,"=== AI changeset: %d change%s (Alt-T / Alt-V to step) ===", changes,
                  changes == 1 ? "" : "s");
 return ck_say(out, n,"=== AI changeset: %d change%s, %d not seen (Alt-T / Alt-V to step) ===",
                 changes, changes == 1 ? "" : "s", unseen);
}

/** wpe_ckrec_more_sentences - The pane line that stands for the file sentences not shown. */
int wpe_ckrec_more_sentences(char *out, size_t n, int more)
{
 return ck_say(out, n,"and %d more like these (all of them are in the trace)", more);
}

/** wpe_ckrec_unseen_line - The Messages line that points at the notes in the pane. */
int wpe_ckrec_unseen_line(char *out, size_t n, int unseen)
{
 return ck_say(out, n,"[AI] %d not seen (the pane says why)", unseen);
}

/**
 * wpe_ckrec_last_line - The review's last line after a revert: only what is true.
 * @failed: files that could not be put back; @kept: files left where the run
 * put them; @unseen: places not seen.
 *
 * "ALL changes reverted" is said only when nothing failed, nothing was kept
 * and nothing was left unseen.
 */
int wpe_ckrec_last_line(char *out, size_t n, int failed, int kept, int unseen)
{
 if (failed == 0 && kept == 0 && unseen == 0)
  return ck_say(out, n,"[AI] ALL changes reverted to the checkpoint");
 if (kept == 0 && unseen == 0)
  return ck_say(out, n,"[AI] %d file%s kept the change (the pane says why)", failed,
                  failed == 1 ? "" : "s");
 return ck_say(out, n,"[AI] reverted; %d kept, %d not seen (the pane says why)", failed + kept,
                 unseen);
}

/** wpe_ckrec_kept_question - The question of the file-by-file review for a file r will not touch. */
int wpe_ckrec_kept_question(char *out, size_t n, WpeCkVerdictKind kind, const char *shown)
{
 WpeCkVerdict v;
 const char *row;
 memset(&v, 0, sizeof v);
 v.kind = kind;
 row = wpe_ckrec_row_text(&v);
 return ck_say(out, n,"%s: %s; r leaves it. Enter = next", shown, row ? row + 5 : "kept");
}
