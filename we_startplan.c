/* we_startplan.c -- what the editor decided about a program, before it starts.
 *
 * See we_startplan.h.  The owner of one object, the start plan, and of the
 * rules that judge it: a folder value is never an option, every path is
 * resolved with its links followed, a removal outside the folder of the
 * file that supplied it is never done, a write or a read outside it asks,
 * a plan with nothing a folder chose runs without a question, and any other
 * plan runs only when it is the user's own in effect or he said yes to that
 * very plan in that folder.  The children of a planned action are started
 * here and nowhere else.
 */
#include "we_startplan.h"
#include "we_startcmd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What a path joined from a value is given in front when it would start
   with '-' (an option) or '@' (a response file of options). */
#define STARTPLAN_OPERAND_PREFIX "./"

/* The folder separator, and the root folder that is never a root. */
#define STARTPLAN_SEP       '/'
#define STARTPLAN_SEP_TEXT  "/"

/* Growth of the plan's arrays. */
#define STARTPLAN_GROW 16

/* Room for one trace line. */
#define STARTPLAN_TRACE_MAX 1024

/* How many links to a missing target a path may pass through before it
   counts as one that never ends; past it the path is judged outside. */
#define STARTPLAN_LINKS_MAX 20

/* The environment names a folder's value may expand: where the user is,
   never what he holds. */
static const char *const startplan_env_allowed[] = {
 "HOME", "USER", "LOGNAME", "PWD", "TMPDIR", NULL
};

/* Endings of names that hold a secret: never expanded into a folder's value,
   whatever the list above says. */
static const char *const startplan_env_secret_end[] = {
 "_KEY", "_TOKEN", "_SECRET", "_PASSWORD", NULL
};

static void (*startplan_trace_hook)(const char *line);

/**
 * wpe_startplan_trace_set - Where the owner's trace lines go.
 * @trace: the editor's trace, or NULL (the C tests).
 */
void wpe_startplan_trace_set(void (*trace)(const char *line))
{
 startplan_trace_hook = trace;
}

/** startplan_trace - Write one trace line, when the editor traces. */
static void startplan_trace(const char *fmt, ...)
{
 char line[STARTPLAN_TRACE_MAX];
 va_list ap;

 if (!startplan_trace_hook)
  return;
 va_start(ap, fmt);
 vsnprintf(line, sizeof line, fmt, ap);
 va_end(ap);
 startplan_trace_hook(line);
}

/** startplan_or_empty - The text, or "" for none. */
static const char *startplan_or_empty(const char *s)
{
 if (!s)
  return "";
 return s;
}

/** startplan_dup - A malloc'd copy of a text (NULL stays NULL). */
static char *startplan_dup(const char *s)
{
 size_t n;
 char *p;

 if (!s)
  return NULL;
 n = strlen(s) + 1;
 p = malloc(n);
 if (p)
  memcpy(p, s, n);
 return p;
}

/**
 * startplan_grow - Make room for one more element of an array.
 * @arr: the array; @n: its length; @size: one element's size.
 * Return: 0, or -1 out of memory or past WPE_STARTPLAN_WORDS_MAX.
 */
static int startplan_grow(void **arr, int n, size_t size)
{
 void *grown;

 if (n >= WPE_STARTPLAN_WORDS_MAX)
  return -1;
 if (n % STARTPLAN_GROW)
  return 0;
 grown = realloc(*arr, (size_t)(n + STARTPLAN_GROW) * size);
 if (!grown)
  return -1;
 *arr = grown;
 return 0;
}

/**
 * startplan_debug_kind_name - The name of a debug session's kind of start.
 * @kind: WPE_START_*.
 * Return: "debug", "debuggee", "debug-compile", "setup-ide", "bsp",
 * "adapter", or "start" for a kind this owner does not know.
 */
static const char *startplan_debug_kind_name(int kind)
{
 if (kind == WPE_START_DEBUGGER)
  return "debug";
 if (kind == WPE_START_DEBUGGEE)
  return "debuggee";
 if (kind == WPE_START_DEBUG_COMPILE)
  return "debug-compile";
 if (kind == WPE_START_SETUP_IDE)
  return "setup-ide";
 if (kind == WPE_START_BUILD_SERVER)
  return "bsp";
 if (kind == WPE_START_DEBUG_ADAPTER)
  return "adapter";
 return "start";
}

/**
 * startplan_user_kind_name - The name of a start the user asks for by a key
 * of the Run menu, File > Shell or opening a file.
 * @kind: WPE_START_*.
 * Return: "run", "script", "make", "shell", "install", "lsp", or the
 * debug session's name for any other kind.
 */
static const char *startplan_user_kind_name(int kind)
{
 if (kind == WPE_START_RUN)
  return "run";
 if (kind == WPE_START_SCRIPT)
  return "script";
 if (kind == WPE_START_MAKE)
  return "make";
 if (kind == WPE_START_SHELL)
  return "shell";
 if (kind == WPE_START_INSTALL)
  return "install";
 if (kind == WPE_START_LANGUAGE_SERVER)
  return "lsp";
 return startplan_debug_kind_name(kind);
}

/**
 * wpe_startplan_kind_name - The name of a kind, as the store's key names it.
 * Return: "build", "project", ..., or "start" for a kind this owner does
 * not know.
 */
const char *wpe_startplan_kind_name(int kind)
{
 if (kind == WPE_START_BUILD)
  return "build";
 if (kind == WPE_START_PROJECT)
  return "project";
 if (kind == WPE_START_PRINT)
  return "print";
 if (kind == WPE_START_MAN)
  return "man";
 if (kind == WPE_START_UNPACK)
  return "unpack";
 if (kind == WPE_START_PROBE)
  return "probe";
 return startplan_user_kind_name(kind);
}

/**
 * wpe_startplan_new - Begin the plan of one user action.
 * @kind: WPE_START_*; @name: the key's second part (the language of a
 * build, the project file's name).
 *
 * One plan per key the user pressed: F9 on a file is one plan for its
 * compile and its link, F9 on a project one plan for every compile, the
 * library and the link.  So the user is asked once per action, never once
 * per file.
 * Return: the empty plan, or NULL out of memory.
 */
WpeStartPlan *wpe_startplan_new(int kind, const char *name)
{
 WpeStartPlan *p = calloc(1, sizeof *p);

 if (!p)
  return NULL;
 p->kind = kind;
 p->origin = WPE_CHILD_ORIGIN_USER;
 p->name = startplan_dup(startplan_or_empty(name));
 if (!p->name)
 {
  free(p);
  return NULL;
 }
 return p;
}

/**
 * wpe_startplan_free - Free a plan, and the user's own plan it holds.
 * @p: the plan, or NULL.
 */
void wpe_startplan_free(WpeStartPlan *p)
{
 int i;

 if (!p)
  return;
 for (i = 0; i < p->nword; i++)
 {
  free(p->word[i].text);
  free(p->word[i].source);
 }
 for (i = 0; i < p->npath; i++)
 {
  free(p->path[i].path);
  free(p->path[i].root);
  free(p->path[i].source);
  free(p->path[i].content);
 }
 for (i = 0; i < p->ninput; i++)
 {
  free(p->input[i].text);
  free(p->input[i].source);
 }
 free(p->input);
 for (i = 0; i < p->nvalue; i++)
 {
  free(p->value[i].file);
  free(p->value[i].key);
  free(p->value[i].raw);
  free(p->value[i].expanded);
 }
 free(p->word);
 free(p->path);
 free(p->value);
 free(p->name);
 free(p->file);
 free(p->folder);
 free(p->cwd);
 free(p->text);
 wpe_startplan_free(p->own);
 free(p);
}

/**
 * wpe_startplan_key - The store's key of a plan: "<kind>/<name>".
 * @p: the plan; @out/@sz: receive it.
 * Return: 0, or -1 when it does not fit.
 */
int wpe_startplan_key(const WpeStartPlan *p, char *out, size_t sz)
{
 int n = snprintf(out, sz, "%s/%s", wpe_startplan_kind_name(p->kind), p->name);

 if (n < 0 || (size_t)n >= sz)
  return -1;
 return 0;
}

/**
 * startplan_realpath_dup - A folder resolved, links followed, malloc'd.
 * Return: the resolved folder, or NULL when it cannot be followed.
 */
static char *startplan_realpath_dup(const char *dir)
{
 char real[PATH_MAX];

 if (!dir || !*dir || !realpath(dir, real))
  return NULL;
 return startplan_dup(real);
}

/**
 * wpe_startplan_set_file - Name the folder file that supplied the plan's values.
 * @p: the plan; @file: the file as the user knows it ("./xwperc", "demo.prj");
 * @folder: the folder that holds it.
 *
 * The answer the user gives is kept for that folder, and the box's title
 * names the file.
 * Return: 0, or -1 when the folder cannot be followed (the plan then asks
 * and remembers nothing).
 */
int wpe_startplan_set_file(WpeStartPlan *p, const char *file, const char *folder)
{
 free(p->file);
 free(p->folder);
 p->file = startplan_dup(file);
 p->folder = startplan_realpath_dup(folder);
 return p->folder ? 0 : -1;
}

/**
 * wpe_startplan_set_cwd - The folder the plan's children run in.
 * @p: the plan; @dir: the folder.
 * Return: 0, or -1 when it cannot be followed.
 */
int wpe_startplan_set_cwd(WpeStartPlan *p, const char *dir)
{
 free(p->cwd);
 p->cwd = startplan_realpath_dup(dir);
 return p->cwd ? 0 : -1;
}

/**
 * wpe_startplan_word - Add a word the plan's command lines may hold.
 * @p: the plan; @text: the word as execvp gets it; @role: WPE_WORD_*;
 * @from: WPE_FROM_*; @source: where it came from, for the box, or NULL.
 *
 * Every word a child of this action is started with must be one of these
 * (or one of its paths): wpe_startplan_start refuses any other.
 * Return: 0, or -1 out of memory or past the plan's size.
 */
int wpe_startplan_word(WpeStartPlan *p, const char *text, int role, int from,
                       const char *source)
{
 WpeStartWord *w;

 if (!text || startplan_grow((void **)&p->word, p->nword, sizeof *p->word) < 0)
  return -1;
 w = &p->word[p->nword];
 w->text = startplan_dup(text);
 w->source = startplan_dup(startplan_or_empty(source));
 w->role = role;
 w->from = from;
 if (!w->text || !w->source)
 {
  free(w->text);
  free(w->source);
  return -1;
 }
 p->nword++;
 return 0;
}

/**
 * wpe_startplan_words - Add the words of a value as the build splits it.
 * @p: the plan; @text: the value (an option string, a library list);
 * @role/@from/@source: as wpe_startplan_word, for every word.
 *
 * The value is cut at each single blank, as the build's own split does, so
 * the plan holds exactly the words the command line gets, an empty one
 * between two blanks included.
 * Return: 0, or -1 as wpe_startplan_word.
 */
int wpe_startplan_words(WpeStartPlan *p, const char *text, int role, int from,
                        const char *source)
{
 char *copy, *start, *blank;
 int rc = 0;

 if (!text || !*text)
  return 0;
 if (!(copy = startplan_dup(text)))
  return -1;
 start = copy;
 while (*start && rc == 0)
 {
  blank = strchr(start, ' ');
  if (blank)
   *blank = '\0';
  rc = wpe_startplan_word(p, start, role, from, source);
  if (!blank)
   break;
  start = blank + 1;
 }
 free(copy);
 return rc;
}

/**
 * wpe_startplan_path - Add a path the start writes, removes or is handed.
 * @p: the plan; @path: as the tool gets it, relative to the plan's working
 * folder or absolute; @role: WPE_PATH_*; @from: WPE_FROM_*; @source: where
 * it came from, for the box; @root: the folder of the file that supplied
 * the value, or NULL (a folder value is then judged against the plan's
 * file's folder, the user's own against the working folder).
 * Return: 0, or -1 out of memory or past the plan's size.
 */
int wpe_startplan_path(WpeStartPlan *p, const char *path, int role, int from,
                       const char *source, const char *root)
{
 WpeStartPath *s;

 if (!path || startplan_grow((void **)&p->path, p->npath, sizeof *p->path) < 0)
  return -1;
 s = &p->path[p->npath];
 memset(s, 0, sizeof *s);
 s->path = startplan_dup(path);
 s->source = startplan_dup(startplan_or_empty(source));
 s->root = NULL;
 if (root)
  s->root = startplan_realpath_dup(root);
 s->role = role;
 s->from = from;
 if (!s->path || !s->source)
 {
  free(s->path);
  free(s->source);
  free(s->root);
  return -1;
 }
 p->npath++;
 return 0;
}

/**
 * wpe_startplan_mark_output - Say what the last path added is to the build.
 * @p: the plan; @output: WPE_OUTPUT_*.
 *
 * A project kept in the home folder builds beside its own file only what
 * is plainly its own (its objects, its sources, a program or library it
 * made before); the sites say which paths those are.
 */
void wpe_startplan_mark_output(WpeStartPlan *p, int output)
{
 if (p && p->npath > 0)
  p->path[p->npath - 1].output = output;
}

/**
 * wpe_startplan_relabel - Correct whose a word of the plan is.
 * @p: the plan; @index: the word; @role/@from: what it is and whose.
 *
 * For a short command built with wpe_startplan_fixed whose words are not
 * all the editor's: the print command a folder's xwperc names is the
 * folder's, and the plan must say so.  The plan is judged again.
 * Return: 0, or -1 for a word the plan does not have.
 */
int wpe_startplan_relabel(WpeStartPlan *p, int index, int role, int from)
{
 if (!p || index < 0 || index >= p->nword)
  return -1;
 p->word[index].role = role;
 p->word[index].from = from;
 wpe_startplan_judge(p);
 return 0;
}

/**
 * wpe_startplan_answered - Record the answer the user gave another question
 * about this very command.
 * @p: the plan; @verdict: WPE_PLAN_RUN.
 *
 * The print command is still asked about by its own question, keyed by the
 * command; its "Run it" (now or remembered) is the plan's, so the plan does
 * not ask a second time.  "Use mine" there puts the user's own command in
 * the plan instead, which is his and needs no answer.
 */
void wpe_startplan_answered(WpeStartPlan *p, int verdict)
{
 if (!p || p->verdict == WPE_PLAN_REFUSED)
  return;
 if (verdict == WPE_PLAN_RUN)
  p->verdict = verdict;
}

/**
 * wpe_startplan_value - Note a value a folder file set, for the box.
 * @p: the plan; @file: the file ("./xwperc", "p.prj"); @key: the key;
 * @raw: the value as the file writes it; @expanded: as the build uses it,
 * or NULL when that is the same.
 *
 * The box lists each one as "From <file>: KEY = value", so the user sees
 * which words of the command are not his, and what $(HOME) became.
 * Return: 0, or -1 out of memory.
 */
int wpe_startplan_value(WpeStartPlan *p, const char *file, const char *key,
                        const char *raw, const char *expanded)
{
 WpeStartValue *v;

 if (!file || !key || !raw)
  return -1;
 if (startplan_grow((void **)&p->value, p->nvalue, sizeof *p->value) < 0)
  return -1;
 v = &p->value[p->nvalue];
 v->file = startplan_dup(file);
 v->key = startplan_dup(key);
 v->raw = startplan_dup(raw);
 v->expanded = (expanded && strcmp(expanded, raw) != 0) ? startplan_dup(expanded) : NULL;
 if (!v->file || !v->key || !v->raw)
 {
  free(v->file);
  free(v->key);
  free(v->raw);
  free(v->expanded);
  return -1;
 }
 p->nvalue++;
 return 0;
}

/**
 * wpe_startplan_set_own - The plan "Use mine" starts instead.
 * @p: the plan; @own: the same action with the user's own values (taken),
 * or NULL when he has none.
 */
void wpe_startplan_set_own(WpeStartPlan *p, WpeStartPlan *own)
{
 wpe_startplan_free(p->own);
 p->own = own;
}

/**
 * startplan_needs_prefix - Would a program take this word for something else?
 * Return: 1 when it starts with '-' (an option) or '@' (a response file).
 */
static int startplan_needs_prefix(const char *word)
{
 return word[0] == '-' || word[0] == '@';
}

/**
 * wpe_startplan_operand - The one way a path reaches a command line.
 * @out/@n: receive the path; @dir: a folder, with or without its trailing
 * separator, or NULL (or "") for a word that is a path already;
 * @name: the file's name.
 *
 * A path built from a value a folder sets (Exedir, the object and program
 * names, FILES, LIBNAME) that would start with '-' or '@' gets "./" in
 * front, so it is only ever the file it names, never an option or a
 * response file a compiler reads more options out of.  A path that does
 * not fit is refused, never cut: a cut path is another file.
 * Return: 0, or -1 when it does not fit.
 */
int wpe_startplan_operand(char *out, size_t n, const char *dir, const char *name)
{
 size_t len = dir ? strlen(dir) : 0;
 const char *prefix = "", *first;
 int wrote;

 if (!out || !n || !name)
  return -1;
 if (len > 0)
  first = dir;
 else
  first = name;
 if (startplan_needs_prefix(first))
  prefix = STARTPLAN_OPERAND_PREFIX;
 if (len == 0)
  wrote = snprintf(out, n, "%s%s", prefix, name);
 else if (dir[len - 1] == STARTPLAN_SEP)
  wrote = snprintf(out, n, "%s%s%s", prefix, dir, name);
 else
  wrote = snprintf(out, n, "%s%s%c%s", prefix, dir, STARTPLAN_SEP, name);
 if (wrote < 0 || (size_t)wrote >= n)
  return -1;
 return 0;
}

/**
 * startplan_append - Append one component to a resolved path.
 * @out/@sz: the path so far ("/" or "/a/b"); @comp: the component.
 * Return: 0, or -1 when it does not fit.
 */
static int startplan_append(char *out, size_t sz, const char *comp)
{
 size_t len = strlen(out), clen = strlen(comp);
 size_t sep = (len > 0 && out[len - 1] == STARTPLAN_SEP) ? 0 : 1;

 if (len + sep + clen + 1 > sz)
  return -1;
 if (sep)
  out[len++] = STARTPLAN_SEP;
 memcpy(out + len, comp, clen + 1);
 return 0;
}

/** startplan_up - Go to the parent of a resolved path ("/" stays "/"). */
static void startplan_up(char *out)
{
 char *slash = strrchr(out, STARTPLAN_SEP);

 if (!slash || slash == out)
  out[1] = '\0';
 else
  *slash = '\0';
}

static int startplan_resolve_at(const char *path, const char *cwd, char *out, size_t sz,
                                int depth);

/**
 * startplan_follow_dangling - Follow a link whose target is not there yet.
 * @out/@sz: the resolved path so far, ending in the missing component;
 * @depth: how many links were followed to get here.
 *
 * A linker or a compiler told to write a link that points nowhere writes
 * through it and creates the target, so a project that ships "demo ->
 * ~/.local/bin/git" would put the program there while the path still looks
 * like one inside its folder.  The plan takes the link's target instead.
 * Return: 0 (@out is the target, or unchanged when it is no link), or -1
 * for a link that cannot be read or that never ends.
 */
static int startplan_follow_dangling(char *out, size_t sz, int depth)
{
 char target[WPE_STARTPLAN_PATH_MAX], parent[WPE_STARTPLAN_PATH_MAX];
 struct stat st;
 ssize_t n;

 if (lstat(out, &st) != 0 || !S_ISLNK(st.st_mode))
  return 0;
 if (depth >= STARTPLAN_LINKS_MAX || strlen(out) + 1 > sizeof parent)
  return -1;
 n = readlink(out, target, sizeof target - 1);
 if (n <= 0)
  return -1;
 target[n] = '\0';
 memcpy(parent, out, strlen(out) + 1);
 startplan_up(parent);
 return startplan_resolve_at(target, parent, out, sz, depth + 1);
}

/**
 * startplan_step - Take one component of a path being resolved.
 * @out/@sz: the resolved path so far; @comp: the component;
 * @exists: 1 while every component so far exists, kept up to date;
 * @depth: how many links were followed to get here.
 *
 * While the path exists each component is followed through its link, so a
 * folder that is a link to elsewhere resolves to elsewhere; a link that
 * points nowhere yet is followed to where it points.  From the first
 * missing component on, the rest is taken as written, ".." going up.
 * Return: 0, or -1 when it does not fit or a link cannot be followed.
 */
static int startplan_step(char *out, size_t sz, const char *comp, int *exists, int depth)
{
 char real[PATH_MAX];

 if (strcmp(comp, ".") == 0 || !*comp)
  return 0;
 if (strcmp(comp, "..") == 0)
 {
  startplan_up(out);
  return 0;
 }
 if (startplan_append(out, sz, comp) < 0)
  return -1;
 if (!*exists)
  return 0;
 if (!realpath(out, real))
 {
  *exists = 0;
  return startplan_follow_dangling(out, sz, depth);
 }
 if (strlen(real) + 1 > sz)
  return -1;
 memcpy(out, real, strlen(real) + 1);
 return 0;
}

/**
 * wpe_startplan_resolve - Where a path really is.
 * @path: the path, absolute or relative to @cwd; @cwd: the folder the tool
 * runs in (resolved), or NULL for the editor's own; @out/@sz: receive it.
 *
 * Every link in the path is followed, the last component's too, so a
 * folder or an object that is a link out of the tree resolves outside it,
 * and so does a link whose target is not made yet; a missing tail (an
 * output not made yet) is kept as written.
 * Return: 0, or -1 when it cannot be resolved.
 */
int wpe_startplan_resolve(const char *path, const char *cwd, char *out, size_t sz)
{
 return startplan_resolve_at(path, cwd, out, sz, 0);
}

/**
 * startplan_resolve_at - wpe_startplan_resolve, @depth links in.
 * Return: as wpe_startplan_resolve.
 */
static int startplan_resolve_at(const char *path, const char *cwd, char *out, size_t sz,
                                int depth)
{
 char work[WPE_STARTPLAN_PATH_MAX], here[PATH_MAX];
 char *comp, *save = NULL;
 int exists = 1;

 if (!path || !*path || sz < 2)
  return -1;
 if (path[0] == STARTPLAN_SEP)
  snprintf(out, sz, "%s", STARTPLAN_SEP_TEXT);
 else if (cwd && *cwd)
 {
  if (strlen(cwd) + 1 > sz)
   return -1;
  memcpy(out, cwd, strlen(cwd) + 1);
 }
 else if (getcwd(here, sizeof here) && strlen(here) + 1 <= sz)
  memcpy(out, here, strlen(here) + 1);
 else
  return -1;
 if (strlen(path) + 1 > sizeof work)
  return -1;
 memcpy(work, path, strlen(path) + 1);
 for (comp = strtok_r(work, STARTPLAN_SEP_TEXT, &save); comp;
      comp = strtok_r(NULL, STARTPLAN_SEP_TEXT, &save))
  if (startplan_step(out, sz, comp, &exists, depth) < 0)
   return -1;
 return 0;
}

/**
 * startplan_under - Does a resolved path lie in a resolved folder?
 * Return: 1 when @path is @dir or below it.
 */
static int startplan_under(const char *path, const char *dir)
{
 size_t n = strlen(dir);

 if (n == 0)
  return 0;
 if (n == 1 && dir[0] == STARTPLAN_SEP)
  return 1;
 if (strncmp(path, dir, n) != 0)
  return 0;
 return path[n] == '\0' || path[n] == STARTPLAN_SEP;
}

/**
 * startplan_home - The home folder, resolved.
 * @out: receives it (PATH_MAX bytes).
 *
 * $HOME, or the user database's entry when $HOME is not set: an editor
 * started without $HOME must still never take the home folder for a
 * project's own, or ~/.local/bin would be "inside" it.
 * Return: 1 when it is known.
 */
static int startplan_home(char *out)
{
 const char *h = getenv("HOME");
 struct passwd *pw;

 if (!h || !*h)
 {
  pw = getpwuid(getuid());
  if (pw)
   h = pw->pw_dir;
 }
 return h && *h && realpath(h, out) != NULL;
}

/**
 * wpe_startplan_root_usable - May this folder be what "inside" means?
 * @root: a resolved folder.
 *
 * Not "/", not the home folder and not any folder above it: a project that
 * lives in the home folder, or an editor started there, must not make
 * ~/.local/bin or ~/.ssh "inside the folder".
 * Return: 1 when it may.
 */
int wpe_startplan_root_usable(const char *root)
{
 char home[PATH_MAX];

 if (!root || !*root || strcmp(root, STARTPLAN_SEP_TEXT) == 0)
  return 0;
 if (startplan_home(home) && startplan_under(home, root))
  return 0;
 return 1;
}

/**
 * startplan_is_home - Is this resolved folder the home folder itself?
 * Return: 1 when it is.
 */
static int startplan_is_home(const char *dir)
{
 char home[PATH_MAX];

 return startplan_home(home) && strcmp(home, dir) == 0;
}

/* The first bytes of what a link writes: an executable or object (ELF),
   or a library (ar). */
#define STARTPLAN_ELF_MAGIC "\177ELF"
#define STARTPLAN_AR_MAGIC  "!<arch>\n"
#define STARTPLAN_MAGIC_MAX 8
#define STARTPLAN_LIB_SUFFIX ".a"
#define STARTPLAN_LIB_SUFFIX_LEN 2

/**
 * startplan_beside - Is a path a file directly in a folder, not a dot file?
 * @path: the resolved path; @root: the folder, resolved.
 * Return: 1 for such a file.
 */
static int startplan_beside(const char *path, const char *root)
{
 size_t n = strlen(root);
 const char *name;

 if (strncmp(path, root, n) != 0 || path[n] != STARTPLAN_SEP)
  return 0;
 name = path + n + 1;
 return *name && *name != '.' && !strchr(name, STARTPLAN_SEP);
}

/**
 * startplan_starts_with - Does an open file begin with these bytes?
 * @fd: the file; @magic: the bytes.
 * Return: 1 when it does.
 */
static int startplan_starts_with(int fd, const char *magic)
{
 char head[STARTPLAN_MAGIC_MAX];
 size_t n = strlen(magic);

 if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, head, n) != (ssize_t)n)
  return 0;
 return memcmp(head, magic, n) == 0;
}

/**
 * startplan_is_build_output - May a compile or a link write this path over
 * what is there?
 * @path: the resolved path.
 *
 * Only when nothing is there yet, or a regular file (not a link) that a
 * compile or a link made before: an executable or object (ELF), or, for a
 * library named `*.a`, an ar archive.  A text, a script, a downloaded
 * package (a .deb is an ar archive too) or anything else the user keeps
 * there is not the build's to overwrite.
 * @library: 1 when the path is a library the build makes, named `*.a`.
 * Return: 1 when it may.
 */
static int startplan_is_build_output(const char *path, int library)
{
 struct stat st;
 int fd, ok;

 if (lstat(path, &st) != 0)
  return errno == ENOENT;
 if (!S_ISREG(st.st_mode))
  return 0;
 if ((fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)) < 0)
  return 0;
 ok = startplan_starts_with(fd, STARTPLAN_ELF_MAGIC);
 if (!ok && library)
  ok = startplan_starts_with(fd, STARTPLAN_AR_MAGIC);
 close(fd);
 return ok;
}

/**
 * startplan_is_static_library - Is a build product a static library?
 * @path: the resolved path.
 *
 * Only a product named `*.a` may be found as an ar archive: a program
 * named like a package (`tool.deb`) or anything else stays ELF-only, so a
 * project can never overwrite a downloaded archive the user keeps beside it.
 * Return: 1 for a name ending in ".a".
 */
static int startplan_is_static_library(const char *path)
{
 size_t n = strlen(path);

 return n > STARTPLAN_LIB_SUFFIX_LEN &&
        strcmp(path + n - STARTPLAN_LIB_SUFFIX_LEN, STARTPLAN_LIB_SUFFIX) == 0;
}

/**
 * startplan_flat_in_home - Is a path a project in the home folder names its own?
 * @s: the path, resolved; @root: the home folder, resolved.
 *
 * The home folder is never a root: a project kept there does not make
 * ~/.local/bin, ~/.ssh or ~/notes.txt "inside".  Right beside its own file
 * it still builds what is plainly its own: its FILES themselves, read, and
 * the objects of its FILES and its program or library when that path is
 * new or holds what a compile or a link made.  Everything else there (a
 * text that happens to be named like an object, a script, a dot file) is
 * asked about, and not removed.
 * Return: 1 when it is the project's own.
 */
static int startplan_flat_in_home(const WpeStartPath *s, const char *root)
{
 if (!startplan_beside(s->resolved, root))
  return 0;
 if (s->output == WPE_OUTPUT_SOURCE)
  return 1;
 if (s->output == WPE_OUTPUT_OBJECT)
  return startplan_is_build_output(s->resolved, 0);
 if (s->output == WPE_OUTPUT_PRODUCT)
  return startplan_is_build_output(s->resolved,
                                   startplan_is_static_library(s->resolved));
 return 0;
}

/**
 * startplan_root_of - The folder a path is judged against.
 * @p: the plan; @s: the path.
 * Return: the path's own root, else the plan's file's folder for a folder
 * value, else the working folder; NULL when there is none.
 */
static const char *startplan_root_of(const WpeStartPlan *p, const WpeStartPath *s)
{
 if (s->root)
  return s->root;
 if (s->from == WPE_FROM_FOLDER)
  return p->folder;
 return p->cwd;
}

/**
 * startplan_inside - Is a resolved path inside the folder it is judged against?
 * @p: the plan; @s: the path, resolved.
 *
 * A folder's value is inside only under a usable root (not "/", not the
 * home folder or above); for a file kept in the home folder itself, only
 * what it writes right beside it (startplan_flat_in_home).  The user's own
 * value is judged against the folder his file is in, whatever that is:
 * his setting, his folder.
 * Return: 1 when inside.
 */
static int startplan_inside(const WpeStartPlan *p, const WpeStartPath *s)
{
 const char *root = startplan_root_of(p, s);

 if (!root)
  return 0;
 if (s->from == WPE_FROM_FOLDER && startplan_is_home(root))
  return startplan_flat_in_home(s, root);
 if (s->from == WPE_FROM_FOLDER && !wpe_startplan_root_usable(root))
  return 0;
 return startplan_under(s->resolved, root);
}

/**
 * plan_option_origin - Rule 2: a folder value is never an option.
 * @p: the plan.
 *
 * Folder text a tool reads as options (CompilerOptions, CMPFLAGS) is a
 * WPE_WORD_TEXT and is asked about; a site that marks such a value an
 * option is a mistake this rule turns into a refusal.
 * Return: 0, or -1 with the plan refused.
 */
static int plan_option_origin(WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->nword; i++)
 {
  if (p->word[i].from != WPE_FROM_FOLDER)
   continue;
  if (p->word[i].role == WPE_WORD_OPTION)
   p->refused = WPE_STARTPLAN_REFUSE_OPTION;
  else if (p->word[i].role == WPE_WORD_CARRIER)
   p->refused = WPE_STARTPLAN_REFUSE_CARRIER;
  else
   continue;
  p->verdict = WPE_PLAN_REFUSED;
  return -1;
 }
 return 0;
}

/**
 * wpe_startplan_line_ok - May this value go into a command the editor
 * writes to a tool's input?
 * @value: a value (a program's arguments, a file name, a watch expression).
 *
 * A debugger reads its input a line at a time, so a value holding a line
 * break (or any other control character) would end the editor's command
 * and start one of the value's own: a watch "x" followed by "shell rm ..."
 * on the next line.  Such a value is never sent.
 * Return: 1 when it holds no control character.
 */
int wpe_startplan_line_ok(const char *value)
{
 const unsigned char *s = (const unsigned char *)value;

 if (!s)
  return 1;
 for (; *s; s++)
  if (*s < ' ' || *s == 0x7f)
   return 0;
 return 1;
}

/**
 * plan_input_lines - Rule 2b: no value the editor writes to the tool's
 * input after the start may hold a line break.
 * @p: the plan.
 * Return: 0, or -1 with the plan refused.
 */
static int plan_input_lines(WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->ninput; i++)
  if (!wpe_startplan_line_ok(p->input[i].text))
  {
   p->verdict = WPE_PLAN_REFUSED;
   p->refused = WPE_STARTPLAN_REFUSE_INPUT;
   return -1;
  }
 return 0;
}

/**
 * plan_resolve_paths - Rule 3: every path resolved, and judged inside or not.
 * @p: the plan.
 *
 * A path that cannot be resolved is taken as outside.
 */
static void plan_resolve_paths(WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->npath; i++)
 {
  WpeStartPath *s = &p->path[i];
  if (wpe_startplan_resolve(s->path, p->cwd, s->resolved, sizeof s->resolved) < 0)
  {
   snprintf(s->resolved, sizeof s->resolved, "%s", s->path);
   s->inside = 0;
   continue;
  }
  s->inside = startplan_inside(p, s);
 }
}

/**
 * plan_remove_inside_only - Rule 4: a removal outside the folder is dropped.
 * @p: the plan.
 *
 * The editor removes an object only to force its rebuild, and the
 * compiler's "-o" writes it anyway; there is no reason for the editor to
 * delete a file a folder's value put outside that folder.  The drop is
 * traced and said on the status row when the build reaches it.  A removal
 * the user's own values alone decide (his own Exedir) is his, and is done
 * as it always was.
 */
static void plan_remove_inside_only(WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->npath; i++)
  if (p->path[i].role == WPE_PATH_REMOVE && p->path[i].from == WPE_FROM_FOLDER &&
      !p->path[i].inside)
   p->path[i].dropped = 1;
}

/** plan_trace_dropped - Trace each removal the judge dropped. */
static void plan_trace_dropped(const WpeStartPlan *p)
{
 int i;

 for (i = 0; i < p->npath; i++)
  if (p->path[i].dropped)
   startplan_trace("startplan dropped-remove %s", p->path[i].resolved);
}

/**
 * plan_path_asks - Rule 5: does this path make the plan ask?
 * Return: 1 for a folder value the tool writes or is handed outside the
 * folder of the file that supplied it.
 */
static int plan_path_asks(const WpeStartPath *s)
{
 if (s->from != WPE_FROM_FOLDER || s->inside)
  return 0;
 return s->role == WPE_PATH_WRITE || s->role == WPE_PATH_INPUT;
}

/**
 * plan_path_runs_at_start - Rule 7: is this a file the tool runs from its
 * folder on its own?
 * Return: 1 for a start-up file the folder holds (wpe_startplan_tool_reads).
 */
static int plan_path_runs_at_start(const WpeStartPath *s)
{
 return s->role == WPE_PATH_READS && s->from == WPE_FROM_FOLDER;
}

/**
 * plan_from_folder - Rule 8: did a folder choose anything the user must see?
 * @p: the plan.
 *
 * A folder's program, option text or a path it puts outside counts; a path
 * a folder puts inside its own folder does not (a project names its own
 * files and its program inside it), nor does a value equal to the user's
 * own or empty, which the sites mark as his.  A start-up file the tool will
 * run from the folder counts, and so does a folder's value the editor will
 * write to the tool's input (a folder's Arguments in the debugger's run).
 * A program that runs the folder's own code as it starts (a language server
 * that runs build scripts) counts too: the box is its only notice.
 * Return: 1 when the plan must be compared with the store.
 */
static int plan_from_folder(const WpeStartPlan *p)
{
 int i;

 if (p->runs_code)
  return 1;
 for (i = 0; i < p->nword; i++)
  if (p->word[i].from == WPE_FROM_FOLDER && p->word[i].role != WPE_WORD_OPERAND)
   return 1;
 for (i = 0; i < p->npath; i++)
  if (plan_path_asks(&p->path[i]) || plan_path_runs_at_start(&p->path[i]))
   return 1;
 for (i = 0; i < p->ninput; i++)
  if (p->input[i].from == WPE_FROM_FOLDER)
   return 1;
 return 0;
}

/* A growing text, for the canonical form. */
typedef struct {
 char *s;
 size_t len, room;
 int bad;
} startplan_buf;

/** startplan_put - Append a line part to the canonical text. */
static void startplan_put(startplan_buf *b, const char *a, const char *c)
{
 size_t need = strlen(a) + (c ? strlen(c) : 0) + 2;
 char *grown;

 if (b->bad)
  return;
 if (b->len + need > WPE_STARTPLAN_TEXT_MAX)
 {
  b->bad = 1;
  return;
 }
 if (b->len + need > b->room)
 {
  size_t room = (b->room ? b->room * 2 : 256) + need;
  if (!(grown = realloc(b->s, room)))
  {
   b->bad = 1;
   return;
  }
  b->s = grown;
  b->room = room;
 }
 b->len += (size_t)sprintf(b->s + b->len, "%s%s\n", a, startplan_or_empty(c));
}

/** startplan_cmp_str - qsort's comparison of two texts. */
static int startplan_cmp_str(const void *a, const void *b)
{
 return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * startplan_put_inside - The inside operands, sorted, for a project.
 * @p: the plan; @b: the text.
 */
static void startplan_put_inside(const WpeStartPlan *p, startplan_buf *b)
{
 const char **list;
 int i, n = 0;

 if (!p->list_inside || p->npath == 0)
  return;
 if (!(list = malloc((size_t)p->npath * sizeof *list)))
 {
  b->bad = 1;
  return;
 }
 for (i = 0; i < p->npath; i++)
  if (p->path[i].inside && p->path[i].from == WPE_FROM_FOLDER &&
      p->path[i].role != WPE_PATH_REMOVE)
   list[n++] = p->path[i].path;
 qsort(list, (size_t)n, sizeof *list, startplan_cmp_str);
 for (i = 0; i < n; i++)
  startplan_put(b, "inside ", list[i]);
 free(list);
}

/**
 * startplan_put_reads - Each start-up file the tool runs, with its text.
 * @p: the plan; @b: the text.
 *
 * The file's size and its text are part of the plan's effect: a changed
 * .pdbrc is a changed plan, and asks again.
 */
static void startplan_put_reads(const WpeStartPlan *p, startplan_buf *b)
{
 char head[WPE_STARTPLAN_PATH_MAX + 32];
 int i;

 for (i = 0; i < p->npath; i++)
 {
  const WpeStartPath *s = &p->path[i];
  if (s->role != WPE_PATH_READS)
   continue;
  snprintf(head, sizeof head, "%s %ld", s->resolved, s->length);
  startplan_put(b, "reads ", head);
  startplan_put(b, startplan_or_empty(s->content), NULL);
 }
}

/** startplan_path_word - The canonical text's word for an outside path. */
static const char *startplan_path_word(int role)
{
 if (role == WPE_PATH_WRITE)
  return "write ";
 if (role == WPE_PATH_READS)
  return "reads ";
 return "input ";
}

/**
 * startplan_build_text - The plan's effect as one text.
 * @p: the plan, its paths resolved.
 *
 * The working folder, the words a folder or the user chose (programs,
 * options, text), and every path that lies outside; never where a value
 * came from, so a folder's plan equal in effect to the user's own compares
 * equal to it.  The file the user pressed the key in is not part of it:
 * one answer covers every file of the folder built the same way.
 * Return: the malloc'd text, or NULL when it is too large or out of memory.
 */
static char *startplan_build_text(const WpeStartPlan *p)
{
 startplan_buf b = { NULL, 0, 0, 0 };
 int i;

 startplan_put(&b, WPE_STARTPLAN_TEXT_MAGIC, NULL);
 startplan_put(&b, "kind ", wpe_startplan_kind_name(p->kind));
 startplan_put(&b, "cwd ", startplan_or_empty(p->cwd));
 for (i = 0; i < p->nword; i++)
  if (p->word[i].role != WPE_WORD_OPERAND && p->word[i].role != WPE_WORD_CARRIER)
   startplan_put(&b, "argv ", p->word[i].text);
 for (i = 0; i < p->npath; i++)
  if (!p->path[i].inside && p->path[i].role != WPE_PATH_REMOVE &&
      p->path[i].role != WPE_PATH_READS)
   startplan_put(&b, startplan_path_word(p->path[i].role), p->path[i].resolved);
 startplan_put_inside(p, &b);
 startplan_put_reads(p, &b);
 for (i = 0; i < p->ninput; i++)
  startplan_put(&b, "then ", p->input[i].text);
 if (p->runs_code)
  startplan_put(&b, "runs ", p->runs_code);
 if (b.bad)
 {
  free(b.s);
  return NULL;
 }
 if (b.len > 0)
  b.s[b.len - 1] = '\0';               /* no newline after the last line */
 return b.s;
}

/**
 * wpe_startplan_text - The canonical text: shown, compared, remembered.
 * @p: the plan, judged.
 * Return: the text, or NULL when it could not be made.
 */
const char *wpe_startplan_text(WpeStartPlan *p)
{
 if (!p->text)
  p->text = startplan_build_text(p);
 return p->text;
}

/**
 * startplan_judge_paths - Rules 2 to 4 on one plan, without the store.
 * Return: 0, or -1 when the plan is refused.
 */
static int startplan_judge_paths(WpeStartPlan *p)
{
 if (plan_option_origin(p) < 0)
  return -1;
 if (plan_input_lines(p) < 0)
  return -1;
 plan_resolve_paths(p);
 plan_remove_inside_only(p);
 return 0;
}

/**
 * plan_note_own - The user's own plan's text counts as his for the key.
 * @p: the plan; @key: its key.
 *
 * So a folder's plan that does exactly what his own does runs without a
 * question, whichever file named it.
 */
static void plan_note_own(WpeStartPlan *p, const char *key)
{
 const char *own;

 if (!wpe_startplan_own_usable(p))
  return;                /* his plan still puts a folder's path outside: not his to run unasked */
 own = wpe_startplan_text(p->own);
 if (own)
  wpe_startcmd_note_own(key, own);
}

/**
 * plan_compare - Rule 9: the store's answer for this folder, key and plan.
 * @p: the plan; @key: its key.
 * Return: WPE_PLAN_RUN, WPE_PLAN_USE_OWN or WPE_PLAN_ASK.
 */
static int plan_compare(WpeStartPlan *p, const char *key)
{
 const char *text = wpe_startplan_text(p);
 int verdict;

 if (!text)
  return WPE_PLAN_ASK;
 plan_note_own(p, key);
 verdict = wpe_startcmd_check(p->folder, key, text);
 if (verdict == WPE_STARTCMD_RUN)
  return WPE_PLAN_RUN;
 if (verdict == WPE_STARTCMD_USE_OWN)
  return WPE_PLAN_USE_OWN;
 return WPE_PLAN_ASK;
}

/**
 * wpe_startplan_judge - Apply the rules, in order, and stop at a refusal.
 * @p: the plan, whole.
 *
 * A folder value made an option is refused; every path is resolved; a
 * removal outside is dropped; a plan in which no folder chose a program,
 * an option text or a path outside runs (CLEAN) without looking at the
 * store; any other runs when it is the user's own in effect or he said yes
 * to it in this folder, starts his own when he said "Use mine", and asks
 * otherwise.
 * Return: the verdict, WPE_PLAN_*.
 */
int wpe_startplan_judge(WpeStartPlan *p)
{
 char key[WPE_STARTPLAN_KEY_MAX];

 free(p->text);
 p->text = NULL;
 p->refused = NULL;
 if (startplan_judge_paths(p) < 0)
  return p->verdict;
 plan_trace_dropped(p);
 if (!plan_from_folder(p))
  p->verdict = WPE_PLAN_CLEAN;
 else if (wpe_startplan_key(p, key, sizeof key) < 0)
  p->verdict = WPE_PLAN_ASK;
 else
  p->verdict = plan_compare(p, key);
 startplan_trace("startplan judge %s/%s %d", wpe_startplan_kind_name(p->kind), p->name,
                 p->verdict);
 return p->verdict;
}

/**
 * wpe_startplan_own_usable - May "Use mine" be offered for this plan?
 * @p: the plan.
 *
 * Only when the user has his own plan for the action and it puts nothing a
 * folder chose outside: "Use mine" must start a plan he can run without
 * being asked about it, or the button would be a promise the editor cannot
 * keep.
 * Return: 1 when it may.
 */
int wpe_startplan_own_usable(WpeStartPlan *p)
{
 int i;

 if (!p->own || startplan_judge_paths(p->own) < 0)
  return 0;
 for (i = 0; i < p->own->npath; i++)
  if (plan_path_asks(&p->own->path[i]))
   return 0;
 return 1;
}

/**
 * wpe_startplan_counts - What the box summarises.
 * @p: the plan, judged; @outside: receives the paths that ask; @dropped:
 * the removals dropped; @inside: the folder's operands inside its folder.
 * Return: 0.
 */
int wpe_startplan_counts(const WpeStartPlan *p, int *outside, int *dropped, int *inside)
{
 int i;

 *outside = *dropped = *inside = 0;
 for (i = 0; i < p->npath; i++)
 {
  const WpeStartPath *s = &p->path[i];
  if (plan_path_asks(s))
   (*outside)++;
  if (s->dropped)
   (*dropped)++;
  if (s->inside && s->from == WPE_FROM_FOLDER && s->role == WPE_PATH_INPUT)
   (*inside)++;
 }
 return 0;
}

/**
 * wpe_startplan_remember - Keep the user's answer about this very plan.
 * @p: the plan, judged; @verdict: WPE_PLAN_RUN (Run it) or
 * WPE_PLAN_USE_OWN (Use mine).
 *
 * Kept for the plan's folder, its key and its text, beside every other
 * answer for that folder and key: a second program debugged, or a file in
 * a subfolder, adds its own answer and never pushes the first one out.
 * Return: 0, or -1 when it could not be kept.
 */
int wpe_startplan_remember(WpeStartPlan *p, int verdict)
{
 char key[WPE_STARTPLAN_KEY_MAX];
 const char *text = wpe_startplan_text(p);
 int answer = WPE_STARTCMD_NO;

 if (!text || !p->folder || wpe_startplan_key(p, key, sizeof key) < 0)
  return -1;
 if (verdict == WPE_PLAN_RUN)
  answer = WPE_STARTCMD_YES;
 return wpe_startcmd_remember(p->folder, key, text, answer);
}

/**
 * startplan_holds - Is this word one the plan holds?
 * @p: the plan; @word: a word of a child's command line.
 * Return: the plan's own copy of it, or NULL.
 */
static char *startplan_holds(const WpeStartPlan *p, const char *word)
{
 int i;

 for (i = 0; i < p->nword; i++)
  if (strcmp(p->word[i].text, word) == 0)
   return p->word[i].text;
 for (i = 0; i < p->npath; i++)
  if (p->path[i].role != WPE_PATH_REMOVE && strcmp(p->path[i].path, word) == 0)
   return p->path[i].path;
 return NULL;
}

/**
 * startplan_running - The plan whose words a child of this action takes.
 * @p: the plan, answered.
 * Return: the user's own after "Use mine", the plan after a yes or when it
 * is clean, NULL when nothing may run.
 */
static const WpeStartPlan *startplan_running(const WpeStartPlan *p)
{
 return wpe_startplan_running((WpeStartPlan *)p);
}

/**
 * wpe_startplan_running - The plan that runs for a judged and answered plan.
 * @p: the plan.
 *
 * A site whose child is carried by a program of the editor's own (the
 * graphical editors' debugger script) adds the carrier's words to this one.
 * Return: the user's own after "Use mine", @p after a yes or when clean,
 * NULL when nothing may run.
 */
WpeStartPlan *wpe_startplan_running(WpeStartPlan *p)
{
 if (!p)
  return NULL;
 if (p->verdict == WPE_PLAN_USE_OWN)
  return p->own;
 if (p->verdict == WPE_PLAN_RUN || p->verdict == WPE_PLAN_CLEAN)
  return p;
 return NULL;
}

/**
 * wpe_startplan_argv - The command line of a plan whose words are in order.
 * @p: the plan, answered.
 *
 * For the starts that build their plan word by word in the order the
 * program gets them (a debugger, an adapter, a build server): the words of
 * the plan that runs, its carriers left out.
 * Return: a malloc'd NULL-ended list of the plan's own words (free the list,
 * not the words), or NULL when nothing may run or out of memory.
 */
char **wpe_startplan_argv(WpeStartPlan *p)
{
 const WpeStartPlan *run = startplan_running(p);
 char **argv;
 int i, n = 0;

 if (!run || run->nword == 0)
  return NULL;
 if (!(argv = calloc((size_t)run->nword + 1, sizeof *argv)))
  return NULL;
 for (i = 0; i < run->nword; i++)
  if (run->word[i].role != WPE_WORD_CARRIER)
   argv[n++] = run->word[i].text;
 return argv;
}

/**
 * wpe_startplan_covers - Does the plan hold every word of a command line?
 * @p: the plan that runs; @argv: the command line, NULL-ended; @bad:
 * receives the first word it does not hold.
 * Return: 1 when it holds them all.
 */
int wpe_startplan_covers(const WpeStartPlan *p, char *const argv[], const char **bad)
{
 int i;

 *bad = NULL;
 if (!argv || !argv[0])
 {
  *bad = "";
  return 0;
 }
 for (i = 0; argv[i]; i++)
  if (!startplan_holds(p, argv[i]))
  {
   *bad = argv[i];
   return 0;
  }
 return 1;
}

/**
 * startplan_spec - The child's spec: the I/O shape, and what the plan decided.
 * @spec: filled in; @io: the shape; @argv: the plan's copies of the words;
 * @p: the plan.
 */
static void startplan_spec(WpeChildSpec *spec, const WpeStartIo *io, char *const *argv,
                           const WpeStartPlan *p)
{
 memset(spec, 0, sizeof *spec);
 spec->argv = argv;
 spec->cwd = p->cwd;
 spec->plan = p;
 spec->origin = p->origin;
 spec->io = io->io;
 spec->group = io->group;
 spec->stdin_null = io->stdin_null;
 spec->stdout_to_null = io->stdout_to_null;
 spec->stdout_path = io->stdout_path;
 spec->stderr_to_null = io->stderr_to_null;
 spec->stderr_to_stdout = io->stderr_to_stdout;
 spec->stderr_inherit = io->stderr_inherit;
 spec->pty_raw = io->pty_raw;
 spec->die_with_editor = io->die_with_editor;
 spec->signal_leader_only = io->signal_leader_only;
 spec->stderr_nonblocking = io->stderr_nonblocking;
 spec->side_pty_slave = io->side_pty_slave;
 spec->tty_fd = io->tty_fd;
 spec->tty_path = io->tty_path;
 spec->say_failed = io->say_failed;
 spec->echo_argv = io->echo_argv;
 spec->exec_failed = io->exec_failed;
 spec->what = io->what;
}

/**
 * startplan_refuse - Say why a step of a planned action did not start.
 * @p: the plan; @why: the reason; @word: the word, or NULL.
 * Return: -1.
 */
static int startplan_refuse(const WpeStartPlan *p, const char *why, const char *word)
{
 if (word)
  startplan_trace("startplan refused %s/%s: %s: %s", wpe_startplan_kind_name(p->kind),
                  p->name, why, word);
 else
  startplan_trace("startplan refused %s/%s: %s", wpe_startplan_kind_name(p->kind),
                  p->name, why);
 return -1;
}

/**
 * startplan_refuse_capture - A short command whose plan may not run.
 * @p: the plan, or NULL out of memory.
 * Return: WPE_CAPTURE_NOT_STARTED.
 */
static int startplan_refuse_capture(const WpeStartPlan *p)
{
 if (p)
  startplan_refuse(p, WPE_STARTPLAN_REFUSE_JUDGE, NULL);
 return WPE_CAPTURE_NOT_STARTED;
}

/**
 * wpe_startplan_start - Start one child of a planned action.
 * @p: the plan the user's action was judged by; @argv: the step's command
 * line, NULL-ended; @io: its I/O shape; @c/@ended/@data: as wpe_child_start.
 *
 * The only way a child of a planned action starts.  It runs only when the
 * plan was clean or answered for, in the plan's working folder, and only
 * when every word of its command line is one the judged plan holds (the
 * user's own after "Use mine"): a word the box did not show never reaches
 * the program.  The child gets the plan's own copies of the words.
 * Return: 0, or -1 with nothing started.
 */
int wpe_startplan_start(WpeStartPlan *p, char *const argv[], const WpeStartIo *io,
                        WpeChild *c, WpeChildEndedFn ended, void *data)
{
 const WpeStartPlan *run;
 const char *bad;
 WpeChildSpec spec;
 char **words;
 int i, n, rc;

 if (!p || !io)
  return -1;
 if (!(run = startplan_running(p)))
  return startplan_refuse(p, WPE_STARTPLAN_REFUSE_JUDGE, NULL);
 if (!wpe_startplan_covers(run, argv, &bad))
  return startplan_refuse(p, WPE_STARTPLAN_REFUSE_WORD, bad);
 for (n = 0; argv[n]; n++)
  ;
 if (!(words = calloc((size_t)n + 1, sizeof *words)))
  return -1;
 for (i = 0; i < n; i++)
  words[i] = startplan_holds(run, argv[i]);
 startplan_spec(&spec, io, words, run);
 spec.cwd = p->cwd;
 rc = wpe_child_start(&spec, c, ended, data);
 free(words);
 return rc;
}

/**
 * wpe_startplan_start_words - Start the child of a plan whose words are
 * its command line, in order.
 * @p: the plan, judged and answered; @io/@c/@ended/@data: as
 * wpe_startplan_start.
 *
 * A debugger, its program, an adapter, a build server: the child gets
 * exactly the words the user was shown (his own after "Use mine").  When
 * the site added carrier words (the graphical editors' helper script, which
 * holds those very words), the child is the carrier.
 * Return: 0, or -1 with nothing started.
 */
int wpe_startplan_start_words(WpeStartPlan *p, const WpeStartIo *io, WpeChild *c,
                              WpeChildEndedFn ended, void *data)
{
 const WpeStartPlan *run;
 char **argv;
 int i, n = 0, rc;

 if (!p || !io)
  return -1;
 if (!(run = startplan_running(p)))
  return startplan_refuse(p, WPE_STARTPLAN_REFUSE_JUDGE, NULL);
 if (!(argv = calloc((size_t)run->nword + 1, sizeof *argv)))
  return -1;
 for (i = 0; i < run->nword; i++)
  if (run->word[i].role == WPE_WORD_CARRIER)
   argv[n++] = run->word[i].text;
 if (n == 0)
  for (i = 0; i < run->nword; i++)
   argv[n++] = run->word[i].text;
 rc = wpe_startplan_start(p, argv, io, c, ended, data);
 free(argv);
 return rc;
}

/**
 * wpe_startplan_shell - Begin a plan's command line as a shell line.
 * @p: the plan; @script: the line the shell runs; @from: whose it is
 * (WPE_FROM_USER for the user's own text or one the editor wrote,
 * WPE_FROM_FOLDER for a project's install line); @source: for the box.
 *
 * The plan's words become "/bin/sh -c <script> sh", and every file name or
 * folder value the line works on is added after them with
 * wpe_startplan_word: the shell hands them to the line as "$1", "$2" ...,
 * so a file named "a;touch x.c" is only ever that file's name, never shell
 * text.  The script itself holds only the editor's own words and the
 * user's own text, or is the folder's whole line, which the box shows.
 * Return: 0, or -1 out of memory.
 */
int wpe_startplan_shell(WpeStartPlan *p, const char *script, int from, const char *source)
{
 if (!p || !script)
  return -1;
 if (wpe_startplan_word(p, WPE_STARTPLAN_SHELL, WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL) < 0 ||
     wpe_startplan_word(p, WPE_STARTPLAN_SHELL_OPT, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL) < 0 ||
     wpe_startplan_word(p, script, WPE_WORD_TEXT, from, source) < 0)
  return -1;
 return wpe_startplan_word(p, WPE_STARTPLAN_SHELL_NAME, WPE_WORD_TEXT, WPE_FROM_EDITOR, NULL);
}

/**
 * wpe_startplan_carry - Name the editor's own program that carries the
 * plan's command to where the user watches it.
 * @p: the plan, answered; @carrier: the carrier's command line, NULL-ended
 * (the terminal the graphical editors open, and the helper script that
 * waits for Return in it); @from: WPE_FROM_EDITOR or WPE_FROM_USER (the
 * user's own shell).
 *
 * The carrier's words go to the plan that runs, in front of its command
 * when wpe_startplan_start_carried starts it; they are neither shown nor
 * remembered, and a folder's value may never be one.
 * Return: 0, or -1 when nothing may run or out of memory.
 */
int wpe_startplan_carry(WpeStartPlan *p, char *const carrier[], int from)
{
 WpeStartPlan *run = wpe_startplan_running(p);
 int i;

 if (!run || !carrier || from == WPE_FROM_FOLDER)
  return -1;
 for (i = 0; carrier[i]; i++)
  if (wpe_startplan_word(run, carrier[i], WPE_WORD_CARRIER, from, NULL) < 0)
   return -1;
 return 0;
}

/**
 * wpe_startplan_start_carried - Start a plan's command inside its carrier.
 * @p: the plan, judged, answered and carried; @io/@c/@ended/@data: as
 * wpe_startplan_start.
 *
 * The child is the carrier's command line followed by the plan's own
 * words, in the order they were added: on the graphical editors a File >
 * Shell command or an executed file runs in a terminal of its own that
 * waits for Return when it ends.
 * Return: 0, or -1 with nothing started.
 */
int wpe_startplan_start_carried(WpeStartPlan *p, const WpeStartIo *io, WpeChild *c,
                                WpeChildEndedFn ended, void *data)
{
 const WpeStartPlan *run;
 char **argv;
 int i, n = 0, rc;

 if (!p || !io)
  return -1;
 if (!(run = startplan_running(p)))
  return startplan_refuse(p, WPE_STARTPLAN_REFUSE_JUDGE, NULL);
 if (!(argv = calloc((size_t)run->nword + 1, sizeof *argv)))
  return -1;
 for (i = 0; i < run->nword; i++)
  if (run->word[i].role == WPE_WORD_CARRIER)
   argv[n++] = run->word[i].text;
 for (i = 0; i < run->nword; i++)
  if (run->word[i].role != WPE_WORD_CARRIER)
   argv[n++] = run->word[i].text;
 rc = wpe_startplan_start(p, argv, io, c, ended, data);
 free(argv);
 return rc;
}

/**
 * wpe_startplan_runs_code - Say what of the folder's own code the program
 * runs on its own as it starts.
 * @p: the plan; @what: the sentence the box shows ("rust-analyzer runs this
 * folder's build scripts and proc macros"), a constant.
 *
 * Opening a file in a cloned folder would otherwise run that folder's build
 * code with no notice at all: the plan now asks once per folder, and the
 * answer is kept.
 */
void wpe_startplan_runs_code(WpeStartPlan *p, const char *what)
{
 if (p)
  p->runs_code = what;
}

/**
 * wpe_startplan_remove - Remove a build output, as the plan allows.
 * @p: the plan; @path: the output, as the site names it.
 *
 * The only way a planned action removes a file.  A removal outside the
 * folder was dropped by the judge and is never done; a path the plan does
 * not list as a removal is not removed either.
 * Return: WPE_STARTPLAN_REMOVED, _NOT_REMOVED (dropped: the caller says
 * so on the status row) or _NOT_PLANNED.
 */
int wpe_startplan_remove(WpeStartPlan *p, const char *path)
{
 int i;

 if (!p || !path)
  return WPE_STARTPLAN_NOT_PLANNED;
 for (i = 0; i < p->npath; i++)
 {
  WpeStartPath *s = &p->path[i];
  if (s->role != WPE_PATH_REMOVE || strcmp(s->path, path) != 0)
   continue;
  if (s->dropped)
  {
   startplan_trace("startplan not-removed %s", s->resolved);
   return WPE_STARTPLAN_NOT_REMOVED;
  }
  if (remove(path) != 0 && errno != ENOENT)
   startplan_trace("startplan remove-failed %s", s->resolved);
  return WPE_STARTPLAN_REMOVED;
 }
 return WPE_STARTPLAN_NOT_PLANNED;
}

/**
 * wpe_startplan_fixed - The plan of a short command whose words the editor
 * chose.
 * @kind: WPE_START_*; @name: the key's second part; @argv: the command
 * line, NULL-ended, in the order it runs.
 *
 * gunzip on a help file, man for Help, the version of a Java, the desktop's
 * font, the print command once its question was answered: no word comes
 * from a folder file here, so the plan is clean and runs without a
 * question, and the child still carries a plan like every other start.
 * Return: the judged plan (the caller frees it), or NULL out of memory.
 */
WpeStartPlan *wpe_startplan_fixed(int kind, const char *name, char *const argv[])
{
 WpeStartPlan *p = wpe_startplan_new(kind, name);
 int i, role = WPE_WORD_PROGRAM;

 if (!p || !argv || !argv[0])
 {
  wpe_startplan_free(p);
  return NULL;
 }
 for (i = 0; argv[i]; i++)
 {
  if (wpe_startplan_word(p, argv[i], role, WPE_FROM_EDITOR, NULL) < 0)
  {
   wpe_startplan_free(p);
   return NULL;
  }
  role = WPE_WORD_TEXT;
 }
 wpe_startplan_judge(p);
 return p;
}

/**
 * startplan_argv - The command line a short command's plan runs.
 * @p: the plan, answered.
 * Return: a malloc'd NULL-ended list of the plan's own words, in order, or
 * NULL when the plan may not run or out of memory.
 */
static char **startplan_argv(const WpeStartPlan *p)
{
 const WpeStartPlan *run;
 char **argv;
 int i;

 if (!p || !(run = startplan_running(p)) || run->nword == 0)
  return NULL;
 if (!(argv = calloc((size_t)run->nword + 1, sizeof *argv)))
  return NULL;
 for (i = 0; i < run->nword; i++)
  argv[i] = run->word[i].text;
 return argv;
}

/**
 * wpe_startplan_capture - Run a short command's plan and read what it prints.
 * @p: the plan; the rest as wpe_child_capture.
 *
 * The only way the editor runs a short command for the user: the plan's
 * words, in order, and the plan in the child's spec.
 * Return: as wpe_child_capture; WPE_CAPTURE_NOT_STARTED when the plan may
 * not run.
 */
int wpe_startplan_capture(WpeStartPlan *p, char *out, size_t max, long deadline_ms,
                          int *status, const char *what)
{
 char **argv = startplan_argv(p);
 int rc;

 if (!argv)
  return startplan_refuse_capture(p);
 rc = wpe_child_capture_plan(p, argv, out, max, deadline_ms, status, what);
 free(argv);
 return rc;
}

/**
 * wpe_startplan_probe - Run a short command's plan without taking Esc.
 * @p: the plan; the rest as wpe_child_probe.
 * Return: as wpe_child_probe.
 */
int wpe_startplan_probe(WpeStartPlan *p, char *out, size_t max, long deadline_ms,
                        int *status, const char *what)
{
 char **argv = startplan_argv(p);
 int rc;

 if (!argv)
  return startplan_refuse_capture(p);
 rc = wpe_child_probe_plan(p, argv, out, max, deadline_ms, status, what);
 free(argv);
 return rc;
}

/**
 * wpe_startplan_run_to_file - Run a short command's plan into a file.
 * @p: the plan; the rest as wpe_child_run_to_file.
 * Return: as wpe_child_run_to_file.
 */
int wpe_startplan_run_to_file(WpeStartPlan *p, const char *path, long deadline_ms,
                              int *status, const char *what)
{
 char **argv = startplan_argv(p);
 int rc;

 if (!argv)
  return startplan_refuse_capture(p);
 rc = wpe_child_run_to_file_plan(p, argv, path, deadline_ms, status, what);
 free(argv);
 return rc;
}

/**
 * startplan_ends_with - Does a name end with a suffix?
 */
static int startplan_ends_with(const char *name, const char *end)
{
 size_t n = strlen(name), e = strlen(end);

 return n >= e && strcmp(name + n - e, end) == 0;
}

/**
 * wpe_startplan_env_name_allowed - May a folder's value expand this name
 * from the editor's environment?
 * @name: the variable's name.
 *
 * A project file is the folder's; what it may take from the editor's
 * environment is where the user is (HOME, USER, LOGNAME, PWD, TMPDIR),
 * never a key or a token he holds, which would otherwise reach the
 * compiler's command line, the box and the object file.
 * Return: 1 when it may.
 */
int wpe_startplan_env_name_allowed(const char *name)
{
 int i;

 if (!name || !*name)
  return 0;
 for (i = 0; startplan_env_secret_end[i]; i++)
  if (startplan_ends_with(name, startplan_env_secret_end[i]))
   return 0;
 for (i = 0; startplan_env_allowed[i]; i++)
  if (strcmp(name, startplan_env_allowed[i]) == 0)
   return 1;
 return 0;
}

/**
 * wpe_startplan_input - Add a value the editor will write to the tool's
 * input once it runs.
 * @p: the plan; @text: the command as it will be written, without its line
 * end ("r --verbose"); @from: WPE_FROM_*; @source: where the value came
 * from, for the box.
 *
 * A debugger's run command carries the program's Arguments: a folder's
 * Arguments are shown ("Then runs: r ...") and asked about like a word of
 * the command line, and no such value may hold a line break (rule 2b).
 * Return: 0, or -1 out of memory or past the plan's size.
 */
int wpe_startplan_input(WpeStartPlan *p, const char *text, int from, const char *source)
{
 WpeStartWord *w;

 if (!p || !text || startplan_grow((void **)&p->input, p->ninput, sizeof *p->input) < 0)
  return -1;
 w = &p->input[p->ninput];
 w->text = startplan_dup(text);
 w->source = startplan_dup(startplan_or_empty(source));
 w->role = WPE_WORD_TEXT;
 w->from = from;
 if (!w->text || !w->source)
 {
  free(w->text);
  free(w->source);
  return -1;
 }
 p->ninput++;
 return 0;
}

/**
 * wpe_startplan_reads_too_long - Does the plan run a start-up file the box
 * cannot show whole?
 * @p: the plan.
 * Return: 1 when one is too long (or not a plain file): Run it is not offered.
 */
int wpe_startplan_reads_too_long(const WpeStartPlan *p)
{
 int i;

 for (i = 0; p && i < p->npath; i++)
  if (p->path[i].role == WPE_PATH_READS && p->path[i].too_long)
   return 1;
 return 0;
}

/* What a tool reads and runs from the folder it starts in, on its own,
   before the editor sends it anything.  One row per file; a measured
   correction is one row.
   - pdb: https://docs.python.org/3/library/pdb.html -- ".pdbrc" in the home
     folder and in the current folder, the home one first, each line run as
     if typed; there is no switch that turns it off (only Pdb(readrc=False)).
   - jdb: OpenJDK test/jdk/com/sun/jdi/JdbReadTwiceTest.sh -- "jdb.ini",
     else ".jdbrc", in user.home and in the working folder, both read, the
     same file through a link once.  Measured with JDK 25: the working
     folder's jdb.ini is read ("Reading commands from ...").
   - gdb: gdb manual, "Init File in the Current Directory" -- ./.gdbinit is
     read only when the auto-load safe-path admits the folder; the -iex
     "set auto-load local-gdbinit off" of "Use mine" skips it (measured).
     gdb manual, "Auto-loading extensions" ("objfile-gdb.ext file") -- a
     script named after the program, <program>-gdb.gdb, -gdb.py or
     -gdb.scm, beside it (its real path first, then the name it was given)
     runs under the same safe-path rule; "set auto-load gdb-scripts off",
     "python-scripts off" and "guile-scripts off" skip them (measured on
     gdb 18.1, which has no Guile and refuses the last setting).
   - lldb: lldb(1) -- ./.lldbinit only with target.load-cwd-lldbinit true;
     lldb-dap reads init files the same way (to be measured on lldb-dap).
   - dbx: IBM AIX 7.2, "Customizing the dbx debugging environment" --
     .dbxinit in the home and the current folder.  To be measured on AIX.
   - xdb: .xdbrc in the current folder (HP-UX manual).  To be measured.
   - sdb, a68g --monitor, dlv: nothing known in the working folder.
   - perl -d: .perldb; no perl debugger is started by the editor, so the
     row is off until one is. */
typedef struct {
 const char *tool;           /* the tool, as the site names it                      */
 const char *file;           /* the file it reads in its working folder             */
 int alternative;            /* read only when the row before found no file          */
 int always;                 /* 1: runs it at every start; 0: only under a setting
                                of the user's the editor cannot see                 */
 int enabled;                /* 0: a tool the editor does not start                 */
 const char *note;           /* the box's sentence for a conditional file           */
 int of_program;             /* 1: file is a suffix to the program's own name, and
                                the script lies beside the program               */
} startplan_startup_file;

/* The box's sentences for the files gdb runs only under the user's settings. */
#define STARTPLAN_GDB_AUTOLOAD_NOTE \
 "gdb runs it only if your gdb settings allow auto-load from this folder"
#define STARTPLAN_GDB_GUILE_NOTE \
 "gdb runs it only if it has Guile and your gdb settings allow auto-load from this folder"

static const startplan_startup_file startplan_startup_files[] = {
 { "pdb",  ".pdbrc",    0, 1, 1, NULL, 0 },
 { "jdb",  "jdb.ini",   0, 1, 1, NULL, 0 },
 { "jdb",  ".jdbrc",    1, 1, 1, NULL, 0 },
 { "gdb",  ".gdbinit",  0, 0, 1, STARTPLAN_GDB_AUTOLOAD_NOTE, 0 },
 { "gdb",  "-gdb.gdb",  0, 0, 1, STARTPLAN_GDB_AUTOLOAD_NOTE, 1 },
 { "gdb",  "-gdb.py",   0, 0, 1, STARTPLAN_GDB_AUTOLOAD_NOTE, 1 },
 { "gdb",  "-gdb.scm",  0, 0, 1, STARTPLAN_GDB_GUILE_NOTE, 1 },
 { "lldb", ".lldbinit", 0, 0, 1,
   "lldb runs it only if your ~/.lldbinit says load-cwd-lldbinit", 0 },
 { "dbx",  ".dbxinit",  0, 1, 1, NULL, 0 },
 { "xdb",  ".xdbrc",    0, 1, 1, NULL, 0 },
 { "perl", ".perldb",   0, 1, 0, NULL, 0 },
 { NULL,   NULL,        0, 0, 0, NULL, 0 }
};

/**
 * startplan_same_file_as_in - Is @st the file of that name in @dir?
 * @dir: a folder, or NULL; @name: the start-up file's name; @st: the file.
 * Return: 1 when @dir/@name is that very file (links followed).
 */
static int startplan_same_file_as_in(const char *dir, const char *name, const struct stat *st)
{
 char path[WPE_STARTPLAN_PATH_MAX];
 struct stat other;

 if (!dir || !*dir)
  return 0;
 if (snprintf(path, sizeof path, "%s%c%s", dir, STARTPLAN_SEP, name) >= (int)sizeof path)
  return 0;
 if (stat(path, &other) != 0)
  return 0;
 return other.st_dev == st->st_dev && other.st_ino == st->st_ino;
}

/**
 * startplan_is_users_startup - Is the folder's start-up file the user's own?
 * @name: the file's name; @st: the file in the working folder.
 *
 * Started in the home folder, ./.pdbrc is ~/.pdbrc: the user's own, which
 * the box does not list.  Java's home is the user database's, whatever
 * $HOME says, so both are compared.
 * Return: 1 when it is the user's.
 */
static int startplan_is_users_startup(const char *name, const struct stat *st)
{
 struct passwd *pw = getpwuid(getuid());

 if (startplan_same_file_as_in(getenv("HOME"), name, st))
  return 1;
 return pw && startplan_same_file_as_in(pw->pw_dir, name, st);
}

/**
 * startplan_read_whole - Is the start-up file's text all the box can show?
 * @content: what was read; @got: its bytes, or -1; @size: the file's size.
 *
 * The box shows the text up to its first NUL; the tool reads past it.
 * Return: 1 when the whole file was read and holds no NUL byte.
 */
static int startplan_read_whole(const char *content, ssize_t got, off_t size)
{
 if (got < 0 || size > WPE_STARTPLAN_READS_MAX || (off_t)got != size)
  return 0;
 return memchr(content, '\0', (size_t)got) == NULL;
}

/**
 * startplan_read_startup - Take a start-up file's text into its path.
 * @s: the path, its length set; @full: the file's path.
 *
 * At most WPE_STARTPLAN_READS_MAX bytes; a longer file, one that is not
 * a plain file (a pipe would hold the tool forever), or one with a NUL
 * byte in it is marked too long, so the box shows it in part and does not
 * offer to run it.  The box shows text up to its first NUL, while the tool
 * reads every line after it: a folder could hide its real commands behind
 * one harmless line.
 */
static void startplan_read_startup(WpeStartPath *s, const char *full)
{
 struct stat st;
 ssize_t got;
 int fd;

 s->too_long = 1;
 if (!(s->content = calloc(1, WPE_STARTPLAN_READS_MAX + 1)))
  return;
 if ((fd = open(full, O_RDONLY | O_NONBLOCK)) < 0)
  return;
 if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
 {
  s->length = (long)st.st_size;
  got = read(fd, s->content, WPE_STARTPLAN_READS_MAX);
  if (got >= 0)
   s->content[got] = '\0';
  s->too_long = !startplan_read_whole(s->content, got, st.st_size);
 }
 close(fd);
}

/**
 * startplan_add_startup_at - List one start-up file, when it exists.
 * @p: the plan; @row: the table's row; @full: the file's path; @given: the
 * path the box shows; @name: its name, to tell the user's own by; @st:
 * receives the file's identity.
 * Return: 1 when the file exists (listed, or the user's own), 0 when it
 * does not, -1 out of memory.
 */
static int startplan_add_startup_at(WpeStartPlan *p, const startplan_startup_file *row,
                                    const char *full, const char *given, const char *name,
                                    struct stat *st)
{
 WpeStartPath *s;

 if (stat(full, st) != 0)
  return 0;
 if (startplan_is_users_startup(name, st))
  return 1;
 if (wpe_startplan_path(p, given, WPE_PATH_READS, WPE_FROM_FOLDER, name, p->cwd) < 0)
  return -1;
 s = &p->path[p->npath - 1];
 s->note = row->note;
 startplan_read_startup(s, full);
 startplan_trace("startplan reads %s %ld", full, s->length);
 return 1;
}

/**
 * startplan_add_startup - List one start-up file the working folder holds.
 * @p: the plan; @row: the table's row.
 * Return: 1 when the folder holds it (listed, or the user's own),
 * 0 when it does not, -1 out of memory.
 */
static int startplan_add_startup(WpeStartPlan *p, const startplan_startup_file *row)
{
 char full[WPE_STARTPLAN_PATH_MAX], given[WPE_STARTPLAN_PATH_MAX];
 struct stat st;

 if (snprintf(full, sizeof full, "%s%c%s", p->cwd, STARTPLAN_SEP, row->file) >= (int)sizeof full)
  return 0;
 snprintf(given, sizeof given, ".%c%s", STARTPLAN_SEP, row->file);
 return startplan_add_startup_at(p, row, full, given, row->file, &st);
}

/**
 * startplan_last_name - The last component of a path.
 */
static const char *startplan_last_name(const char *path)
{
 const char *slash = strrchr(path, STARTPLAN_SEP);

 if (slash)
  return slash + 1;
 return path;
}

/**
 * startplan_program_paths - Where a program's own script would lie, as the
 * working folder names the program and as the box shows it.
 * @p: the plan, its working folder set; @program: the program as the tool
 * gets it; @suffix: the script's ending; @full/@given: receive the paths.
 * Return: 0, or -1 when they do not fit.
 */
static int startplan_program_paths(const WpeStartPlan *p, const char *program, const char *suffix,
                                   char *full, char *given, size_t sz)
{
 int n, m;

 if (program[0] == STARTPLAN_SEP)
 {
  n = snprintf(full, sz, "%s%s", program, suffix);
  m = snprintf(given, sz, "%s%s", program, suffix);
 }
 else if (strncmp(program, "." STARTPLAN_SEP_TEXT, 2) == 0)
 {
  n = snprintf(full, sz, "%s%c%s%s", p->cwd, STARTPLAN_SEP, program + 2, suffix);
  m = snprintf(given, sz, "%s%s", program, suffix);
 }
 else
 {
  n = snprintf(full, sz, "%s%c%s%s", p->cwd, STARTPLAN_SEP, program, suffix);
  m = snprintf(given, sz, ".%c%s%s", STARTPLAN_SEP, program, suffix);
 }
 if (n < 0 || m < 0 || (size_t)n >= sz || (size_t)m >= sz)
  return -1;
 return 0;
}

/**
 * startplan_program_real - The script beside the program's real file, when
 * the program is reached through a link.
 * @full: the script as the program was named; @suffix: its ending; @out:
 * receives the script beside the program's real path.
 * Return: 1 when that is another path, else 0.
 */
static int startplan_program_real(const char *full, const char *suffix, char *out, size_t sz)
{
 char program[WPE_STARTPLAN_PATH_MAX], real[PATH_MAX];
 size_t len = strlen(full) - strlen(suffix);
 int n;

 if (len >= sizeof program)
  return 0;
 memcpy(program, full, len);
 program[len] = '\0';
 if (!realpath(program, real) || strcmp(real, program) == 0)
  return 0;
 n = snprintf(out, sz, "%s%s", real, suffix);
 return n >= 0 && (size_t)n < sz;
}

/**
 * startplan_add_program_startup - List a script named after the program
 * that the tool runs from beside it (gdb's <program>-gdb.py).
 * @p: the plan; @row: the table's row; @program: the program as the tool
 * gets it, or NULL.
 *
 * gdb looks beside the program's real file first, then beside the name it
 * was given, so a program reached through a link has two places; the same
 * file in both is listed once.
 * Return: 1 when one was found, 0 when none, -1 out of memory.
 */
static int startplan_add_program_startup(WpeStartPlan *p, const startplan_startup_file *row,
                                         const char *program)
{
 char full[WPE_STARTPLAN_PATH_MAX], given[WPE_STARTPLAN_PATH_MAX];
 char real[WPE_STARTPLAN_PATH_MAX];
 struct stat st, real_st;
 int rc, rc_real;

 if (!program || !*program)
  return 0;
 if (startplan_program_paths(p, program, row->file, full, given, sizeof given) < 0)
  return 0;
 rc = startplan_add_startup_at(p, row, full, given, startplan_last_name(full), &st);
 if (rc < 0 || !startplan_program_real(full, row->file, real, sizeof real))
  return rc;
 if (stat(real, &real_st) == 0 && rc > 0 &&
     real_st.st_dev == st.st_dev && real_st.st_ino == st.st_ino)
  return rc;
 rc_real = startplan_add_startup_at(p, row, real, real, startplan_last_name(real), &real_st);
 if (rc_real < 0)
  return -1;
 return rc || rc_real;
}

/**
 * wpe_startplan_tool_reads - Rule 7: list what the tool runs from its folder.
 * @p: the plan, its working folder set; @tool: "gdb", "pdb", "jdb", "lldb",
 * "dbx", "xdb".
 *
 * As wpe_startplan_tool_reads_of with no program known.
 * Return: the number of files listed, or -1 out of memory.
 */
int wpe_startplan_tool_reads(WpeStartPlan *p, const char *tool)
{
 return wpe_startplan_tool_reads_of(p, tool, NULL);
}

/**
 * wpe_startplan_tool_reads_of - Rule 7: list what the tool runs from its
 * folder and from beside the program it is started on.
 * @p: the plan, its working folder set; @tool: "gdb", "pdb", "jdb", "lldb",
 * "dbx", "xdb"; @program: the program as the tool gets it, or NULL.
 *
 * Each start-up file of the tool the working folder holds, and each script
 * named after the program that lies beside it (gdb's <program>-gdb.py),
 * becomes a path the tool reads, with its text: the plan then asks, the
 * box shows the file's lines, and a changed file asks again.  A file that
 * is the user's own (the folder is his home) is not listed.
 * Return: the number of files listed, or -1 out of memory.
 */
int wpe_startplan_tool_reads_of(WpeStartPlan *p, const char *tool, const char *program)
{
 const startplan_startup_file *row;
 int found = 0, before = 0, had, rc;

 if (!p || !tool || !p->cwd)
  return 0;
 for (row = startplan_startup_files; row->tool; row++)
 {
  if (!row->enabled || strcmp(row->tool, tool) != 0)
   continue;
  if (row->alternative && before)
   continue;
  had = p->npath;
  if (row->of_program)
   rc = startplan_add_program_startup(p, row, program);
  else
   rc = startplan_add_startup(p, row);
  if (rc < 0)
   return -1;
  before = rc;
  if (p->npath > had)
   found++;
 }
 return found;
}

/**
 * wpe_startplan_tool_reads_known - Does the table know what this tool runs?
 * @tool: the tool's name.
 * Return: 1 when one of its rows is on.
 */
int wpe_startplan_tool_reads_known(const char *tool)
{
 const startplan_startup_file *row;

 for (row = startplan_startup_files; tool && row->tool; row++)
  if (row->enabled && strcmp(row->tool, tool) == 0)
   return 1;
 return 0;
}
