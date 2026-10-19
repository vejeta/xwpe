/* we_ai_ws.c - the AI assistant's WORKSPACE layer: which files the agent may
 * study (scope), a checkpoint before an unattended run, the changeset review
 * afterwards (navigable with the Next/Prev-Error keys), and sessions tied to
 * the workspace.  Editor-side.  Compiled only under --enable-ai (WPE_AI). */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include "progr.h"
#include "we_ai.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <json-c/json.h>

extern struct dirfile **e_p_df;      /* project member list (we_prog.c)      */

/* ===================== small helpers ===================================== */

static char *ws_strdup(const char *s)
{
 size_t n = strlen(s ? s : "") + 1;
 char *p = malloc(n);
 if (p) memcpy(p, s ? s : "", n);
 return p;
}

/**
 * wpe_ai_shell_quote - Single-quote one argument for /bin/sh.
 * @dst/@sz: the quoted result; @src: the argument as the user knows it.
 *
 * Every path the assistant hands to a shell goes through this, so a file
 * called "a b.c" or "it's.c" reaches the command as that one file instead of
 * being split at the space or ending the quote early.  An embedded single
 * quote is written as '\'' , the only way to carry one inside single quotes.
 */
void wpe_ai_shell_quote(char *dst, size_t sz, const char *src)
{
 size_t o = 0;
 if (sz < 3) { if (sz) dst[0] = '\0'; return; }
 dst[o++] = '\'';
 for (; *src && o + 5 < sz; src++) {
  if (*src == '\'') { memcpy(dst + o, "'\\''", 4); o += 4; }
  else dst[o++] = *src;
 }
 dst[o++] = '\'';
 dst[o] = '\0';
}

/* Base directory for xwpe's runtime state, per the XDG Base Directory spec:
   $XDG_STATE_HOME/xwpe (default ~/.local/state/xwpe).  Writes into buf. */
static void ws_state_dir(char *buf, size_t sz)
{
 const char *xdg = getenv("XDG_STATE_HOME");
 const char *home = getenv("HOME");
 if (xdg && *xdg) snprintf(buf, sz, "%s/xwpe", xdg);
 else             snprintf(buf, sz, "%s/.local/state/xwpe", (home && *home) ? home : ".");
}

static void ws_list_push(char ***l, int *n, int *cap, char *s)
{
 if (*n == *cap) { *cap = *cap ? *cap * 2 : 8; *l = realloc(*l, (size_t)(*cap) * sizeof **l); }
 (*l)[(*n)++] = s;
}

static int ws_list_has(char **l, int n, const char *s)
{
 int i;
 for (i = 0; i < n; i++) if (!strcmp(l[i], s)) return 1;
 return 0;
}

void wpe_ai_free_list(char **list, int n)
{
 int i;
 if (!list) return;
 for (i = 0; i < n; i++) free(list[i]);
 free(list);
}

static char *ws_read_file(const char *path, size_t maxlen)
{
 FILE *fp = fopen(path, "rb");
 char *buf;
 size_t cap = 4096, len = 0;
 int ch;
 if (!fp) return NULL;
 buf = malloc(cap);
 if (!buf) { fclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < maxlen) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(buf, cap); if (!nb) break; buf = nb; }
  buf[len++] = (char)ch;
 }
 buf[len] = '\0';
 fclose(fp);
 return buf;
}

/**
 * ws_write_file - Write @text to @path, replacing what was there.
 *
 * Every file this layer writes is one the user relies on later: the
 * conversation to resume, the before image a revert restores, the copy a
 * post-edit command formats.  A disk that filled up or a device that went
 * away reports itself at the flush, not at the write, so the close is part of
 * the answer: the text is on disk only when all of it was written and the
 * close agreed.
 * Return: 0 when the file holds @text, -1 with errno saying why it does not.
 */
static int ws_write_file(const char *path, const char *text)
{
 FILE *fp = fopen(path, "wb");
 size_t want, got;
 int err;
 if (!fp) return -1;
 want = strlen(text);
 got = fwrite(text, 1, want, fp);
 if (got != want) {
  err = errno ? errno : ENOSPC;      /* a short write is a full device until told otherwise */
  fclose(fp);
  errno = err;
  return -1;
 }
 if (fclose(fp) != 0) return -1;    /* the buffered tail never reached the disk */
 return 0;
}

/**
 * ws_say_failed - Tell the user one file this layer could not do what it promised.
 * @f: any window of the editor; @what: the job as a verb phrase ("restore",
 * "save the conversation to"); @path: the file it was about; @reason: why it
 * did not happen, in the words of whoever refused.
 *
 * A conversation, a before image or a restored file that never reached the disk
 * looks exactly like one that did, and the next thing the user does depends on
 * which it was.  So the pane says it in the error colour, naming the path
 * (where to look) and the reason (what to fix: a full disk, a folder that is
 * not one, a file left read-only, a workspace that refused).  Every failure
 * this layer reports comes through here, so one shape covers whatever refused:
 * the kernel, a command the workspace owns, or a checkpoint holding nothing.
 */
static void ws_say_failed(FENSTER *f, const char *what, const char *path, const char *reason)
{
 char line[1700];
 snprintf(line, sizeof line, "could not %s %s: %s", what, path, reason);
 wpe_ai_pane_error(f, line);
}

/**
 * ws_write_failed - Tell the user one file this layer could not write.
 * @f: any window of the editor; @what: the file's job as a verb phrase
 * ("save the conversation to"); @path: where it was to go; @err: the errno the
 * failed call left.
 *
 * A write is refused by the kernel, so its reason is the errno's own words.
 */
static void ws_write_failed(FENSTER *f, const char *what, const char *path, int err)
{
 ws_say_failed(f, what, path, strerror(err));
}

/**
 * ws_output_reason - The first non-empty line of a command's output.
 * @out: what the command printed, NULL when it printed nothing; @status: its
 * exit status; @buf/@sz: where the reason goes.
 *
 * A restore the workspace refused is refused by the workspace's own tool, so
 * the reason is that tool's sentence and not an errno: git says why it would
 * not put the file back ("error: unable to unlink old 't.c': Permission
 * denied"), and that is the line the user can act on.  One line is enough
 * because the pane is where the user reads it, and the review's summary counts
 * the files that kept the change.
 */
static void ws_output_reason(const char *out, int status, char *buf, size_t sz)
{
 const char *p = out;
 while (p && *p) {
  const char *e = strchr(p, '\n');
  size_t l = e ? (size_t)(e - p) : strlen(p);
  while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) l--;
  if (l) {
   if (l >= sz) l = sz - 1;
   memcpy(buf, p, l);
   buf[l] = '\0';
   return;
  }
  if (!e) break;
  p = e + 1;
 }
 snprintf(buf, sz, "the command failed (status %d)", status);
}

/**
 * ws_make_dir - Create a directory this layer is about to write into.
 * @f: any window of the editor; @path: the directory to create.
 *
 * The directory is made by handing `mkdir -p` to a shell, and the result used
 * to be dropped: every write below it then failed too, and the user was told
 * nothing about either.  Checking turns that into one pane line naming the
 * directory and the reason, before the writes that need it are attempted, so a
 * folder that is missing is reported once and not once per file that would
 * have gone into it.
 *
 * The check asks the filesystem, not the shell's exit code.  What the writes
 * need is a directory at @path, however it came to be there, so a status that
 * disagrees with the filesystem would report a failure that is not one, and
 * name a reason errno never held.
 * Return: 0 when @path is a directory afterwards.
 */
static int ws_make_dir(FENSTER *f, const char *path)
{
 char q[1100], cmd[1400], *out;
 struct stat st;

 wpe_ai_shell_quote(q, sizeof q, path);
 snprintf(cmd, sizeof cmd, "mkdir -p %s", q);
 out = wpe_ai_run_capture(cmd);
 free(out);
 errno = 0;
 if (stat(path, &st) == 0) {
  if (S_ISDIR(st.st_mode)) return 0;
  errno = ENOTDIR;               /* a file is in the way, which stat does not call an error */
 }
 ws_write_failed(f, "create the folder", path, errno);
 return -1;
}

/* The workspace root: the project dir when a .prj is open, else the file's. */
static char *ws_root(FENSTER *f)
{
 char *d = e_build_dir(f);
 if (d && *d) return d;
 free(d);
 return ws_strdup(f->dirct ? f->dirct : ".");
}

/* ===================== scope ============================================= */

static const char *ws_reserved[] = {
 "Messages", "AI", "Watches", "Stack", "Project", "Grep", "Find", "Windows",
 "Variables", "Install", "Help", "Function-Index", "Noname", NULL };

static int ws_is_real_file_window(FENSTER *w)
{
 int i;
 if (!w || !w->datnam || !*w->datnam || !w->dirct) return 0;
 if (!DTMD_ISTEXT(w->dtmd)) return 0;
 for (i = 0; ws_reserved[i]; i++) if (!strcmp(w->datnam, ws_reserved[i])) return 0;
 return 1;
}

/**
 * wpe_ai_window_is_editable_file - May the assistant rewrite this window's text?
 * @w: the window an Edit, a Complete or a Multi-file edit was asked from.
 *
 * What the user gains: the assistant edits files and nothing else.  The AI
 * pane, the Messages window and every other tool pane are text windows to the
 * editor (they are buffers with a name), and the pane is where the focus rests
 * after a chat reply or a cancelled task; without this question the next Alt-G
 * e diffed the pane's own transcript and "y" overwrote it with a file.  A
 * window is editable here when it is a real file window (ws_is_real_file_window:
 * a text buffer backed by a path, not one of the editor's named panes) whose
 * text the user may change as well: the read-only viewers (Help, the clipboard,
 * a locked file on disk) and the output panes all carry WPE_INS_READONLY.
 *
 * Return: 1 for a file window the assistant may edit, 0 for anything else.
 */
int wpe_ai_window_is_editable_file(FENSTER *w)
{
 if (!ws_is_real_file_window(w)) return 0;
 if (w->ins == WPE_INS_READONLY) return 0;
 return 1;
}

static int ws_source_like(const char *name)
{
 const char *ext = strrchr(name, '.');
 static const char *ok[] = { ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".go",
   ".rs", ".java", ".scala", ".js", ".ts", ".sh", ".md", ".txt", ".mk",
   ".am", ".ac", ".pl", ".f", ".pas", ".cob", NULL };
 int i;
 if (!ext) return !strcmp(name, "Makefile");
 for (i = 0; ok[i]; i++) if (!strcasecmp(ext, ok[i])) return 1;
 return 0;
}

int wpe_ai_scope_files(FENSTER *f, int with_project, int with_folder, char ***out)
{
 ECNT *cn = f->ed;
 char **l = NULL;
 int n = 0, cap = 0, i;

 for (i = cn->mxedt; i > 0; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  if (p && !ws_list_has(l, n, p)) ws_list_push(&l, &n, &cap, p); else free(p);
 }
 if (with_project && e_project_is_open() && e_p_df && e_p_df[0]) {
  char *root = ws_root(f);
  int k;
  for (k = 0; k < e_p_df[0]->anz; k++) {
   const char *m = e_p_df[0]->name[k];
   char *p;
   if (!m || !*m || !strcmp(m, " ")) continue;
   if (m[0] == '/') p = ws_strdup(m);
   else { size_t L = strlen(root) + strlen(m) + 2; p = malloc(L); if (p) snprintf(p, L, "%s/%s", root, m); }
   if (p && !ws_list_has(l, n, p)) ws_list_push(&l, &n, &cap, p); else free(p);
  }
  free(root);
 }
 if (with_folder && f->dirct) {
  DIR *d = opendir(f->dirct);
  struct dirent *de;
  int added = 0;
  while (d && (de = readdir(d)) && added < 60) {
   char *p;
   size_t L;
   if (de->d_name[0] == '.' || !ws_source_like(de->d_name)) continue;
   L = strlen(f->dirct) + strlen(de->d_name) + 2;
   p = malloc(L);
   if (!p) continue;
   snprintf(p, L, "%s/%s", f->dirct, de->d_name);
   if (!ws_list_has(l, n, p)) { ws_list_push(&l, &n, &cap, p); added++; } else free(p);
  }
  if (d) closedir(d);
 }
 *out = l;
 return n;
}

/* A prompt block naming the files currently OPEN in the editor, with the one the
   user is focused on marked, so every AI mode is handed the working set directly
   instead of having to discover it (e.g. by inspecting running processes).
   Writes into buf (empty string when nothing qualifies) and returns it. */
char *wpe_ai_open_windows_block(FENSTER *f, char *buf, size_t n)
{
 ECNT *cn = f->ed;
 char **seen = NULL;
 size_t len = 0;
 /* When AI is invoked, f is often the AI pane rather than a file window, so mark
    the topmost real file window as focused; when f IS a file window, mark it. */
 int i, nseen = 0, cap = 0, count = 0, f_real = ws_is_real_file_window(f);

 buf[0] = '\0';
 for (i = cn->mxedt; i > 0 && len < n - 96; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  int focused;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  if (!p) continue;
  if (ws_list_has(seen, nseen, p)) { free(p); continue; }   /* one entry per file */
  ws_list_push(&seen, &nseen, &cap, p);                     /* takes ownership of p */
  if (count == 0)
   len += (size_t)snprintf(buf + len, n - len,
     "FILES OPEN IN THE EDITOR (the user's working set right now; prefer these "
     "when the request does not name a file):\n");
  focused = f_real ? (w == f) : (count == 0);
  len += (size_t)snprintf(buf + len, n - len, "  %s%s\n", p,
     focused ? "   <- focused (the file the user is looking at)" : "");
  count++;
 }
 wpe_ai_free_list(seen, nseen);
 return buf;
}

static char *ws_window_text(FENSTER *w)
{
 BUFFER *b = w->b;
 size_t cap = 4096, len = 0;
 char *t = malloc(cap);
 int y;
 if (!t) return NULL;
 for (y = 0; y < b->mxlines; y++) {
  const char *ls = (const char *)b->bf[y].s;
  int ll = b->bf[y].len < 0 ? 0 : b->bf[y].len;
  if (len + (size_t)ll + 2 > cap) { char *nt; while (len + (size_t)ll + 2 > cap) cap *= 2; nt = realloc(t, cap); if (!nt) { free(t); return NULL; } t = nt; }
  if (ls && ll > 0) { memcpy(t + len, ls, (size_t)ll); len += (size_t)ll; }
  t[len++] = '\n';
 }
 t[len] = '\0';
 return t;
}

static FENSTER *ws_find_window(FENSTER *f, const char *path)
{
 ECNT *cn = f->ed;
 int i;
 for (i = cn->mxedt; i > 0; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  int same;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  same = p && !strcmp(p, path);
  free(p);
  if (same) return w;
 }
 return NULL;
}

static const char *ws_absolute_path(const char *path, char *buf, size_t sz);

/**
 * wpe_ai_read_scope_file - A workspace file as the editor has it now.
 * @f: any window of the editor; @path: the file, as the model named it.
 *
 * An open window wins over the disk, unsaved edits included, so a plan's
 * read_file and the approval box's counts see the text Apply all will
 * replace.  The path is resolved the way the window table spells it, so a
 * relative "t.c" finds the open "/home/u/proj/t.c" instead of falling back
 * to the disk copy that lacks the user's last keystrokes.
 *
 * Return: malloc'd text, or NULL when the file is neither open nor on disk.
 */
char *wpe_ai_read_scope_file(FENSTER *f, const char *path)
{
 char abs[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 FENSTER *w = ws_find_window(f, full);
 if (w) return ws_window_text(w);
 return ws_read_file(full, 200000);
}

/* ===================== checkpoint ======================================== */

static int    ck_mode;                 /* 0 none, 1 git ref, 2 snapshot copies */
static char   ck_ref[160];
static char   ck_dir[1024];
static char   ck_snapdir[1024];
static char **ck_files;
static int    ck_n;
static char **ck_before;  static int ck_before_n;  /* files present before   */
static char **ck_dirs;    static int ck_dirs_n;    /* dirs watched for new   */

int wpe_ai_checkpoint_active(void) { return ck_mode != 0; }

static void ck_clear(void)
{
 wpe_ai_free_list(ck_files, ck_n);
 wpe_ai_free_list(ck_before, ck_before_n);
 wpe_ai_free_list(ck_dirs, ck_dirs_n);
 ck_files = NULL; ck_n = 0;
 ck_before = NULL; ck_before_n = 0;
 ck_dirs = NULL; ck_dirs_n = 0;
 ck_mode = 0; ck_ref[0] = '\0';
}

/* Regular, non-hidden files directly inside dir (full paths), bounded. */
static void ws_list_dir_files(const char *dir, char ***l, int *n, int *cap)
{
 DIR *d = opendir(dir);
 struct dirent *de;
 int added = 0;
 while (d && (de = readdir(d)) && added < 2000) {
  char *p;
  size_t L;
  struct stat st;
  if (de->d_name[0] == '.') continue;
  L = strlen(dir) + strlen(de->d_name) + 2;
  p = malloc(L);
  if (!p) continue;
  snprintf(p, L, "%s/%s", dir, de->d_name);
  if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && !ws_list_has(*l, *n, p)) {
   ws_list_push(l, n, cap, p);
   added++;
  } else free(p);
 }
 if (d) closedir(d);
}

/* Drop trailing slashes so the same directory always has ONE spelling (the
 * root may arrive as ".../dir/" while a file's dirname is ".../dir"). */
static void ws_strip_slash(char *d)
{
 size_t l = strlen(d);
 while (l > 1 && d[l - 1] == '/') d[--l] = '\0';
}

/* The directories to watch for NEW files: the root plus every scope file's dir. */
static void ws_watch_dirs(char **scope, int nscope)
{
 int i, cap = 0;
 wpe_ai_free_list(ck_dirs, ck_dirs_n);
 ck_dirs = NULL; ck_dirs_n = 0;
 ws_list_push(&ck_dirs, &ck_dirs_n, &cap, ws_strdup(ck_dir));
 for (i = 0; i < nscope; i++) {
  const char *sl = strrchr(scope[i], '/');
  char *d;
  if (!sl) continue;
  d = ws_strdup(scope[i]);
  d[sl - scope[i]] = '\0';
  if (!d[0]) { free(d); d = ws_strdup("/"); }
  ws_strip_slash(d);
  if (!ws_list_has(ck_dirs, ck_dirs_n, d)) ws_list_push(&ck_dirs, &ck_dirs_n, &cap, d);
  else free(d);
 }
}

int wpe_ai_checkpoint_create(FENSTER *f, char **scope, int nscope)
{
 char *root = ws_root(f);
 char q[1100], cmd[1400], *out;
 int i;

 ck_clear();
 strncpy(ck_dir, root, sizeof ck_dir - 1);
 ck_dir[sizeof ck_dir - 1] = '\0';
 ws_strip_slash(ck_dir);
 free(root);

 wpe_ai_shell_quote(q, sizeof q, ck_dir);
 snprintf(cmd, sizeof cmd, "cd %s && git rev-parse --is-inside-work-tree 2>/dev/null", q);
 out = wpe_ai_run_capture(cmd);
 if (out && !strncmp(out, "true", 4)) {
  free(out);
  snprintf(cmd, sizeof cmd, "cd %s && git stash create 2>/dev/null", q);
  out = wpe_ai_run_capture(cmd);
  if (out) { size_t l = strlen(out); while (l && (out[l-1] == '\n' || out[l-1] == ' ')) out[--l] = '\0'; }
  if (out && *out && strlen(out) < sizeof ck_ref) strcpy(ck_ref, out);
  else strcpy(ck_ref, "HEAD");
  free(out);
  ck_mode = 1;
  wpe_ai_trace("checkpoint git ref=%s dir=%s", ck_ref, ck_dir);
  return 0;
 }
 free(out);

 /* snapshot copies */
 {
  char sdir[1024];
  time_t now = time(NULL);
  int failed = 0, told = 0, readable = 0, have_dir;
  ws_state_dir(sdir, sizeof sdir);
  snprintf(ck_snapdir, sizeof ck_snapdir, "%s/checkpoints/%ld", sdir, (long)now);
  /* without the folder no copy can exist, and the folder's own failure has
     just been reported: the loop stays out rather than repeat it per file */
  have_dir = ws_make_dir(f, ck_snapdir) == 0;
  if (have_dir) {
   for (i = 0; i < nscope; i++) {
    char *text = ws_read_file(scope[i], 4000000), dst[1200];
    if (!text) continue;
    readable++;
    snprintf(dst, sizeof dst, "%s/%d", ck_snapdir, ck_n);
    if (ws_write_file(dst, text) == 0) {
     int cap = ck_n;
     ws_list_push(&ck_files, &ck_n, &cap, ws_strdup(scope[i]));
    } else {
     int err = errno;
     failed++;
     /* one line for the whole loop: a full disk fails every copy alike, and
        the first path plus the reason is all the user can act on */
     if (!told) { ws_write_failed(f, "save the before image of", scope[i], err); told = 1; }
    }
    free(text);
   }
  } else failed = nscope;
  /* remember what exists now, so files the run CREATES are detected too */
  ws_watch_dirs(scope, nscope);
  {
   int cap = 0, k;
   wpe_ai_free_list(ck_before, ck_before_n);
   ck_before = NULL; ck_before_n = 0;
   for (k = 0; k < ck_dirs_n; k++) ws_list_dir_files(ck_dirs[k], &ck_before, &ck_before_n, &cap);
  }
  /* A checkpoint with no before image in it can put nothing back, so it is not
     taken: leaving it active would open a review that offers a revert over
     files it could only restore nothing from, and the review's word is what
     the user believes about the workspace.  The reason is already in the pane,
     from the folder or from the first copy that failed, so this says it to the
     record only.  Files that were never readable are not counted as wanted:
     there was nothing of theirs to save, and a run that only CREATES files
     still gets a working revert, which needs no before image. */
  if (!have_dir || (readable && ck_n == 0)) {
   wpe_ai_trace("checkpoint refused n=%d failed=%d dir=%s", ck_n, failed, ck_snapdir);
   return -1;
  }
  wpe_ai_trace("checkpoint snapshot n=%d failed=%d dir=%s", ck_n, failed, ck_snapdir);
 }
 ck_mode = 2;
 return 0;
}

/* ===================== changeset ========================================= */

typedef struct { char *path; int line; char *text; int is_new; } ws_change;

/* Collapse runs of '/' to a single '/' in place, so paths built from a scope
   list (which may carry a "dir//name" from a trailing-slash join) compare equal
   to the plainly-spelled ones -- otherwise the same file shows up twice in the
   changeset and reverts match only one spelling. */
static void ws_path_norm(char *p)
{
 char *r = p, *w = p;
 int prev_slash = 0;
 for (; *r; r++) {
  if (*r == '/' && prev_slash) continue;
  *w++ = *r;
  prev_slash = (*r == '/');
 }
 *w = '\0';
}

static void ws_add_change(ws_change **c, int *n, int *cap, const char *path,
                          int line, const char *text, int is_new)
{
 char *np = ws_strdup(path);
 int i;
 if (np) ws_path_norm(np);
 for (i = 0; i < *n; i++)                       /* drop a duplicate for the same spot */
  if ((*c)[i].line == line && np && (*c)[i].path && !strcmp((*c)[i].path, np))
   { free(np); return; }
 if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *c = realloc(*c, (size_t)(*cap) * sizeof **c); }
 (*c)[*n].path = np ? np : ws_strdup(path);
 (*c)[*n].line = line;
 (*c)[*n].text = ws_strdup(text);
 (*c)[*n].is_new = is_new;
 (*n)++;
}

/* Parse `git diff` output: one change per hunk, at the new-file start line.
   The caller asks git for no context lines (-U0), so that start line is the
   first line the run changed and the '+'/'-' counts are the hunk's own. */
static void ws_changes_from_gitdiff(const char *diff, const char *path,
                                    ws_change **c, int *n, int *cap)
{
 const char *p = diff;
 int start = -1, plus = 0, minus = 0;
 while (p && *p) {
  const char *nl = strchr(p, '\n');
  size_t l = nl ? (size_t)(nl - p) : strlen(p);
  if (l >= 2 && p[0] == '@' && p[1] == '@') {
   const char *pl;
   char text[64];
   if (start >= 0) {
    snprintf(text, sizeof text, "[AI] +%d -%d", plus, minus);
    ws_add_change(c, n, cap, path, start, text, 0);
   }
   pl = strchr(p, '+');
   start = pl ? atoi(pl + 1) : 1;
   if (start < 1) start = 1;
   plus = minus = 0;
  } else if (l >= 1 && start >= 0) {
   if (p[0] == '+' && !(l >= 3 && p[1] == '+' && p[2] == '+')) plus++;
   else if (p[0] == '-' && !(l >= 3 && p[1] == '-' && p[2] == '-')) minus++;
  }
  if (!nl) break;
  p = nl + 1;
 }
 if (start >= 0) {
  char text[64];
  snprintf(text, sizeof text, "[AI] +%d -%d", plus, minus);
  ws_add_change(c, n, cap, path, start, text, 0);
 }
}

/* Dot-paths (the project-mode ".xwpe-ai-session" sidecar and any other hidden
 * file) are not part of a reviewable changeset. */
static int ws_hidden_path(const char *rel)
{
 const char *p = rel;
 while (p && *p) {
  if (*p == '.' && (p == rel || p[-1] == '/')) return 1;
  p = strchr(p, '/');
  if (p) p++;
 }
 return 0;
}

static int ws_collect_changes(ws_change **c, int *n)
{
 int cap = 0;
 *c = NULL; *n = 0;
 if (ck_mode == 1) {
  char q[1100], cmd[1400], *out, *line;
  wpe_ai_shell_quote(q, sizeof q, ck_dir);
  snprintf(cmd, sizeof cmd, "cd %s && git diff --name-only %s 2>/dev/null", q, ck_ref);
  out = wpe_ai_run_capture(cmd);
  line = out;
  while (line && *line) {
   char *nl = strchr(line, '\n'), qf[1100], full[1200], *d;
   if (nl) *nl = '\0';
   if (*line && !ws_hidden_path(line)) {
    snprintf(full, sizeof full, "%s/%s", ck_dir, line);
    wpe_ai_shell_quote(qf, sizeof qf, line);
    /* -U0: no context, so a hunk starts at the first line the run changed.
       With git's default three context lines the hunk starts three lines above
       it, and both the row and the jump it registers then point at a line the
       run never touched. */
    snprintf(cmd, sizeof cmd, "cd %s && git diff -U0 %s -- %s 2>/dev/null", q, ck_ref, qf);
    d = wpe_ai_run_capture(cmd);
    ws_changes_from_gitdiff(d ? d : "", full, c, n, &cap);
    free(d);
   }
   if (!nl) break;
   line = nl + 1;
  }
  free(out);
  snprintf(cmd, sizeof cmd, "cd %s && git status --porcelain --untracked-files=all 2>/dev/null", q);
  out = wpe_ai_run_capture(cmd);
  line = out;
  while (line && *line) {
   char *nl = strchr(line, '\n');
   if (nl) *nl = '\0';
   if (!strncmp(line, "?? ", 3) && !ws_hidden_path(line + 3)) {
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", ck_dir, line + 3);
    ws_add_change(c, n, &cap, full, 1, "[AI] new file", 1);
   }
   if (!nl) break;
   line = nl + 1;
  }
  free(out);
  return *n;
 }
 if (ck_mode == 2) {
  int i;
  for (i = 0; i < ck_n; i++) {
   char snap[1200];
   char *old, *now;
   wpe_ai_seg *segs;
   int ns, k, newline = 1;
   snprintf(snap, sizeof snap, "%s/%d", ck_snapdir, i);
   old = ws_read_file(snap, 4000000);
   now = ws_read_file(ck_files[i], 4000000);
   if (!old || !now) { free(old); free(now); continue; }
   ns = wpe_ai_diff_segments(old, now, &segs);
   for (k = 0; k < ns; k++) {
    if (segs[k].is_change) {
     char text[64];
     snprintf(text, sizeof text, "[AI] +%d -%d", segs[k].bn, segs[k].an);
     ws_add_change(c, n, &cap, ck_files[i], newline, text, 0);
     newline += segs[k].bn;
    } else newline += segs[k].an;
   }
   wpe_ai_segs_free(segs, ns);
   free(old); free(now);
  }
  /* files that did not exist at checkpoint time = new files */
  {
   char **nowl = NULL;
   int nn = 0, ncap = 0, k;
   for (k = 0; k < ck_dirs_n; k++) ws_list_dir_files(ck_dirs[k], &nowl, &nn, &ncap);
   for (k = 0; k < nn; k++)
    if (!ws_list_has(ck_before, ck_before_n, nowl[k]))
     ws_add_change(c, n, &cap, nowl[k], 1, "[AI] new file", 1);
   wpe_ai_free_list(nowl, nn);
  }
  return *n;
 }
 return 0;
}

static void ws_free_changes(ws_change *c, int n)
{
 int i;
 for (i = 0; i < n; i++) { free(c[i].path); free(c[i].text); }
 free(c);
}

/* The most a file reloaded into a window may hold, in bytes. */
#define WS_RELOAD_MAX 4000000

/**
 * ws_window_show_text - Put `text` in the window's buffer and repaint it.
 * @w: an open file window; @text: the complete new content.
 *
 * Takes no undo snapshot of its own: the caller decides whether this is a new
 * step for Ctrl-U or the continuation of one already taken.  The cursor is
 * kept on a line that still exists and the window is marked modified, so the
 * user sees the new text at once and is asked before losing it.
 */
static void ws_window_show_text(FENSTER *w, const char *text)
{
 e_buffer_set_text(w->b, text);
 if (w->b->b.y >= w->b->mxlines) w->b->b.y = w->b->mxlines ? w->b->mxlines - 1 : 0;
 w->save++;
 e_firstl(w, 1);
 e_schirm(w, 1);
 e_rep_win_tree(w->ed);
 e_refresh();
}

/**
 * ws_window_reload_undoable - Reload a window from its file as ONE undo step.
 * @w: the open window; @path: the file to read.
 *
 * The text the user had is snapshotted first, so a single Ctrl-U brings it
 * back after a tool rewrote the file or a changeset revert restored it.
 * Return: 1 when the file was read and shown, 0 when it could not be read
 * (the buffer is then left untouched and no undo step is taken).
 */
static int ws_window_reload_undoable(FENSTER *w, const char *path)
{
 char *text = ws_read_file(path, WS_RELOAD_MAX);
 if (!text) return 0;
 e_add_undo('B', w->b, w->b->b.x, w->b->b.y, 0);
 ws_window_show_text(w, text);
 free(text);
 return 1;
}

/* Reload an open window from disk (after a revert) with one undo snapshot. */
static void ws_reload_window(FENSTER *f, const char *path)
{
 FENSTER *w = ws_find_window(f, path);
 if (!w) return;
 ws_window_reload_undoable(w, path);
}

/**
 * ws_path_resolve_dots - Fold the "." and ".." segments of an absolute path in place.
 * @p: an absolute path with single slashes (see ws_path_norm).
 *
 * A proposal for "../outside.c" and the window table's "/home/u/outside.c"
 * must name the same file, for the counts to come from the open buffer and
 * for the workspace check to see where the file really goes.  Lexical only:
 * no symbolic link is followed and the file need not exist, since a proposal
 * may be the one creating it.
 */
static void ws_path_resolve_dots(char *p)
{
 char *r = p, *w = p;
 if (*r != '/') return;
 while (*r) {
  char *seg = r;                        /* at the '/' that opens a segment */
  size_t n;
  r++;
  while (*r && *r != '/') r++;
  n = (size_t)(r - seg - 1);            /* the segment without its slash   */
  if (n == 0) continue;                 /* "//" or a trailing slash        */
  if (n == 1 && seg[1] == '.') continue;
  if (n == 2 && seg[1] == '.' && seg[2] == '.') {
   while (w > p && *--w != '/') ;       /* drop the segment before it      */
   continue;
  }
  memmove(w, seg, n + 1);
  w += n + 1;
 }
 if (w == p) *w++ = '/';
 *w = '\0';
}

/**
 * ws_absolute_path - The path a tool gave, as the window table knows it.
 * @path: as the tool wrote it; @buf/@sz: room for the resolved path.
 *
 * ws_find_window compares against the window's ABSOLUTE path, but a tool may
 * name a file relative to the editor's working directory, or through ".."
 * segments; resolving it here means a relative write still finds and
 * refreshes the window the user is looking at, a Multi-file count is taken
 * from that window, and the workspace check compares like with like.
 *
 * Return: `buf` when resolved, else `path` itself (no working directory, or
 * a path too long for the buffer).
 */
static const char *ws_absolute_path(const char *path, char *buf, size_t sz)
{
 char cwd[1024];
 if (!path) return path;
 if (path[0] == '/') {
  if (strlen(path) >= sz) return path;
  snprintf(buf, sz, "%s", path);
 } else {
  if (!getcwd(cwd, sizeof cwd)) return path;
  if (strlen(cwd) + 1 + strlen(path) >= sz) return path;
  snprintf(buf, sz, "%s/%s", cwd, path);
 }
 ws_path_norm(buf);
 ws_path_resolve_dots(buf);
 return buf;
}

/**
 * wpe_ai_path_outside_workspace - Would this path land outside the workspace?
 * @f: any window of the editor; @path: the file, as the model named it.
 *
 * The workspace is the project's directory when a project is open, else the
 * directory of the file the request came from: the same root the scope list
 * and the checkpoint use.  A Multi-file proposal for "../outside.c" is
 * flagged with this so the user sees, before approving, that the model is
 * writing beyond the tree.  It is not refused: the model may be right.
 *
 * Return: 1 when the file lies outside, 0 when inside or when it cannot be
 * told (no root, or the root is "/", below which nothing is outside).
 */
int wpe_ai_path_outside_workspace(FENSTER *f, const char *path)
{
 char abs[1200], rootbuf[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 char *root = ws_root(f);
 const char *rootfull;
 size_t rl;
 int outside;
 if (!root || !full) { free(root); return 0; }
 rootfull = ws_absolute_path(root, rootbuf, sizeof rootbuf);
 rl = strlen(rootfull);
 while (rl > 1 && rootfull[rl - 1] == '/') rl--;
 if (rl <= 1) outside = 0;
 else if (strncmp(full, rootfull, rl) != 0) outside = 1;
 else if (full[rl] != '/' && full[rl] != '\0') outside = 1;
 else outside = 0;
 free(root);
 return outside;
}

/**
 * wpe_ai_reload_open_window - Show a file a tool just wrote, as one undo step.
 * @f: any window of the editor; @path: the file, as the tool named it.
 *
 * Called after the agent's write_file or apply_patch so an edit to the file
 * the user is looking at shows immediately, instead of leaving a stale buffer
 * on screen while disk has the new content.  Return: 1 when a window showed
 * the file, 0 when it is not open (nothing happens).
 */
int wpe_ai_reload_open_window(FENSTER *f, const char *path)
{
 char abs[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 FENSTER *w = ws_find_window(f, full);
 if (!w) return 0;
 return ws_window_reload_undoable(w, full);
}

/**
 * wpe_ai_window_take_file - Replace a window's text with a file's, no new undo step.
 * @w: the open window; @path: the file whose text the window takes.
 *
 * For the post-edit hook: its reformatting joins the undo step the change
 * already took, so the user undoes change and reformat with one Ctrl-U instead
 * of seeing a half-undone file after the first.  Return: 1 when taken.
 */
int wpe_ai_window_take_file(FENSTER *w, const char *path)
{
 char *text = ws_read_file(path, WS_RELOAD_MAX);
 if (!text) return 0;
 ws_window_show_text(w, text);
 free(text);
 return 1;
}

/**
 * wpe_ai_refresh_open_window - Re-read an open file after the hook rewrote it.
 * @f: any window of the editor; @path: the file, as the tool named it.
 *
 * The same lookup as wpe_ai_reload_open_window, but the buffer is refreshed
 * within the undo step that reload took.  Return: 1 when refreshed.
 */
int wpe_ai_refresh_open_window(FENSTER *f, const char *path)
{
 char abs[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 FENSTER *w = ws_find_window(f, full);
 if (!w) return 0;
 return wpe_ai_window_take_file(w, full);
}

/**
 * wpe_ai_window_hook_copy - Write a window's text to a temporary copy for the hook.
 * @w: the window holding the accepted change; @tmp/@sz: receives the copy's path.
 *
 * An accepted Edit stays in the buffer until the user saves, and a hook must
 * not break that promise by touching the file, so it works on a copy.  The
 * copy is a dot-file beside the real file that keeps its name and extension
 * (".ai-hook-<pid>-t.c"), so a formatter still sees the language and finds the
 * configuration file of that directory.  A copy that could not be written is
 * said in the pane: without it the command silently does not run, and the user
 * is left wondering why the text was not reformatted.
 * Return: 1 when the copy was written.
 */
int wpe_ai_window_hook_copy(FENSTER *w, char *tmp, size_t sz)
{
 char name[600], *full, *text;
 if (!ws_is_real_file_window(w)) return 0;
 snprintf(name, sizeof name, ".ai-hook-%ld-%s", (long)getpid(), w->datnam);
 full = e_mkfilename(w->dirct, name);
 if (!full) return 0;
 text = ws_window_text(w);
 if (!text || strlen(full) >= sz) { free(text); free(full); return 0; }
 if (ws_write_file(full, text) != 0) {
  int err = errno;
  ws_write_failed(w, "write the post-edit copy of", full, err);
  wpe_ai_trace("post-edit copy failed %s: %s", full, strerror(err));
  free(text);
  free(full);
  return 0;
 }
 strcpy(tmp, full);
 free(text);
 free(full);
 return 1;
}

/* What a revert says for a file the checkpoint holds no copy of: there is no
 * errno and no command to quote, since nothing was attempted that could fail.
 * The lookup that misses compares the changeset's normalized spelling against
 * ck_files' own, so a scope path carrying "//" lands here too; saying the copy
 * is missing is still the truth the user acts on, since nothing was restored. */
#define WS_NO_BEFORE_IMAGE "the checkpoint holds no before image of it"

/**
 * ws_revert_failed - Say one file the revert could not put back, and fail.
 * @f: any window of the editor; @what: "restore" or "remove"; @path: the file;
 * @reason: why it kept the change, in the words of whoever refused.
 *
 * Undoing a change is the promise the whole review stands on, so the user hears
 * at once which file kept the change and why, and the trace says the revert
 * failed instead of claiming one that did not happen.  The window is left
 * alone, since reloading it would show text the disk does not hold.
 * Return: -1, so the caller's count of files that kept the change stays honest.
 */
static int ws_revert_failed(FENSTER *f, const char *what, const char *path, const char *reason)
{
 ws_say_failed(f, what, path, reason);
 wpe_ai_trace("changeset revert failed %s: %s", path, reason);
 return -1;
}

/**
 * ws_revert_path - Put one file back the way the checkpoint found it.
 * @f: any window of the editor; @path: the file to put back;
 * @is_new: the run created it, so putting it back means taking it away.
 *
 * Every way this can fail is said, because a revert that reports success over a
 * file that kept the change leaves the user believing the workspace is clean:
 * the workspace's own tool refusing the restore (git's sentence is the reason,
 * not an errno), a directory that will not let a created file go, a copy that
 * could not be written back, and a checkpoint that holds no before image of the
 * file at all.
 * Return: 0 when the file is back, -1 when it kept the change.
 */
static int ws_revert_path(FENSTER *f, const char *path, int is_new)
{
 if (ck_mode == 1) {
  char q[1100], qf[1100], cmd[1400], *out, reason[600];
  int status = 0;
  const char *rel = path;
  size_t dl = strlen(ck_dir);
  if (!strncmp(path, ck_dir, dl) && path[dl] == '/') rel = path + dl + 1;
  wpe_ai_shell_quote(q, sizeof q, ck_dir);
  wpe_ai_shell_quote(qf, sizeof qf, rel);
  if (is_new) snprintf(cmd, sizeof cmd, "cd %s && rm -f -- %s 2>&1", q, qf);
  else        snprintf(cmd, sizeof cmd, "cd %s && git checkout %s -- %s 2>&1", q, ck_ref, qf);
  out = wpe_ai_run_capture_status(cmd, &status);
  if (status != 0) {
   ws_output_reason(out, status, reason, sizeof reason);
   free(out);
   return ws_revert_failed(f, is_new ? "remove" : "restore", path, reason);
  }
  free(out);
 } else if (ck_mode == 2) {
  char snap[1200], *text;
  int i;
  if (is_new) {
   if (unlink(path) != 0) return ws_revert_failed(f, "remove", path, strerror(errno));
   wpe_ai_trace("changeset revert %s", path);
   return 0;
  }
  for (i = 0; i < ck_n; i++) if (!strcmp(ck_files[i], path)) break;
  if (i == ck_n) return ws_revert_failed(f, "restore", path, WS_NO_BEFORE_IMAGE);
  snprintf(snap, sizeof snap, "%s/%d", ck_snapdir, i);
  text = ws_read_file(snap, 4000000);
  if (!text) return ws_revert_failed(f, "restore", path, WS_NO_BEFORE_IMAGE);
  if (ws_write_file(path, text) != 0) {
   int err = errno;
   free(text);
   return ws_revert_failed(f, "restore", path, strerror(err));
  }
  free(text);
 }
 if (!is_new) ws_reload_window(f, path);
 wpe_ai_trace("changeset revert %s", path);
 return 0;
}

/**
 * ws_say_kept_changes - Say how many files a revert left changed.
 * @f: any window of the editor; @failed: how many files kept the change.
 *
 * The review's last line is what the user believes about the workspace, so a
 * revert that could not write a file is counted here, in the window the review
 * has been speaking through; the pane holds each file's name and the reason.
 * Nothing is printed when every file went back, which the caller says in its
 * own words.
 */
static void ws_say_kept_changes(FENSTER *f, int failed)
{
 char line[200];
 if (failed <= 0) return;
 snprintf(line, sizeof line, "[AI] %d file%s kept the change (the pane says why)",
          failed, failed == 1 ? "" : "s");
 e_d_p_message(line, f, 0);
}

/**
 * ws_say_kept_unseen - Say how many files a review stopped asking about.
 * @f: any window of the editor; @left: how many files were never answered.
 *
 * Backing out of a per-file question leaves the files as the run wrote them,
 * which is the right outcome but not a visible one: without a count the user
 * cannot tell afterwards how much of the changeset went unreviewed.  The line
 * names the count and no path, so it fits the Messages window the review speaks
 * through at its narrowest.
 */
static void ws_say_kept_unseen(FENSTER *f, int left)
{
 char line[200];
 if (left <= 0) return;
 snprintf(line, sizeof line, "[AI] Esc: %d file%s kept unseen",
          left, left == 1 ? "" : "s");
 e_d_p_message(line, f, 0);
}

/**
 * ws_files_left - How many files a review backed out of never had their answer.
 * @c/@n: the changeset; @from: the entry the review stopped at, included;
 * @seen/@ns: the files the review had already asked about, the current one
 * among them.
 *
 * One file can carry several rows (a row per hunk), and the count the user is
 * told is a count of files, not of rows.  The file the review was asking about
 * is part of it, since a question backed out of was never answered; a file
 * answered earlier whose later hunk still lies ahead is not, since its answer
 * covered the whole file.
 */
static int ws_files_left(ws_change *c, int n, int from, char **seen, int ns)
{
 char **l = NULL; int nl = 0, cap = 0, i;
 ws_list_push(&l, &nl, &cap, ws_strdup(c[from].path));
 for (i = from + 1; i < n; i++)
  if (!ws_list_has(seen, ns, c[i].path) && !ws_list_has(l, nl, c[i].path))
   ws_list_push(&l, &nl, &cap, ws_strdup(c[i].path));
 wpe_ai_free_list(l, nl);
 return nl;
}

/**
 * ws_path_under_root - The spelling a Messages row names a file by.
 * @path: the file, absolute as the changeset holds it; @root: the workspace root.
 *
 * A row is read in a window that is 38 columns wide in the default layout, and
 * an absolute path fills it on its own: the user sees a fragment of a temporary
 * directory where the line and the counts should be.  Naming the file the way a
 * compiler run from the root names it leaves room for both, and matches the
 * spelling the rest of the editor's output uses.  Only the row is spelled this
 * way; the list the Next/Prev-Error keys walk keeps the absolute path, so a jump
 * opens the file it always opened.  A file outside the root has no shorter
 * spelling and keeps its absolute one.
 * Return: @path's tail under @root, or @path itself when it is not under it.
 */
static const char *ws_path_under_root(const char *path, const char *root)
{
 size_t rl = strlen(root);
 if (rl && !strncmp(path, root, rl) && path[rl] == '/') return path + rl + 1;
 return path;
}

/* The words in the pane when the changeset's path list could not be written. */
#define WS_PATHSPEC_WHAT "write the path list"

/**
 * ws_write_pathspec - Write the changeset's paths, one per file, for git to read.
 * @f: any window of the editor; @c/@n: the changeset on screen; @buf/@sz:
 * receives the list file's path.
 *
 * git reads the list through --pathspec-from-file, so the paths never meet a
 * shell and a changeset of any length is one command.  Each file is written
 * once, by its path under the workspace root, NUL-terminated so a name may
 * hold anything a file name may.  The list lives under the editor's state
 * directory, named for this process, and is removed once git has read it.
 * Return: 0 with @buf filled, -1 when the list could not be written, said in
 * the pane like any other write the editor could not make.
 */
static int ws_write_pathspec(FENSTER *f, ws_change *c, int n, char *buf, size_t sz)
{
 char dir[1024];
 char **done = NULL; int nd = 0, cap = 0, i;
 FILE *fp;
 ws_state_dir(dir, sizeof dir);
 if (ws_make_dir(f, dir) != 0) return -1;
 snprintf(buf, sz, "%s/changeset-%ld.pathspec", dir, (long)getpid());
 fp = fopen(buf, "w");
 if (!fp) { ws_write_failed(f, WS_PATHSPEC_WHAT, buf, errno); return -1; }
 for (i = 0; i < n; i++) {
  const char *rel;
  if (ws_list_has(done, nd, c[i].path)) continue;        /* one file, one entry */
  ws_list_push(&done, &nd, &cap, ws_strdup(c[i].path));
  rel = ws_path_under_root(c[i].path, ck_dir);
  fwrite(rel, 1, strlen(rel) + 1, fp);                    /* the NUL included */
 }
 wpe_ai_free_list(done, nd);
 if (ferror(fp) || fclose(fp) != 0) {
  ws_write_failed(f, WS_PATHSPEC_WHAT, buf, errno);
  return -1;
 }
 return 0;
}

/**
 * ws_git_on_changeset - Run one git command over the changeset's path list.
 * @f: any window of the editor; @what: the verb for the pane when git refuses
 * ("stage", "commit"); @verb: the git words before the pathspec option;
 * @list: the path list file; @out: receives git's output to free, or NULL.
 *
 * `git add` and `git commit` both take --pathspec-from-file, so staging the
 * listed files and committing exactly those files are one command shape.  When
 * git refuses, its own first line goes to the pane: a commit that quietly held
 * less than the review listed would be the same lie the pane may not tell about
 * a revert.
 * Return: 0 when git accepted the command, -1 when it refused.
 */
static int ws_git_on_changeset(FENSTER *f, const char *what, const char *verb,
                               const char *list, char **out)
{
 char q[1100], ql[1100], cmd[3600], reason[600], *o;
 int status = 0;
 wpe_ai_shell_quote(q, sizeof q, ck_dir);
 wpe_ai_shell_quote(ql, sizeof ql, list);
 snprintf(cmd, sizeof cmd,
          "cd %s && git %s --pathspec-from-file=%s --pathspec-file-nul 2>&1",
          q, verb, ql);
 o = wpe_ai_run_capture_status(cmd, &status);
 if (status != 0) {
  ws_output_reason(o, status, reason, sizeof reason);
  free(o);
  ws_say_failed(f, what, "the changeset", reason);
  return -1;
 }
 if (out) *out = o;
 else free(o);
 return 0;
}

/**
 * ws_commit_changeset_steps - Stage the listed files, then commit those files only.
 * @f: any window of the editor; @c/@n: the changeset on screen; @msg: the
 * commit message the user gave; @out: receives git's answer to the commit.
 *
 * A file the run created has to be staged before a commit can take it; the
 * commit then names the same paths, so whatever the user had staged for other
 * files before the run stays in the index and out of this commit.
 * Return: 0 when the commit was made, -1 when a step was refused.
 */
static int ws_commit_changeset_steps(FENSTER *f, ws_change *c, int n,
                                     const char *msg, char **out)
{
 char list[1100], qm[600], verb[800];
 int rc;
 if (ws_write_pathspec(f, c, n, list, sizeof list) != 0) return -1;
 rc = ws_git_on_changeset(f, "stage", "add", list, NULL);
 if (rc == 0) {
  wpe_ai_shell_quote(qm, sizeof qm, msg);
  snprintf(verb, sizeof verb, "commit -q -m %s", qm);
  rc = ws_git_on_changeset(f, "commit", verb, list, out);
 }
 unlink(list);
 return rc;
}

/**
 * ws_commit_changeset - Commit the files the review listed, and nothing else.
 * @f: any window of the editor; @c/@n: the changeset on screen; @msg: the
 * commit message the user gave.
 *
 * What the user gains: the commit `c` makes is the change the review just
 * listed, so an edit they made before the run stays where they left it,
 * unstaged or staged, and `git show` on the commit reads as one run's work.
 * Messages shows git's answer or "[AI] committed"; when a step was refused the
 * pane has said why and Messages says no commit was made.
 */
static void ws_commit_changeset(FENSTER *f, ws_change *c, int n, const char *msg)
{
 char *out = NULL;
 if (ws_commit_changeset_steps(f, c, n, msg, &out) != 0) {
  e_d_p_message("[AI] not committed (the pane says why)", f, 0);
  wpe_ai_trace("changeset not committed");
  return;
 }
 e_d_p_message(out && *out ? out : "[AI] committed", f, 0);
 free(out);
 wpe_ai_trace("changeset commit");
}

int wpe_ai_changeset_review(FENSTER *f)
{
 ws_change *c = NULL;
 int n = 0, i, kept = 0;
 char line[1400];
 char **files; int *lines; char **texts;

 if (!ck_mode) { e_d_p_message("[AI] no checkpoint - nothing to review", f, 1); return 0; }
 ws_collect_changes(&c, &n);
 if (n == 0) {
  e_d_p_message("=== AI changeset: no files changed ===", f, 1);
  ws_free_changes(c, n);
  wpe_ai_trace("changeset empty");
  return 0;
 }

 snprintf(line, sizeof line, "=== AI changeset: %d change%s (Alt-T / Alt-V to step) ===", n, n == 1 ? "" : "s");
 e_d_p_message(line, f, 1);
 files = malloc((size_t)n * sizeof *files);
 lines = malloc((size_t)n * sizeof *lines);
 texts = malloc((size_t)n * sizeof *texts);
 for (i = 0; i < n; i++) {
  /* the row is spelled from the root so its line and counts survive a narrow
     Messages window; files[i] is what Alt-T opens, so it stays absolute */
  snprintf(line, sizeof line, "%s:%d: %s",
           ws_path_under_root(c[i].path, ck_dir), c[i].line, c[i].text);
  e_d_p_message(line, f, 0);
  files[i] = c[i].path; lines[i] = c[i].line; texts[i] = c[i].text;
 }
 e_set_error_list(n, files, lines, NULL, texts);   /* Alt-T / Alt-V now step these */
 free(files); free(lines); free(texts);
 wpe_ai_trace("changeset n=%d", n);

 e_d_p_message("   a = keep all   r = REVERT all   f = file by file   c = commit (git)", f, 0);
 for (;;) {
  int k = e_toupper(e_getch());
  if (k == 'A' || k == WPE_ESC || k == 13 || k == '\r' || k == '\n') {
   kept = n;
   e_d_p_message("[AI] changes kept", f, 0);
   wpe_ai_trace("changeset keep all");
   break;
  }
  if (k == 'R') {
   char **bad = NULL; int nb = 0, bc = 0;
   for (i = 0; i < n; i++)
    if (ws_revert_path(f, c[i].path, c[i].is_new) != 0 && !ws_list_has(bad, nb, c[i].path))
     ws_list_push(&bad, &nb, &bc, ws_strdup(c[i].path));   /* one file, one count */
   if (nb) ws_say_kept_changes(f, nb);
   else    e_d_p_message("[AI] ALL changes reverted to the checkpoint", f, 0);
   wpe_ai_free_list(bad, nb);
   wpe_ai_trace("changeset revert all failed=%d", nb);
   break;
  }
  if (k == 'F') {
   char **seen = NULL; int ns = 0, sc = 0, bad = 0, left = 0;
   for (i = 0; i < n; i++) {
    if (ws_list_has(seen, ns, c[i].path)) continue;
    ws_list_push(&seen, &ns, &sc, ws_strdup(c[i].path));
    /* the question names the file the way its row does, so it fits the
       Messages window at its narrowest and the y/n hint stays visible */
    snprintf(line, sizeof line, "keep %s ?  y = keep  n = revert",
             ws_path_under_root(c[i].path, ck_dir));
    e_d_p_message(line, f, 0);
    for (;;) {
     int kk = e_toupper(e_getch());
     if (kk == 'Y' || kk == 13 || kk == '\r' || kk == '\n') { kept++; break; }
     if (kk == 'N') {
      if (ws_revert_path(f, c[i].path, c[i].is_new) != 0) bad++;
      break;
     }
     /* Esc backs out of the question the way it backs out of the review: the
        files stay as the run wrote them, and no further file is asked about. */
     if (kk == WPE_ESC) { left = ws_files_left(c, n, i, seen, ns); break; }
    }
    if (left) break;
   }
   wpe_ai_free_list(seen, ns);
   if (left) ws_say_kept_unseen(f, left);
   ws_say_kept_changes(f, bad);
   e_d_p_message("[AI] file-by-file review done", f, 0);
   kept += left;
   wpe_ai_trace("changeset file-by-file failed=%d", bad);
   break;
  }
  if (k == 'C' && ck_mode == 1) {
   static char msg[256];
   strcpy(msg, "AI-assisted change");
   if (e_add_arguments(msg, "Commit message", f, 0, AltB, NULL) && msg[0])
    ws_commit_changeset(f, c, n, msg);
   kept = n;
   break;
  }
 }
 ws_free_changes(c, n);
 return kept;
}

/* ===================== sessions ========================================== */

static struct {
 char  *cli;                 /* claudecli session id                         */
 char **role, **content;
 int    n;
 int    project_announced;   /* the pane has said which project instructions
                                this session folds in; cleared with the session */
 int    tool_turn_opened;    /* a chat turn of this session answered with a tool
                                line: the chat now acts on the repository       */
} sess;

#define WS_SESS_MAX 40

static void sess_free(void)
{
 int i;
 free(sess.cli); sess.cli = NULL;
 for (i = 0; i < sess.n; i++) { free(sess.role[i]); free(sess.content[i]); }
 free(sess.role); free(sess.content);
 sess.role = sess.content = NULL; sess.n = 0;
 sess.project_announced = 0;
 sess.tool_turn_opened = 0;
}

/**
 * wpe_ai_session_project_announced - Has this session already named its project instructions?
 *
 * The chat folds the project instructions into every turn once it has used
 * a tool (or always, under AIProjectInstructions "all") but tells the user
 * once per session, so the transcript is not peppered with the same line; the flag
 * lives with the session and starts over when the session is loaded or
 * cleared, not when a backend or model changes.
 * Return: 1 once the pane has shown the line for this session.
 */
int wpe_ai_session_project_announced(void) { return sess.project_announced; }

/** wpe_ai_session_mark_project_announced - Record that the pane showed the line. */
void wpe_ai_session_mark_project_announced(void) { sess.project_announced = 1; }

/**
 * wpe_ai_session_tool_turn_opened - Has a chat turn of this session used a tool yet?
 *
 * A question answered from the open file costs the user nothing of the
 * project instructions; the turn in which the model first asks to read or
 * search the repository is where those instructions start to matter, and
 * from that turn on the chat carries them for the rest of the session.  The
 * flag lives with the session, so a cleared or reloaded session starts plain
 * again.
 * Return: 1 once a chat turn has answered with a tool line.
 */
int wpe_ai_session_tool_turn_opened(void) { return sess.tool_turn_opened; }

/** wpe_ai_session_mark_tool_turn - Record that a chat turn answered with a tool line. */
void wpe_ai_session_mark_tool_turn(void) { sess.tool_turn_opened = 1; }

static unsigned long ws_hash(const char *s)
{
 unsigned long h = 5381;
 while (s && *s) h = h * 33 + (unsigned char)*s++;
 return h;
}

static void ws_session_path(FENSTER *f, char *buf, size_t sz)
{
 if (e_project_is_open()) {
  char *root = ws_root(f);
  snprintf(buf, sz, "%s/.xwpe-ai-session", root);
  free(root);
 } else {
  char sdir[1024];
  ws_state_dir(sdir, sizeof sdir);
  snprintf(buf, sz, "%s/ai", sdir);
  ws_make_dir(f, buf);        /* the write that follows says so if this failed */
  snprintf(buf, sz, "%s/ai/%lu.session", sdir, ws_hash(f->dirct ? f->dirct : "."));
 }
}

void wpe_ai_session_append(const char *role, const char *content)
{
 if (sess.n == WS_SESS_MAX) {                     /* drop the oldest turn */
  free(sess.role[0]); free(sess.content[0]);
  memmove(sess.role, sess.role + 1, (size_t)(sess.n - 1) * sizeof *sess.role);
  memmove(sess.content, sess.content + 1, (size_t)(sess.n - 1) * sizeof *sess.content);
  sess.n--;
 }
 sess.role = realloc(sess.role, (size_t)(sess.n + 1) * sizeof *sess.role);
 sess.content = realloc(sess.content, (size_t)(sess.n + 1) * sizeof *sess.content);
 sess.role[sess.n] = ws_strdup(role);
 sess.content[sess.n] = ws_strdup(content);
 sess.n++;
}

int wpe_ai_session_messages(wpe_ai_msg *buf, int max)
{
 int start = sess.n > max ? sess.n - max : 0, i, k = 0;
 for (i = start; i < sess.n; i++) { buf[k].role = sess.role[i]; buf[k].content = sess.content[i]; k++; }
 return k;
}

void wpe_ai_session_load(FENSTER *f)
{
 char path[1300], *text;
 struct json_object *o, *v, *arr;
 sess_free();
 free(e_ai_resume_session); e_ai_resume_session = NULL;
 ws_session_path(f, path, sizeof path);
 text = ws_read_file(path, 2000000);
 if (!text) return;
 o = json_tokener_parse(text);
 free(text);
 if (!o) return;
 if (json_object_object_get_ex(o, "cli_session", &v)) {
  const char *s = json_object_get_string(v);
  if (s && *s) { sess.cli = ws_strdup(s); e_ai_resume_session = ws_strdup(s); }
 }
 if (json_object_object_get_ex(o, "messages", &arr)) {
  int L = json_object_array_length(arr), i;
  for (i = 0; i < L; i++) {
   struct json_object *m = json_object_array_get_idx(arr, i), *r, *c;
   if (json_object_object_get_ex(m, "role", &r) && json_object_object_get_ex(m, "content", &c))
    wpe_ai_session_append(json_object_get_string(r), json_object_get_string(c));
  }
 }
 json_object_put(o);
 wpe_ai_trace("session load %s cli=%s n=%d", path, sess.cli ? sess.cli : "-", sess.n);
}

void wpe_ai_session_save(FENSTER *f)
{
 char path[1300];
 struct json_object *o = json_object_new_object(), *arr = json_object_new_array();
 int i;
 if (e_ai_last_session_id && *e_ai_last_session_id) {
  free(sess.cli); sess.cli = ws_strdup(e_ai_last_session_id);
 }
 json_object_object_add(o, "cli_session", json_object_new_string(sess.cli ? sess.cli : ""));
 for (i = 0; i < sess.n; i++) {
  struct json_object *m = json_object_new_object();
  json_object_object_add(m, "role", json_object_new_string(sess.role[i]));
  json_object_object_add(m, "content", json_object_new_string(sess.content[i]));
  json_object_array_add(arr, m);
 }
 json_object_object_add(o, "messages", arr);
 ws_session_path(f, path, sizeof path);
 /* The conversation on screen stays usable when this fails, so the turn is not
    abandoned: the user is told the exchange was not persisted, and the trace
    says so instead of naming a file that is not there. */
 if (ws_write_file(path, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN)) != 0) {
  int err = errno;
  json_object_put(o);
  ws_write_failed(f, "save the conversation to", path, err);
  wpe_ai_trace("session save failed %s: %s", path, strerror(err));
  return;
 }
 json_object_put(o);
 wpe_ai_trace("session save %s cli=%s n=%d", path, sess.cli ? sess.cli : "-", sess.n);
}

void wpe_ai_session_reset(FENSTER *f)
{
 char path[1300];
 sess_free();
 free(e_ai_resume_session); e_ai_resume_session = NULL;
 free(e_ai_last_session_id); e_ai_last_session_id = NULL;
 ws_session_path(f, path, sizeof path);
 unlink(path);
 wpe_ai_trace("session reset");
}

#endif /* WPE_AI */

typedef int wpe_ai_ws_translation_unit;
