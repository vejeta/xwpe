/* we_ai_path.c -- where a path really lands, for the assistant's write policy.
 *
 * The workspace check used to compare path text: "ws/vendor/x.c" counted as
 * inside however "ws/vendor" was made.  A symbolic link inside the workspace
 * that leads outside it (a vendored directory, a link the model itself made
 * with a shell command) therefore let an unattended run write beyond the tree
 * without the box that every outside write gets.  Here a path is followed the
 * way the kernel will follow it when the file is opened, so the question is
 * asked of the place the bytes will really go.
 *
 * Pure C, no display header: the C tests link this file alone.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "we_ai_path.h"
#include "we_fmt.h"
#include "we_places.h"
#include "we_read.h"

/* The noun the read owner's sentences use for a file or folder read here. */
#define PATH_READ_WHAT WPE_AI_READ_WHAT

/* The walk's state: what is resolved so far, what is left to read, and how
   many links were followed.  `missing` is set from the first component that
   does not exist: nothing below it can be a link, so the rest is appended as
   written (a new file under a linked directory is judged by the directory). */
typedef struct {
 char out[WPE_PATH_RESOLVED_MAX];    /* the resolved prefix, "/" at the start */
 char rest[WPE_PATH_RESOLVED_MAX];   /* the text still to read                */
 size_t pos;                         /* where in `rest` the next component is */
 int hops;
 int missing;
} wpe_path_walk;

/**
 * path_next_component - Read the next component of the walk's remaining text.
 * @w: the walk; @comp/@sz: receive the component.
 *
 * Return: its length, 0 when nothing is left, -1 when it does not fit.
 */
static int path_next_component(wpe_path_walk *w, char *comp, size_t sz)
{
 size_t n = 0;
 while (w->rest[w->pos] == '/') w->pos++;
 while (w->rest[w->pos + n] && w->rest[w->pos + n] != '/') n++;
 if (n >= sz) return -1;
 memcpy(comp, w->rest + w->pos, n);
 comp[n] = '\0';
 w->pos += n;
 return (int)n;
}

/* Drop the last component of the resolved prefix; the root stays the root. */
static void path_pop(char *out)
{
 char *slash = strrchr(out, '/');
 if (!slash || slash == out) { out[0] = '/'; out[1] = '\0'; return; }
 *slash = '\0';
}

/* Put "<dir>/<name>" in @dst; -1 when it does not fit. */
static int path_join(char *dst, size_t sz, const char *dir, const char *name)
{
 if (!strcmp(dir, "/")) {
  if (wpe_fmt_whole(dst, sz, "/%s", name) != WPE_FMT_OK) return -1;
  return 0;
 }
 if (wpe_fmt_whole(dst, sz, "%s/%s", dir, name) != WPE_FMT_OK) return -1;
 return 0;
}

/**
 * path_follow_link - Replace a link's name in the walk by what it points to.
 * @w: the walk; @link: the full path of a name lstat(2) just saw as a link.
 *
 * An absolute target restarts from the root, a relative one continues from
 * the directory that holds the link; either way the target's own components
 * are read next, ahead of whatever followed the link.  A link that cannot be
 * read (swapped between the look and the read) stops the walk: taking it as a
 * plain name would let the kernel follow it later where this walk did not,
 * and the answer would no longer be where the bytes go.
 * Return: 0 when the target was spliced in, -1 when the walk cannot go on (too
 * many links, a link that cannot be read, no room).
 */
static int path_follow_link(wpe_path_walk *w, const char *link)
{
 char target[WPE_PATH_RESOLVED_MAX], joined[WPE_PATH_RESOLVED_MAX];
 ssize_t n;
 if (++w->hops > WPE_PATH_MAX_HOPS) return -1;
 n = readlink(link, target, sizeof target - 1);
 if (n <= 0) return -1;
 target[n] = '\0';
 if (target[0] == '/') { w->out[0] = '/'; w->out[1] = '\0'; }
 if (WPE_FMT_WHOLE(joined, "%s/%s", target, w->rest + w->pos) != WPE_FMT_OK) return -1;
 memcpy(w->rest, joined, strlen(joined) + 1);
 w->pos = 0;
 return 0;
}

/**
 * path_step - Take one component: a dot, a ".." or a name, and follow it.
 * @w: the walk; @comp: the component.
 *
 * Return: 0 to go on, -1 when the walk cannot.
 */
static int path_step(wpe_path_walk *w, const char *comp)
{
 char cand[WPE_PATH_RESOLVED_MAX];
 struct stat st;
 if (!strcmp(comp, ".")) return 0;
 if (!strcmp(comp, "..")) {
  path_pop(w->out);
  w->missing = 0;                   /* back on a place that exists */
  return 0;
 }
 if (path_join(cand, sizeof cand, w->out, comp) < 0) return -1;
 if (!w->missing && lstat(cand, &st) != 0) w->missing = 1;
 if (!w->missing && S_ISLNK(st.st_mode)) return path_follow_link(w, cand);
 memcpy(w->out, cand, strlen(cand) + 1);
 return 0;
}

/**
 * wpe_path_resolve - Follow a path to where it really lands, file or not.
 * @path: an absolute path, as the model wrote it; @out/@sz: the result.
 *
 * Every symbolic link on the way is followed, the last component's included,
 * so a link to a file that does not exist yet is judged by where that file
 * would be made.  Dots and ".." are folded as the kernel folds them, after
 * the links before them.  The part of the path that does not exist (a new
 * file, a new directory) is kept as written below the deepest part that does.
 * Unlike realpath(3) it does not fail on a missing file, which is the common
 * case for a write.
 *
 * Return: WPE_PATH_RESOLVED, or WPE_PATH_UNRESOLVABLE for a link loop, a link
 * that cannot be read, a path that does not fit WPE_PATH_RESOLVED_MAX, or a
 * path that is not absolute.
 */
int wpe_path_resolve(const char *path, char *out, size_t sz)
{
 wpe_path_walk w;
 char comp[WPE_PATH_RESOLVED_MAX];
 int n;
 if (!path || path[0] != '/' || strlen(path) >= sizeof w.rest) return WPE_PATH_UNRESOLVABLE;
 memcpy(w.rest, path, strlen(path) + 1);
 w.out[0] = '/'; w.out[1] = '\0';
 w.pos = 0; w.hops = 0; w.missing = 0;
 while ((n = path_next_component(&w, comp, sizeof comp)) != 0) {
  if (n < 0 || path_step(&w, comp) < 0) return WPE_PATH_UNRESOLVABLE;
 }
 if (strlen(w.out) >= sz) return WPE_PATH_UNRESOLVABLE;
 memcpy(out, w.out, strlen(w.out) + 1);
 return WPE_PATH_RESOLVED;
}

/**
 * path_is_under - Is a resolved path the directory itself or something below it?
 * @rdir: a resolved directory; @rpath: a resolved path.
 *
 * The one test of "inside" every scope question goes through, so the
 * workspace, a grant and a walk's start cannot disagree about it.  The
 * directory's name must end at a component: "/ws2/x" is not under "/ws".
 * The root "/" holds everything.
 * Return: 1 when @rpath is @rdir or below it.
 */
static int path_is_under(const char *rdir, const char *rpath)
{
 size_t n = strlen(rdir);
 if (n <= 1) return 1;
 if (strncmp(rpath, rdir, n) != 0) return 0;
 return rpath[n] == '/' || rpath[n] == '\0';
}

/**
 * path_under_a_grant - Is a resolved path below any of the granted directories?
 * @grants/@ngrants: the directories as written; @rpath: a resolved path.
 *
 * A grant is followed like every other path, so a grant given through a link
 * covers the place the link leads and nothing else.  One that cannot be
 * followed grants nothing, and neither does the root "/": it holds every
 * file, so honouring it would switch the scope off instead of widening it.
 * Return: 1 when some grant holds the path.
 */
static int path_under_a_grant(const char *const *grants, int ngrants, const char *rpath)
{
 char rgrant[WPE_PATH_RESOLVED_MAX];
 int i;
 for (i = 0; i < ngrants; i++) {
  if (!grants[i]) continue;
  if (wpe_path_resolve(grants[i], rgrant, sizeof rgrant) != WPE_PATH_RESOLVED) continue;
  if (!strcmp(rgrant, "/")) continue;
  if (path_is_under(rgrant, rpath)) return 1;
 }
 return 0;
}

/* The names directly under the workspace whose content another program runs
   or obeys: git's own folder (hooks, and a config whose fsmonitor, sshCommand
   or filter drivers run commands), the editor's start-folder config, the
   instructions the assistant folds into its prompt, the instructions the
   hosted engines read at a project's top (CLAUDE.md, QWEN.md), and their
   project settings (hooks, MCP servers, helper commands). */
static const char *const path_protected_names[] = {
 ".git", "xwperc", ".xwpe-ai.md", "AGENTS.md", "CLAUDE.md", "QWEN.md", ".claude", ".mcp.json",
 ".qwen",
 /* What makes git or a commit framework run a program: husky's hook
    folder, pre-commit's and lefthook's configs, the submodules git fetches
    and the attributes that pick a filter driver. */
 ".husky", ".pre-commit-config.yaml", "lefthook.yml", "lefthook.yaml", ".lefthook.yml",
 ".lefthook.yaml", "lefthook-local.yml", "lefthook-local.yaml", ".gitmodules", NULL
};

/* git reads an attributes file in every folder, and one can send a file
   through a filter driver, so it is protected at any depth. */
#define PATH_GITATTRIBUTES_NAME ".gitattributes"

/* The git config key that moves the hooks out of .git, and where it is. */
#define PATH_GIT_CONFIG_NAME     "config"
#define PATH_GIT_COMMONDIR_NAME  "commondir"
#define PATH_GIT_CORE_SECTION    "core"
#define PATH_GIT_HOOKS_PATH_KEY  "hookspath"

/* git's folder: protected at any depth, since a repository nested in the
   workspace runs its own hooks and config as soon as git is used there. */
#define PATH_GIT_NAME ".git"

/* How a .git file (a worktree, a submodule) names the real git folder. */
#define PATH_GITDIR_PREFIX "gitdir:"

/**
 * path_component_matches - Does one path component spell @name?
 * @comp/@len: the component, not terminated; @name: the name.
 *
 * The match ignores case, so a ".GIT" or "Xwperc" on a filesystem that folds
 * case (macOS, Windows shares) is the same file to this check as to the
 * program that reads it.
 * Return: 1 when it matches.
 */
static int path_component_matches(const char *comp, size_t len, const char *name)
{
 return strlen(name) == len && strncasecmp(comp, name, len) == 0;
}

/**
 * path_first_component - The first component of a path relative to a folder.
 * @rel: the relative text; @len: receives the component's length.
 * Return: where the component starts.
 */
static const char *path_first_component(const char *rel, size_t *len)
{
 const char *end = strchr(rel, '/');
 if (end) *len = (size_t)(end - rel);
 else *len = strlen(rel);
 return rel;
}

/**
 * path_names_a_protected_top - Is the first component one of the protected names?
 * @rel: the path relative to the workspace.
 * Return: 1 when the file is, or lies below, one of path_protected_names.
 */
static int path_names_a_protected_top(const char *rel)
{
 size_t len;
 const char *comp = path_first_component(rel, &len);
 int i;
 for (i = 0; path_protected_names[i]; i++)
  if (path_component_matches(comp, len, path_protected_names[i])) return 1;
 return 0;
}

/**
 * path_has_component - Does any component of the path spell @name?
 * @rel: the path relative to the workspace; @name: the name.
 * Return: 1 when some component is @name.
 */
static int path_has_component(const char *rel, const char *name)
{
 const char *p = rel, *end;
 size_t len;
 while (*p) {
  end = strchr(p, '/');
  if (end) len = (size_t)(end - p);
  else len = strlen(p);
  if (path_component_matches(p, len, name)) return 1;
  if (!end) break;
  p = end + 1;
 }
 return 0;
}

/**
 * path_passes_a_git_folder - Does any component of the path name git's folder?
 * @rel: the path relative to the workspace.
 * Return: 1 when some component is ".git".
 */
static int path_passes_a_git_folder(const char *rel)
{
 return path_has_component(rel, PATH_GIT_NAME);
}

/**
 * path_trim_line_end - Cut the newline and spaces that end a line read from a file.
 * @s: the line, changed in place.
 */
static void path_trim_line_end(char *s)
{
 size_t n = strlen(s);
 while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
  s[--n] = '\0';
}

/**
 * path_open_git_file - Open a file a cloned folder keeps for git, to read its lines.
 * @file: the name; it leads into a folder the user opened, so another party chose it.
 *
 * What the user gains: a pipe, a device or a link put at ".git", "commondir"
 * or the git config by a cloned repository is refused by what it is and never
 * waited for or followed, so reading it cannot stop the editor.  The file is
 * read whole to 1 MiB at most and refused over it, then handed out as a
 * stream, so the callers keep their line loops.
 * Return: the stream (close it with fclose), or NULL when the file is refused.
 */
static FILE *path_open_git_file(const char *file)
{
 WpeReadAsk ask = WPE_READ_ASK_FOLDER_CONFIG(PATH_READ_WHAT);
 WpeReadOutcome o;
 return wpe_read_stream_open(file, &ask, &o);
}

/**
 * path_gitdir_of_workspace - Where a .git file at the workspace's top sends git.
 * @rroot: the resolved workspace; @out/@sz: receive the resolved git folder.
 *
 * A worktree or a submodule has a ".git" file holding "gitdir: <folder>"
 * instead of the folder itself; the hooks and the config git runs are there.
 * Return: 1 when the workspace has such a file and the folder it names could
 * be followed, else 0.
 */
static int path_gitdir_of_workspace(const char *rroot, char *out, size_t sz)
{
 char file[WPE_PATH_RESOLVED_MAX], line[WPE_GITDIR_LINE_MAX], target[WPE_PATH_RESOLVED_MAX];
 const char *named;
 FILE *fp;
 if (path_join(file, sizeof file, rroot, PATH_GIT_NAME) < 0) return 0;
 fp = path_open_git_file(file);
 if (!fp) return 0;
 line[0] = '\0';
 if (!fgets(line, sizeof line, fp)) line[0] = '\0';
 fclose(fp);
 if (strncmp(line, PATH_GITDIR_PREFIX, strlen(PATH_GITDIR_PREFIX)) != 0) return 0;
 path_trim_line_end(line);
 named = line + strlen(PATH_GITDIR_PREFIX);
 while (*named == ' ' || *named == '\t') named++;
 if (!*named) return 0;
 if (named[0] == '/') snprintf(target, sizeof target, "%s", named);
 else if (path_join(target, sizeof target, rroot, named) < 0) return 0;
 return wpe_path_resolve(target, out, sz) == WPE_PATH_RESOLVED;
}

/**
 * path_git_folder - The git folder of a repository whose top is the workspace.
 * @rroot: the resolved workspace; @out/@sz: receive the resolved git folder.
 * Return: 1 when the workspace has a .git folder, or a .git file that leads
 * to one, else 0.
 */
static int path_git_folder(const char *rroot, char *out, size_t sz)
{
 char dir[WPE_PATH_RESOLVED_MAX];
 struct stat st;
 if (path_join(dir, sizeof dir, rroot, PATH_GIT_NAME) < 0) return 0;
 if (lstat(dir, &st) == 0 && S_ISDIR(st.st_mode))
  return wpe_path_resolve(dir, out, sz) == WPE_PATH_RESOLVED;
 return path_gitdir_of_workspace(rroot, out, sz);
}

/**
 * path_trim - Cut the spaces around a text, in place.
 * Return: where the text now starts.
 */
static char *path_trim(char *s)
{
 while (*s == ' ' || *s == '\t') s++;
 path_trim_line_end(s);
 return s;
}

/**
 * path_config_value - The value of a "key = value" line of a git config.
 * @line: the line, changed in place.
 *
 * A value in double quotes loses them; a ';' or '#' outside quotes starts
 * a comment, as git reads it.
 * Return: the value.
 */
static char *path_config_value(char *line)
{
 char *v = path_trim(line), *r, *w;
 int quoted = 0;
 for (r = w = v; *r; r++) {
  if (*r == '"') { quoted = !quoted; continue; }
  if (!quoted && (*r == ';' || *r == '#')) break;
  *w++ = *r;
 }
 *w = '\0';
 return path_trim(v);
}

/**
 * path_config_section - The section a "[name]" or "[name "sub"]" line opens.
 * @line: the line; @out/@sz: receive the text between the brackets, so a
 * subsection ("core "x"", "core.x") is never taken for the section itself.
 * Return: 1 when the line is a section header.
 */
static int path_config_section(const char *line, char *out, size_t sz)
{
 const char *p = line, *end;
 size_t n;
 while (*p == ' ' || *p == '\t') p++;
 if (*p != '[') return 0;
 p++;
 for (end = p; *end && *end != ']'; end++)
  ;
 n = (size_t)(end - p);
 if (n >= sz) n = sz - 1;
 memcpy(out, p, n);
 out[n] = '\0';
 return 1;
}

/**
 * path_config_hooks_path - The core.hooksPath one git config file sets.
 * @file: the config file; @out/@sz: receive the value as written (the
 * last one wins, as for git); left as it is when the file sets none.
 *
 * The file is only read: no git runs, so no hook and no config command can
 * run while the editor looks.
 */
static void path_config_hooks_path(const char *file, char *out, size_t sz)
{
 char line[WPE_GITDIR_LINE_MAX], section[WPE_GITDIR_LINE_MAX] = "", *eq, *key;
 FILE *fp = path_open_git_file(file);
 if (!fp) return;
 while (fgets(line, sizeof line, fp)) {
  if (path_config_section(line, section, sizeof section)) continue;
  if (strcasecmp(section, PATH_GIT_CORE_SECTION) != 0) continue;
  eq = strchr(line, '=');
  if (!eq) continue;
  *eq = '\0';
  key = path_trim(line);
  if (strcasecmp(key, PATH_GIT_HOOKS_PATH_KEY) != 0) continue;
  snprintf(out, sz, "%s", path_config_value(eq + 1));
 }
 fclose(fp);
}

/**
 * path_git_common_folder - The folder a worktree's git folder shares its config with.
 * @gitdir: the resolved git folder; @out/@sz: receive the common folder.
 * Return: 1 when @gitdir has a "commondir" file that could be followed.
 */
static int path_git_common_folder(const char *gitdir, char *out, size_t sz)
{
 char file[WPE_PATH_RESOLVED_MAX], line[WPE_GITDIR_LINE_MAX], target[WPE_PATH_RESOLVED_MAX];
 const char *named;
 FILE *fp;
 if (path_join(file, sizeof file, gitdir, PATH_GIT_COMMONDIR_NAME) < 0) return 0;
 fp = path_open_git_file(file);
 if (!fp) return 0;
 line[0] = '\0';
 if (!fgets(line, sizeof line, fp)) line[0] = '\0';
 fclose(fp);
 named = path_trim(line);
 if (!*named) return 0;
 if (named[0] == '/') snprintf(target, sizeof target, "%s", named);
 else if (path_join(target, sizeof target, gitdir, named) < 0) return 0;
 return wpe_path_resolve(target, out, sz) == WPE_PATH_RESOLVED;
}

/**
 * path_hooks_folder - The folder git runs the workspace repository's hooks from
 * when core.hooksPath moves them out of .git (husky's .husky, lefthook).
 * @rroot: the resolved workspace; @gitdir: its resolved git folder;
 * @out/@sz: receive the resolved folder.
 *
 * The worktree's own config wins over the shared one, as for git.  A
 * relative value is taken from the workspace's top, "~/" from $HOME.
 * Return: 1 when a hooks folder is named and could be followed.
 */
static int path_hooks_folder(const char *rroot, const char *gitdir, char *out, size_t sz)
{
 char file[WPE_PATH_RESOLVED_MAX], common[WPE_PATH_RESOLVED_MAX];
 char value[WPE_GITDIR_LINE_MAX] = "", target[WPE_PATH_RESOLVED_MAX];
 char home[WPE_PATH_RESOLVED_MAX];
 int have_home = wpe_user_home(home, sizeof home) == 0;
 if (path_git_common_folder(gitdir, common, sizeof common) &&
     path_join(file, sizeof file, common, PATH_GIT_CONFIG_NAME) == 0)
  path_config_hooks_path(file, value, sizeof value);
 if (path_join(file, sizeof file, gitdir, PATH_GIT_CONFIG_NAME) == 0)
  path_config_hooks_path(file, value, sizeof value);
 if (!value[0]) return 0;
 if (value[0] == '/') snprintf(target, sizeof target, "%s", value);
 else if (value[0] == '~' && value[1] == '/' && have_home) {
  if (path_join(target, sizeof target, home, value + 2) < 0) return 0;
 } else if (path_join(target, sizeof target, rroot, value) < 0) return 0;
 return wpe_path_resolve(target, out, sz) == WPE_PATH_RESOLVED;
}

/**
 * path_in_git_machinery - Does a write land where git finds what it runs?
 * @rroot: the resolved workspace; @rpath: the resolved file.
 * Return: 1 for the folder a top-level .git file names, or the folder
 * core.hooksPath names.
 */
static int path_in_git_machinery(const char *rroot, const char *rpath)
{
 char gitdir[WPE_PATH_RESOLVED_MAX], hooks[WPE_PATH_RESOLVED_MAX];
 if (!path_git_folder(rroot, gitdir, sizeof gitdir)) return 0;
 if (path_is_under(gitdir, rpath)) return 1;
 if (path_hooks_folder(rroot, gitdir, hooks, sizeof hooks) && strlen(hooks) > 1 &&
     path_is_under(hooks, rpath))
  return 1;
 return 0;
}

/**
 * wpe_path_is_protected - Is a place under the workspace one whose content runs code?
 * @rroot: the workspace, resolved; @rpath: the file, resolved.
 *
 * What the user gains: an assistant allowed to edit the project freely
 * still cannot, without a question, plant a git hook, a git config that runs
 * a command on the next "git status", a start-folder xwperc that names a
 * program, new instructions for itself, or hosted-engine settings that run
 * a helper on the next session.  Those files are inside the workspace, so
 * the plain inside/outside answer let them through under edits and auto;
 * each such write now asks, and says why.
 * Return: 1 when @rpath is one of path_protected_names at the workspace's
 * top or below one, has a ".git" or ".gitattributes" component at any
 * depth, lies in the folder a top-level .git file names, or in the folder
 * the repository's core.hooksPath names (read from its config file, no git
 * run); 0 otherwise, and for a path that is not under @rroot at all.
 */
int wpe_path_is_protected(const char *rroot, const char *rpath)
{
 const char *rel;
 size_t n;
 if (!rroot || !rpath || !path_is_under(rroot, rpath)) return 0;
 n = strlen(rroot);
 if (n <= 1) n = 0;
 rel = rpath + n;
 while (*rel == '/') rel++;
 if (!*rel) return 0;                        /* the workspace folder itself */
 if (path_names_a_protected_top(rel)) return 1;
 if (path_passes_a_git_folder(rel)) return 1;
 if (path_has_component(rel, PATH_GITATTRIBUTES_NAME)) return 1;
 return path_in_git_machinery(rroot, rpath);
}

/**
 * wpe_path_lies_in - Is a resolved path a folder, or below it, once the folder is followed?
 * @dir: the folder, absolute, as written; @rpath: a path wpe_path_resolve gave.
 *
 * Lets a caller protect a folder it knows by name (the user's own config
 * folder) with the same link-following comparison the scope uses, so a link
 * to that folder is caught like the folder itself.
 * Return: 1 when @rpath lies in @dir; 0 otherwise, and for a folder that
 * cannot be followed or is the root "/".
 */
int wpe_path_lies_in(const char *dir, const char *rpath)
{
 char rdir[WPE_PATH_RESOLVED_MAX];
 if (!dir || !rpath || wpe_path_resolve(dir, rdir, sizeof rdir) != WPE_PATH_RESOLVED) return 0;
 if (strlen(rdir) <= 1) return 0;
 return path_is_under(rdir, rpath);
}

/**
 * wpe_scope_classify - Where does this path land: in the workspace, in a grant, or beyond?
 * @root: the workspace's directory, absolute; @grants/@ngrants: directories the
 * user allowed the assistant to read, absolute; @path: the file, absolute, as
 * the model named it; @access: WPE_SCOPE_READ or WPE_SCOPE_WRITE;
 * @real/@sz: receive where the file really lands, empty when it could not be
 * told; may be NULL.
 *
 * What the user gains: one answer for every tool, native or hosted.  A link
 * inside the project that leads elsewhere no longer hides a write or a read.
 * Both sides are followed (a project reached through a linked home directory
 * still compares like with like), and a path that cannot be followed, a loop,
 * counts as outside: the safe answer is to ask.  A grant widens reads only: a
 * write beyond the workspace stays outside whatever was granted.  A write
 * inside it to a file that changes how git, the editor or the engine runs
 * code (wpe_path_is_protected) is told apart, so it can ask at every dial;
 * a read of the same file stays an ordinary read inside.
 *
 * Return: WPE_SCOPE_INSIDE, WPE_SCOPE_GRANTED, WPE_SCOPE_OUTSIDE or, for a
 * write only, WPE_SCOPE_PROTECTED.  A path is inside when it cannot be told
 * (no root, a root that cannot be followed, or the root "/", below which
 * nothing is outside).
 */
int wpe_scope_classify(const char *root, const char *const *grants, int ngrants,
                       const char *path, int access, char *real, size_t sz)
{
 char rroot[WPE_PATH_RESOLVED_MAX], rpath[WPE_PATH_RESOLVED_MAX];
 if (real && sz) real[0] = '\0';
 if (!root || !path) return WPE_SCOPE_INSIDE;
 if (wpe_path_resolve(root, rroot, sizeof rroot) != WPE_PATH_RESOLVED) return WPE_SCOPE_INSIDE;
 if (strlen(rroot) <= 1) return WPE_SCOPE_INSIDE;
 if (wpe_path_resolve(path, rpath, sizeof rpath) != WPE_PATH_RESOLVED) return WPE_SCOPE_OUTSIDE;
 if (real && strlen(rpath) < sz) memcpy(real, rpath, strlen(rpath) + 1);
 if (access == WPE_SCOPE_WRITE && wpe_path_is_protected(rroot, rpath)) return WPE_SCOPE_PROTECTED;
 if (path_is_under(rroot, rpath)) return WPE_SCOPE_INSIDE;
 if (access == WPE_SCOPE_READ && path_under_a_grant(grants, ngrants, rpath))
  return WPE_SCOPE_GRANTED;
 return WPE_SCOPE_OUTSIDE;
}

/**
 * wpe_path_lands_outside - Will writing this path put bytes outside the root?
 * @root: the workspace's directory, absolute; @path: the file, absolute, as
 * the model named it; @real/@sz: as for wpe_scope_classify.
 *
 * The write side of wpe_scope_classify, for callers that have no grants.
 * Return: 1 when the file lands outside, 0 when inside or when it cannot be told.
 */
int wpe_path_lands_outside(const char *root, const char *path, char *real, size_t sz)
{
 int where = wpe_scope_classify(root, NULL, 0, path, WPE_SCOPE_WRITE, real, sz);
 return where == WPE_SCOPE_OUTSIDE;
}

/**
 * path_make_absolute - Join a path to the working directory when it is relative.
 * @path: the path, as the model named it; @abs/@sz: receives the absolute path.
 * Return: 0, or -1 with errno set (ENOENT for an empty name, ENAMETOOLONG).
 */
static int path_make_absolute(const char *path, char *abs, size_t sz)
{
 char cwd[WPE_PATH_RESOLVED_MAX];
 if (!path || !path[0]) { errno = ENOENT; return -1; }
 if (path[0] == '/') {
  if (wpe_fmt_whole(abs, sz, "%s", path) != WPE_FMT_OK) { errno = ENAMETOOLONG; return -1; }
  return 0;
 }
 if (!getcwd(cwd, sizeof cwd)) return -1;
 if (wpe_fmt_whole(abs, sz, "%s/%s", cwd, path) != WPE_FMT_OK) { errno = ENAMETOOLONG; return -1; }
 return 0;
}

/**
 * path_look_leaf - Fill a look from one look at the name of a resolved place.
 * @real: the place; @x: receives the look.
 * Return: 0, or -1 with errno set for an error other than "nothing there".
 *
 * The name is looked at once, without following a link: the record the write
 * owner later judges the opened file against, the kind that says whether it
 * is a regular file and the number of names the question about links asks
 * about all come from that one look, so no second look can find another file
 * under the name.
 */
static int path_look_leaf(const char *real, WpeWriteExpect *x)
{
 if (wpe_seen_look_name(real, 0, &x->seen) != 0) {
  if (errno != ENOENT) return -1;
  x->judged = 1;
  return 0;
 }
 x->judged = 1;
 x->exists = 1;
 x->nlink = wpe_seen_nlink(&x->seen);
 x->regular = wpe_seen_kind(&x->seen) == S_IFREG ? 1 : 0;
 return 0;
}

/**
 * wpe_path_look_write - Look at the place a write will land, once, before the question.
 * @path: the file, as the model named it (relative to the working directory
 * or absolute); @real/@sz: receives the place it really leads to; @x: receives
 * what is there.
 *
 * What the user gains: the write goes where the scope check looked and into
 * the file it looked at.  The path is followed here, once, exactly as the
 * check follows it, and the place it leads to is written under that very
 * name, never resolved again.  What is there (no file, or which file by
 * device and inode, how many names it has, whether it is a regular file) is
 * kept in @x, so the write owner refuses a file swapped in, or given another
 * name, while the user was being asked.  The last place itself is looked at
 * with lstat: a link there is not followed, and the owner refuses it.
 *
 * Return: 0 with @real and @x filled (x->judged is 1), or -1 with errno set
 * (ELOOP for a path that cannot be followed, or the error of the look).
 */
int wpe_path_look_write(const char *path, char *real, size_t sz, WpeWriteExpect *x)
{
 char abs[WPE_PATH_RESOLVED_MAX];
 memset(x, 0, sizeof *x);
 if (sz) real[0] = '\0';
 if (path_make_absolute(path, abs, sizeof abs) != 0) return -1;
 if (wpe_path_resolve(abs, real, sz) != WPE_PATH_RESOLVED) { errno = ELOOP; return -1; }
 return path_look_leaf(real, x);
}

/**
 * wpe_path_resolve_for_read - The place a path a model named leads to, for the read owner.
 * @path: the path as the model wrote it, absolute or relative to the working
 * directory; @real/@sz: receive the resolved place.
 *
 * What the user gains: a model that names a link inside the workspace reads
 * the file the scope check judged, exactly as that check followed it, and the
 * read owner then opens that place without following a link, so a link put
 * there afterwards is refused and named instead of followed.
 * Return: 0 with @real filled, or -1 with errno set (ELOOP for a path that
 * cannot be followed).
 */
int wpe_path_resolve_for_read(const char *path, char *real, size_t sz)
{
 char abs[WPE_PATH_RESOLVED_MAX];
 if (sz) real[0] = '\0';
 if (path_make_absolute(path, abs, sizeof abs) != 0) return -1;
 if (wpe_path_resolve(abs, real, sz) != WPE_PATH_RESOLVED) { errno = ELOOP; return -1; }
 return 0;
}

/**
 * wpe_path_read_judged - Read a file whole, judged against a look taken just before.
 * @path: the file, already resolved; @maxlen: the most bytes taken; @over:
 * what a longer file gets (WPE_READ_CUT keeps the first @maxlen bytes,
 * WPE_READ_WHOLE_OR_REFUSE refuses the file); @len: receives the byte count,
 * or NULL; @o: receives why nothing was read.
 *
 * What the user gains: a before image that holds a NUL byte is read in full
 * and can be put back in full, because the length travels with the text; a
 * pipe, a socket, a terminal, a folder or a link under the name is refused by
 * what it is, and never waited for or read.  The name is looked at first and
 * the file opened is judged against that look, so a file that took the name
 * in between is not read.  The text is NUL-terminated for callers that treat
 * it as a string.
 * Return: the malloc'd bytes, or NULL with @o saying why.
 */
char *wpe_path_read_judged(const char *path, size_t maxlen, WpeReadOver over,
                           size_t *len, WpeReadOutcome *o)
{
 WpeSeen named;
 WpeReadAsk ask;
 size_t got = 0;
 char *text;
 if (len) *len = 0;
 if (wpe_seen_look_name(path, 0, &named) != 0) {
  wpe_read_outcome_errno(o, errno);
  return NULL;
 }
 ask = WPE_READ_ASK_JUDGED(PATH_READ_WHAT, maxlen, over, &named);
 text = wpe_read_text(path, &ask, &got, o);
 if (text && len) *len = got;
 return text;
}

/**
 * wpe_path_read_inside - Read a file a folder names, following a link only into the workspace.
 * @root: the workspace folder; @name: the file, as the folder names it
 * (absolute or relative to the working directory); @maxlen: the most bytes
 * taken (a longer file is cut); @len: receives the byte count, or NULL; @o:
 * receives why nothing was read; @how: WPE_PATH_INSIDE_READ, or why a link
 * was refused (its target lies outside @root, or it leads to a file that is
 * not there), so a caller can name it instead of treating it as missing.
 *
 * What the user gains: an instructions file kept as a link to another file of
 * the project (AGENTS.md pointing at CLAUDE.md) is read, while a cloned
 * folder cannot point it at a file of the user's elsewhere, at a pipe or at a
 * device.  A name that is not a link is read as it is.  A link is followed
 * once, the whole chain, and the place it leads to must be inside @root and
 * is then opened without following another link, as a regular file only.
 * (A folder on the way swapped after the resolve is the walk's concern, not
 * this function's.)
 * Return: the malloc'd text, NULL with @o saying why.
 */
char *wpe_path_read_inside(const char *root, const char *name, size_t maxlen,
                           size_t *len, WpeReadOutcome *o, WpePathInside *how)
{
 WpeSeen named;
 char real[WPE_PATH_RESOLVED_MAX], *text;
 *how = WPE_PATH_INSIDE_READ;
 if (len) *len = 0;
 if (wpe_seen_look_name(name, 0, &named) != 0) {
  wpe_read_outcome_errno(o, errno);
  return NULL;
 }
 if (wpe_seen_kind(&named) != S_IFLNK)
  return wpe_path_read_judged(name, maxlen, WPE_READ_CUT, len, o);
 if (wpe_path_resolve_for_read(name, real, sizeof real) != 0) {
  wpe_read_outcome_errno(o, ELOOP);
  return NULL;
 }
 if (!wpe_path_lies_in(root, real)) {
  *how = WPE_PATH_INSIDE_OUTSIDE;
  memset(o, 0, sizeof *o);
  o->why = WPE_READ_IS_A_LINK;
  return NULL;
 }
 text = wpe_path_read_judged(real, maxlen, WPE_READ_CUT, len, o);
 if (!text && wpe_read_outcome_missing(o))
  *how = WPE_PATH_INSIDE_DANGLING;
 return text;
}

/**
 * path_folder_part - The folder part of an absolute path, whole.
 * @dir/@n: receives the bytes of @abs before @slash (the root when @slash is
 * its first byte); @abs: the path; @slash: its last '/'.
 *
 * An exact slice of a path that is already held, not a sentence: the byte
 * count is kept as it is and a folder that does not fit is refused.
 * Return: 0, or -1 when it does not fit.
 */
static int path_folder_part(char *dir, size_t n, const char *abs, const char *slash)
{
 WpeFmtText t;
 wpe_fmt_begin_whole(&t, dir, n);
 if (slash == abs)
  wpe_fmt_add(&t, "/");
 else
  wpe_fmt_add_bytes(&t, abs, (size_t)(slash - abs));
 if (wpe_fmt_end(&t, NULL) != WPE_FMT_OK) return -1;
 return 0;
}

/**
 * wpe_path_look_name - Look at the reviewed NAME itself, never through a link at its end.
 * @path: the file, as the review names it (absolute or relative to the working
 * directory); @real/@sz: receives the resolved folder and the name; @x: receives
 * what is at the name.
 *
 * What the user gains: a revert puts text back into the file the review listed
 * and nowhere else.  Only the folder is followed; the last part is looked at
 * with lstat, so a link planted at the name (or one that was there and now
 * leads elsewhere) is seen as the link it is, and the write owner refuses it
 * instead of writing into whatever it leads to.
 *
 * Return: 0 with @real and @x filled (x->judged is 1), or -1 with errno set.
 */
int wpe_path_look_name(const char *path, char *real, size_t sz, WpeWriteExpect *x)
{
 char abs[WPE_PATH_RESOLVED_MAX], dir[WPE_PATH_RESOLVED_MAX], rdir[WPE_PATH_RESOLVED_MAX];
 const char *slash, *leaf;
 memset(x, 0, sizeof *x);
 if (sz) real[0] = '\0';
 if (path_make_absolute(path, abs, sizeof abs) != 0) return -1;
 slash = strrchr(abs, '/');
 leaf = slash + 1;
 if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..")) { errno = EINVAL; return -1; }
 if (path_folder_part(dir, sizeof dir, abs, slash) != 0) { errno = ENAMETOOLONG; return -1; }
 if (wpe_path_resolve(dir, rdir, sizeof rdir) != WPE_PATH_RESOLVED) { errno = ELOOP; return -1; }
 if (wpe_fmt_whole(real, sz, "%s%s%s", rdir, strcmp(rdir, "/") ? "/" : "", leaf) != WPE_FMT_OK) { errno = ENAMETOOLONG; return -1; }
 return path_look_leaf(real, x);
}

/* The state of one look for links that lead out of a directory. */
typedef struct {
 char rstart[WPE_PATH_RESOLVED_MAX];  /* the resolved directory the walk began in */
 int  max_depth;                      /* levels to enter; negative for no bound   */
 int  visited;                        /* entries seen so far                      */
 int  outward;                        /* links found that lead out                */
 int  truncated;                      /* the entry bound was reached              */
} wpe_path_scan;

static void path_scan_dir(wpe_path_scan *sc, const char *dir, int depth);

/**
 * path_scan_entry - Judge one entry of a directory: a link out, a directory to enter, or nothing.
 * @sc: the scan; @path: the entry's full path; @kind: what the entry is, as
 * the folder's own descriptor said (S_IFMT bits); @depth: the level of its directory.
 *
 * A link is never entered: it is counted when it leads outside the start and
 * left alone either way, which is what keeps a walk from reading beyond the
 * tree through it.
 */
static void path_scan_entry(wpe_path_scan *sc, const char *path, mode_t kind, int depth)
{
 char real[WPE_PATH_RESOLVED_MAX];
 if (kind == S_IFLNK) {
  if (wpe_path_resolve(path, real, sizeof real) != WPE_PATH_RESOLVED ||
      !path_is_under(sc->rstart, real))
   sc->outward++;
  return;
 }
 if (kind == S_IFDIR && (sc->max_depth < 0 || depth + 1 < sc->max_depth))
  path_scan_dir(sc, path, depth + 1);
}

/**
 * path_scan_dir - Visit the entries of one directory, within the scan's bound.
 * @sc: the scan; @dir: the directory; @depth: its level below the start.
 *
 * The folder is opened by the read owner and each entry is looked at through
 * the folder, so a folder replaced by a link while the scan runs is not
 * followed.
 */
static void path_scan_dir(wpe_path_scan *sc, const char *dir, int depth)
{
 WpeReadAsk ask = WPE_READ_ASK_FOLDER_ENTRY(PATH_READ_WHAT);
 WpeReadOutcome o;
 DIR *d = wpe_read_folder(dir, &ask, &o);
 struct dirent *de;
 char path[WPE_PATH_RESOLVED_MAX];
 mode_t kind;
 if (!d) return;
 while ((de = readdir(d)) != NULL) {
  if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
  if (sc->visited >= WPE_SCAN_ENTRY_MAX) { sc->truncated = 1; break; }
  sc->visited++;
  if (WPE_FMT_WHOLE(path, "%s/%s", dir, de->d_name) != WPE_FMT_OK) continue;
  kind = wpe_read_entry_kind(d, de->d_name);
  if (kind != 0) path_scan_entry(sc, path, kind, depth);
 }
 closedir(d);
}

/**
 * wpe_path_count_outward_links - How many links below a directory lead out of it?
 * @start: the directory a search starts in, absolute; @max_depth: how many
 * levels to enter, 1 for the directory's own entries, negative for all;
 * @truncated: set to 1 when the scan stopped at WPE_SCAN_ENTRY_MAX entries,
 * else 0; may be NULL.
 *
 * What the user gains: a search that did not follow such a link says so, so
 * an empty result is not read as "nothing there".  Links are looked at, never
 * entered, so the count itself reads nothing beyond the tree.
 *
 * Return: the number of links whose target is outside @start or cannot be
 * followed.
 */
int wpe_path_count_outward_links(const char *start, int max_depth, int *truncated)
{
 wpe_path_scan sc;
 memset(&sc, 0, sizeof sc);
 if (truncated) *truncated = 0;
 if (!start || wpe_path_resolve(start, sc.rstart, sizeof sc.rstart) != WPE_PATH_RESOLVED)
  return 0;
 sc.max_depth = max_depth;
 path_scan_dir(&sc, sc.rstart, 0);
 if (truncated) *truncated = sc.truncated;
 return sc.outward;
}
