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
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_ai.h"
#include "we_ai_path.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
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

/**
 * ws_read_file_len - Read a file whole, up to @maxlen bytes, and say how long it is.
 * @path: the file; @maxlen: the most bytes taken; @len: receives the count, or NULL.
 *
 * What the user gains: a before image that holds a NUL byte is read in full
 * and can be put back in full, because the length travels with the text.  The
 * buffer is NUL-terminated for the callers that treat it as a string.
 * Return: the malloc'd bytes, or NULL when the file cannot be read.
 */
static char *ws_read_file_len(const char *path, size_t maxlen, size_t *len_out)
{
 FILE *fp = fopen(path, "rb");
 char *buf;
 size_t cap = 4096, len = 0;
 int ch;
 if (len_out) *len_out = 0;
 if (!fp) return NULL;
 buf = malloc(cap);
 if (!buf) { fclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < maxlen) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(buf, cap); if (!nb) break; buf = nb; }
  buf[len++] = (char)ch;
 }
 buf[len] = '\0';
 fclose(fp);
 if (len_out) *len_out = len;
 return buf;
}

static char *ws_read_file(const char *path, size_t maxlen)
{
 return ws_read_file_len(path, maxlen, NULL);
}

/**
 * ws_state_leaf_ok - May this name be a file under the editor's state folder?
 * @leaf: a path relative to the state folder.
 *
 * A leaf is relative and stays below the folder: no leading slash, no empty
 * name and no ".." part.  A user's path is never one by accident, so the
 * state-file writer cannot be handed one.
 * Return: 1 when it may.
 */
static int ws_state_leaf_ok(const char *leaf)
{
 const char *p = leaf;
 if (!leaf || !leaf[0] || leaf[0] == '/') return 0;
 while (*p) {
  size_t part = strcspn(p, "/");
  if (part == 2 && p[0] == '.' && p[1] == '.') return 0;
  p += part;
  if (*p == '/') p++;
 }
 return 1;
}

/**
 * ws_state_path - The full path of a file under the editor's state folder.
 * @leaf: its path below the state folder; @path/@sz: receives the full path.
 * Return: 0, or -1 with errno set (EINVAL for a leaf that is not below the
 * folder, ENAMETOOLONG).
 */
static int ws_state_path(const char *leaf, char *path, size_t sz)
{
 char dir[WPE_PATH_SIZE];
 if (!ws_state_leaf_ok(leaf)) { errno = EINVAL; return -1; }
 ws_state_dir(dir, sizeof dir);
 if (snprintf(path, sz, "%s/%s", dir, leaf) >= (int)sz) {
  errno = ENAMETOOLONG;
  return -1;
 }
 return 0;
}

/**
 * ws_write_state_bytes - Write @len bytes to a file under the editor's state folder.
 * @leaf: the file's path below the state folder; @text/@len: the bytes.
 *
 * Every file this layer keeps for itself is one the user relies on later: the
 * conversation to resume, the before image and the recorded mode a revert
 * puts back.  The path is built here, from the state folder and a leaf, so no
 * user path can reach this function; and the write is the editor's one
 * writer of text (wpe_write_bytes), so a disk that filled up or a device that
 * went away is a failure and not a half file.
 * Return: 0 when the file holds the bytes, -1 with errno saying why it does not.
 */
static int ws_write_state_bytes(const char *leaf, const char *text, size_t len)
{
 char path[WPE_PATH_SIZE];
 WpeWriteOutcome o;
 if (ws_state_path(leaf, path, sizeof path) != 0) return -1;
 if (wpe_write_bytes(path, text, len, &o) == 0) return 0;
 errno = o.err ? o.err : EIO;
 return -1;
}

/** ws_write_state_file - ws_write_state_bytes for a string. */
static int ws_write_state_file(const char *leaf, const char *text)
{
 return ws_write_state_bytes(leaf, text, strlen(text));
}

/**
 * ws_say_failed -Tell the user one file this layer could not do what it promised.
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

/* The permissions a folder this layer makes gets, before the user's umask. */
#define WS_DIR_MODE 0777

/**
 * ws_make_dir_parents - Make @path and every folder above it that is missing.
 * @path: the directory.
 *
 * What `mkdir -p` does, done here: no shell is started for it, so making a
 * folder for the agent's write or a checkpoint costs the editor no process
 * and no wait.  Each failure is left to the check that follows, which asks
 * the filesystem.
 */
static void ws_make_dir_parents(const char *path)
{
 char buf[WPE_PATH_SIZE];
 size_t i, n = strlen(path);

 if (n == 0 || n >= sizeof buf) return;
 memcpy(buf, path, n + 1);
 for (i = 1; i < n; i++) {
  if (buf[i] != '/') continue;
  buf[i] = '\0';
  mkdir(buf, WS_DIR_MODE);
  buf[i] = '/';
 }
 mkdir(buf, WS_DIR_MODE);
}

/**
 * ws_make_dir - Create a directory this layer is about to write into.
 * @f: any window of the editor; @path: the directory to create.
 *
 * The result used to be dropped: every write below it then failed too, and
 * the user was told nothing about either.  Checking turns that into one pane
 * line naming the directory and the reason, before the writes that need it
 * are attempted, so a folder that is missing is reported once and not once
 * per file that would have gone into it.
 *
 * The check asks the filesystem.  What the writes need is a directory at
 * @path, however it came to be there.
 * Return: 0 when @path is a directory afterwards.
 */
static int ws_make_dir(FENSTER *f, const char *path)
{
 struct stat st;

 ws_make_dir_parents(path);
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
  for (k = 0; k < wpe_listsel_count(e_p_df[0], 1); k++) {
   const char *m = wpe_listsel_entry(e_p_df[0], k, 1);
   char *p;
   if (!m || !*m) continue;
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
#define WS_CK_NONE     0
#define WS_CK_GIT      1
#define WS_CK_SNAPSHOT 2
static char   ck_ref[160];
static char   ck_dir[WPE_PATH_SIZE];
/* the workspace folder as it really was when the checkpoint was taken, and the
   device and inode it had; a revert trusts the folder only while it is still
   that one */
static char   ck_rdir[WPE_PATH_RESOLVED_MAX];
static dev_t  ck_rdev;
static ino_t  ck_rino;
static char   ck_snapdir[1024];
static char **ck_files;
static int    ck_n;
static char **ck_before;  static int ck_before_n;  /* files present before   */
static char **ck_dirs;    static int ck_dirs_n;    /* dirs watched for new   */
/* git mode: the untracked files that already existed when the checkpoint was
   taken, as absolute paths under the workspace.  A file in this list is the
   user's, never the run's, so it is never listed as new and never removed. */
static char **ck_untracked; static int ck_untracked_n;
/* git mode: what git ignored when the checkpoint was taken, a whole ignored
   folder as one entry ending in '/'.  A run that edits .gitignore can make
   these show up as untracked; they are still the user's. */
static char **ck_ignored; static int ck_ignored_n;
/* The checkpoint's folder below the state folder ("checkpoints/<time>": the
   before images, and the modes recorded for them), and what the assistant's
   own writes leave behind: the files whose permission bits were recorded
   before its first write to them (the list position is the record's number),
   and the .bak copies those writes made.  The copies are the price of a write
   being a Save, not the run's work: the review never lists them and a revert
   never removes them. */
#define WS_LEAF_MAX 96
static char   ck_leaf[WS_LEAF_MAX];
static char **ck_mode_paths; static int ck_mode_n, ck_mode_cap;
static char **ck_baks;       static int ck_baks_n,  ck_baks_cap;
/* snapshot mode: every directory the checkpoint's walk met, and those whose
   entries it listed to the end.  A name missing from a listed directory was
   made after the checkpoint; in a directory the bound cut short, nobody can
   say. */
static char **ck_met;     static int ck_met_n,    ck_met_cap;
static char **ck_listed;  static int ck_listed_n, ck_listed_cap;

int wpe_ai_checkpoint_active(void) { return ck_mode != 0; }

static void ck_clear(void)
{
 wpe_ai_free_list(ck_files, ck_n);
 wpe_ai_free_list(ck_before, ck_before_n);
 wpe_ai_free_list(ck_dirs, ck_dirs_n);
 wpe_ai_free_list(ck_untracked, ck_untracked_n);
 wpe_ai_free_list(ck_ignored, ck_ignored_n);
 wpe_ai_free_list(ck_met, ck_met_n);
 wpe_ai_free_list(ck_listed, ck_listed_n);
 wpe_ai_free_list(ck_mode_paths, ck_mode_n);
 wpe_ai_free_list(ck_baks, ck_baks_n);
 ck_mode_paths = NULL; ck_mode_n = ck_mode_cap = 0;
 ck_baks = NULL; ck_baks_n = ck_baks_cap = 0;
 ck_leaf[0] = '\0';
 ck_files = NULL; ck_n = 0;
 ck_before = NULL; ck_before_n = 0;
 ck_dirs = NULL; ck_dirs_n = 0;
 ck_untracked = NULL; ck_untracked_n = 0;
 ck_ignored = NULL; ck_ignored_n = 0;
 ck_met = NULL; ck_met_n = ck_met_cap = 0;
 ck_listed = NULL; ck_listed_n = ck_listed_cap = 0;
 ck_mode = 0; ck_ref[0] = '\0';
 ck_rdir[0] = '\0'; ck_rdev = 0; ck_rino = 0;
}

/**
 * ws_root_unchanged - Is the workspace folder still the one the checkpoint saw?
 *
 * What the user gets: if a command moved the workspace folder aside and put a
 * link to another place under its name, the review lists nothing from that
 * place and a revert writes and removes nothing there.  The folder is judged
 * by its resolved name and its device and inode from checkpoint time, so a
 * workspace opened through a link keeps working: the link was followed once,
 * at the checkpoint, and only the real folder is compared afterwards.  A link
 * put at the resolved name, or a different folder under it, has another inode.
 * Return: 1 while it is the same folder, 0 when it was replaced or is gone.
 */
static int ws_root_unchanged(void)
{
 struct stat st;
 if (!ck_rdir[0]) return 0;
 if (stat(ck_rdir, &st) != 0) return 0;
 return st.st_dev == ck_rdev && st.st_ino == ck_rino;
}

static void ws_strip_slash(char *d);

/* The most files the snapshot watches for new ones, over every watched
   directory together.  When the walk stops here the pane says so, so a run that
   creates a file beyond the bound is never silently unreviewed. */
#define WS_WATCH_MAX 2000

/**
 * ws_walk_files - List the regular files under a directory, as far down as it goes.
 * @dir: where to start; @l/@n/@cap: the list the paths are added to.
 *
 * What the user gains: a file the run creates in a deeper directory of the
 * workspace, through a command or a tool, is listed by the review and removed
 * by r, the same as one created beside the open file; the git mode sees the
 * whole tree, and the snapshot mode now watches the same set.  Hidden entries
 * are skipped, which keeps `.git` and the editor's own folders out; a symbolic
 * link is listed as one entry and never followed, as git lists an untracked
 * link without descending into it, so a link the run makes is reviewed and
 * removable while a link to a tree outside the workspace does not spend the
 * bound on files the review could not revert; and the walk stops at
 * WS_WATCH_MAX entries in all, so a workspace holding a built tree does not
 * stall the checkpoint.
 * @skip: one directory left out of the walk, the editor's own state directory
 * in its resolved spelling: the session files and the checkpoint's copies it
 * writes there are the editor's bookkeeping, never the run's changes, and a
 * workspace that holds that directory would otherwise see them listed as new
 * files.  Each directory met is resolved the same way before the comparison,
 * so the spelling of the environment variable does not decide the match.
 * A directory's own entries are all read before any of its subdirectories is
 * entered, so a deep tree spends the bound below it and not the files beside
 * the open one.
 * @record: the checkpoint's own walk; each directory met goes into ck_met and,
 * when its entries were read to the end, into ck_listed, so the review can
 * tell a file the run made from one the bound kept out of sight.
 * Return: 1 when the bound stopped the walk, 0 when everything was seen.
 */
static int ws_walk_files(const char *dir, const char *skip, char ***l, int *n, int *cap,
                         int record)
{
 DIR *d = opendir(dir);
 struct dirent *de;
 char **subs = NULL;
 int nsubs = 0, scap = 0, stopped = 0, k;
 if (!d) return 0;
 while ((de = readdir(d))) {
  char *p, real[WPE_PATH_SIZE];
  size_t L;
  struct stat st;
  if (*n >= WS_WATCH_MAX) { stopped = 1; break; }
  if (de->d_name[0] == '.') continue;
  L = strlen(dir) + strlen(de->d_name) + 2;
  p = malloc(L);
  if (!p) continue;
  snprintf(p, L, "%s/%s", dir, de->d_name);
  if (lstat(p, &st) != 0) { free(p); continue; }
  if (S_ISLNK(st.st_mode)) {
   /* one entry, whatever it points at: the run may have made it, and r
      removes the link itself, never what it points at */
   if (!ws_list_has(*l, *n, p)) ws_list_push(l, n, cap, p);
   else free(p);
   continue;
  }
  if (S_ISDIR(st.st_mode)) {
   if (skip && realpath(p, real) && !strcmp(real, skip)) { free(p); continue; }
   ws_list_push(&subs, &nsubs, &scap, p);
  } else if (S_ISREG(st.st_mode) && !ws_list_has(*l, *n, p)) {
   ws_list_push(l, n, cap, p);
  } else free(p);
 }
 closedir(d);
 if (record) {
  ws_list_push(&ck_met, &ck_met_n, &ck_met_cap, ws_strdup(dir));
  if (!stopped) ws_list_push(&ck_listed, &ck_listed_n, &ck_listed_cap, ws_strdup(dir));
 }
 for (k = 0; k < nsubs; k++) {
  if (stopped) {
   /* never read, but met: a file in it is not the run's for that */
   if (record) ws_list_push(&ck_met, &ck_met_n, &ck_met_cap, ws_strdup(subs[k]));
  } else if (ws_walk_files(subs[k], skip, l, n, cap, record)) stopped = 1;
 }
 wpe_ai_free_list(subs, nsubs);
 return stopped;
}

/**
 * ws_list_watched_files - The files under every watched directory, now.
 * @l/@n: receives the list.
 * @record: the checkpoint's own walk; it also notes which directories it met
 * and which it read to the end (ck_met, ck_listed).
 * Return: 1 when the bound stopped the walk before every file was seen.
 */
static int ws_list_watched_files(char ***l, int *n, int record)
{
 char own[WPE_PATH_SIZE], real[WPE_PATH_SIZE];
 const char *skip;
 int cap = 0, k, stopped = 0;
 ws_state_dir(own, sizeof own);
 /* resolved, so "state/", "state//" and a path through a link all name the
    one directory the walk compares against; unresolvable means not there,
    and a directory that is not there cannot be met by the walk */
 skip = realpath(own, real) ? real : NULL;
 *l = NULL; *n = 0;
 /* a workspace folder replaced since the checkpoint lists nothing: whatever
    the name leads to now is not the run's work */
 if (!record && !ws_root_unchanged()) return 0;
 if (record) {
  wpe_ai_free_list(ck_met, ck_met_n);
  wpe_ai_free_list(ck_listed, ck_listed_n);
  ck_met = NULL; ck_met_n = ck_met_cap = 0;
  ck_listed = NULL; ck_listed_n = ck_listed_cap = 0;
 }
 for (k = 0; k < ck_dirs_n; k++)
  if (ws_walk_files(ck_dirs[k], skip, l, n, &cap, record)) stopped = 1;
 return stopped;
}

/**
 * ws_snapshot_run_created - Can the snapshot say the run made this file?
 * @path: a file the walk sees now.
 *
 * A file is the run's when the checkpoint did not see it and the directory
 * that should have shown it was read to the end: its own directory, or, for a
 * directory the run made, the nearest one the checkpoint met.  In a workspace
 * larger than the bound, the files the checkpoint never reached were there all
 * along: the review does not call them new and `r` does not remove them,
 * whatever the run deleted to bring them into view.
 * Return: 1 when the file is the run's, 0 when it was there or nobody can say.
 */
static int ws_snapshot_run_created(const char *path)
{
 char dir[WPE_PATH_SIZE];
 char *sl;
 if (ws_list_has(ck_before, ck_before_n, path)) return 0;
 if (strlen(path) >= sizeof dir) return 0;
 strcpy(dir, path);
 for (;;) {
  sl = strrchr(dir, '/');
  if (!sl) return 0;
  if (sl == dir) sl[1] = '\0';
  else           sl[0] = '\0';
  if (ws_list_has(ck_listed, ck_listed_n, dir)) return 1;
  if (ws_list_has(ck_met, ck_met_n, dir)) return 0;
  if (ws_list_has(ck_dirs, ck_dirs_n, dir) || sl == dir) return 0;
 }
}

/**
 * ws_say_watch_bound - Tell the user the snapshot could not watch the whole workspace.
 * @f: any window of the editor; @seen: how many files it watches.
 *
 * The review lists new files by comparing what exists now with what existed at
 * the checkpoint; a file beyond the bound is in neither list, so it would be
 * kept without ever being shown.  Saying the bound is what keeps "ALL changes
 * reverted" honest in a workspace too large to walk.
 */
static void ws_say_watch_bound(FENSTER *f, int seen)
{
 char line[1300];
 snprintf(line, sizeof line,
          "the snapshot watches %d files under %s; a file the run creates beyond them is unseen",
          seen, ck_dir);
 wpe_ai_pane_error(f, line);
}

/* Drop trailing slashes so the same directory always has ONE spelling (the
 * root may arrive as ".../dir/" while a file's dirname is ".../dir"). */
static void ws_strip_slash(char *d)
{
 size_t l = strlen(d);
 while (l > 1 && d[l - 1] == '/') d[--l] = '\0';
}

/* Is `dir` the root or somewhere below it? */
static int ws_dir_under_root(const char *dir)
{
 size_t l = strlen(ck_dir);
 if (strncmp(dir, ck_dir, l) != 0) return 0;
 return dir[l] == '\0' || dir[l] == '/';
}

/* The directories to watch for NEW files: the root, walked as deep as it goes,
   plus the directory of every scope file that lives outside it. */
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
  if (!ws_dir_under_root(d) && !ws_list_has(ck_dirs, ck_dirs_n, d))
   ws_list_push(&ck_dirs, &ck_dirs_n, &cap, d);
  else free(d);
 }
}

/* The most a git file list may hold, in bytes (16 MB: some 200,000 names). */
#define WS_GIT_LIST_MAX 16000000L
/* What wpe_ai_shell_quote needs beyond the quoted text itself: room it keeps
   free for the last escape, the closing quote and the terminating NUL. */
#define WS_QUOTE_SLACK 5

/**
 * ws_quote_whole - Shell-quote a path only when all of it fits.
 * @dst/@sz: the quoted result; @src: the path.
 *
 * wpe_ai_shell_quote stops at the end of its buffer and still closes the
 * quote, which turns a long name into a shorter one: a removal would then hit
 * whatever file has that shorter name.  Every command that removes or
 * restores a file builds its paths here, so a name that does not fit is
 * refused instead of cut.
 * Return: 0 with @dst holding all of @src, -1 when it would not fit.
 */
static int ws_quote_whole(char *dst, size_t sz, const char *src)
{
 size_t need = 2;
 const char *p;
 for (p = src; *p; p++) need += (*p == '\'') ? 4 : 1;
 if (need + WS_QUOTE_SLACK > sz) return -1;
 wpe_ai_shell_quote(dst, sz, src);
 return 0;
}

/**
 * wpe_ai_shell_quote_whole - Shell-quote one argument, or refuse when it is too long.
 * @dst/@sz: the quoted result; @src: the argument as the user knows it.
 *
 * For a command the user reads and the shell runs, such as the Build & fix
 * line: a name that does not fit is refused, never cut to a shorter name.
 * Return: 0 with @dst holding all of @src, -1 when it would not fit.
 */
int wpe_ai_shell_quote_whole(char *dst, size_t sz, const char *src)
{
 return ws_quote_whole(dst, sz, src);
}

/**
 * ws_format_whole - Build a command only when all of it fits.
 * @dst/@sz: the command; @fmt: its pattern and arguments.
 *
 * A command cut short can still be a valid one that does something else (the
 * tail of "-- 'name' 2>&1" cut to "-- 'name' 2" removes a file called 2), so
 * the caller runs nothing when this fails.
 * Return: 0 when the whole command is in @dst, -1 when it was cut.
 */
static int ws_format_whole(char *dst, size_t sz, const char *fmt, ...)
{
 va_list ap;
 int n;
 va_start(ap, fmt);
 n = vsnprintf(dst, sz, fmt, ap);
 va_end(ap);
 if (n < 0 || (size_t)n >= sz) { if (sz) dst[0] = '\0'; return -1; }
 return 0;
}

/**
 * ws_rel_stays_inside - Is a workspace-relative path really below the workspace?
 * @rel: a path as git prints it for the workspace folder.
 *
 * A path that is empty, absolute or climbs out with ".." is not a file of the
 * workspace, so nothing built from it may be listed or removed.
 * Return: 1 when it names something under the workspace, 0 otherwise.
 */
static int ws_rel_stays_inside(const char *rel)
{
 const char *p = rel;
 if (!rel || !*rel || *rel == '/') return 0;
 while (*p) {
  const char *e = strchr(p, '/');
  size_t l = e ? (size_t)(e - p) : strlen(p);
  if (l == 2 && p[0] == '.' && p[1] == '.') return 0;
  if (!e) break;
  p = e + 1;
 }
 return 1;
}

/**
 * ws_join_workspace - The one place a workspace-relative path becomes a full path.
 * @rel: the path git printed from inside the workspace; @out/@sz: the result.
 *
 * Every file the review lists in git mode is named through here, so a path
 * that would land outside the workspace is dropped before it can be shown to
 * the user or handed to a revert.
 * Return: 0 with @out filled, -1 when @rel leaves the workspace or does not fit.
 */
static int ws_join_workspace(const char *rel, char *out, size_t sz)
{
 int n;
 if (!ws_rel_stays_inside(rel)) return -1;
 n = snprintf(out, sz, "%s/%s", ck_dir, rel);
 if (n < 0 || (size_t)n >= sz) return -1;
 return 0;
}

/**
 * ws_workspace_rel - The part of a full path below the workspace, or NULL.
 * @path: a full path as a review row holds it.
 *
 * The reverse of ws_join_workspace: a revert acts on what this returns, so a
 * row that somehow names a place outside the workspace cannot reach it.
 */
static const char *ws_workspace_rel(const char *path)
{
 size_t dl = strlen(ck_dir);
 if (strncmp(path, ck_dir, dl) != 0 || path[dl] != '/') return NULL;
 if (!ws_rel_stays_inside(path + dl + 1)) return NULL;
 return path + dl + 1;
}

/**
 * ws_parse_nul_list - Split a NUL-terminated name list into full workspace paths.
 * @buf/@len: the bytes git wrote with -z; @l/@n: receives the paths.
 *
 * A name with a newline or leading blanks arrives whole, because only a NUL
 * ends a name.  A name that would leave the workspace is skipped.
 */
static void ws_parse_nul_list(const char *buf, size_t len, char ***l, int *n)
{
 size_t i = 0;
 int cap = 0;
 while (i < len) {
  const char *name = buf + i;
  size_t nl = strnlen(name, len - i);
  char full[1200];
  if (nl && ws_join_workspace(name, full, sizeof full) == 0)
   ws_list_push(l, n, &cap, ws_strdup(full));
  i += nl + 1;
 }
}

/**
 * ws_git_list - Ask git for a list of files in the workspace, whatever their names.
 * @git_args: the git words after "git --literal-pathspecs", ending in -z output;
 * @l/@n: receives the full paths, in git's order.
 *
 * The answer goes through a file under the state folder, not through the
 * command's captured text: the capture is cut at a few thousand bytes and a
 * list cut short would let a file that was there all along look like one the
 * run made.  git runs from the workspace, so its paths are relative to it.
 * The file gets a fresh name of its own each time (mkstemp), so two editors,
 * or a link left at a known name, never decide where git's answer is written.
 * Return: 0 when git answered in full, -1 when it failed or the answer did not
 * fit; the caller then takes the safe side.
 */
static int ws_git_list(const char *git_args, char ***l, int *n)
{
 char dir[1024], file[1100], q[1100], qf[1100], cmd[2800], *out, *buf;
 int status = 0, fd;
 long len;
 FILE *fp;
 *l = NULL; *n = 0;
 ws_state_dir(dir, sizeof dir);
 ws_make_dir_parents(dir);
 if (ws_format_whole(file, sizeof file, "%s/git-list-XXXXXX", dir) != 0) return -1;
 fd = mkstemp(file);
 if (fd < 0) return -1;
 close(fd);
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 || ws_quote_whole(qf, sizeof qf, file) != 0 ||
     ws_format_whole(cmd, sizeof cmd, "cd %s && git --literal-pathspecs %s > %s 2>/dev/null",
                     q, git_args, qf) != 0) {
  unlink(file);
  return -1;
 }
 out = wpe_ai_run_capture_status(cmd, &status);
 free(out);
 fp = status == 0 ? fopen(file, "rb") : NULL;
 if (!fp) { unlink(file); return -1; }
 fseek(fp, 0, SEEK_END);
 len = ftell(fp);
 rewind(fp);
 buf = (len >= 0 && len <= WS_GIT_LIST_MAX) ? malloc((size_t)len + 1) : NULL;
 if (buf && fread(buf, 1, (size_t)len, fp) != (size_t)len) { free(buf); buf = NULL; }
 fclose(fp);
 unlink(file);
 if (!buf) return -1;
 ws_parse_nul_list(buf, (size_t)len, l, n);
 free(buf);
 return 0;
}

/**
 * ws_git_tracks_workspace - Is git really keeping this workspace?
 * @q: the shell-quoted workspace.
 *
 * Git mode puts a revert in git's hands, which is right only when git tracks
 * the files the run will change.  A workspace inside a folder an outer repo
 * ignores answers "inside a work tree" yet holds nothing of that repo, so the
 * review would list the repo's own files; one with nothing committed has no
 * before image to revert to.  Both are better served by the snapshot copies.
 * Return: 1 when the workspace is inside a work tree, not ignored by it, and
 * holds at least one tracked file; 0 otherwise.
 */
static int ws_git_tracks_workspace(const char *q)
{
 char cmd[1400], **tracked, *out;
 int ntracked, yes;
 if (ws_format_whole(cmd, sizeof cmd, "cd %s && git rev-parse --is-inside-work-tree 2>/dev/null",
                     q) != 0) return 0;
 out = wpe_ai_run_capture(cmd);
 yes = out && !strncmp(out, "true", 4);
 free(out);
 if (!yes) return 0;
 if (ws_format_whole(cmd, sizeof cmd, "cd %s && git check-ignore -q . 2>/dev/null && echo ignored",
                     q) != 0) return 0;
 out = wpe_ai_run_capture(cmd);
 yes = !(out && !strncmp(out, "ignored", 7));
 free(out);
 if (!yes) return 0;
 if (ws_git_list("ls-files -z -- .", &tracked, &ntracked) != 0) return 0;
 wpe_ai_free_list(tracked, ntracked);
 return ntracked > 0;
}

/**
 * ws_git_stash_ref - Save the workspace as git sees it, as the revert's reference.
 * @q: the shell-quoted workspace.
 *
 * `git stash create` writes the worktree's uncommitted edits into a commit and
 * prints its name, or prints nothing when there is nothing to save, in which
 * case HEAD is the image.  When it fails (a merge stopped on a conflict,
 * another git process holding the index, nothing committed yet) there is no
 * image of the user's uncommitted edits at all: HEAD in its place would list
 * them as the run's and `r` would throw them away.  So a failure is a failure,
 * and the caller takes the snapshot copies instead.
 *
 * The index is refreshed first: a file whose timestamp changed but whose
 * content did not (saved again, touched, rewritten by a checkout) makes the
 * first `git stash create` after it exit 1 with nothing printed, which would
 * pass for a failure and silently turn the review's commit key off.
 * Return: 0 with ck_ref set, -1 when git could not save the worktree.
 */
static int ws_git_stash_ref(const char *q)
{
 char cmd[1400], *out;
 int status = 0;
 size_t l;
 if (ws_format_whole(cmd, sizeof cmd,
                     "cd %s && { git update-index -q --refresh 2>/dev/null; git stash create 2>/dev/null; }",
                     q) != 0) return -1;
 out = wpe_ai_run_capture_status(cmd, &status);
 if (status != 0 || !out) { free(out); return -1; }
 l = strlen(out);
 while (l && (out[l-1] == '\n' || out[l-1] == ' ')) out[--l] = '\0';
 if (l == 0) strcpy(ck_ref, "HEAD");
 else if (l < sizeof ck_ref && strspn(out, "0123456789abcdef") == l) strcpy(ck_ref, out);
 else { free(out); return -1; }
 free(out);
 return 0;
}

/**
 * ws_git_was_there - Was this file already in the workspace, untracked, at the checkpoint?
 * @path: the full path of a file git calls untracked now.
 *
 * True for a file git listed as untracked then, and for one git ignored then,
 * alone or inside an ignored folder: a run that edits .gitignore can make
 * those show up as untracked, and they are still the user's.
 */
static int ws_git_was_there(const char *path)
{
 int i;
 if (ws_list_has(ck_untracked, ck_untracked_n, path)) return 1;
 for (i = 0; i < ck_ignored_n; i++) {
  size_t l = strlen(ck_ignored[i]);
  if (!strcmp(ck_ignored[i], path)) return 1;
  if (l && ck_ignored[i][l - 1] == '/' && !strncmp(ck_ignored[i], path, l)) return 1;
 }
 return 0;
}

/**
 * ws_git_record_untracked - Note the files git does not track, before the run.
 *
 * Every one of them is the user's.  When git cannot list them the review
 * could not tell them from the run's, so the caller does not take git mode.
 * Return: 0 when both lists were taken, -1 otherwise.
 */
static int ws_git_record_untracked(void)
{
 if (ws_git_list("ls-files --others --exclude-standard -z -- .",
                 &ck_untracked, &ck_untracked_n) != 0) return -1;
 return ws_git_list("ls-files --others --ignored --exclude-standard --directory -z -- .",
                    &ck_ignored, &ck_ignored_n);
}

/**
 * ws_record_root - Note which real folder the workspace is, for the revert.
 *
 * Resolves the workspace folder once, through any link it was opened by, and
 * keeps its real name, device and inode.  What the user gets: a review that
 * can tell the workspace from a link put in its place later.  When the folder
 * cannot be resolved or looked at, no checkpoint is taken and the trace says
 * why, rather than a revert with nothing to hold it to.
 * Return: 0 when recorded, -1 when not (ck_clear state is left).
 */
static int ws_record_root(void)
{
 struct stat st;
 if (wpe_path_resolve(ck_dir, ck_rdir, sizeof ck_rdir) != WPE_PATH_RESOLVED ||
     stat(ck_rdir, &st) != 0 || !S_ISDIR(st.st_mode)) {
  ck_rdir[0] = '\0';
  wpe_ai_trace("checkpoint refused: workspace folder cannot be resolved");
  return -1;
 }
 ck_rdev = st.st_dev;
 ck_rino = st.st_ino;
 return 0;
}

int wpe_ai_checkpoint_create(FENSTER *f, char **scope, int nscope)
{
 char *root = ws_root(f);
 char q[1100];
 int i;

 ck_clear();
 if (wpe_path_copy(ck_dir, sizeof ck_dir, root)) {
  /* a workspace the system cannot hold: no checkpoint, rather than one of a
     cut folder */
  free(root);
  wpe_ai_trace("checkpoint refused: workspace path too long");
  return -1;
 }
 ws_strip_slash(ck_dir);
 free(root);
 if (ws_record_root() != 0) return -1;

 /* git mode needs the workspace tracked, the user's untracked and ignored
    files on record, and an image of the worktree to go back to; short of any
    of them, the snapshot copies. */
 if (ws_quote_whole(q, sizeof q, ck_dir) == 0 && ws_git_tracks_workspace(q) &&
     ws_git_record_untracked() == 0 && ws_git_stash_ref(q) == 0) {
  ck_mode = 1;
  wpe_ai_trace("checkpoint git ref=%s dir=%s", ck_ref, ck_dir);
  return 0;
 }
 wpe_ai_free_list(ck_untracked, ck_untracked_n);
 wpe_ai_free_list(ck_ignored, ck_ignored_n);
 ck_untracked = NULL; ck_untracked_n = 0;
 ck_ignored = NULL; ck_ignored_n = 0;
 ck_ref[0] = '\0';

 /* snapshot copies */
 {
  time_t now = time(NULL);
  int failed = 0, told = 0, readable = 0, have_dir;
  snprintf(ck_leaf, sizeof ck_leaf, "checkpoints/%ld", (long)now);
  ck_snapdir[0] = '\0';
  /* without the folder no copy can exist, and the folder's own failure has
     just been reported: the loop stays out rather than repeat it per file */
  have_dir = ws_state_path(ck_leaf, ck_snapdir, sizeof ck_snapdir) == 0 &&
             ws_make_dir(f, ck_snapdir) == 0;
  if (have_dir) {
   for (i = 0; i < nscope; i++) {
    size_t tlen = 0;
    char *text = ws_read_file_len(scope[i], 4000000, &tlen), dst[WS_LEAF_MAX + 16];
    if (!text) continue;
    readable++;
    snprintf(dst, sizeof dst, "%s/%d", ck_leaf, ck_n);
    if (ws_write_state_bytes(dst, text, tlen) == 0) {
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
  wpe_ai_free_list(ck_before, ck_before_n);
  if (ws_list_watched_files(&ck_before, &ck_before_n, 1))
   ws_say_watch_bound(f, ck_before_n);
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

/* ===================== the assistant's writes ============================ */

/* How many digits a recorded mode needs (07777 is four, plus the newline), and
   the room for the name of a record's file below the checkpoint's folder. */
#define WS_MODE_TEXT_MAX 16
#define WS_MODE_LEAF_MAX (WS_LEAF_MAX + 32)
/* The twelve permission bits: the nine, setuid, setgid and sticky. */
#define WS_MODE_BITS 07777

/**
 * ws_mode_of_file - The twelve permission bits a file has, read from the file itself.
 * @real: the file, already resolved; @mode: receives the bits.
 * Return: 0, or -1 when it cannot be read or is not a regular file.
 *
 * The file is opened without following a link and without waiting, and the
 * bits come from fstat of that descriptor: never from a name looked up
 * separately, which is the mode of a link when a link took the place.
 */
static int ws_mode_of_file(const char *real, mode_t *mode)
{
 struct stat st, seen;
 int fd;
 if (lstat(real, &seen) != 0 || !S_ISREG(seen.st_mode)) return -1;   /* a device is never opened */
 fd = open(real, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
 if (fd < 0) return -1;
 if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_ino != seen.st_ino ||
     st.st_dev != seen.st_dev) { close(fd); return -1; }
 close(fd);
 *mode = st.st_mode & WS_MODE_BITS;
 return 0;
}

/**
 * ws_modes_folder_ready - Make the folder a checkpoint keeps its recorded modes in.
 * @f: any window of the editor.
 * Return: 0 when the folder is there, -1 when it could not be made (said in the pane).
 *
 * A snapshot checkpoint has its folder from the start; a git checkpoint keeps
 * its before image in git and gets this folder at the first write it must
 * remember the permissions of.
 */
static int ws_modes_folder_ready(FENSTER *f)
{
 char dir[WPE_PATH_SIZE];
 if (!ck_leaf[0]) snprintf(ck_leaf, sizeof ck_leaf, "checkpoints/%ld-modes", (long)time(NULL));
 if (ws_state_path(ck_leaf, dir, sizeof dir) != 0) return -1;
 return ws_make_dir(f, dir);
}

/**
 * ws_mode_record - Remember a file's permission bits before the assistant's first write to it.
 * @f: any window of the editor; @real: the file the write will change, resolved.
 *
 * What the user gains: a revert puts back the bits the file had, a setgid
 * program still a program, never the mode git would give a new file.  The bits
 * are read from the file just before the run's first write to it and kept in
 * a record under the state folder; a file that is not there yet has nothing to
 * record, and a file the run only changes by a command has no record, so a
 * revert leaves its bits alone.  Only a run with a checkpoint records.
 */
static void ws_mode_record(FENSTER *f, const char *real)
{
 char leaf[WS_MODE_LEAF_MAX], text[WS_MODE_TEXT_MAX];
 mode_t mode;
 if (!ck_mode || ws_list_has(ck_mode_paths, ck_mode_n, real)) return;
 if (ws_mode_of_file(real, &mode) != 0 || ws_modes_folder_ready(f) != 0) return;
 snprintf(leaf, sizeof leaf, "%s/m%d.mode", ck_leaf, ck_mode_n);
 snprintf(text, sizeof text, "%o\n", (unsigned)mode);
 if (ws_write_state_file(leaf, text) != 0) {
  ws_write_failed(f, "record the permissions of", real, errno);
  return;
 }
 ws_list_push(&ck_mode_paths, &ck_mode_n, &ck_mode_cap, ws_strdup(real));
}

/**
 * ws_mode_recorded - The permission bits recorded for a file before the assistant wrote it.
 * @real: the file, resolved.
 * Return: the twelve bits, or WPE_WRITE_NO_RECORDED_MODE when none was recorded
 * or the record cannot be read: the revert then keeps the file's own bits.
 */
static mode_t ws_mode_recorded(const char *real)
{
 char leaf[WS_MODE_LEAF_MAX], path[WPE_PATH_SIZE], *text, *end;
 unsigned long bits;
 int i;
 for (i = 0; i < ck_mode_n; i++) if (!strcmp(ck_mode_paths[i], real)) break;
 if (i == ck_mode_n) return WPE_WRITE_NO_RECORDED_MODE;
 snprintf(leaf, sizeof leaf, "%s/m%d.mode", ck_leaf, i);
 if (ws_state_path(leaf, path, sizeof path) != 0) return WPE_WRITE_NO_RECORDED_MODE;
 text = ws_read_file(path, WS_MODE_TEXT_MAX);
 if (!text) return WPE_WRITE_NO_RECORDED_MODE;
 bits = strtoul(text, &end, 8);
 if (end == text || bits > WS_MODE_BITS) bits = WPE_WRITE_NO_RECORDED_MODE;
 free(text);
 return (mode_t)bits;
}

/**
 * ws_backup_note - Remember a .bak that an assistant write made.
 * @bak: the .bak's path as the write owner reported it ("" when none).
 *
 * The review, the created-file detection and the revert skip exactly the
 * files noted here (ws_is_run_backup).  Only a run with a checkpoint notes.
 */
static void ws_backup_note(const char *bak)
{
 if (!ck_mode || !bak[0] || ws_list_has(ck_baks, ck_baks_n, bak)) return;
 ws_list_push(&ck_baks, &ck_baks_n, &ck_baks_cap, ws_strdup(bak));
}

/**
 * ws_window_of_file - The open window that holds a file the assistant is about to write.
 * @f: any window of the editor; @real: the file, resolved; @x: what the look found there.
 * Return: the window, or NULL when none is open on it.
 *
 * Found by name first and then by the file's identity, so a window opened
 * through another spelling of the path (a link, a hard link) is the same
 * window.
 */
static FENSTER *ws_window_of_file(FENSTER *f, const char *real, const WpeWriteExpect *x)
{
 FENSTER *w = ws_find_window(f, real);
 int i;
 if (w || !x || !x->exists) return w;
 for (i = f->ed->mxedt; i > 0; i--) {
  FENSTER *c = f->ed->f[i];
  if (ws_is_real_file_window(c) && wpe_write_identity_is(&c->file_id, x->dev, x->ino)) return c;
 }
 return NULL;
}

/**
 * wpe_ai_write_judged_file - Put the assistant's text in the file the scope judged: a Save.
 * @f: any window of the editor; @real: the place the look resolved, written
 * exactly as it is and never resolved again; @text: the file's whole new
 * content; @x: what the look saw there; @o: receives the outcome.
 *
 * What the user gains: an approved write is a Save, as the editor's own.  The
 * old text is kept in the file's .bak, the file keeps its inode, its other
 * names and its setuid, setgid and sticky bits, and a bit the system would not
 * keep is named.  It goes where the box said it would, into the file the box
 * was about: a link put in the last place, another file swapped in, or a name
 * the file gained while the question was up refuses the write with nothing
 * changed.  When a window is open on the file its identity is passed, so a
 * file that is not the one the window read is refused as a Save refuses it.
 * Before the run's first write to a file its permission bits are recorded for
 * the revert, and the .bak is noted, so the review never lists it and the
 * revert never removes it.
 * Return: 0 when the file holds @text, -1 when not (@o says why).
 */
int wpe_ai_write_judged_file(FENSTER *f, const char *real, const char *text,
                             const WpeWriteExpect *x, WpeWriteOutcome *o)
{
 FENSTER *w = ws_window_of_file(f, real, x);
 int rc;
 ws_mode_record(f, real);
 rc = wpe_write_save_text(real, text, w ? &w->file_id : NULL, WPE_WRITE_NOFOLLOW, x, o);
 ws_backup_note(o->bak_path);
 return rc;
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

/**
 * ws_is_run_backup - Is this file a .bak that one of the assistant's writes made?
 * @path: a file as the review or the revert names it.
 *
 * What the user gains: the .bak copies the assistant's writes leave beside
 * your files (the price of a write being a Save) are not listed as new files
 * of the run, are not offered for removal, and a revert never takes one away.
 * Only the exact files the writes reported are meant, never a name that
 * merely ends in .bak: a backup of yours stays yours.  The path is compared
 * as spelled and as it resolves, the way the write recorded it.
 * Return: 1 when a write of this run made it.
 */
static int ws_is_run_backup(const char *path)
{
 char real[WPE_PATH_RESOLVED_MAX];
 if (ck_baks_n == 0 || !path) return 0;
 if (ws_list_has(ck_baks, ck_baks_n, path)) return 1;
 if (wpe_path_resolve(path, real, sizeof real) != WPE_PATH_RESOLVED) return 0;
 return ws_list_has(ck_baks, ck_baks_n, real);
}

static void ws_add_change(ws_change **c, int *n, int *cap, const char *path,
                          int line, const char *text, int is_new)
{
 char *np = ws_strdup(path);
 int i;
 if (ws_is_run_backup(path)) { free(np); return; }
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

/**
 * ws_collect_git_edits - Add a row for each tracked file the run changed.
 * @c/@n/@cap: the changeset being built.
 *
 * Only files below the workspace are asked about (`--relative` and `-- .`), so
 * a change the user made elsewhere in the repository never shows up as the
 * run's, and every row names its file by the path the workspace knows.
 */
/* The row for a changed file whose path is too long to hand to git diff. */
#define WS_CHANGED_UNREAD "[AI] changed"

static void ws_collect_git_edits(ws_change **c, int *n, int *cap)
{
 char **names, q[1100], cmd[2800];
 int nn, i;
 char args[300];
 snprintf(args, sizeof args, "diff --relative --name-only -z %s -- .", ck_ref);
 if (ws_git_list(args, &names, &nn) != 0) {
  wpe_ai_trace("changeset git edit list failed dir=%s", ck_dir);
  return;
 }
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0) {
  wpe_ai_free_list(names, nn);
  return;
 }
 for (i = 0; i < nn; i++) {
  char qf[1300], *d;
  const char *rel = ws_workspace_rel(names[i]);
  if (!rel || ws_hidden_path(rel)) continue;
  /* -U0: no context, so a hunk starts at the first line the run changed.
     With git's default three context lines the hunk starts three lines above
     it, and both the row and the jump it registers then point at a line the
     run never touched. */
  if (ws_quote_whole(qf, sizeof qf, rel) != 0 ||
      ws_format_whole(cmd, sizeof cmd,
                      "cd %s && git --literal-pathspecs diff -U0 --relative %s -- %s 2>/dev/null",
                      q, ck_ref, qf) != 0) {
   /* still the run's change: listed, so the user sees it and r says why it
      cannot put this one back */
   ws_add_change(c, n, cap, names[i], 1, WS_CHANGED_UNREAD, 0);
   continue;
  }
  d = wpe_ai_run_capture(cmd);
  ws_changes_from_gitdiff(d ? d : "", names[i], c, n, cap);
  free(d);
 }
 wpe_ai_free_list(names, nn);
}

/**
 * ws_collect_git_new_files - Add a row for each file the run created.
 * @c/@n/@cap: the changeset being built.
 *
 * A file is the run's only when it is untracked now and was neither untracked
 * nor ignored when the checkpoint was taken.  The notes or build output the
 * user already had lying in the workspace are therefore never offered for
 * removal, even when the run's edit to .gitignore uncovers them.
 */
static void ws_collect_git_new_files(ws_change **c, int *n, int *cap)
{
 char **now;
 int nn, i;
 if (ws_git_list("ls-files --others --exclude-standard -z -- .", &now, &nn) != 0) {
  wpe_ai_trace("changeset git new-file list failed dir=%s", ck_dir);
  return;
 }
 for (i = 0; i < nn; i++) {
  const char *rel = ws_workspace_rel(now[i]);
  if (!rel || ws_hidden_path(rel)) continue;
  if (ws_git_was_there(now[i])) continue;
  ws_add_change(c, n, cap, now[i], 1, "[AI] new file", 1);
 }
 wpe_ai_free_list(now, nn);
}

static int ws_collect_changes(ws_change **c, int *n)
{
 int cap = 0;
 *c = NULL; *n = 0;
 if (ck_mode == 1) {
  ws_collect_git_edits(c, n, &cap);
  ws_collect_git_new_files(c, n, &cap);
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
   char **nowl;
   int nn, k;
   ws_list_watched_files(&nowl, &nn, 0);
   for (k = 0; k < nn; k++)
    if (ws_snapshot_run_created(nowl[k]))
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
 * ws_window_take_text - Put `text` in the window's buffer.
 * @w: an open file window; @text: the complete new content.
 *
 * Takes no undo step of its own: the lines that differ are one change for
 * Ctrl-U, inside the step the caller opened when it opened one, so a write
 * and the hook's reformatting that follows are one Ctrl-U.  The cursor is
 * kept on a line that still exists and the window is marked modified, so the
 * user is asked before losing the new text.  The caller shows the window.
 */
static void ws_window_take_text(FENSTER *w, const char *text)
{
 if (e_eol_replace_all(w->b, text, E_BUFFER_STEP_NEW, E_EOL_ROWS) == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(w->b, "the new text");
 e_buffer_cursor_set(w->b, w->b->b.y, w->b->b.x);
 w->save++;
}

/**
 * ws_window_reload_undoable - Reload a window from its file as ONE undo step.
 * @w: the open window; @path: the file to read.
 *
 * Only the lines that differ change, so a single Ctrl-U brings back the text
 * the user had after a tool rewrote the file or a changeset revert restored
 * it, and the lines the file kept have their breakpoints and block.  The new
 * text is shown at once, or when the box in the way closes.
 * Return: 1 when the file was read and shown, 0 when it could not be read
 * (the buffer is then left untouched and no undo step is taken).
 */
static int ws_window_reload_undoable(FENSTER *w, const char *path)
{
 char *text = ws_read_file(path, WS_RELOAD_MAX);
 if (!text) return 0;
 ws_window_take_text(w, text);
 e_window_show_replaced_text(w);
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
 * ws_absolute_unfolded - Make a path absolute without folding its dots.
 * @path: as the model wrote it; @buf/@sz: room for the result.
 *
 * ws_absolute_path folds ".." before any link is looked at, which is wrong
 * for the workspace check: "vendor/../x" is not "x" when "vendor" is a link
 * to another directory.  The check hands the text to the resolver as it is.
 *
 * Return: `buf`, or NULL when there is no working directory or it does not fit.
 */
static const char *ws_absolute_unfolded(const char *path, char *buf, size_t sz)
{
 char cwd[WPE_PATH_RESOLVED_MAX];
 int n;
 if (!path) return NULL;
 if (path[0] == '/') n = snprintf(buf, sz, "%s", path);
 else if (getcwd(cwd, sizeof cwd)) n = snprintf(buf, sz, "%s/%s", cwd, path);
 else return NULL;
 if (n < 0 || (size_t)n >= sz) return NULL;
 return buf;
}

/* ---- the read grants: directories beyond the workspace the assistant may read ---
 * Three sources, one list for the judge: the include directories the project
 * already declares (Project Options, "System-Include-Path"), the roots the user
 * wrote into xwperc (AIReadRoots, never written by the editor on its own),
 * and the folders the user answered "Yes for this folder" for in this session.
 * Only the last kind is kept here; it ends with the session. */
#define WS_GRANT_MAX WPE_AI_GRANTS_MAX
static char *ws_session_grants[WS_GRANT_MAX];
static int   ws_session_ngrants;

/**
 * ws_grants_clear - Forget every folder granted in this session.
 *
 * A new conversation starts with only the workspace, the project's include
 * directories and the roots the user wrote down.
 */
static void ws_grants_clear(void)
{
 int i;
 for (i = 0; i < ws_session_ngrants; i++) {
  free(ws_session_grants[i]);
  ws_session_grants[i] = NULL;
 }
 ws_session_ngrants = 0;
}

/**
 * wpe_ai_scope_can_grant - May this folder be opened for reading for the session?
 * @dir: the folder a read box would offer, absolute.
 *
 * The root of the disk holds every file, so opening it would switch the scope
 * off for reads rather than widen it; the classifier gives such a grant no
 * weight from any source, and the read box does not offer it.  A folder that
 * cannot be followed is not offered either.
 * Return: 1 when the folder can be granted.
 */
int wpe_ai_scope_can_grant(const char *dir)
{
 char real[WPE_PATH_RESOLVED_MAX];
 if (!dir || !dir[0]) return 0;
 if (wpe_path_resolve(dir, real, sizeof real) != WPE_PATH_RESOLVED) return 0;
 return strcmp(real, "/") != 0;
}

/**
 * wpe_ai_scope_grant - Let the assistant read a folder for the rest of the session.
 * @dir: the folder, absolute, as the user was shown it.
 *
 * The folder is followed first and the place it leads to is what is kept, so a
 * grant given through a link covers that place and nothing the link was made
 * for later.  Granting a folder twice keeps one.
 * Return: 0 when granted, -1 when the folder cannot be followed or the list is full.
 */
int wpe_ai_scope_grant(const char *dir)
{
 char real[WPE_PATH_RESOLVED_MAX];
 int i;
 if (!wpe_ai_scope_can_grant(dir)) return -1;
 if (wpe_path_resolve(dir, real, sizeof real) != WPE_PATH_RESOLVED) return -1;
 for (i = 0; i < ws_session_ngrants; i++)
  if (!strcmp(ws_session_grants[i], real)) return 0;
 if (ws_session_ngrants >= WS_GRANT_MAX) return -1;
 ws_session_grants[ws_session_ngrants] = ws_strdup(real);
 if (!ws_session_grants[ws_session_ngrants]) return -1;
 ws_session_ngrants++;
 return 0;
}

int wpe_ai_scope_grant_count(void) { return ws_session_ngrants; }

const char *wpe_ai_scope_grant_at(int i)
{
 if (i < 0 || i >= ws_session_ngrants) return NULL;
 return ws_session_grants[i];
}

/**
 * wpe_ai_scope_grant_withdraw - Take one granted folder back.
 * @i: its position in the list the status shows.
 *
 * Reads there ask again from the next tool call on.
 */
void wpe_ai_scope_grant_withdraw(int i)
{
 if (i < 0 || i >= ws_session_ngrants) return;
 free(ws_session_grants[i]);
 memmove(ws_session_grants + i, ws_session_grants + i + 1,
         (size_t)(ws_session_ngrants - i - 1) * sizeof *ws_session_grants);
 ws_session_ngrants--;
 ws_session_grants[ws_session_ngrants] = NULL;
}

/**
 * ws_push_path_list - Add the directories of a colon-separated list to a grant list.
 * @l/@n/@cap: the list being built; @text: the list as the user wrote it, or NULL.
 *
 * An entry that is not absolute is read from the working directory, as the
 * editor reads the same include path when it looks for a header.
 */
static void ws_push_path_list(char ***l, int *n, int *cap, const char *text)
{
 char *copy, *p, *next;
 char abs[WPE_PATH_RESOLVED_MAX];
 if (!text || !*text) return;
 copy = ws_strdup(text);
 if (!copy) return;
 for (p = copy; p; p = next) {
  next = strchr(p, ':');
  if (next) *next++ = '\0';
  if (!*p) continue;
  if (ws_absolute_unfolded(p, abs, sizeof abs))
   ws_list_push(l, n, cap, ws_strdup(abs));
 }
 free(copy);
}

/**
 * ws_collect_read_grants - Every folder outside the workspace the assistant may read.
 * @l/@n: receive a malloc'd list of malloc'd absolute paths (free with wpe_ai_free_list).
 */
static void ws_collect_read_grants(char ***l, int *n)
{
 int cap = 0, i;
 *l = NULL; *n = 0;
 ws_push_path_list(l, n, &cap, e_prog.sys_include);
 ws_push_path_list(l, n, &cap, e_ai_read_roots);
 for (i = 0; i < ws_session_ngrants; i++)
  ws_list_push(l, n, &cap, ws_strdup(ws_session_grants[i]));
}

/**
 * wpe_ai_scope_read_dirs - The folders the hosted engine is told it may read.
 * @out: receives a malloc'd array of malloc'd paths.
 *
 * The same folders the judge lets through, minus any that is not a directory
 * on this machine (the engine refuses a name it cannot open), so the engine
 * does not ask about what the user has already allowed.
 * Return: the number of folders.
 */
int wpe_ai_scope_read_dirs(char ***out)
{
 char **all, **keep = NULL;
 int nall, nkeep = 0, cap = 0, i;
 struct stat st;
 ws_collect_read_grants(&all, &nall);
 for (i = 0; i < nall; i++) {
  if (!wpe_ai_scope_can_grant(all[i])) continue;      /* "/" would let the engine read everything unasked */
  if (stat(all[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
  if (ws_list_has(keep, nkeep, all[i])) continue;
  ws_list_push(&keep, &nkeep, &cap, ws_strdup(all[i]));
 }
 wpe_ai_free_list(all, nall);
 *out = keep;
 return nkeep;
}

/**
 * wpe_ai_scope_folder_of - The folder a "Yes for this folder" answer would grant.
 * @path: what the model read, as it named it; @dir/@sz: receive the folder,
 * empty when the path cannot be followed.
 *
 * The place the path really leads to is the one that counts.  A directory
 * that is read (a listing, a search) is granted whole; a file is granted
 * through the folder that holds it, so the next file beside it is not asked
 * about again.
 */
void wpe_ai_scope_folder_of(const char *path, char *dir, size_t sz)
{
 char abs[WPE_PATH_RESOLVED_MAX];
 struct stat st;
 char *slash;
 if (sz) dir[0] = '\0';
 if (!path || !ws_absolute_unfolded(path, abs, sizeof abs)) return;
 if (wpe_path_resolve(abs, dir, sz) != WPE_PATH_RESOLVED) { dir[0] = '\0'; return; }
 if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return;
 slash = strrchr(dir, '/');
 if (!slash) return;
 if (slash == dir) dir[1] = '\0';
 else *slash = '\0';
}

/**
 * wpe_ai_scope_count_skipped_links - How many links below a folder lead out of it?
 * @path: the folder a search starts in, as the model named it, relative to
 * the working directory or absolute; @depth: WPE_SCAN_ONE_LEVEL or
 * WPE_SCAN_ALL_LEVELS; @truncated: set to 1 when the look stopped at its entry
 * bound; may be NULL.
 *
 * A search never follows such a link; the count is what the result tells the
 * model so a short answer is not taken for the whole tree.
 * Return: the number of links, 0 when @path is not a folder.
 */
int wpe_ai_scope_count_skipped_links(const char *path, int depth, int *truncated)
{
 char abs[WPE_PATH_RESOLVED_MAX];
 if (truncated) *truncated = 0;
 if (!path || !ws_absolute_unfolded(path, abs, sizeof abs)) return 0;
 return wpe_path_count_outward_links(abs, depth, truncated);
}

/**
 * wpe_ai_workspace_dir - The workspace's folder, absolute.
 * @f: any window of the editor.
 *
 * The same root the judge compares against.  The hosted engine is started
 * there, so its own idea of the project's folder (what it reads without
 * asking, what a path it names relative means) is the editor's, and not the
 * folder the editor happened to be started in.
 * Return: the malloc'd folder, or NULL when it cannot be formed.
 */
char *wpe_ai_workspace_dir(FENSTER *f)
{
 char abs[WPE_PATH_RESOLVED_MAX];
 char *root = ws_root(f), *out = NULL;
 if (root && ws_absolute_unfolded(root, abs, sizeof abs)) out = ws_strdup(abs);
 free(root);
 return out;
}

/* The folder below the config home that holds the user's own xwperc. */
#define WS_CONFIG_SUBDIR "xwpe"
/* The config home when $XDG_CONFIG_HOME is not set, below $HOME. */
#define WS_CONFIG_HOME_DEFAULT ".config"

/**
 * ws_in_user_config - Does a resolved path lie in the user's own config folder?
 * @rpath: the place a write would really land.
 *
 * The user's own xwperc is the one file trusted with the hook, the engine
 * command, the endpoints and the dial, and the same folder holds the API key
 * files.  When the workspace is the home folder (a note edited in ~), that
 * folder is inside it, so without this check an assistant under auto could
 * plant a hook there that then runs in every project.  Both the XDG folder
 * and the legacy ~/.xwpe are covered, as the editor reads either.
 * Return: 1 when @rpath lies in one of them.
 */
static int ws_in_user_config(const char *rpath)
{
 const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
 char dir[WPE_PATH_RESOLVED_MAX];
 if (xdg && *xdg) {
  snprintf(dir, sizeof dir, "%s/%s", xdg, WS_CONFIG_SUBDIR);
  if (wpe_path_lies_in(dir, rpath)) return 1;
 }
 if (!home || !*home) return 0;
 snprintf(dir, sizeof dir, "%s/%s/%s", home, WS_CONFIG_HOME_DEFAULT, WS_CONFIG_SUBDIR);
 if (wpe_path_lies_in(dir, rpath)) return 1;
 snprintf(dir, sizeof dir, "%s/%s", home, XWPE_HOME);
 return wpe_path_lies_in(dir, rpath);
}

/**
 * ws_protect_user_config - Mark a write inside the workspace that lands in the user's own config.
 * @where: the scope's answer; @access: the access judged; @rpath: where the
 * write really lands, or an empty string when it could not be told.
 * Return: WPE_SCOPE_PROTECTED for such a write, else @where unchanged.
 */
static int ws_protect_user_config(int where, int access, const char *rpath)
{
 if (access != WPE_SCOPE_WRITE || where != WPE_SCOPE_INSIDE || !rpath[0]) return where;
 if (ws_in_user_config(rpath)) return WPE_SCOPE_PROTECTED;
 return where;
}

/**
 * ws_protect_start_folder - Mark a write inside the workspace that lands on a
 * protected file of the folder the editor was started in.
 * @where: the scope's answer; @access: the access judged; @rpath: where the
 * write really lands, or an empty string when it could not be told.
 *
 * The editor reads ./xwperc, and the assistant ./.xwpe-ai.md and
 * ./AGENTS.md, from its working folder, which can be a folder below the
 * workspace (started in proj/src, editing proj/t.c).  Those files are
 * protected there as at the workspace's top, so an assistant under auto
 * cannot plant a compiler or new instructions for the next start.  The
 * folder is the one recorded at start (e_opt_launch_dir), not the working
 * folder of the moment, which a build changes while it runs.
 * Return: WPE_SCOPE_PROTECTED for such a write, else @where unchanged.
 */
static int ws_protect_start_folder(int where, int access, const char *rpath)
{
 const char *start = e_opt_launch_dir();
 char rcwd[WPE_PATH_RESOLVED_MAX];
 if (access != WPE_SCOPE_WRITE || where != WPE_SCOPE_INSIDE || !rpath[0]) return where;
 if (!start || !*start) return where;
 if (wpe_path_resolve(start, rcwd, sizeof rcwd) != WPE_PATH_RESOLVED) return where;
 if (wpe_path_is_protected(rcwd, rpath)) return WPE_SCOPE_PROTECTED;
 return where;
}

/**
 * wpe_ai_scope_judge - Where does a path land for a read or a write: inside, granted or outside?
 * @f: any window of the editor; @path: the file, as the model named it;
 * @access: WPE_SCOPE_READ or WPE_SCOPE_WRITE; @real/@sz: receive the place the
 * file really lands when that is not the place the text names (a link on the
 * way), else an empty string; may be NULL.
 *
 * The one owner of the assistant's reach, for the editor's own tools and for
 * the hosted engine's alike.  The workspace is the project's directory when a
 * project is open, else the directory of the file the request came from: the
 * same root the scope list and the checkpoint use.  Links are followed on
 * both sides (we_ai_path.c), so a vendored directory or a link the model made
 * with a command, pointing out of the tree, no longer passes unseen.  A read
 * is also let through below the project's include directories, the roots the
 * user wrote into xwperc and the folders granted in this session; a write is
 * never widened by any of them.
 *
 * Return: WPE_SCOPE_INSIDE, WPE_SCOPE_GRANTED or WPE_SCOPE_OUTSIDE, and
 * WPE_SCOPE_PROTECTED for a write inside the workspace to a file that
 * changes how git, the editor or the engine runs code, the user's own config
 * folder (ws_protect_user_config) and the protected files of the folder the
 * editor was started in (ws_protect_start_folder) included.  A path
 * that cannot be made absolute (too long, no working directory) is outside:
 * the tool would still be attempted, so the safe answer is to ask.  With no
 * root, or the root "/", nothing is outside.
 */
int wpe_ai_scope_judge(FENSTER *f, const char *path, int access, char *real, size_t sz)
{
 char rootabs[WPE_PATH_RESOLVED_MAX], pathabs[WPE_PATH_RESOLVED_MAX];
 char lexbuf[WPE_PATH_RESOLVED_MAX], resolved[WPE_PATH_RESOLVED_MAX];
 char *root = ws_root(f), **grants = NULL;
 const char *lexical, *rp, *pp;
 int where, ngrants = 0;
 if (real && sz) real[0] = '\0';
 if (!root || !path) { free(root); return WPE_SCOPE_INSIDE; }
 rp = ws_absolute_unfolded(root, rootabs, sizeof rootabs);
 pp = ws_absolute_unfolded(path, pathabs, sizeof pathabs);
 free(root);
 if (!rp) return WPE_SCOPE_INSIDE;
 if (!pp) return WPE_SCOPE_OUTSIDE;
 if (access == WPE_SCOPE_READ) ws_collect_read_grants(&grants, &ngrants);
 where = wpe_scope_classify(rp, (const char *const *)grants, ngrants, pp, access,
                            resolved, sizeof resolved);
 wpe_ai_free_list(grants, ngrants);
 where = ws_protect_user_config(where, access, resolved);
 where = ws_protect_start_folder(where, access, resolved);
 lexical = ws_absolute_path(path, lexbuf, sizeof lexbuf);
 if (real && sz && resolved[0] && lexical && strcmp(resolved, lexical) != 0)
  snprintf(real, sz, "%s", resolved);
 return where;
}

/**
 * wpe_ai_path_landing - Where would a write to this path really go, and is that outside?
 * @f: any window of the editor; @path: the file, as the model named it;
 * @real/@sz: as for wpe_ai_scope_judge.
 *
 * A Multi-file proposal for "../outside.c" is flagged with this so the user
 * sees, before approving, that the model is writing beyond the tree.  It is
 * not refused: the model may be right.
 *
 * Return: 1 when the file lands outside, 0 when inside or when it cannot be told.
 */
int wpe_ai_path_landing(FENSTER *f, const char *path, char *real, size_t sz)
{
 return wpe_ai_scope_judge(f, path, WPE_SCOPE_WRITE, real, sz) == WPE_SCOPE_OUTSIDE;
}

/**
 * wpe_ai_path_outside_workspace - Would this path land outside the workspace?
 * @f: any window of the editor; @path: the file, as the model named it.
 *
 * The yes/no of wpe_ai_path_landing, for the callers that do not name where.
 *
 * Return: 1 when the file lands outside, 0 when inside or when it cannot be told.
 */
int wpe_ai_path_outside_workspace(FENSTER *f, const char *path)
{
 return wpe_ai_path_landing(f, path, NULL, 0);
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
 * wpe_ai_open_window_buffer - The buffer of the window showing a file.
 * @f: any window of the editor; @path: the file, as the tool named it.
 *
 * The same lookup as wpe_ai_reload_open_window.  A caller that reloads the
 * file and then runs the post-edit hook on it opens one undo step on this
 * buffer around both, so the user takes back the write and its reformatting
 * with one Ctrl-U and nothing typed before them.  Return: the buffer, or
 * NULL when the file is not open.
 */
BUFFER *wpe_ai_open_window_buffer(FENSTER *f, const char *path)
{
 char abs[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 FENSTER *w = ws_find_window(f, full);
 if (!w) return NULL;
 return w->b;
}

/**
 * wpe_ai_window_take_file - Replace a window's text with a file's, no step of its own.
 * @w: the open window; @path: the file whose text the window takes.
 *
 * For the post-edit hook: the caller has opened the undo step the change and
 * its reformatting share, so the user undoes both with one Ctrl-U instead of
 * seeing a half-undone file after the first.  The caller shows the window.
 * Return: 1 when taken.
 */
int wpe_ai_window_take_file(FENSTER *w, const char *path)
{
 char *text = ws_read_file(path, WS_RELOAD_MAX);
 if (!text) return 0;
 ws_window_take_text(w, text);
 free(text);
 return 1;
}

/**
 * wpe_ai_refresh_open_window - Re-read an open file after the hook rewrote it.
 * @f: any window of the editor; @path: the file, as the tool named it.
 *
 * The same lookup as wpe_ai_reload_open_window, but the buffer is refreshed
 * within the undo step its caller opened around the reload and the hook.
 * Return: 1 when refreshed.
 */
int wpe_ai_refresh_open_window(FENSTER *f, const char *path)
{
 char abs[1200];
 const char *full = ws_absolute_path(path, abs, sizeof abs);
 FENSTER *w = ws_find_window(f, full);
 if (!w) return 0;
 if (!wpe_ai_window_take_file(w, full)) return 0;
 e_window_show_replaced_text(w);
 return 1;
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
 WpeWriteOutcome o;
 if (!ws_is_real_file_window(w)) return 0;
 snprintf(name, sizeof name, ".ai-hook-%ld-%s", (long)getpid(), w->datnam);
 full = e_mkfilename(w->dirct, name);
 if (!full) return 0;
 text = ws_window_text(w);
 if (!text || strlen(full) >= sz) { free(text); free(full); return 0; }
 if (wpe_write_text(full, text, &o) != 0) {
  int err = o.err;
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

/* Why a revert refused a path, in the words the pane shows. */
#define WS_OUTSIDE_WORKSPACE "it is outside the workspace"
#define WS_NOT_RUN_CREATED   "the run did not create it"
#define WS_NAME_TOO_LONG     "its name is too long to hand to the shell whole"
/* The most bytes of one file a before image holds. */
#define WS_BEFORE_IMAGE_MAX  4000000
/* The longest sentence the write owner says about a revert. */
#define WS_REVERT_SENTENCE_MAX WPE_WRITE_SENTENCE_MAX

/**
 * ws_revert_command - The shell command that removes one file a git-mode run created.
 * @cmd/@sz: the command; @rel: the file, relative to the workspace.
 *
 * The only shell command a revert still runs: a file the run created is taken
 * away, and git has no before image of it.  A file that existed is put back by
 * ws_restore_before_image, never by a command that would make a new inode.
 * The workspace and the file name are quoted whole or not at all, and the
 * command is built whole or not at all, so a revert never acts on a name
 * other than the one the review listed.
 * Return: 0 with @cmd ready to run, -1 when it could not be built whole.
 */
static int ws_revert_command(char *cmd, size_t sz, const char *rel)
{
 char q[1100], qf[1300];
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 || ws_quote_whole(qf, sizeof qf, rel) != 0)
  return -1;
 return ws_format_whole(cmd, sz, "cd %s && rm -f -- %s 2>&1", q, qf);
}

/**
 * ws_git_show - Fetch a file's content as the checkpoint's git ref holds it.
 * @rel: the file, relative to the workspace; @buf/@len: receive the malloc'd
 * bytes and their count; @reason/@rsz: receive git's own sentence on failure.
 * Return: 0 with the content, -1 when git refused or the content could not be read.
 *
 * What the user gains: the before image comes from git, byte for byte, and
 * goes back into the very file through the write owner, so the file keeps its
 * inode, its other names and its bits.  The content goes through a file under
 * the state folder (made by mkstemp, so its name is never a known one) and not
 * through the command's captured text, which is cut short and holds no NUL byte.
 */
static int ws_git_show(const char *rel, char **buf, size_t *len, char *reason, size_t rsz)
{
 char dir[WPE_PATH_SIZE], file[WPE_PATH_SIZE + 24], q[1100], spec[1300], qs[2700], qf[1300];
 char cmd[4200], *out;
 int fd, status = 0;
 *buf = NULL; *len = 0;
 ws_state_dir(dir, sizeof dir);
 ws_make_dir_parents(dir);
 if (ws_format_whole(file, sizeof file, "%s/git-show-XXXXXX", dir) != 0) return -1;
 fd = mkstemp(file);
 if (fd < 0) { snprintf(reason, rsz, "%s", strerror(errno)); return -1; }
 close(fd);
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 || ws_quote_whole(qf, sizeof qf, file) != 0 ||
     ws_format_whole(spec, sizeof spec, "%s:./%s", ck_ref, rel) != 0 ||
     ws_quote_whole(qs, sizeof qs, spec) != 0 ||
     ws_format_whole(cmd, sizeof cmd, "cd %s && git --literal-pathspecs show %s 2>&1 >%s",
                     q, qs, qf) != 0) {
  unlink(file);
  snprintf(reason, rsz, "%s", WS_NAME_TOO_LONG);
  return -1;
 }
 out = wpe_ai_run_capture_status(cmd, &status);
 if (status != 0) ws_output_reason(out, status, reason, rsz);
 else *buf = ws_read_file_len(file, WS_BEFORE_IMAGE_MAX, len);
 free(out);
 unlink(file);
 if (status == 0 && !*buf) snprintf(reason, rsz, "%s", "git's copy could not be read");
 return *buf ? 0 : -1;
}

/**
 * ws_git_run_created - Did the run create this file, as far as git can tell?
 * @path: the full path of a file a review row calls new.
 *
 * Removing a file is the one revert that cannot be undone, so it is allowed
 * only for a file that is untracked now and was not there when the checkpoint
 * was taken.  A file the user had before the run, or one git tracks, stays.
 * Return: 1 when it is the run's, 0 when it is not or git could not say.
 */
static int ws_git_run_created(const char *path)
{
 char **now;
 int nn, yes;
 if (ws_git_was_there(path)) return 0;
 if (ws_git_list("ls-files --others --exclude-standard -z -- .", &now, &nn) != 0) return 0;
 yes = ws_list_has(now, nn, path);
 wpe_ai_free_list(now, nn);
 return yes;
}

/* Why a revert refused a path whose folders now lead outside the tree.  Git
   mode refuses too: `git checkout` of a path beyond a link touches nothing
   outside but also exits 0 having restored nothing, which the review must not
   count as a file put back. */
#define WS_FOLDER_LINK "a link has taken the place of a folder on the way.  Nothing was written."
#define WS_ROOT_LINK   "the workspace folder was replaced by a link; nothing was restored or removed."

/**
 * ws_revert_root_refused - Refuse a revert when the workspace folder was replaced.
 * @f: any window of the editor; @what: "restore" or "remove"; @path: the file.
 * Return: 0 when the folder is still the one of the checkpoint, -1 after
 * saying the refusal (counted as a failure by the caller).
 *
 * Asked before a revert writes or removes anything, so a link put in place of
 * the workspace folder never leads a restore or a removal to another place.
 */
static int ws_revert_root_refused(FENSTER *f, const char *what, const char *path)
{
 if (ws_root_unchanged()) return 0;
 return ws_revert_failed(f, what, path, WS_ROOT_LINK);
}

/**
 * ws_real_stays_in_tree - Does the reviewed path still lead where the review listed it?
 * @path: the file as the review names it; @real: where its folder really leads,
 * from the look.
 * Return: 1 when @real is the resolved workspace plus the path's own part, or
 * when the path is not below the workspace at all (a file beyond it, which no
 * folder link inside the tree can have moved); 0 when a link sits on the way.
 *
 * What the user gains: a link a command planted in place of a folder inside the
 * workspace cannot lead a revert to a file outside it.  The workspace itself is
 * resolved once, at the checkpoint (ck_rdir), so one reached through a link keeps
 * working, and one replaced by a link later is caught by ws_root_unchanged.
 */
static int ws_real_stays_in_tree(const char *path, const char *real)
{
 char want[WPE_PATH_RESOLVED_MAX];
 const char *rel = ws_workspace_rel(path);
 if (!rel) return 1;
 if (snprintf(want, sizeof want, "%s%s%s", ck_rdir, strcmp(ck_rdir, "/") ? "/" : "", rel) >= (int)sizeof want)
  return 0;
 return strcmp(real, want) == 0;
}

/**
 * ws_folders_stay_in_tree - Do the folders of a reviewed path still lead where the review listed them?
 * @path: the file as the review names it.
 * Return: 1 when they do (or the path is not below the workspace), 0 when a
 * link sits on the way or the folders cannot be followed.
 *
 * Asked before any link branch of a revert, so a file reached through a
 * planted folder link is never restored, checked out or counted as put back.
 */
static int ws_folders_stay_in_tree(const char *path)
{
 char real[WPE_PATH_RESOLVED_MAX];
 WpeWriteExpect x;
 if (!ws_workspace_rel(path)) return 1;
 if (wpe_path_look_name(path, real, sizeof real, &x) != 0) return 0;
 return ws_real_stays_in_tree(path, real);
}

/**
 * ws_restore_before_image - Put a file's before image back, through the write owner.
 * @f: any window of the editor; @path: the file, as the review names it;
 * @text/@len: the before image, which may hold NUL bytes.
 *
 * The one place a revert writes a user's file.  What the user gets: the file
 * is what it was before the run, in the very same file (the inode, the other
 * names and a window's identity are kept), wearing the permission bits recorded
 * before the assistant's first write, or its own when none was recorded.  The
 * name is looked at afresh, without following a link at its end (only the
 * folder is followed), and the write is held to that look, so a link planted
 * at the name, or a file swapped in, since the review began is refused and
 * said: the revert never writes into whatever a link leads to.  No
 * .bak is made, and a bit the system would not give back is named in the pane.
 * Return: 0 when the file holds the before image, -1 when it kept the change.
 */
static int ws_restore_before_image(FENSTER *f, const char *path, const char *text, size_t len)
{
 char real[WPE_PATH_RESOLVED_MAX], say[WS_REVERT_SENTENCE_MAX];
 WpeWriteExpect x;
 WpeWriteOutcome o;
 FENSTER *w;
 if (wpe_path_look_name(path, real, sizeof real, &x) != 0)
  return ws_revert_failed(f, "restore", path, strerror(errno));
 if (!ws_real_stays_in_tree(path, real))
  return ws_revert_failed(f, "restore", path, WS_FOLDER_LINK);
 w = ws_window_of_file(f, real, &x);
 if (wpe_write_restore_text(real, text, len, ws_mode_recorded(real),
                            w ? &w->file_id : NULL, &x, &o) != 0) {
  wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE);
  return ws_revert_failed(f, "restore", path, say);
 }
 if (w) wpe_write_identity_set(&w->file_id, &o.written);
 if (wpe_write_sentence(say, sizeof say, &o, WPE_WRITE_THEN_NOTHING_MORE) > 0)
  wpe_ai_pane_error(f, say);
 return 0;
}

/**
 * ws_git_entry_is_link - Is this file a symbolic link in the checkpoint's git ref?
 * @rel: the file, relative to the workspace.
 * Return: 1 when git's entry for it has mode 120000, else 0 (also when git
 * could not say).
 */
static int ws_git_entry_is_link(const char *rel)
{
 char q[1100], spec[1300], qs[2700], cmd[4200], *out;
 int status = 0, yes;
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 ||
     ws_format_whole(spec, sizeof spec, "./%s", rel) != 0 ||
     ws_quote_whole(qs, sizeof qs, spec) != 0 ||
     ws_format_whole(cmd, sizeof cmd, "cd %s && git --literal-pathspecs ls-tree %s -- %s 2>/dev/null",
                     q, ck_ref, qs) != 0) return 0;
 out = wpe_ai_run_capture_status(cmd, &status);
 yes = status == 0 && out && !strncmp(out, "120000", 6);
 free(out);
 return yes;
}

/**
 * ws_revert_by_checkout - Put one git entry back with git itself.
 * @f: any window of the editor; @path: the file; @rel: relative to the workspace.
 *
 * Used where the entry is a link, in git's ref or at the name now: a link has
 * no inode to keep and no bits to record, and a checkout replaces the entry
 * and never writes through a link, so a revert of a link, or of a file a link
 * took the place of, cannot reach what the link leads to.
 * Return: 0 when git put it back, -1 when it refused (git's sentence is said).
 */
static int ws_revert_by_checkout(FENSTER *f, const char *path, const char *rel)
{
 char q[1100], qf[1300], cmd[2800], reason[600], *out;
 int status = 0;
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 || ws_quote_whole(qf, sizeof qf, rel) != 0 ||
     ws_format_whole(cmd, sizeof cmd, "cd %s && git --literal-pathspecs checkout %s -- %s 2>&1",
                     q, ck_ref, qf) != 0)
  return ws_revert_failed(f, "restore", path, WS_NAME_TOO_LONG);
 out = wpe_ai_run_capture_status(cmd, &status);
 if (status != 0) ws_output_reason(out, status, reason, sizeof reason);
 free(out);
 return status != 0 ? ws_revert_failed(f, "restore", path, reason) : 0;
}

/**
 * ws_name_is_link - Is there a symbolic link at this name now?
 * @path: the file.
 */
static int ws_name_is_link(const char *path)
{
 struct stat st;
 return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

/**
 * ws_revert_git - Put one git-mode file back, or take away one the run created.
 * @f: any window of the editor; @path: the file; @is_new: the run created it.
 *
 * A file that existed comes back from git's copy of it (git show) through
 * ws_restore_before_image, never from a checkout that would unlink it and make
 * a new file with the umask's mode.  The one exception is a link, in git's ref
 * or at the name now, which a checkout puts back (ws_revert_by_checkout); a
 * path in the run's .bak list never gets here (ws_revert_path skips it).  A
 * file the run created is removed by the
 * one shell command left, and only when it is untracked now and was not there
 * at the checkpoint.
 * Return: 0 when the file is back, -1 when it kept the change.
 */
static int ws_revert_git(FENSTER *f, const char *path, int is_new)
{
 const char *what = is_new ? "remove" : "restore";
 const char *rel = ws_workspace_rel(path);
 char cmd[2800], *out, reason[600], *text;
 size_t len;
 int status = 0, rc;
 if (!rel) return ws_revert_failed(f, what, path, WS_OUTSIDE_WORKSPACE);
 if (ws_revert_root_refused(f, what, path) != 0) return -1;
 if (!is_new) {
  if (!ws_folders_stay_in_tree(path))
   return ws_revert_failed(f, what, path, WS_FOLDER_LINK);
  if (ws_git_entry_is_link(rel) || ws_name_is_link(path))
   return ws_revert_by_checkout(f, path, rel);
  if (ws_git_show(rel, &text, &len, reason, sizeof reason) != 0)
   return ws_revert_failed(f, what, path, reason);
  rc = ws_restore_before_image(f, path, text, len);
  free(text);
  return rc;
 }
 if (!ws_git_run_created(path)) return ws_revert_failed(f, what, path, WS_NOT_RUN_CREATED);
 if (!ws_folders_stay_in_tree(path)) return ws_revert_failed(f, what, path, WS_FOLDER_LINK);
 if (ws_revert_command(cmd, sizeof cmd, rel) != 0)
  return ws_revert_failed(f, what, path, WS_NAME_TOO_LONG);
 out = wpe_ai_run_capture_status(cmd, &status);
 if (status != 0) ws_output_reason(out, status, reason, sizeof reason);
 free(out);
 return status != 0 ? ws_revert_failed(f, what, path, reason) : 0;
}

/**
 * ws_revert_snapshot - Put one snapshot-mode file back, or take away one the run created.
 * @f: any window of the editor; @path: the file; @is_new: the run created it.
 *
 * The before image is the copy the checkpoint made; it goes back through
 * ws_restore_before_image whole, a NUL byte included.
 * Return: 0 when the file is back, -1 when it kept the change.
 */
static int ws_revert_snapshot(FENSTER *f, const char *path, int is_new)
{
 char snap[1200], *text;
 size_t len;
 int i, rc;
 if (ws_revert_root_refused(f, is_new ? "remove" : "restore", path) != 0) return -1;
 if (is_new) {
  if (!ws_snapshot_run_created(path)) return ws_revert_failed(f, "remove", path, WS_NOT_RUN_CREATED);
  if (!ws_folders_stay_in_tree(path)) return ws_revert_failed(f, "remove", path, WS_FOLDER_LINK);
  if (unlink(path) != 0) return ws_revert_failed(f, "remove", path, strerror(errno));
  return 0;
 }
 for (i = 0; i < ck_n; i++) if (!strcmp(ck_files[i], path)) break;
 if (i == ck_n) return ws_revert_failed(f, "restore", path, WS_NO_BEFORE_IMAGE);
 snprintf(snap, sizeof snap, "%s/%d", ck_snapdir, i);
 text = ws_read_file_len(snap, WS_BEFORE_IMAGE_MAX, &len);
 if (!text) return ws_revert_failed(f, "restore", path, WS_NO_BEFORE_IMAGE);
 rc = ws_restore_before_image(f, path, text, len);
 free(text);
 return rc;
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
 int rc = 0;
 if (ws_is_run_backup(path)) return 0;       /* the price of a write, not the run's work */
 if (ck_mode == WS_CK_GIT) rc = ws_revert_git(f, path, is_new);
 else if (ck_mode == WS_CK_SNAPSHOT) rc = ws_revert_snapshot(f, path, is_new);
 if (rc != 0) return rc;
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
 * the pane like any other write the editor could not make; a list written
 * only in part is removed, so nothing of it waits under the state directory.
 */
static int ws_write_pathspec(FENSTER *f, ws_change *c, int n, char *buf, size_t sz)
{
 char dir[1024];
 char **done = NULL; int nd = 0, cap = 0, i;
 WpeWriteOutcome o;
 FILE *fp;
 ws_state_dir(dir, sizeof dir);
 if (ws_make_dir(f, dir) != 0) return -1;
 snprintf(buf, sz, "%s/changeset-%ld.pathspec", dir, (long)getpid());
 fp = wpe_write_stream_open(buf, &o);
 if (!fp) { ws_write_failed(f, WS_PATHSPEC_WHAT, buf, o.err); return -1; }
 for (i = 0; i < n; i++) {
  const char *rel;
  if (ws_list_has(done, nd, c[i].path)) continue;        /* one file, one entry */
  ws_list_push(&done, &nd, &cap, ws_strdup(c[i].path));
  rel = ws_path_under_root(c[i].path, ck_dir);
  fputs(rel, fp);
  fputc('\0', fp);                                        /* the NUL ends each path */
 }
 wpe_ai_free_list(done, nd);
 if (wpe_write_stream_close(fp, &o) != 0) {
  ws_write_failed(f, WS_PATHSPEC_WHAT, buf, o.err);
  unlink(buf);                                            /* no half list left behind */
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
 if (ws_quote_whole(q, sizeof q, ck_dir) != 0 || ws_quote_whole(ql, sizeof ql, list) != 0 ||
     ws_format_whole(cmd, sizeof cmd,
                     "cd %s && git %s --pathspec-from-file=%s --pathspec-file-nul 2>&1",
                     q, verb, ql) != 0) {
  ws_say_failed(f, what, "the changeset", WS_NAME_TOO_LONG);
  return -1;
 }
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
 * ws_say_left_staged - Tell the user where a refused commit left the files.
 * @f: any window of the editor.
 *
 * git refused the commit after the listed files were staged (a hook, most
 * often), so they sit in the index with no commit.  A hook's refusal usually
 * means "fix it and commit again", and the staged files are what that retry
 * needs, so they are left there; a `git commit` the user runs next would carry
 * them, which is why the pane says so under git's own reason.
 */
static void ws_say_left_staged(FENSTER *f)
{
 wpe_ai_pane_error(f, "the listed files stay staged");
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
  if (rc != 0) ws_say_left_staged(f);
 }
 unlink(list);
 return rc;
}

/**
 * ws_show_review_keys - Print the keys the changeset review answers to.
 * @f: any window of the editor.
 *
 * Two Messages rows, so each is whole in the 38-column Messages window the
 * docked pane leaves at 80 columns; one row of them was cut before the commit
 * key.  The commit key is printed only in a git workspace, where it does
 * something: a key that does nothing is not offered.
 */
static void ws_show_review_keys(FENSTER *f)
{
 e_d_p_message("   a = keep all   r = REVERT all", f, 0);
 if (ck_mode == WS_CK_GIT) e_d_p_message("   f = file by file   c = commit", f, 0);
 else                      e_d_p_message("   f = file by file", f, 0);
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

 wpe_ai_messages_run_output_begins(f);   /* the run's first Messages rows */
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

 ws_show_review_keys(f);
 for (;;) {
  int k = e_toupper(wpe_paint_modal_getch());
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
     int kk = e_toupper(wpe_paint_modal_getch());
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
  if (k == 'C' && ck_mode == WS_CK_GIT) {
   static char msg[256];
   strcpy(msg, "AI-assisted change");
   if (e_add_arguments(msg, sizeof msg, "Commit message", f, 0, AltB, NULL) && msg[0])
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
 int    dropped;             /* messages dropped from the front of the store
                                since the session was cleared or loaded         */
} sess;

/* How many messages the saved session keeps; what a turn sends of them is
   bounded by bytes (wpe_ai_session_window), not by this count. */
#define WS_SESS_MAX 200

static void sess_free(void)
{
 int i;
 free(sess.cli); sess.cli = NULL;
 for (i = 0; i < sess.n; i++) { free(sess.role[i]); free(sess.content[i]); }
 free(sess.role); free(sess.content);
 sess.role = sess.content = NULL; sess.n = 0;
 sess.dropped = 0;
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

/**
 * ws_session_leaf - Where a conversation is kept below the state folder.
 * @f: the window the conversation belongs to; @leaf/@sz: receives the leaf.
 * Return: 1 with the leaf, 0 when a project is open: its conversation lives
 * in the project's own folder, not the state folder.
 */
static int ws_session_leaf(FENSTER *f, char *leaf, size_t sz)
{
 if (e_project_is_open()) return 0;
 snprintf(leaf, sz, "ai/%lu.session", ws_hash(f->dirct ? f->dirct : "."));
 return 1;
}

/**
 * ws_session_store - Write the conversation to the file it is kept in.
 * @f: the window; @path: ws_session_path's answer; @text: the JSON.
 * Return: 0 when the file holds @text, -1 with errno set when not.
 *
 * Under the state folder it is a state file (built from a leaf, so no user
 * path reaches it); in a project's folder it goes through the write owner by
 * the path ws_session_path built.
 */
static int ws_session_store(FENSTER *f, const char *path, const char *text)
{
 char leaf[WS_MODE_LEAF_MAX];
 WpeWriteOutcome o;
 if (ws_session_leaf(f, leaf, sizeof leaf)) return ws_write_state_file(leaf, text);
 if (wpe_write_text(path, text, &o) == 0) return 0;
 errno = o.err ? o.err : EIO;
 return -1;
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
  sess.dropped++;
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

/**
 * wpe_ai_session_dropped - How many messages the store has dropped this session.
 *
 * The store keeps WS_SESS_MAX messages and drops the oldest past that; a
 * turn that cannot send them says so in the pane, so the user knows the
 * model no longer sees the start of a very long conversation.  Cleared
 * with the session and when a saved session is loaded.
 * Return: the count.
 */
int wpe_ai_session_dropped(void) { return sess.dropped; }

/**
 * ws_window_takes - Does the next older message join the window?
 * @taken: messages already in the window; @used: their bytes;
 * @len: this message's bytes; @budget: the window's bytes.
 *
 * The newest message is always sent, however long, so a question is never
 * sent without itself; the others join while they fit whole.
 * Return: 1 when the message is sent.
 */
static int ws_window_takes(int taken, size_t used, size_t len, size_t budget)
{
 if (taken == 0)
  return 1;
 return used + len <= budget;
}

/**
 * wpe_ai_session_window - Which stored messages a turn sends.
 * @budget: the bytes of message content a turn may carry;
 * @w: filled with the window.
 *
 * Counted from the newest message backwards, whole messages only, so a
 * long conversation keeps its latest exchanges and the model never reads a
 * message cut in half.  The chat and the agent both take their history
 * from here, so one conversation reads the same to the model whichever of
 * the two the user typed into; the counts it leaves are what the pane line
 * about unsent messages says.  wpe_ai_session_messages(buf, w->count)
 * then hands out exactly these messages.
 */
void wpe_ai_session_window(size_t budget, wpe_ai_history_window *w)
{
 size_t used = 0, len;
 int i = sess.n;
 while (i > 0) {
  len = strlen(sess.content[i - 1]);
  if (!ws_window_takes(sess.n - i, used, len, budget))
   break;
  used += len;
  i--;
 }
 w->start = i;
 w->count = sess.n - i;
 w->over_budget = i;
 w->dropped = sess.dropped;
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
 if (ws_session_store(f, path, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN)) != 0) {
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
 ws_grants_clear();                      /* a granted folder lasts as long as the conversation */
 free(e_ai_resume_session); e_ai_resume_session = NULL;
 free(e_ai_last_session_id); e_ai_last_session_id = NULL;
 ws_session_path(f, path, sizeof path);
 unlink(path);
 wpe_ai_trace("session reset");
}

#endif /* WPE_AI */

typedef int wpe_ai_ws_translation_unit;
