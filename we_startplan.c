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
#include <stdint.h>
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
 p->program_from = WPE_FROM_USER;
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
  int a;

  free(p->path[i].path);
  free(p->path[i].root);
  free(p->path[i].source);
  free(p->path[i].content);
  for (a = 0; a < p->path[i].nadmit; a++)
   free(p->path[i].admit[a]);
  free(p->path[i].admit);
 }
 for (i = 0; i < p->ninput; i++)
 {
  free(p->input[i].text);
  free(p->input[i].source);
  free(p->input[i].file);
 }
 free(p->input);
 for (i = 0; i < p->ninit; i++)
 {
  free(p->init[i].text);
  free(p->init[i].source);
 }
 free(p->init);
 free(p->program_dir);
 free(p->program_source);
 if (p->debug_dir)
  wpe_startplan_debug_dir_end(p->debug_dir);
 free(p->debug_dir);
 free(p->launch_cwd);
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
 * startplan_base - The folder a relative path of the plan means, and the
 * start-up files are listed from: the program's folder when the debugger
 * starts apart from it, else the working folder.
 * @p: the plan.
 * Return: the folder, or NULL when the plan has none.
 */
static const char *startplan_base(const WpeStartPlan *p)
{
 if (p->program_dir)
  return p->program_dir;
 return p->cwd;
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
 w->on_refusal = WPE_REFUSAL_STOPS;
 w->file = NULL;
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
 char real[PATH_MAX];
 int resolved;

 if (!path || startplan_grow((void **)&p->path, p->npath, sizeof *p->path) < 0)
  return -1;
 s = &p->path[p->npath];
 memset(s, 0, sizeof *s);
 s->path = startplan_dup(path);
 s->source = startplan_dup(startplan_or_empty(source));
 resolved = root && *root && realpath(root, real);
 s->root = resolved ? startplan_dup(real) : NULL;
 s->role = role;
 s->from = from;
 /* A root that does not resolve is no root; one that resolved and could
    not be kept is out of memory, and the path is refused, not judged
    against another folder. */
 if (!s->path || !s->source || (resolved && !s->root))
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
 * wpe_startplan_operand_in - The path of a file the user named, as the
 * program that runs it gets it.
 * @out/@n: receive the path; @dir: the folder a bare or relative name is
 * in; @name: the name as picked or typed.
 *
 * A name typed with its folder ("/usr/bin/id") is kept as typed.  Any other
 * is a file of @dir, so it gets the folder in front ("./name") and is never
 * looked up on PATH.
 * Return: 0, or -1 when it does not fit.
 */
int wpe_startplan_operand_in(char *out, size_t n, const char *dir, const char *name)
{
 if (name && name[0] == STARTPLAN_SEP)
  return wpe_startplan_operand(out, n, NULL, name);
 return wpe_startplan_operand(out, n, dir, name);
}

/**
 * startplan_append - Append one component to a resolved path.
 * @out/@sz: the path so far ("/" or "/a/b"); @comp: the component.
 * Return: 0, or -1 when it does not fit, with errno ENAMETOOLONG.
 */
static int startplan_append(char *out, size_t sz, const char *comp)
{
 size_t len = strlen(out), clen = strlen(comp);
 size_t sep = (len > 0 && out[len - 1] == STARTPLAN_SEP) ? 0 : 1;

 if (len + sep + clen + 1 > sz)
 {
  errno = ENAMETOOLONG;
  return -1;
 }
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
 if (depth >= STARTPLAN_LINKS_MAX)
  return -1;
 if (strlen(out) + 1 > sizeof parent)
 {
  errno = ENAMETOOLONG;
  return -1;
 }
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
 {
  errno = ENAMETOOLONG;
  return -1;
 }
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
 * Return: 0, or -1 when it cannot be resolved; errno is ENAMETOOLONG when
 * the reason is that the path does not fit E_D_REQ_PATH_MAX.
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
  {
   errno = ENAMETOOLONG;
   return -1;
  }
  memcpy(out, cwd, strlen(cwd) + 1);
 }
 else if (getcwd(here, sizeof here) && strlen(here) + 1 <= sz)
  memcpy(out, here, strlen(here) + 1);
 else
  return -1;
 if (strlen(path) + 1 > sizeof work)
 {
  errno = ENAMETOOLONG;
  return -1;
 }
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
 return startplan_base(p);
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

static int plan_debugger_apart(WpeStartPlan *p);

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
  if (wpe_startplan_resolve(s->path, startplan_base(p), s->resolved, sizeof s->resolved) < 0)
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
 startplan_put(&b, "cwd ", startplan_or_empty(startplan_base(p)));
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
 for (i = 0; i < p->ninit; i++)
  startplan_put(&b, "init ", p->init[i].text);
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
 if (plan_debugger_apart(p) < 0)
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

static const char *startplan_apart_breach(const WpeStartPlan *p);

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
 if (startplan_apart_breach(run))
  return startplan_refuse(p, startplan_apart_breach(run), NULL);
 if (!wpe_startplan_covers(run, argv, &bad))
  return startplan_refuse(p, WPE_STARTPLAN_REFUSE_WORD, bad);
 for (n = 0; argv[n]; n++)
  ;
 if (!(words = calloc((size_t)n + 1, sizeof *words)))
  return -1;
 for (i = 0; i < n; i++)
  words[i] = startplan_holds(run, argv[i]);
 startplan_spec(&spec, io, words, run);
 spec.cwd = run->cwd ? run->cwd : p->cwd;
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
 w->on_refusal = WPE_REFUSAL_STOPS;
 w->file = NULL;
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

 if (p && p->load_refused)
  return 1;
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
     Measured with Python 3.14: ./.pdbrc is read once, when the debugger is
     built, from the folder it starts in, and not again on restart.
   - jdb: OpenJDK test/jdk/com/sun/jdi/JdbReadTwiceTest.sh -- "jdb.ini",
     else ".jdbrc", in user.home and in the working folder, both read, the
     same file through a link once.  Measured with JDK 25: the working
     folder's jdb.ini is read ("Reading commands from ...").  Measured with
     JDK 26: its read command takes one word and an absolute path.
   - gdb: gdb manual, "Init File in the Current Directory" -- ./.gdbinit is
     read only when the auto-load safe-path admits the folder, and only from
     the folder gdb starts in (measured).
     gdb manual, "Auto-loading extensions" ("objfile-gdb.ext file") -- a
     script named after the program, <program>-gdb.gdb, -gdb.py or
     -gdb.scm, beside it (its real path first, then the name it was given)
     runs under the same safe-path rule.
     gdb manual, "The .debug_gdb_scripts section" -- scripts carried inside
     the program, under the same safe-path rule (measured on gdb 18.1: a
     file entry is looked up in gdb's working folder).
     The rows of gdb (console and MI), pdb and jdb start apart
     (startplan_debugger_rows), so for them these rows only say what the box
     shows and what Run it loads; the notes below stay for gdb as a Rust
     adapter, which still starts in the folder.
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
 int of_program;             /* STARTPLAN_BESIDE: file is a suffix to the program's own
                                name, and the script lies beside the program;
                                STARTPLAN_INSIDE: file is a section of the program */
} startplan_startup_file;

/* Where a start-up file of the program lies (startplan_startup_file.of_program). */
#define STARTPLAN_IN_FOLDER 0   /* the working folder                                 */
#define STARTPLAN_BESIDE    1   /* beside the program, named after it                 */
#define STARTPLAN_INSIDE    2   /* inside the program, in a section of it             */

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
 { "gdb",  WPE_STARTPLAN_GDB_SECTION, 0, 0, 1, STARTPLAN_GDB_AUTOLOAD_NOTE, STARTPLAN_INSIDE },
 { "lldb", ".lldbinit", 0, 0, 1,
   "lldb runs it only if your ~/.lldbinit says load-cwd-lldbinit", 0 },
 { "dbx",  ".dbxinit",  0, 1, 1, NULL, 0 },
 { "xdb",  ".xdbrc",    0, 1, 1, NULL, 0 },
 { "perl", ".perldb",   0, 1, 0, NULL, 0 },
 { NULL,   NULL,        0, 0, 0, NULL, 0 }
};

/**
 * startplan_note_of - The sentence the box adds to a listed start-up file.
 * @p: the plan; @row: the file's row.
 *
 * A file the debugger would read only under the user's own settings carries
 * a condition.  When the debugger starts apart the condition no longer
 * exists: the file runs only when the user chose Run it.
 * Return: the sentence, or NULL.
 */
static const char *startplan_note_of(const WpeStartPlan *p, const startplan_startup_file *row)
{
 return wpe_startplan_is_apart(p) ? NULL : row->note;
}

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
 if (wpe_startplan_path(p, given, WPE_PATH_READS, WPE_FROM_FOLDER, name, startplan_base(p)) < 0)
  return -1;
 s = &p->path[p->npath - 1];
 s->note = startplan_note_of(p, row);
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

 if (snprintf(full, sizeof full, "%s%c%s", startplan_base(p), STARTPLAN_SEP, row->file) >=
     (int)sizeof full)
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
  n = snprintf(full, sz, "%s%c%s%s", startplan_base(p), STARTPLAN_SEP, program + 2, suffix);
  m = snprintf(given, sz, "%s%s", program, suffix);
 }
 else
 {
  n = snprintf(full, sz, "%s%c%s%s", startplan_base(p), STARTPLAN_SEP, program, suffix);
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

/* The parts of an ELF file the section reader reads (elf(5)).  Both classes,
   both byte orders; a number is read byte by byte, never through a cast. */
#define STARTPLAN_ELF_MAGIC       "\177ELF"
#define STARTPLAN_ELF_MAGIC_LEN   4
#define STARTPLAN_ELF_IDENT_LEN   16
#define STARTPLAN_ELF_CLASS_AT    4
#define STARTPLAN_ELF_DATA_AT     5
#define STARTPLAN_ELF_CLASS_32    1
#define STARTPLAN_ELF_CLASS_64    2
#define STARTPLAN_ELF_LSB         1
#define STARTPLAN_ELF_MSB         2
#define STARTPLAN_ELF32_HEADER    52
#define STARTPLAN_ELF64_HEADER    64
#define STARTPLAN_ELF32_SECTION   40
#define STARTPLAN_ELF64_SECTION   64
#define STARTPLAN_ELF_NOBITS      8        /* a section with no bytes in the file */
/* The section names' table is read whole: a program's is a few hundred bytes. */
#define STARTPLAN_ELF_NAMES_MAX   1048576

/* What the section reader knows of the file it reads. */
typedef struct {
 int wide;                   /* 64-bit class                                        */
 int big;                    /* big-endian                                          */
 uint64_t file_size;
 uint64_t table;             /* where the section headers start                     */
 unsigned entsize;           /* the size of one section header                      */
 unsigned count;             /* how many there are                                  */
 unsigned names;             /* the index of the section that holds their names    */
} startplan_elf;

/* One section header: what the reader needs of it. */
typedef struct {
 uint64_t name;              /* offset of its name in the names' table              */
 uint64_t type;
 uint64_t offset;
 uint64_t size;
} startplan_elf_section;

/**
 * startplan_elf_num - A number of an ELF file.
 * @b: its bytes; @size: how many; @big: 1 when the first is the most significant.
 */
static uint64_t startplan_elf_num(const unsigned char *b, unsigned size, int big)
{
 uint64_t v = 0;
 unsigned i;

 for (i = 0; i < size; i++)
 {
  if (big)
   v = (v << 8) | b[i];
  else
   v |= (uint64_t)b[i] << (8 * i);
 }
 return v;
}

/**
 * startplan_elf_fits - Do @n bytes at @off lie inside the file?
 * Return: 1 when they do (no sum can overflow), else 0.
 */
static int startplan_elf_fits(const startplan_elf *e, uint64_t off, uint64_t n)
{
 return n <= e->file_size && off <= e->file_size - n;
}

/**
 * startplan_elf_read - Read @n bytes of the file at @off.
 * Return: 0, or -1 when they are not all inside the file.
 */
static int startplan_elf_read(int fd, const startplan_elf *e, uint64_t off, void *buf, size_t n)
{
 size_t done = 0;
 ssize_t got;

 if (!startplan_elf_fits(e, off, n))
  return -1;
 while (done < n)
 {
  got = pread(fd, (char *)buf + done, n - done, (off_t)(off + done));
  if (got <= 0)
   return -1;
  done += (size_t)got;
 }
 return 0;
}

/**
 * startplan_elf_fields - Take the section table's place from the file header.
 * @h: the header; @e: its class and byte order set, receives the fields.
 * Return: 1 when the table is whole inside the file, else 0.
 */
static int startplan_elf_fields(const unsigned char *h, startplan_elf *e)
{
 unsigned least;

 if (e->wide)
 {
  e->table = startplan_elf_num(h + 0x28, 8, e->big);
  e->entsize = (unsigned)startplan_elf_num(h + 0x3A, 2, e->big);
  e->count = (unsigned)startplan_elf_num(h + 0x3C, 2, e->big);
  e->names = (unsigned)startplan_elf_num(h + 0x3E, 2, e->big);
  least = STARTPLAN_ELF64_SECTION;
 }
 else
 {
  e->table = startplan_elf_num(h + 0x20, 4, e->big);
  e->entsize = (unsigned)startplan_elf_num(h + 0x2E, 2, e->big);
  e->count = (unsigned)startplan_elf_num(h + 0x30, 2, e->big);
  e->names = (unsigned)startplan_elf_num(h + 0x32, 2, e->big);
  least = STARTPLAN_ELF32_SECTION;
 }
 if (e->count == 0 || e->names >= e->count || e->entsize < least)
  return 0;
 return startplan_elf_fits(e, e->table, (uint64_t)e->count * e->entsize);
}

/**
 * startplan_elf_open_header - Read the header of an ELF file.
 * @fd: the file; @e: receives what the reader needs.
 * Return: 1 when it is an ELF file with a whole section table, else 0 (not
 * ELF, another class, cut short, a table that does not fit the file).
 */
static int startplan_elf_open_header(int fd, startplan_elf *e)
{
 unsigned char h[STARTPLAN_ELF64_HEADER];
 struct stat st;
 size_t len;

 if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
  return 0;
 e->file_size = (uint64_t)st.st_size;
 if (startplan_elf_read(fd, e, 0, h, STARTPLAN_ELF_IDENT_LEN) != 0 ||
     memcmp(h, STARTPLAN_ELF_MAGIC, STARTPLAN_ELF_MAGIC_LEN) != 0)
  return 0;
 if (h[STARTPLAN_ELF_CLASS_AT] != STARTPLAN_ELF_CLASS_32 &&
     h[STARTPLAN_ELF_CLASS_AT] != STARTPLAN_ELF_CLASS_64)
  return 0;
 if (h[STARTPLAN_ELF_DATA_AT] != STARTPLAN_ELF_LSB && h[STARTPLAN_ELF_DATA_AT] != STARTPLAN_ELF_MSB)
  return 0;
 e->wide = h[STARTPLAN_ELF_CLASS_AT] == STARTPLAN_ELF_CLASS_64;
 e->big = h[STARTPLAN_ELF_DATA_AT] == STARTPLAN_ELF_MSB;
 len = e->wide ? STARTPLAN_ELF64_HEADER : STARTPLAN_ELF32_HEADER;
 if (startplan_elf_read(fd, e, 0, h, len) != 0)
  return 0;
 return startplan_elf_fields(h, e);
}

/**
 * startplan_elf_section_at - Read the @i'th section header.
 * Return: 0, or -1 when it cannot be read.
 */
static int startplan_elf_section_at(int fd, const startplan_elf *e, unsigned i,
                                    startplan_elf_section *s)
{
 unsigned char b[STARTPLAN_ELF64_SECTION];
 size_t len = e->wide ? STARTPLAN_ELF64_SECTION : STARTPLAN_ELF32_SECTION;

 if (startplan_elf_read(fd, e, e->table + (uint64_t)i * e->entsize, b, len) != 0)
  return -1;
 s->name = startplan_elf_num(b, 4, e->big);
 s->type = startplan_elf_num(b + 4, 4, e->big);
 if (e->wide)
 {
  s->offset = startplan_elf_num(b + 24, 8, e->big);
  s->size = startplan_elf_num(b + 32, 8, e->big);
 }
 else
 {
  s->offset = startplan_elf_num(b + 16, 4, e->big);
  s->size = startplan_elf_num(b + 20, 4, e->big);
 }
 return 0;
}

/**
 * startplan_elf_has_bytes - Does the section hold bytes that lie in the file?
 */
static int startplan_elf_has_bytes(const startplan_elf *e, const startplan_elf_section *s)
{
 return s->type != STARTPLAN_ELF_NOBITS && startplan_elf_fits(e, s->offset, s->size);
}

/**
 * startplan_elf_names - Read the table of the sections' names.
 * Return: the names, malloc'd and ended by a NUL, with their size in @len,
 * or NULL when there is no such table in the file.
 */
static char *startplan_elf_names(int fd, const startplan_elf *e, size_t *len)
{
 startplan_elf_section s;
 char *names;

 if (startplan_elf_section_at(fd, e, e->names, &s) != 0 || !startplan_elf_has_bytes(e, &s) ||
     s.size > STARTPLAN_ELF_NAMES_MAX)
  return NULL;
 if (!(names = malloc((size_t)s.size + 1)))
  return NULL;
 if (startplan_elf_read(fd, e, s.offset, names, (size_t)s.size) != 0)
 {
  free(names);
  return NULL;
 }
 names[s.size] = '\0';
 *len = (size_t)s.size;
 return names;
}

/**
 * startplan_elf_find - Find a section by its name.
 * @names/@len: the names' table; @name: the section's name; @s: receives it.
 * Return: 1 when found with its bytes in the file, else 0.
 */
static int startplan_elf_find(int fd, const startplan_elf *e, const char *names, size_t len,
                              const char *name, startplan_elf_section *s)
{
 unsigned i;

 for (i = 0; i < e->count; i++)
 {
  if (startplan_elf_section_at(fd, e, i, s) != 0)
   return 0;
  if (s->name < len && strcmp(names + s->name, name) == 0)
   return startplan_elf_has_bytes(e, s);
 }
 return 0;
}

/**
 * wpe_startplan_elf_section - Read a named section of an ELF program.
 * @file: the program; @name: the section (".debug_gdb_scripts"); @out/@max:
 * receive its first bytes; @size: receives its whole size.
 *
 * The user is shown what gdb would run out of the program before it starts,
 * so the debug box can name a script that travels inside the executable and
 * not only one that lies beside it.  The file is read with its own header's
 * numbers only after each is checked against the file's size: a program
 * that is not ELF, is cut short, or whose headers point past its end has no
 * such section.
 * Return: 1 when the section is there, else 0.
 */
int wpe_startplan_elf_section(const char *file, const char *name, unsigned char *out, size_t max,
                              size_t *size)
{
 startplan_elf e;
 startplan_elf_section s;
 char *names;
 size_t len = 0, take;
 int fd, found = 0;

 if (!file || !name || !out || !size)
  return 0;
 if ((fd = open(file, O_RDONLY | O_NONBLOCK)) < 0)
  return 0;
 if (startplan_elf_open_header(fd, &e) && (names = startplan_elf_names(fd, &e, &len)) != NULL)
 {
  if (startplan_elf_find(fd, &e, names, len, name, &s))
  {
   take = s.size < max ? (size_t)s.size : max;
   if (startplan_elf_read(fd, &e, s.offset, out, take) == 0)
   {
    *size = (size_t)s.size;
    found = 1;
   }
  }
  free(names);
 }
 close(fd);
 return found;
}

/* The kinds of entry of gdb's .debug_gdb_scripts section (gdb manual,
   "The .debug_gdb_scripts section"): the first byte of each, followed by
   one NUL-ended string.  A file entry's string is a name gdb looks for in
   its working folder; a text entry's string is the script itself, its
   first line the script's name (measured on gdb 18.1). */
#define STARTPLAN_SCRIPT_PYTHON_FILE 1
#define STARTPLAN_SCRIPT_SCHEME_FILE 3
#define STARTPLAN_SCRIPT_PYTHON_TEXT 4
#define STARTPLAN_SCRIPT_SCHEME_TEXT 6

/**
 * startplan_script_is_file - Does this kind of entry name a file?
 */
static int startplan_script_is_file(int kind)
{
 return kind == STARTPLAN_SCRIPT_PYTHON_FILE || kind == STARTPLAN_SCRIPT_SCHEME_FILE;
}

/**
 * startplan_script_is_text - Does this kind of entry hold the script?
 */
static int startplan_script_is_text(int kind)
{
 return kind == STARTPLAN_SCRIPT_PYTHON_TEXT || kind == STARTPLAN_SCRIPT_SCHEME_TEXT;
}

/**
 * startplan_embedded_put - Add text to what the box shows of a program's
 * own scripts, ending its last line.
 * @s: the path; @text/@n: the text.
 *
 * When it does not all fit in what the box can show, the path is marked
 * too long, so Run it is not offered.
 */
static void startplan_embedded_put(WpeStartPath *s, const char *text, size_t n)
{
 size_t have = strlen(s->content);

 if (have + n + 1 > WPE_STARTPLAN_READS_MAX)
 {
  s->too_long = 1;
  return;
 }
 memcpy(s->content + have, text, n);
 have += n;
 if (n == 0 || text[n - 1] != '\n')
  s->content[have++] = '\n';
 s->content[have] = '\0';
}

/**
 * startplan_admit_add - Note a path gdb will check against its safe-path for
 * the scripts a program carries.
 * @s: the path; @what: the path to admit.
 *
 * Kept once.  A path that cannot be kept is left out: Run it then has one
 * script fewer to admit, and gdb refuses that script with its own warning.
 */
static void startplan_admit_add(WpeStartPath *s, const char *what)
{
 char **grown;
 int i;

 for (i = 0; i < s->nadmit; i++)
  if (strcmp(s->admit[i], what) == 0)
   return;
 if (!(grown = realloc(s->admit, (size_t)(s->nadmit + 1) * sizeof *grown)))
  return;
 s->admit = grown;
 if (!(s->admit[s->nadmit] = startplan_dup(what)))
  return;
 s->nadmit++;
}

/**
 * startplan_embedded_file - Add the text of the script file an entry names.
 * @s: the path; @cwd: the folder gdb runs in; @name: the file's name.
 *
 * gdb looks for a relative name in its working folder, so that is where the
 * file is read; an absolute name is read where it lies.  A file that is not
 * there is not run, and the box says so.
 */
static void startplan_embedded_file(WpeStartPath *s, const char *cwd, const char *name)
{
 char full[WPE_STARTPLAN_PATH_MAX];
 WpeStartPath file;
 char real[PATH_MAX];

 if (name[0] == STARTPLAN_SEP)
  snprintf(full, sizeof full, "%s", name);
 else if (snprintf(full, sizeof full, "%s%c%s", cwd, STARTPLAN_SEP, name) >= (int)sizeof full)
 {
  s->too_long = 1;
  return;
 }
 memset(&file, 0, sizeof file);
 if (access(full, F_OK) != 0)
 {
  startplan_embedded_put(s, "(not found)", strlen("(not found)"));
  return;
 }
 if (realpath(full, real))
  startplan_admit_add(s, real);
 startplan_read_startup(&file, full);
 if (file.too_long)
  s->too_long = 1;
 if (file.content)
  startplan_embedded_put(s, file.content, strlen(file.content));
 free(file.content);
}

/**
 * startplan_embedded_entry - Add one entry of the section to what the box shows.
 * @s: the path; @cwd: gdb's working folder; @raw/@n: the section; @at: where
 * the entry starts.
 * Return: where the next one starts, or @n when the rest cannot be read
 * (the path is then marked too long).
 */
static size_t startplan_embedded_entry(WpeStartPath *s, const char *cwd, const char *program,
                                       const unsigned char *raw, size_t n, size_t at)
{
 int kind = raw[at++];
 const char *text = (const char *)raw + at;
 const unsigned char *end = memchr(raw + at, '\0', n - at);
 size_t len;

 if (!end || (!startplan_script_is_file(kind) && !startplan_script_is_text(kind)))
 {
  s->too_long = 1;
  return n;
 }
 len = (size_t)(end - raw) - at;
 startplan_embedded_put(s, text, len);
 if (startplan_script_is_file(kind))
  startplan_embedded_file(s, cwd, text);
 else
  startplan_admit_add(s, program);
 return at + len + 1;
}

/**
 * startplan_embedded_text - What the box shows of the scripts inside a program.
 * @s: the path, its content allocated and empty; @cwd: gdb's working folder;
 * @program: the program's real path, which gdb checks for a text entry;
 * @raw/@n: the section as it is in the file.
 *
 * A file entry is its name, then the file's lines; a text entry is the
 * script itself, whose first line is its name.  Every entry gdb would run
 * is shown, so a text entry cannot hide a file entry behind it.
 */
static void startplan_embedded_text(WpeStartPath *s, const char *cwd, const char *program,
                                    const unsigned char *raw, size_t n)
{
 size_t at = 0;

 while (at < n && !s->too_long)
  at = startplan_embedded_entry(s, cwd, program, raw, n, at);
}

/**
 * startplan_add_embedded_startup - List the scripts gdb finds inside the
 * program, when it has a .debug_gdb_scripts section.
 * @p: the plan; @row: the table's row; @program: the program as the tool
 * gets it, or NULL.
 *
 * The program is read as a file, never run.  With no such section, or no
 * program, nothing is listed and "Use mine" adds no switch for it.
 * Return: 1 when the section was found and listed, 0 when not, -1 out of memory.
 */
static int startplan_add_embedded_startup(WpeStartPlan *p, const startplan_startup_file *row,
                                          const char *program)
{
 char full[WPE_STARTPLAN_PATH_MAX], given[WPE_STARTPLAN_PATH_MAX], shown[WPE_STARTPLAN_PATH_MAX];
 char real[PATH_MAX];
 unsigned char raw[WPE_STARTPLAN_READS_MAX];
 WpeStartPath *s;
 size_t size = 0;

 if (!program || !*program)
  return 0;
 if (startplan_program_paths(p, program, "", full, given, sizeof given) < 0)
  return 0;
 if (!wpe_startplan_elf_section(full, row->file, raw, sizeof raw, &size))
  return 0;
 if (snprintf(shown, sizeof shown, "%s (%s)", given, row->file) >= (int)sizeof shown)
  return 0;
 if (wpe_startplan_path(p, shown, WPE_PATH_READS, WPE_FROM_FOLDER, row->file, startplan_base(p)) < 0)
  return -1;
 s = &p->path[p->npath - 1];
 s->note = startplan_note_of(p, row);
 s->embedded = 1;
 if (!realpath(full, real))
  snprintf(real, sizeof real, "%s", full);
 s->length = (long)size;
 if (!(s->content = calloc(1, WPE_STARTPLAN_READS_MAX + 1)))
 {
  s->too_long = 1;
  return 1;
 }
 if (size > sizeof raw)
  s->too_long = 1;
 else
  startplan_embedded_text(s, startplan_base(p), real, raw, size);
 startplan_trace("startplan reads %s %ld", shown, s->length);
 return 1;
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

 if (!p || !tool || !startplan_base(p))
  return 0;
 for (row = startplan_startup_files; row->tool; row++)
 {
  if (!row->enabled || strcmp(row->tool, tool) != 0)
   continue;
  if (row->alternative && before)
   continue;
  had = p->npath;
  if (row->of_program == STARTPLAN_INSIDE)
   rc = startplan_add_embedded_startup(p, row, program);
  else if (row->of_program == STARTPLAN_BESIDE)
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

/* ---- A debugger starts apart from the program's folder ----------------
 *
 * A debugger the editor starts runs nothing from the program's folder before
 * the editor's first word.  Its process starts in a folder of the editor's own
 * that is empty, so the init file of the working folder that every debugger
 * reads finds nothing; the program's folder is given to it afterwards by the
 * editor's own command, and where the debugger loads scripts by the objfile
 * the editor pins the debugger's own trust setting to its default before any
 * objfile is loaded.  What the folder holds is listed with its text, and
 * Run it loads exactly those files by the editor's own command.
 *
 * The rows below are data, one per debugger.  A row marked apart starts in
 * the empty folder; the others keep today's start, each with its reason.
 */

/* The folder the owner makes for one debug start. */
#define STARTPLAN_DEBUG_DIR_NAME   "xwpe_debug_XXXXXX"
#define STARTPLAN_DEFAULT_BASE     "/tmp"
#define STARTPLAN_TMPDIR_ENV       "TMPDIR"

/* One line the editor writes for a start, and where it goes. */
#define STARTPLAN_LK_INPUT  0   /* a line to the debugger's input                       */
#define STARTPLAN_LK_WORD   1   /* a word of the debugger's command line                */
#define STARTPLAN_LK_INIT   2   /* an adapter's launch init command                     */
#define STARTPLAN_LK_CWD    3   /* the folder an adapter's launch request names         */
#define STARTPLAN_LINES_MAX 24

/* A line the editor writes must hold a path of the longest form its encoder
   writes it in, and a start-up file's line must fit a command whole.  Each
   check is a typedef of a negative size when it is false, so a change to the
   request engine's numbers, or to the encoder's width, stops the build. */
typedef char startplan_command_holds_a_path[
 (E_D_REQ_CMD_MAX >= WPE_STARTPLAN_PATH_MAX * WPE_STARTPLAN_PYTHON_ESCAPE + 64) ? 1 : -1];
typedef char startplan_escape_is_the_encoders[
 (E_D_REQ_PATH_ESCAPE_MAX >= WPE_STARTPLAN_PYTHON_ESCAPE) ? 1 : -1];
typedef char startplan_file_line_fits_a_command[
 (WPE_STARTPLAN_READS_MAX < E_D_REQ_CMD_MAX) ? 1 : -1];

/* How a row's lines reach its debugger. */
#define STARTPLAN_CHAN_NONE   0
#define STARTPLAN_CHAN_INPUT  1   /* gdb's console or MI, pdb, jdb: the input          */
#define STARTPLAN_CHAN_WORD   2   /* gdb speaking DAP: -iex and -ix words              */
#define STARTPLAN_CHAN_LAUNCH 3   /* lldb-dap: initCommands and the launch cwd         */

/* How a row gives the program's folder, and how it loads one accepted file. */
#define STARTPLAN_MOVE_NONE      0
#define STARTPLAN_MOVE_GDB       1   /* cd                                             */
#define STARTPLAN_MOVE_MI        2   /* -environment-cd                                */
#define STARTPLAN_MOVE_GDB_DAP   3   /* -iex "cd ..." and the launch cwd               */
#define STARTPLAN_MOVE_LLDB_DAP  4   /* the launch cwd only: lldb never leaves its folder */
#define STARTPLAN_MOVE_PDB       5   /* os.chdir as pdb's first input                  */

#define STARTPLAN_LOAD_NONE      0
#define STARTPLAN_LOAD_GDB       1   /* add-auto-load-safe-path, source, then file     */
#define STARTPLAN_LOAD_MI        2   /* the same through -interpreter-exec console     */
#define STARTPLAN_LOAD_GDB_DAP   3   /* -iex add-auto-load-safe-path, -ix              */
#define STARTPLAN_LOAD_LLDB_DAP  4   /* command source in initCommands                 */
#define STARTPLAN_LOAD_PDB       5   /* the .pdbrc's lines, one by one                 */
#define STARTPLAN_LOAD_JDB       6   /* read                                           */

/* gdb 18's symbols in front of an error and a warning: emptied by these two
   lines (no argument), so a refusal reads as gdb's words. */
#define STARTPLAN_GDB_STYLE_ERROR   "set style error-prefix"
#define STARTPLAN_GDB_STYLE_WARNING "set style warning-prefix"

/* gdb's own default for where it may auto-load from; the folder is not in it. */
#define STARTPLAN_GDB_SAFE_PATH     "auto-load safe-path $debugdir:$datadir/auto-load"
#define STARTPLAN_GDB_PIN           "set " STARTPLAN_GDB_SAFE_PATH
#define STARTPLAN_MI_PIN            "-gdb-set " STARTPLAN_GDB_SAFE_PATH
#define STARTPLAN_LLDB_PIN          "settings set target.load-script-from-symbol-file false"
/* gdb's history file, and the two commands of the history rule. */
#define STARTPLAN_GDB_HISTORY_NAME  ".gdb_history"
#define STARTPLAN_GDB_HISTORY_FIX_FMT "set history filename %s"
#define STARTPLAN_HISTORY_ASK       "show history filename"
#define STARTPLAN_MI_HISTORY_ASK    "-gdb-show history filename"
#define STARTPLAN_MI_CONSOLE        "-interpreter-exec console "
#define STARTPLAN_IEX               "-iex"
#define STARTPLAN_IX                "-ix"
#define STARTPLAN_GDBINIT           ".gdbinit"
#define STARTPLAN_LLDBINIT          ".lldbinit"
#define STARTPLAN_PDBRC_COMMENT     '#'
/* What add-auto-load-safe-path reads as a list separator, a variable or a
   wildcard: a path holding one cannot be admitted alone. */
#define STARTPLAN_SAFE_PATH_REFUSED ":$*?["
/* pdb cuts an input line at ";;" into two commands. */
#define STARTPLAN_PDB_SEPARATOR     ';'
/* pdb's move: a statement that binds no name in the program's module. */
#define STARTPLAN_PDB_CHDIR_FMT     "!__import__('os').chdir(%s)"
/* The .pdbrc line that defines an alias, and what cuts a line into commands. */
#define STARTPLAN_PDB_ALIAS        "alias"
#define STARTPLAN_PDB_SPLIT         ";;"

struct startplan_row {
 const char *key;            /* e_d_type_names[]: "gdb", "pdb", "jdb", ..., or "dap"   */
 const char *variant;        /* "" or "mi" for gdb; the adapter's program for "dap"    */
 int apart;                  /* 1: the debugger starts in the empty folder (shipped)   */
 int designed;               /* what the design says once every pass has run           */
 const char *reason;         /* why it does not start apart (when apart is 0)          */
 int channel;                /* STARTPLAN_CHAN_*                                       */
 const char *pin;            /* the line that pins the debugger's trust, or NULL       */
 int move;                   /* STARTPLAN_MOVE_*                                       */
 int load;                   /* STARTPLAN_LOAD_*                                       */
};
typedef struct startplan_row startplan_row;

static const startplan_row startplan_debugger_rows[] = {
 { "gdb",  "",         1, 1, "", STARTPLAN_CHAN_INPUT,
   STARTPLAN_GDB_PIN, STARTPLAN_MOVE_GDB, STARTPLAN_LOAD_GDB },
 { "gdb",  "mi",       1, 1, "", STARTPLAN_CHAN_INPUT,
   STARTPLAN_MI_PIN, STARTPLAN_MOVE_MI, STARTPLAN_LOAD_MI },
 { "sdb",  "",         0, 0, "no cd", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { "dbx",  "",         0, 0, "unmeasured", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { "xdb",  "",         0, 0, "unmeasured", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { "jdb",  "",         1, 1, "", STARTPLAN_CHAN_INPUT,
   NULL, STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_JDB },
 { "pdb",  "",         1, 1, "", STARTPLAN_CHAN_INPUT,
   NULL, STARTPLAN_MOVE_PDB, STARTPLAN_LOAD_PDB },
 { "a68g", "",         0, 0, "is the program", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { "dap",  "gdb",      0, 1, "not yet moved to its own folder", STARTPLAN_CHAN_WORD,
   STARTPLAN_GDB_PIN, STARTPLAN_MOVE_GDB_DAP, STARTPLAN_LOAD_GDB_DAP },
 { "dap",  "lldb-dap", 0, 1, "not yet moved to its own folder", STARTPLAN_CHAN_LAUNCH,
   STARTPLAN_LLDB_PIN, STARTPLAN_MOVE_LLDB_DAP, STARTPLAN_LOAD_LLDB_DAP },
 { "dap",  "dlv",      0, 0, "builds the folder", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { "dap",  "scala-cli", 0, 0, "build server, honest rule", STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE },
 { NULL,   NULL,       0, 0, NULL, STARTPLAN_CHAN_NONE, NULL,
   STARTPLAN_MOVE_NONE, STARTPLAN_LOAD_NONE }
};

/* The folders the owner made and has not removed.  It grows: a folder the
   debugger wrote into (a log the user's own init file keeps by a relative
   name) cannot be removed when its session ends and stays here until the
   editor ends; a later start still gets a folder of its own, up to
   WPE_STARTPLAN_WORDS_MAX such folders in one editor run. */
static char **startplan_debug_dirs;
static int startplan_debug_ndirs;
static char *startplan_debug_base;
static int startplan_designed;

/**
 * wpe_startplan_rows_designed - Make every row start as the design says.
 * @on: 1 for the designed rows, 0 for the shipped ones.
 *
 * For the unit test and for the pass that measures a debugger before its row
 * is switched on: the editor never calls it, so no debugger starts apart
 * until its row says so.
 */
void wpe_startplan_rows_designed(int on)
{
 startplan_designed = on;
}

/**
 * wpe_startplan_row_info - Read one row of the table.
 * @index: 0 for the first; @variant/@apart/@designed/@reason: receive its
 * fields (each may be NULL).
 * Return: the row's key, or NULL past the last row.
 */
const char *wpe_startplan_row_info(int index, const char **variant, int *apart, int *designed,
                                   const char **reason)
{
 const startplan_row *row = &startplan_debugger_rows[0];
 int i;

 for (i = 0; i < index; i++)
 {
  if (!row->key)
   return NULL;
  row++;
 }
 if (!row->key)
  return NULL;
 if (variant)
  *variant = row->variant;
 if (apart)
  *apart = row->apart;
 if (designed)
  *designed = row->designed;
 if (reason)
  *reason = row->reason;
 return row->key;
}

/**
 * startplan_row_apart - Does this row start its debugger apart now?
 * Return: 1 when it does.
 */
static int startplan_row_apart(const startplan_row *row)
{
 if (startplan_designed)
  return row->designed;
 return row->apart;
}

/**
 * startplan_row_find - The row of a debugger.
 * @key: "gdb", "pdb", ..., "dap"; @variant: "", "mi", or the adapter's
 * program; NULL means "".
 * Return: the row, or NULL when the table does not know it.
 */
static const startplan_row *startplan_row_find(const char *key, const char *variant)
{
 const startplan_row *row;

 if (!variant)
  variant = "";
 for (row = startplan_debugger_rows; row->key; row++)
  if (strcmp(row->key, key) == 0 && strcmp(row->variant, variant) == 0)
   return row;
 return NULL;
}

/**
 * startplan_row_of - The row a plan was made for.
 * @p: the plan.
 *
 * The row contain() chose, else the one the plan's kind and name name; NULL
 * for a plan of any other kind or a name the table does not know.
 */
static const startplan_row *startplan_row_of(const WpeStartPlan *p)
{
 if (p->row)
  return p->row;
 if (p->kind == WPE_START_DEBUGGER)
  return startplan_row_find(p->name, "");
 if (p->kind == WPE_START_DEBUG_ADAPTER)
 {
  if (strcmp(p->name, "lldb") == 0)
   return startplan_row_find("dap", "lldb-dap");
  return startplan_row_find("dap", p->name);
 }
 return NULL;
}

/**
 * wpe_startplan_debug_base_set - Where the debug folders are made.
 * @base: the editor's own temporary folder, or NULL for $TMPDIR, else /tmp.
 */
void wpe_startplan_debug_base_set(const char *base)
{
 free(startplan_debug_base);
 startplan_debug_base = startplan_dup(base);
}

/**
 * startplan_debug_base - The folder a debug folder is made in.
 * @base: what the caller named, or NULL.
 */
static const char *startplan_debug_base_of(const char *base)
{
 const char *tmp;

 if (base && *base)
  return base;
 if (startplan_debug_base)
  return startplan_debug_base;
 tmp = getenv(STARTPLAN_TMPDIR_ENV);
 if (tmp && *tmp)
  return tmp;
 return STARTPLAN_DEFAULT_BASE;
}

/**
 * startplan_debug_dir_keep - Remember a folder the owner made, in a free
 * slot or a new one.
 * Return: 0, or -1 out of memory.
 */
static int startplan_debug_dir_keep(const char *dir)
{
 int i;

 for (i = 0; i < startplan_debug_ndirs; i++)
  if (!startplan_debug_dirs[i])
  {
   startplan_debug_dirs[i] = startplan_dup(dir);
   return startplan_debug_dirs[i] ? 0 : -1;
  }
 if (startplan_grow((void **)&startplan_debug_dirs, startplan_debug_ndirs,
                    sizeof *startplan_debug_dirs) < 0)
  return -1;
 startplan_debug_dirs[startplan_debug_ndirs] = startplan_dup(dir);
 if (!startplan_debug_dirs[startplan_debug_ndirs])
  return -1;
 startplan_debug_ndirs++;
 return 0;
}

/** startplan_is_debug_dir - Is this a folder the owner made and not yet ended? */
static int startplan_is_debug_dir(const char *dir)
{
 int i;

 for (i = 0; dir && i < startplan_debug_ndirs; i++)
  if (startplan_debug_dirs[i] && strcmp(startplan_debug_dirs[i], dir) == 0)
   return 1;
 return 0;
}

/**
 * wpe_startplan_debug_dir_new - Make the empty folder one debug start runs in.
 * @base: the editor's temporary folder (the C tests pass their own), or NULL.
 *
 * A fresh folder per start, mode 0700, empty: a debugger that reads an init
 * file from its working folder finds nothing there.  Its path is resolved,
 * and the owner remembers it, so a plan can be judged to run in a folder the
 * owner made.
 * Return: the folder's resolved path (free it), or NULL.
 */
char *wpe_startplan_debug_dir_new(const char *base)
{
 char tmpl[WPE_STARTPLAN_PATH_MAX], real[PATH_MAX];

 if (snprintf(tmpl, sizeof tmpl, "%s%c%s", startplan_debug_base_of(base), STARTPLAN_SEP,
              STARTPLAN_DEBUG_DIR_NAME) >= (int)sizeof tmpl)
  return NULL;
 if (!mkdtemp(tmpl))
  return NULL;
 if (!realpath(tmpl, real) || startplan_debug_dir_keep(real) < 0)
 {
  rmdir(tmpl);
  return NULL;
 }
 return startplan_dup(real);
}

/**
 * wpe_startplan_debug_dir_end - Remove the folder of a debug start, by its
 * exact path, when the session ends.
 * @dir: what wpe_startplan_debug_dir_new returned.
 * Return: 0 when removed, -1 when it is not one the owner made or is not empty.
 */
int wpe_startplan_debug_dir_end(const char *dir)
{
 int i;

 for (i = 0; dir && i < startplan_debug_ndirs; i++)
  if (startplan_debug_dirs[i] && strcmp(startplan_debug_dirs[i], dir) == 0)
  {
   if (rmdir(dir) != 0)
    return -1;
   free(startplan_debug_dirs[i]);
   startplan_debug_dirs[i] = NULL;
   return 0;
  }
 return -1;
}

/**
 * startplan_path_ok - May this path go into a command the editor writes?
 * @path: a resolved path; @trailing_blank_ok: 1 when a blank at its end is
 * kept (inside quotes).
 *
 * It must be absolute, so no debugger reads it as an option or as a name
 * looked up elsewhere, and hold no control character, so it cannot end the
 * command and start another.  A debugger that trims the end of a line (gdb)
 * would read a path with a blank at its end as another file.
 * Return: 1 when it may.
 */
static int startplan_path_ok(const char *path, int trailing_blank_ok)
{
 size_t n;

 if (!path || path[0] != STARTPLAN_SEP || !wpe_startplan_line_ok(path))
  return 0;
 n = strlen(path);
 if (!trailing_blank_ok && path[n - 1] == ' ')
  return 0;
 return 1;
}

/** startplan_copy_text - Copy a text into out; -1 when it does not fit. */
static int startplan_copy_text(char *out, size_t n, const char *text)
{
 if (strlen(text) >= n)
  return -1;
 memcpy(out, text, strlen(text) + 1);
 return 0;
}

/**
 * wpe_startplan_gdb_arg - A path as gdb's cd, source or file reads it.
 * @out/@n: receive it; @path: resolved and absolute; @form: WPE_GDB_ARG_RAW
 * for the commands that take the rest of the line as it is (cd, source),
 * WPE_GDB_ARG_SPLIT for the ones gdb splits into words (file), where a blank,
 * a quote or a backslash needs a backslash in front.
 *
 * Measured on gdb 18.1: cd and source take "a b" as it is and keep a
 * backslash as a backslash; file takes "a\ b" and refuses "a b".
 * Return: 0, or -1 for a path gdb cannot be given safely (a control
 * character, a blank at its end, not absolute) or one that does not fit.
 */
int wpe_startplan_gdb_arg(char *out, size_t n, const char *path, int form)
{
 size_t at = 0;

 if (!out || !n || !startplan_path_ok(path, 0))
  return -1;
 if (form == WPE_GDB_ARG_RAW)
  return startplan_copy_text(out, n, path);
 for (; *path; path++)
 {
  if (at + 3 > n)
   return -1;
  if (*path == ' ' || *path == '\'' || *path == '"' || *path == '\\')
   out[at++] = '\\';
  out[at++] = *path;
 }
 out[at] = '\0';
 return 0;
}

/**
 * wpe_startplan_gdb_safe_path_arg - A path as add-auto-load-safe-path reads it.
 * @out/@n: receive it; @path: resolved and absolute.
 *
 * The setting is a list of paths cut at each ':' and expanded at each '$', so
 * a path holding one would admit its first part whole, or a variable's
 * value: refused.  gdb also matches each entry as a wildcard pattern, so a
 * folder named with '*', '?' or '[' would admit the same file in other
 * folders: refused too.  Otherwise as the rest of the line.
 * Return: 0, or -1 as wpe_startplan_gdb_arg, or for ':', '$' and a wildcard.
 */
int wpe_startplan_gdb_safe_path_arg(char *out, size_t n, const char *path)
{
 if (!path || strpbrk(path, STARTPLAN_SAFE_PATH_REFUSED))
  return -1;
 return wpe_startplan_gdb_arg(out, n, path, WPE_GDB_ARG_RAW);
}

/**
 * startplan_mi_quote - Text as an MI c-string, with its escapes.
 * @out/@n: receive it with its quotes; @text: no control character.
 * Return: 0, or -1 when it does not fit or holds a control character.
 */
static int startplan_mi_quote(char *out, size_t n, const char *text)
{
 size_t at = 0;

 if (!out || !text || !wpe_startplan_line_ok(text) || n < 3)
  return -1;
 out[at++] = '"';
 for (; *text; text++)
 {
  if (at + 4 > n)
   return -1;
  if (*text == '"' || *text == '\\')
   out[at++] = '\\';
  out[at++] = *text;
 }
 out[at++] = '"';
 out[at] = '\0';
 return 0;
}

/**
 * wpe_startplan_mi_cstring - A path as the MI commands -environment-cd and
 * -file-exec-and-symbols read it.
 * @out/@n: receive it with its quotes; @path: resolved and absolute.
 *
 * Measured on gdb 18.1: -environment-cd trims a blank at the end even inside
 * the quotes, so such a path would name another folder: refused.
 * Return: 0, or -1 for a path that is not absolute, holds a control
 * character, ends in a blank or does not fit.
 */
int wpe_startplan_mi_cstring(char *out, size_t n, const char *path)
{
 if (!startplan_path_ok(path, 0))
  return -1;
 return startplan_mi_quote(out, n, path);
}

/**
 * wpe_startplan_lldb_source_arg - A path as lldb's "command source" reads it.
 * @out/@n: receive it in double quotes; @path: resolved and absolute.
 * Return: 0, or -1 for a path that is not absolute, holds a control
 * character or does not fit.
 */
int wpe_startplan_lldb_source_arg(char *out, size_t n, const char *path)
{
 if (!startplan_path_ok(path, 1))
  return -1;
 return startplan_mi_quote(out, n, path);
}

/**
 * wpe_startplan_jdb_read_arg - A path as jdb's read command takes it.
 * @out/@n: receive it; @path: resolved and absolute.
 *
 * jdb's read takes one word, so a path with a blank cannot be given.
 * Return: 0, or -1 for a path with a blank, a control character, or that is
 * not absolute.
 */
int wpe_startplan_jdb_read_arg(char *out, size_t n, const char *path)
{
 if (!startplan_path_ok(path, 0) || strchr(path, ' '))
  return -1;
 return startplan_copy_text(out, n, path);
}

/**
 * wpe_startplan_python_bytes - A path as a Python bytes literal, in pure ASCII.
 * @out/@n: receive it, b'...'; @path: resolved and absolute.
 *
 * Every byte over 0x7e, every control byte and ';' are written as \xNN, and
 * the quote and the backslash are escaped, so the literal holds the path's
 * bytes exactly whatever the locale, and pdb, which cuts a line at ";;", gets
 * it whole; a line break is refused (the literal is one input line).
 * Return: 0, or -1 for a path that is not absolute, holds a line break or does
 * not fit.
 */
int wpe_startplan_python_bytes(char *out, size_t n, const char *path)
{
 static const char hex[] = "0123456789abcdef";
 size_t at = 0;
 const unsigned char *s = (const unsigned char *)path;

 if (!out || !path || path[0] != STARTPLAN_SEP || n < 4)
  return -1;
 out[at++] = 'b';
 out[at++] = '\'';
 for (; *s; s++)
 {
  /* room for the widest escape, the closing quote and the terminator */
  if (*s == '\n' || *s == '\r' || at + WPE_STARTPLAN_PYTHON_ESCAPE + 2 > n)
   return -1;
  if (*s == '\'' || *s == '\\')
  {
   out[at++] = '\\';
   out[at++] = (char)*s;
  }
  else if (*s < ' ' || *s > '~' || *s == STARTPLAN_PDB_SEPARATOR)
  {
   out[at++] = '\\';
   out[at++] = 'x';
   out[at++] = hex[*s >> 4];
   out[at++] = hex[*s & 15];
  }
  else
   out[at++] = (char)*s;
 }
 out[at++] = '\'';
 out[at] = '\0';
 return 0;
}

/* The lines the editor writes for one start, in order.  Each line is a heap
   string of its own length, so a line of the longest command costs what it is
   and 24 of them are not 400 KiB of stack.  Always start one with
   startplan_lines_start and end it with startplan_lines_reset: text[0..n) are
   owned by the lines. */
typedef struct {
 int kind[STARTPLAN_LINES_MAX];
 int outcome[STARTPLAN_LINES_MAX];   /* WPE_REFUSAL_*: what a refusal of the line means */
 char *text[STARTPLAN_LINES_MAX];
 int n;
 const char *debugger;       /* the row's key: whose command length a line obeys    */
 int nomem;                  /* a line could not be kept: not a refusal of its path */
} startplan_lines;

/**
 * startplan_lines_reset - Forget every line and free its text.
 * @l: the lines; they hold no line afterwards and may be used again.
 */
static void startplan_lines_reset(startplan_lines *l)
{
 int i;

 for (i = 0; i < l->n; i++)
  free(l->text[i]);
 l->n = 0;
}

/**
 * startplan_lines_start - Begin the lines of one debugger's start.
 * @l: the lines, uninitialized; @row: the debugger whose commands they are.
 */
static void startplan_lines_start(startplan_lines *l, const startplan_row *row)
{
 l->n = 0;
 l->debugger = row->key;
 l->nomem = 0;
}

/**
 * startplan_lines_add - Add one line.
 * @l: the lines; @kind: STARTPLAN_LK_*; @head/@arg: the text is head then arg.
 *
 * A line to the debugger's input must be a command the request engine takes
 * for that debugger (e_d_req_cmd_fits), the one rule on a command's length.
 * Return: 0, or -1 when there is no room, the line is too long for its
 * debugger, or memory ran out (then l->nomem is set).
 */
static int startplan_lines_add(startplan_lines *l, int kind, const char *head, const char *arg)
{
 size_t room = strlen(head) + (arg ? strlen(arg) : 0) + 1;
 char *text;

 if (l->n >= STARTPLAN_LINES_MAX)
  return -1;
 if (!(text = malloc(room)))
 {
  l->nomem = 1;
  return -1;
 }
 snprintf(text, room, "%s%s", head, arg ? arg : "");
 if (kind == STARTPLAN_LK_INPUT && !e_d_req_cmd_fits(l->debugger, text))
 {
  free(text);
  return -1;
 }
 l->kind[l->n] = kind;
 l->outcome[l->n] = WPE_REFUSAL_STOPS;
 l->text[l->n++] = text;
 return 0;
}

/**
 * startplan_lines_add_quiet - Add one line whose refusal is ignored: a
 * nicety of the start, not part of its containment.
 * Return: as startplan_lines_add.
 */
static int startplan_lines_add_quiet(startplan_lines *l, int kind, const char *head)
{
 if (startplan_lines_add(l, kind, head, NULL) < 0)
  return -1;
 l->outcome[l->n - 1] = WPE_REFUSAL_QUIET;
 return 0;
}

/**
 * startplan_lines_gdb_word - Add the pair of words that hand gdb a command
 * at start-up (-iex "<command>").
 * Return: 0, or -1.
 */
static int startplan_lines_gdb_word(startplan_lines *l, const char *option, const char *command)
{
 if (startplan_lines_add(l, STARTPLAN_LK_WORD, option, NULL) < 0)
  return -1;
 return startplan_lines_add(l, STARTPLAN_LK_WORD, command, NULL);
}

/**
 * startplan_lines_pin - The row's pin line, in the channel it travels.
 * Return: 0, or -1.
 */
static int startplan_lines_pin(const startplan_row *row, startplan_lines *l)
{
 if (!row->pin)
  return 0;
 if (row->channel == STARTPLAN_CHAN_WORD)
  return startplan_lines_gdb_word(l, STARTPLAN_IEX, row->pin);
 if (row->channel == STARTPLAN_CHAN_LAUNCH)
  return startplan_lines_add(l, STARTPLAN_LK_INIT, row->pin, NULL);
 return startplan_lines_add(l, STARTPLAN_LK_INPUT, row->pin, NULL);
}

/**
 * startplan_lines_style - The lines that turn off gdb 18's error and warning
 * symbols for the session, after the pin.
 * @row: the row; @l: the lines.
 *
 * gdb 18 writes a symbol and a blank in front of every error and warning,
 * also over a pipe, so a refusal read from its error stream would begin with
 * a symbol and a warning could not be told from an error by its first word.
 * Only gdb's console row has them: gdb over MI says its refusals in a record
 * that carries no symbol.  The pin stays the editor's first command; a gdb
 * that has no such setting refuses these lines, and the start goes on.
 * Return: 0, or -1.
 */
static int startplan_lines_style(const startplan_row *row, startplan_lines *l)
{
 if (row->channel != STARTPLAN_CHAN_INPUT || row->load != STARTPLAN_LOAD_GDB)
  return 0;
 if (startplan_lines_add_quiet(l, STARTPLAN_LK_INPUT, STARTPLAN_GDB_STYLE_ERROR) < 0)
  return -1;
 return startplan_lines_add_quiet(l, STARTPLAN_LK_INPUT, STARTPLAN_GDB_STYLE_WARNING);
}

/**
 * startplan_cd_text - The command that changes the debugger's folder (gdb's
 * cd, MI's -environment-cd, pdb's os.chdir), in the words of
 * the row's interface.
 * @row: the row; @path: an absolute folder; @out/@n: receive it.
 *
 * One command holds the whole folder, however deep: the folder is a path the
 * system allows, and a command holds a path in its longest written form.
 * Return: 0, or -1 when the folder cannot be given safely.
 */
static int startplan_cd_text(const startplan_row *row, const char *path, char *out, size_t n)
{
 char arg[E_D_REQ_CMD_MAX];
 int wrote;

 if (row->move == STARTPLAN_MOVE_MI)
 {
  if (wpe_startplan_mi_cstring(arg, sizeof arg, path) < 0)
   return -1;
  wrote = snprintf(out, n, "-environment-cd %s", arg);
 }
 else if (row->move == STARTPLAN_MOVE_PDB)
 {
  if (wpe_startplan_python_bytes(arg, sizeof arg, path) < 0)
   return -1;
  wrote = snprintf(out, n, STARTPLAN_PDB_CHDIR_FMT, arg);
 }
 else
 {
  if (wpe_startplan_gdb_arg(arg, sizeof arg, path, WPE_GDB_ARG_RAW) < 0)
   return -1;
  wrote = snprintf(out, n, "cd %s", arg);
 }
 if (wrote < 0 || (size_t)wrote >= n)
  return -1;
 return 0;
}

/**
 * startplan_cd_add - Add the command that changes the debugger's folder to
 * the program's.
 * @row: the row; @folder: the program's folder, resolved; @l: the lines.
 * Return: 0, or -1 as startplan_cd_text, or when the command is longer than
 * the debugger takes.
 */
static int startplan_cd_add(const startplan_row *row, const char *folder, startplan_lines *l)
{
 char text[E_D_REQ_CMD_MAX];

 if (startplan_cd_text(row, folder, text, sizeof text) < 0)
  return -1;
 return startplan_lines_add(l, STARTPLAN_LK_INPUT, text, NULL);
}

/**
 * startplan_lines_move - The row's lines that give the program's folder.
 * @row: the row; @folder: the program's folder, resolved; @l: the lines.
 * Return: 0, or -1 when the folder cannot be given safely.
 */
static int startplan_lines_move(const startplan_row *row, const char *folder, startplan_lines *l)
{
 char arg[E_D_REQ_CMD_MAX], cmd[E_D_REQ_CMD_MAX];

 switch (row->move)
 {
 case STARTPLAN_MOVE_GDB:
 case STARTPLAN_MOVE_MI:
  return startplan_cd_add(row, folder, l);
 case STARTPLAN_MOVE_GDB_DAP:
  if (wpe_startplan_gdb_arg(arg, sizeof arg, folder, WPE_GDB_ARG_RAW) < 0)
   return -1;
  snprintf(cmd, sizeof cmd, "cd %s", arg);
  if (startplan_lines_gdb_word(l, STARTPLAN_IEX, cmd) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_CWD, folder, NULL);
 case STARTPLAN_MOVE_LLDB_DAP:
  return startplan_lines_add(l, STARTPLAN_LK_CWD, folder, NULL);
 case STARTPLAN_MOVE_PDB:
  return startplan_cd_add(row, folder, l);
 default:
  return 0;
 }
}

/**
 * startplan_lines_of - The containment lines of a row: the pin, then the move.
 * @row: the row; @folder: the program's folder; @l: started lines, which
 * receive them (any line they held is freed first).
 * Return: 0, or -1 when a path cannot be given safely.
 */
static int startplan_lines_of(const startplan_row *row, const char *folder, startplan_lines *l)
{
 startplan_lines_reset(l);
 if (startplan_lines_pin(row, l) < 0)
  return -1;
 if (startplan_lines_style(row, l) < 0)
  return -1;
 return startplan_lines_move(row, folder, l);
}

/**
 * startplan_insert - Put a word at a place in one of the plan's lists.
 * @list/@n: the list and its length; @at: where; @text/@from: the word.
 * Return: 0, or -1 out of memory or past the plan's size.
 */
static int startplan_insert(WpeStartWord **list, int *n, int at, const char *text, int role,
                            int from)
{
 WpeStartWord *w;

 if (at < 0 || at > *n || startplan_grow((void **)list, *n, sizeof **list) < 0)
  return -1;
 memmove(*list + at + 1, *list + at, (size_t)(*n - at) * sizeof **list);
 w = &(*list)[at];
 w->text = startplan_dup(text);
 w->source = startplan_dup("");
 w->role = role;
 w->from = from;
 w->on_refusal = WPE_REFUSAL_STOPS;
 w->file = NULL;
 if (!w->text || !w->source)
 {
  free(w->text);
  free(w->source);
  memmove(*list + at, *list + at + 1, (size_t)(*n - at) * sizeof **list);
  return -1;
 }
 (*n)++;
 return 0;
}

/**
 * startplan_apply_line - Put one line into the plan at the end of its list.
 * @p: the plan; @kind: STARTPLAN_LK_*; @text: the line.
 * Return: 0, or -1.
 */
static int startplan_apply_line(WpeStartPlan *p, int kind, const char *text, int outcome)
{
 if (kind == STARTPLAN_LK_INPUT)
 {
  if (startplan_insert(&p->input, &p->ninput, p->ninput, text, WPE_WORD_TEXT,
                       WPE_FROM_EDITOR) < 0)
   return -1;
  p->input[p->ninput - 1].on_refusal = outcome;
  return 0;
 }
 if (kind == STARTPLAN_LK_WORD)
  return startplan_insert(&p->word, &p->nword, p->nword, text, WPE_WORD_OPTION, WPE_FROM_EDITOR);
 if (kind == STARTPLAN_LK_INIT)
  return startplan_insert(&p->init, &p->ninit, p->ninit, text, WPE_WORD_TEXT, WPE_FROM_EDITOR);
 free(p->launch_cwd);
 p->launch_cwd = startplan_dup(text);
 return p->launch_cwd ? 0 : -1;
}

/**
 * startplan_apply_lines - Put the lines into the plan, in order.
 *
 * The lines were judged when they were made, so the only way to fail is for
 * want of memory, and the plan's refusal says that.
 * Return: 0, or -1 with p->contain_refused set.
 */
static int startplan_apply_lines(WpeStartPlan *p, const startplan_lines *l)
{
 int i;

 for (i = 0; i < l->n; i++)
  if (startplan_apply_line(p, l->kind[i], l->text[i], l->outcome[i]) < 0)
  {
   p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_MEMORY;
   return -1;
  }
 return 0;
}

/**
 * startplan_program_load - The line that loads the program, for the debuggers
 * that load it by a command (gdb's file, MI's -file-exec-and-symbols).
 * @row: the row; @program: the program, absolute; @l: started lines, which
 * receive the line (any line they held is freed first).
 *
 * Always the absolute path: a command holds a path of the longest form the
 * system allows.
 * Return: 0, or -1 when the program's path cannot be given safely.
 */
static int startplan_program_load(const startplan_row *row, const char *program,
                                  startplan_lines *l)
{
 char arg[E_D_REQ_CMD_MAX];

 startplan_lines_reset(l);
 if (row->load == STARTPLAN_LOAD_GDB)
 {
  if (wpe_startplan_gdb_arg(arg, sizeof arg, program, WPE_GDB_ARG_SPLIT) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_INPUT, "file ", arg);
 }
 if (row->load == STARTPLAN_LOAD_MI)
 {
  if (wpe_startplan_mi_cstring(arg, sizeof arg, program) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_INPUT, "-file-exec-and-symbols ", arg);
 }
 return 0;
}

/**
 * startplan_add_program - Add the program as an absolute path of the plan.
 * @p: the plan; @program: as the site names it; @out: receives it absolute;
 * @refusal: receives the plan's reason when it fails: a path longer than the
 * system allows, a path that cannot be given safely, or no memory.
 *
 * Rooted in the program's folder, so it is inside it, whatever the debugger's
 * own working folder is.
 * Return: 0, or -1 with *@refusal set.
 */
static int startplan_add_program(WpeStartPlan *p, const char *program, char *out, size_t sz,
                                 const char **refusal)
{
 errno = 0;
 if (wpe_startplan_resolve(program, p->program_dir, out, sz) < 0)
 {
  *refusal = errno == ENAMETOOLONG ? WPE_STARTPLAN_REFUSE_DEBUG_LONG
                                   : WPE_STARTPLAN_REFUSE_DEBUG_PATH;
  return -1;
 }
 if (wpe_startplan_path(p, out, WPE_PATH_INPUT, p->program_from,
                        p->program_source ? p->program_source : "the program",
                        p->program_dir) < 0)
 {
  *refusal = WPE_STARTPLAN_REFUSE_DEBUG_MEMORY;
  return -1;
 }
 return 0;
}

/**
 * wpe_startplan_program_origin - Say whose word the program is, before contain.
 * @p: the plan; @from: WPE_FROM_*; @source: what the box calls it, or NULL
 * for "the program".
 *
 * A program a folder names (a project's EXENAME, an xwperc's Executable)
 * that lies outside the folder makes the plan ask, as it did when the
 * program was a word of the command: contain keeps that origin on the path
 * it adds.
 */
void wpe_startplan_program_origin(WpeStartPlan *p, int from, const char *source)
{
 free(p->program_source);
 p->program_source = source ? startplan_dup(source) : NULL;
 p->program_from = from;
}

/**
 * startplan_lines_refusal - Why a start's lines could not be made.
 * @l: the lines that failed.
 * Return: the memory text when a line could not be kept, else the one for a
 * path the debugger cannot be given safely.
 */
static const char *startplan_lines_refusal(const startplan_lines *l)
{
 return l->nomem ? WPE_STARTPLAN_REFUSE_DEBUG_MEMORY : WPE_STARTPLAN_REFUSE_DEBUG_PATH;
}

/**
 * startplan_contain_apart_lines - Make the containment and load lines of a
 * debugger that starts apart and put them in the plan.
 * @p: the plan, its program folder set; @row: its row; @program: or NULL;
 * @lines: started lines, left holding what was made (the caller frees them).
 * Return: 0, or -1 with p->contain_refused saying why.
 */
static int startplan_contain_apart_lines(WpeStartPlan *p, const startplan_row *row,
                                         const char *program, startplan_lines *lines)
{
 char absolute[WPE_STARTPLAN_PATH_MAX];
 const char *refusal;

 if (startplan_lines_of(row, p->program_dir, lines) < 0)
 {
  p->contain_refused = startplan_lines_refusal(lines);
  return -1;
 }
 if (startplan_apply_lines(p, lines) < 0)
  return -1;
 p->load_at = p->ninput;
 p->load_word_at = p->nword;
 if (!program || !*program)
  return 0;
 if (startplan_add_program(p, program, absolute, sizeof absolute, &refusal) < 0)
 {
  p->contain_refused = refusal;
  return -1;
 }
 if (startplan_program_load(row, absolute, lines) < 0)
 {
  p->contain_refused = startplan_lines_refusal(lines);
  return -1;
 }
 return startplan_apply_lines(p, lines);
}

/**
 * startplan_contain_apart - The lines of a debugger that starts apart.
 * @p: the plan, its program folder set; @row: its row; @program: or NULL.
 * Return: 0, or -1 with p->contain_refused saying why.
 */
static int startplan_contain_apart(WpeStartPlan *p, const startplan_row *row, const char *program)
{
 startplan_lines lines;
 int rc;

 startplan_lines_start(&lines, row);
 rc = startplan_contain_apart_lines(p, row, program, &lines);
 startplan_lines_reset(&lines);
 return rc;
}

/**
 * startplan_make_debug_folder - Make the empty folder a debugger that starts
 * apart runs in, and make it the plan's working folder.
 * @p: the plan; a folder of an earlier contain is ended first.
 * Return: 0, or -1 with p->contain_refused saying the folder could not be made.
 */
static int startplan_make_debug_folder(WpeStartPlan *p)
{
 if (p->debug_dir)
  wpe_startplan_debug_dir_end(p->debug_dir);
 free(p->debug_dir);
 p->debug_dir = wpe_startplan_debug_dir_new(NULL);
 if (p->debug_dir && wpe_startplan_set_cwd(p, p->debug_dir) == 0)
  return 0;
 p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_FOLDER;
 return -1;
}

/**
 * wpe_startplan_contain - The one call a debugger site makes to start apart.
 * @p: the plan of a debugger or an adapter; @key: the debugger as the editor
 * names it ("gdb", "pdb", ..., "dap"); @program_tool: "" or "mi" for gdb, the
 * adapter's program for "dap", else NULL; @folder: the program's folder, or
 * NULL for the editor's own; @program: the program, or NULL.
 *
 * For a row that starts apart: the plan's working folder becomes a fresh empty
 * folder, the program's folder is kept as program_dir (what is listed and what
 * the canonical text names), and the plan gets the row's pin and move lines in
 * that order, then the program's load.  For any other row both folders are the
 * program's and nothing is added: today's start.  A debugger the table does
 * not know is refused.
 *
 * A folder or program whose path is longer than the system allows is refused
 * with WPE_STARTPLAN_REFUSE_DEBUG_LONG, for a row that starts apart; any
 * other folder that cannot be resolved still just fails, as it always did.
 * Return: 0, or -1 with the plan refused when it is judged.
 */
int wpe_startplan_contain(WpeStartPlan *p, const char *key, const char *program_tool,
                          const char *folder, const char *program)
{
 const startplan_row *row;
 char here[PATH_MAX], real[PATH_MAX];
 int apart;

 if (!p || !key)
  return -1;
 if (!(row = startplan_row_find(key, program_tool)))
 {
  p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_UNKNOWN;
  return -1;
 }
 if (!folder || !*folder)
  folder = getcwd(here, sizeof here);
 if (!folder)
  return -1;
 errno = 0;
 if (!realpath(folder, real))
 {
  if (errno == ENAMETOOLONG && startplan_row_apart(row))
   p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_LONG;
  return -1;
 }
 free(p->program_dir);
 p->program_dir = startplan_dup(real);
 p->row = row;
 p->contain_refused = NULL;
 apart = startplan_row_apart(row);
 if (!p->program_dir)
 {
  p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_MEMORY;
  return -1;
 }
 if (!apart)
 {
  startplan_trace("startplan contain %s apart=0 dir=%s", key, real);
  if (wpe_startplan_set_cwd(p, real) < 0)
  {
   p->contain_refused = WPE_STARTPLAN_REFUSE_DEBUG_MEMORY;
   return -1;
  }
  return 0;
 }
 if (startplan_make_debug_folder(p) < 0)
  return -1;
 startplan_trace("startplan contain %s apart=1 dir=%s", key, p->debug_dir);
 return startplan_contain_apart(p, row, program);
}

/**
 * wpe_startplan_debug_dir_take - Hand the plan's debug folder to the session.
 * @p: the plan that runs.
 *
 * The folder must outlive the plan, which is freed once the debugger has
 * started: the session ends it (wpe_startplan_debug_dir_end) when it is over.
 * Until then freeing the plan would end it.
 * Return: the folder's path (the caller frees it), or NULL when the plan
 * has none.
 */
char *wpe_startplan_debug_dir_take(WpeStartPlan *p)
{
 char *dir;

 if (!p)
  return NULL;
 dir = p->debug_dir;
 p->debug_dir = NULL;
 return dir;
}

/**
 * wpe_startplan_is_apart - Does this plan start its debugger apart from the
 * program's folder?
 * @p: the plan, after contain.
 * Return: 1 when it does: the debugger gets no program word and loads the
 * program by the editor's own command.
 */
int wpe_startplan_is_apart(const WpeStartPlan *p)
{
 return p && p->row && startplan_row_apart(p->row);
}

/**
 * wpe_startplan_program_path - The program contain gave the debugger to load.
 * @p: the plan.
 * Return: its absolute path, or NULL when the plan has none.
 */
const char *wpe_startplan_program_path(const WpeStartPlan *p)
{
 const char *source = p && p->program_source ? p->program_source : "the program";
 int i;

 for (i = 0; p && i < p->npath; i++)
  if (p->path[i].role == WPE_PATH_INPUT && p->path[i].source &&
      strcmp(p->path[i].source, source) == 0)
   return p->path[i].path;
 return NULL;
}

/**
 * wpe_startplan_debugger_of - The debugger a contained plan starts, as its
 * row names it.
 * @p: the plan, after contain.
 * Return: "gdb", "pdb", "jdb" ..., or NULL when the plan has no row.  It is
 * the name the request engine's command length is keyed by.
 */
const char *wpe_startplan_debugger_of(const WpeStartPlan *p)
{
 return p && p->row ? p->row->key : NULL;
}

/**
 * wpe_startplan_row_is_apart - Does a debugger start apart?
 * @key/@program_tool: as wpe_startplan_contain.
 * Return: 1 when its row says so, 0 when it does not or the table does not
 * know it.
 */
int wpe_startplan_row_is_apart(const char *key, const char *program_tool)
{
 const startplan_row *row = key ? startplan_row_find(key, program_tool) : NULL;

 return row && startplan_row_apart(row);
}

/**
 * wpe_startplan_editor_inputs - How many of the plan's input lines are the
 * editor's own (containment, and what Run it loads), at the start of the list.
 * @p: the plan.
 *
 * The lines a site adds after them (a folder's Arguments, shown as "Then
 * runs") are the user's or the folder's and are sent when the program runs.
 * Return: the count.
 */
int wpe_startplan_editor_inputs(const WpeStartPlan *p)
{
 int i;

 for (i = 0; p && i < p->ninput && p->input[i].from == WPE_FROM_EDITOR; i++)
  ;
 return p ? i : 0;
}

/**
 * wpe_startplan_input_outcome - What a debugger's refusal of one input line
 * means for the start.
 * @p: the plan; @index: the line's place in its input list.
 * Return: WPE_REFUSAL_STOPS, _SAID or _QUIET; STOPS for a place the plan
 * does not hold, since a line nobody vouched for is not ignored.
 */
int wpe_startplan_input_outcome(const WpeStartPlan *p, int index)
{
 if (!p || index < 0 || index >= p->ninput)
  return WPE_REFUSAL_STOPS;
 return p->input[index].on_refusal;
}

/**
 * wpe_startplan_input_file - The start-up file an input line is a line of.
 * @p: the plan; @index: the line's place in its input list.
 * Return: the file as the box lists it, or NULL for a line of the editor's own.
 */
const char *wpe_startplan_input_file(const WpeStartPlan *p, int index)
{
 if (!p || index < 0 || index >= p->ninput)
  return NULL;
 return p->input[index].file;
}

/**
 * startplan_history_fix_line - The line that moves gdb's history file to the
 * program's folder, in the form the row's interface takes.
 * @row: the row; @p: the plan; @out/@n: receive it.
 *
 * The path is the folder, a separator and the file's name, so it may pass the
 * system's path limit by those few bytes for a folder at the limit; it is
 * given to gdb whole and gdb says whether it can keep a history there.
 * Return: 0, or -1 when the path cannot be given to gdb safely.
 */
static int startplan_history_fix_line(const startplan_row *row, const WpeStartPlan *p, char *out,
                                      size_t n)
{
 char path[WPE_STARTPLAN_PATH_MAX + sizeof STARTPLAN_GDB_HISTORY_NAME + 1];
 char arg[E_D_REQ_CMD_MAX], cmd[E_D_REQ_CMD_MAX];

 if (snprintf(path, sizeof path, "%s%c%s", p->program_dir, STARTPLAN_SEP,
              STARTPLAN_GDB_HISTORY_NAME) >= (int)sizeof path)
  return -1;
 if (wpe_startplan_gdb_arg(arg, sizeof arg, path, WPE_GDB_ARG_RAW) < 0)
  return -1;
 snprintf(cmd, sizeof cmd, STARTPLAN_GDB_HISTORY_FIX_FMT, arg);
 if (row->load != STARTPLAN_LOAD_MI)
  return startplan_copy_text(out, n, cmd);
 if (startplan_mi_quote(arg, sizeof arg, cmd) < 0)
  return -1;
 snprintf(cmd, sizeof cmd, "%s%s", STARTPLAN_MI_CONSOLE, arg);
 return startplan_copy_text(out, n, cmd);
}

/**
 * wpe_startplan_history_lines - The two commands of gdb's history rule.
 * @p: the plan, after contain; @ask/@ask_n: receive the question; @fix/@fix_n:
 * receive the command that moves the history file.
 *
 * gdb's default history file is ".gdb_history" in the folder it started in,
 * which is the empty debug folder: a user who turned history saving on would
 * lose his per-project history there.  The session asks gdb where the file
 * is and, when the answer names the debug folder, sends the second command.
 * Return: 0, or -1 for a plan that is not a gdb one starting apart, or a
 * folder whose path gdb cannot be given safely (a control character), or
 * when @fix_n is too small for the command: a buffer of E_D_REQ_CMD_MAX
 * bytes always holds it.
 */
int wpe_startplan_history_lines(const WpeStartPlan *p, char *ask, size_t ask_n, char *fix,
                                size_t fix_n)
{
 const startplan_row *row = p ? p->row : NULL;

 if (!row || !startplan_row_apart(row) || !p->program_dir)
  return -1;
 if (row->load != STARTPLAN_LOAD_GDB && row->load != STARTPLAN_LOAD_MI)
  return -1;
 if (startplan_copy_text(ask, ask_n,
                         row->load == STARTPLAN_LOAD_MI ? STARTPLAN_MI_HISTORY_ASK
                                                        : STARTPLAN_HISTORY_ASK) < 0)
  return -1;
 return startplan_history_fix_line(row, p, fix, fix_n);
}

/**
 * startplan_lines_present - Does the plan hold these lines, in order?
 * @p: the plan; @l: the lines.
 * Return: 1 when every line is there, each kind in the order it was made.
 */
static int startplan_lines_present(const WpeStartPlan *p, const startplan_lines *l)
{
 int in = 0, word = 0, init = 0, i;

 for (i = 0; i < l->n; i++)
 {
  const char *text = l->text[i];

  if (l->kind[i] == STARTPLAN_LK_INPUT)
  {
   while (in < p->ninput && strcmp(p->input[in].text, text) != 0)
    in++;
   if (in++ >= p->ninput)
    return 0;
  }
  else if (l->kind[i] == STARTPLAN_LK_WORD)
  {
   while (word < p->nword && strcmp(p->word[word].text, text) != 0)
    word++;
   if (word++ >= p->nword)
    return 0;
  }
  else if (l->kind[i] == STARTPLAN_LK_INIT)
  {
   while (init < p->ninit && strcmp(p->init[init].text, text) != 0)
    init++;
   if (init++ >= p->ninit)
    return 0;
  }
  else if (!p->launch_cwd || strcmp(p->launch_cwd, text) != 0)
   return 0;
 }
 return 1;
}

/**
 * startplan_apart_lines_breach - Are the row's pin and move lines in the plan?
 * @p: the plan; @row: its row; @lines: started lines, left holding what was
 * made (the caller frees them).
 * Return: the reason they are not, or NULL when every one is there.
 */
static const char *startplan_apart_lines_breach(const WpeStartPlan *p, const startplan_row *row,
                                                startplan_lines *lines)
{
 if (startplan_lines_of(row, startplan_base(p), lines) < 0)
  return lines->nomem ? WPE_STARTPLAN_REFUSE_DEBUG_MEMORY : WPE_STARTPLAN_REFUSE_DEBUG_PIN;
 if (!startplan_lines_present(p, lines))
  return WPE_STARTPLAN_REFUSE_DEBUG_PIN;
 return NULL;
}

/**
 * startplan_apart_breach - Does this plan start a debugger the way its row
 * forbids?
 * @p: the plan.
 *
 * A row that starts apart needs the plan's working folder to be one the owner
 * made, and every pin and move line of the row in the plan, in order.  Nothing
 * here reads what was listed: containment does not depend on the listing.
 * Return: the reason, or NULL when the plan is fine (every row that does not
 * start apart, and every plan that is not a debugger's).
 */
static const char *startplan_apart_breach(const WpeStartPlan *p)
{
 const startplan_row *row;
 startplan_lines lines;
 const char *why;

 if (p->contain_refused)
  return p->contain_refused;
 if (p->kind != WPE_START_DEBUGGER && p->kind != WPE_START_DEBUG_ADAPTER)
  return NULL;
 row = startplan_row_of(p);
 if (!row || !startplan_row_apart(row))
  return NULL;
 if (!p->cwd || !startplan_is_debug_dir(p->cwd))
  return WPE_STARTPLAN_REFUSE_DEBUG_CWD;
 startplan_lines_start(&lines, row);
 why = startplan_apart_lines_breach(p, row, &lines);
 startplan_lines_reset(&lines);
 return why;
}

/**
 * plan_debugger_apart - Rule 11: a debugger that starts apart does.
 * @p: the plan.
 * Return: 0, or -1 with the plan refused.
 */
static int plan_debugger_apart(WpeStartPlan *p)
{
 const char *why = startplan_apart_breach(p);

 if (!why)
  return 0;
 p->verdict = WPE_PLAN_REFUSED;
 p->refused = why;
 return -1;
}

/**
 * startplan_named - Is this path a file of that name?
 */
static int startplan_named(const char *path, const char *name)
{
 return strcmp(startplan_last_name(path), name) == 0;
}

/**
 * startplan_load_input - Put one input line of what Run it loads at @at, with
 * what its refusal means and the start-up file it comes from.
 * @p: the plan; @at: the place in the input list; @text: the line;
 * @outcome: WPE_REFUSAL_*; @file: the file as the box lists it, or NULL.
 * Return: 0, or -1 out of memory.
 */
static int startplan_load_input(WpeStartPlan *p, int at, const char *text, int outcome,
                                const char *file)
{
 if (startplan_insert(&p->input, &p->ninput, at, text, WPE_WORD_TEXT, WPE_FROM_EDITOR) < 0)
  return -1;
 p->input[at].on_refusal = outcome;
 if (!file)
  return 0;
 p->input[at].file = startplan_dup(file);
 return p->input[at].file ? 0 : -1;
}

/**
 * startplan_load_at - Put one load line at the next place of its list.
 * @p: the plan; @l: the line made; @in/@word: where the next input and word go;
 * @outcome: what a refusal of an input line means (WPE_REFUSAL_*); @file: the
 * start-up file the line reads, as the box lists it, or NULL.
 * Return: 0, or -1.
 */
static int startplan_load_at(WpeStartPlan *p, const startplan_lines *l, int *in, int *word,
                             int outcome, const char *file)
{
 int i, rc;

 for (i = 0; i < l->n; i++)
 {
  if (l->kind[i] == STARTPLAN_LK_INPUT)
   rc = startplan_load_input(p, p->load_at + (*in)++, l->text[i], outcome, file);
  else if (l->kind[i] == STARTPLAN_LK_WORD)
   rc = startplan_insert(&p->word, &p->nword, p->load_word_at + (*word)++, l->text[i],
                         WPE_WORD_OPTION, WPE_FROM_EDITOR);
  else
   rc = startplan_apply_line(p, l->kind[i], l->text[i], outcome);
  if (rc < 0)
   return -1;
 }
 return 0;
}

/**
 * startplan_gdb_admit - The lines that let gdb load a script, in the form the
 * row's channel carries them.
 * @row: the row; @path: the script's path, or the program's; @l: receives them.
 * Return: 0, or -1 when the path cannot be given safely.
 */
static int startplan_gdb_admit(const startplan_row *row, const char *path, startplan_lines *l)
{
 char arg[E_D_REQ_CMD_MAX], cmd[E_D_REQ_CMD_MAX], quoted[E_D_REQ_CMD_MAX];

 if (wpe_startplan_gdb_safe_path_arg(arg, sizeof arg, path) < 0)
  return -1;
 snprintf(cmd, sizeof cmd, "add-auto-load-safe-path %s", arg);
 if (row->load == STARTPLAN_LOAD_GDB_DAP)
  return startplan_lines_gdb_word(l, STARTPLAN_IEX, cmd);
 if (row->load == STARTPLAN_LOAD_MI)
 {
  if (startplan_mi_quote(quoted, sizeof quoted, cmd) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_INPUT, "-interpreter-exec console ", quoted);
 }
 return startplan_lines_add(l, STARTPLAN_LK_INPUT, cmd, NULL);
}

/**
 * startplan_gdb_source - The line that makes gdb run the folder's .gdbinit.
 * @row: the row; @path: the file; @l: receives it.
 *
 * A .gdbinit runs before the program loads, as in stock gdb.
 * Return: 0, or -1 when the path cannot be given safely.
 */
static int startplan_gdb_source(const startplan_row *row, const char *path, startplan_lines *l)
{
 char arg[E_D_REQ_CMD_MAX], cmd[E_D_REQ_CMD_MAX], quoted[E_D_REQ_CMD_MAX];

 if (wpe_startplan_gdb_arg(arg, sizeof arg, path, WPE_GDB_ARG_RAW) < 0)
  return -1;
 if (row->load == STARTPLAN_LOAD_GDB_DAP)
 {
  if (startplan_lines_add(l, STARTPLAN_LK_WORD, STARTPLAN_IX, NULL) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_WORD, arg, NULL);
 }
 snprintf(cmd, sizeof cmd, "source %s", arg);
 if (row->load == STARTPLAN_LOAD_MI)
 {
  if (startplan_mi_quote(quoted, sizeof quoted, cmd) < 0)
   return -1;
  return startplan_lines_add(l, STARTPLAN_LK_INPUT, "-interpreter-exec console ", quoted);
 }
 return startplan_lines_add(l, STARTPLAN_LK_INPUT, cmd, NULL);
}

/**
 * startplan_load_gdb_lines - What Run it loads of one file for a gdb row,
 * made through @lines.
 * @p: the plan; @row: its row; @s: the listed path; @in/@word: positions;
 * @lines: started lines, left holding the last made (the caller frees them).
 * Return: 0, or -1 when a path cannot be given safely.
 */
static int startplan_load_gdb_lines(WpeStartPlan *p, const startplan_row *row,
                                    const WpeStartPath *s, int *in, int *word,
                                    startplan_lines *lines)
{
 char real[WPE_STARTPLAN_PATH_MAX];
 int i;

 if (s->embedded)
 {
  for (i = 0; i < s->nadmit; i++)
  {
   startplan_lines_reset(lines);
   if (startplan_gdb_admit(row, s->admit[i], lines) < 0 ||
       startplan_load_at(p, lines, in, word, WPE_REFUSAL_STOPS, NULL) < 0)
    return -1;
  }
  return 0;
 }
 if (wpe_startplan_resolve(s->path, startplan_base(p), real, sizeof real) < 0)
  return -1;
 startplan_lines_reset(lines);
 if (startplan_gdb_admit(row, real, lines) < 0 ||
     startplan_load_at(p, lines, in, word, WPE_REFUSAL_STOPS, NULL) < 0)
  return -1;
 if (!startplan_named(real, STARTPLAN_GDBINIT))
  return 0;
 startplan_lines_reset(lines);
 if (startplan_gdb_source(row, real, lines) < 0)
  return -1;
 return startplan_load_at(p, lines, in, word, WPE_REFUSAL_SAID, s->path);
}

/**
 * startplan_load_gdb - What Run it loads of one file for a gdb row.
 * @p: the plan; @row: its row; @s: the listed path; @in/@word: positions.
 *
 * A script the program carries is admitted by the paths gdb will check for it;
 * a script beside the program by its own path; a .gdbinit is admitted and then
 * run.
 * Return: 0, or -1 when a path cannot be given safely.
 */
static int startplan_load_gdb(WpeStartPlan *p, const startplan_row *row, const WpeStartPath *s,
                              int *in, int *word)
{
 startplan_lines lines;
 int rc;

 startplan_lines_start(&lines, row);
 rc = startplan_load_gdb_lines(p, row, s, in, word, &lines);
 startplan_lines_reset(&lines);
 return rc;
}

/**
 * startplan_pdbrc_line_sendable - Can this .pdbrc line be sent to pdb on its own?
 * @line: one line of the file.
 *
 * Every line is offered: what pdb answers to it decides, at the start, line by
 * line (a commands block is sent with its end, a line pdb reads from the
 * terminal stops the start there).  A line is never longer than a command: a
 * start-up file is read whole only up to WPE_STARTPLAN_READS_MAX bytes, which
 * is under a command's length (checked at the top of this section), so the
 * length test below only guards that arithmetic.
 * Return: 1 when it can.
 */
static int startplan_pdbrc_line_sendable(const char *debugger, const char *line)
{
 return e_d_req_cmd_fits(debugger, line);
}

/**
 * startplan_pdbrc_add - Add one command of a .pdbrc to the plan's input.
 * @p: the plan; @command: the command, no ";;" in it unless it is an alias.
 * @in: how many load lines were added so far; @file: the .pdbrc as listed.
 * Return: 0, or -1 when it cannot be sent (p->load_refused says which kind)
 * or does not fit.
 */
static int startplan_pdbrc_add(WpeStartPlan *p, const char *command, int *in, const char *file)
{
 if (!startplan_pdbrc_line_sendable(p->row->key, command))
 {
  p->load_refused = WPE_STARTPLAN_LOAD_REFUSED_PDBRC;
  return -1;
 }
 if (!wpe_startplan_line_ok(command))
  return -1;
 return startplan_load_input(p, p->load_at + (*in)++, command, WPE_REFUSAL_SAID, file);
}

/**
 * startplan_pdbrc_is_alias - Is this the line that defines an alias?
 * @line: one line of the file.
 *
 * pdb leaves an alias's ";;" for the alias's own expansion.
 */
static int startplan_pdbrc_is_alias(const char *line)
{
 const char *word = line + strspn(line, " \t");
 size_t n = strlen(STARTPLAN_PDB_ALIAS);

 return strncmp(word, STARTPLAN_PDB_ALIAS, n) == 0 &&
        (word[n] == '\0' || word[n] == ' ' || word[n] == '\t');
}

/**
 * startplan_pdbrc_line - Add the commands of one .pdbrc line.
 * @p: the plan; @line: the line; @in: how many load lines were added so far.
 *
 * pdb runs the parts of a line cut at ";;" one after the other, so each part
 * is sent as a command of its own.  The first part is trimmed at its end and the rest
 * at its start, as pdb does; a ";;" at the very start of the line, an alias
 * and an empty part are left whole or dropped as pdb leaves them.
 * Return: 0, or -1 as startplan_pdbrc_add.
 */
static int startplan_pdbrc_line(WpeStartPlan *p, const char *line, int *in, const char *file)
{
 char part[E_D_REQ_CMD_MAX];
 const char *marker;
 size_t len;

 while (line && *line)
 {
  marker = strstr(line, STARTPLAN_PDB_SPLIT);
  if (!marker || marker == line || startplan_pdbrc_is_alias(line))
   return startplan_pdbrc_add(p, line, in, file);
  len = (size_t)(marker - line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
   len--;
  memcpy(part, line, len);
  part[len] = '\0';
  if (startplan_pdbrc_add(p, part, in, file) < 0)
   return -1;
  line = marker + strlen(STARTPLAN_PDB_SPLIT);
  line += strspn(line, " \t");
 }
 return 0;
}

/**
 * startplan_load_pdb - Send a .pdbrc's lines one by one: pdb has no source.
 * @p: the plan; @s: the listed .pdbrc.
 *
 * Blank lines and comments are left out, as pdb leaves them, also when they
 * are indented: typed at pdb's prompt, a line of blanks would repeat the
 * command before it.  A line pdb cannot be given alone makes Run it
 * unavailable, and p->load_refused says which kind.  @one holds any line of a
 * file that was read (WPE_STARTPLAN_READS_MAX bytes at most).
 * Return: 0, or -1 when a line holds a control character or cannot be sent.
 */
static int startplan_load_pdb(WpeStartPlan *p, const WpeStartPath *s, int *in)
{
 const char *line = s->content, *end, *first;
 char one[E_D_REQ_CMD_MAX];
 size_t len;

 while (line && *line)
 {
  end = strchr(line, '\n');
  len = end ? (size_t)(end - line) : strlen(line);
  if (len >= sizeof one)
   return -1;
  memcpy(one, line, len);
  one[len] = '\0';
  line = end ? end + 1 : NULL;
  first = one + strspn(one, " \t");
  if (first[0] == '\0' || first[0] == STARTPLAN_PDBRC_COMMENT)
   continue;
  if (startplan_pdbrc_line(p, one, in, s->path) < 0)
   return -1;
 }
 return 0;
}

/**
 * startplan_load_other_lines - What Run it loads of one listed file for lldb's
 * adapter and jdb, made through @lines.
 * @p: the plan; @row: its row; @s: the path; @in/@word: positions;
 * @lines: started lines, left holding what was made (the caller frees them).
 * Return: 0, or -1 when its path cannot be given safely.
 */
static int startplan_load_other_lines(WpeStartPlan *p, const startplan_row *row,
                                      const WpeStartPath *s, int *in, int *word,
                                      startplan_lines *lines)
{
 char real[WPE_STARTPLAN_PATH_MAX], arg[E_D_REQ_CMD_MAX];

 if (wpe_startplan_resolve(s->path, startplan_base(p), real, sizeof real) < 0)
  return -1;
 if (row->load == STARTPLAN_LOAD_LLDB_DAP)
 {
  if (wpe_startplan_lldb_source_arg(arg, sizeof arg, real) < 0)
   return -1;
  if (startplan_lines_add(lines, STARTPLAN_LK_INIT, "command source ", arg) < 0)
   return -1;
 }
 else if (row->load == STARTPLAN_LOAD_JDB)
 {
  if (wpe_startplan_jdb_read_arg(arg, sizeof arg, real) < 0)
   return -1;
  if (startplan_lines_add(lines, STARTPLAN_LK_INPUT, "read ", arg) < 0)
   return -1;
 }
 return startplan_load_at(p, lines, in, word, WPE_REFUSAL_SAID, s->path);
}

/**
 * startplan_load_one - What Run it loads of one listed file.
 * @p: the plan; @row: its row; @s: the path; @in/@word: positions.
 * Return: 0, or -1 when its path cannot be given safely.
 */
static int startplan_load_one(WpeStartPlan *p, const startplan_row *row, const WpeStartPath *s,
                              int *in, int *word)
{
 startplan_lines lines;
 int rc;

 if (row->load == STARTPLAN_LOAD_PDB)
  return startplan_load_pdb(p, s, in);
 if (row->load == STARTPLAN_LOAD_GDB || row->load == STARTPLAN_LOAD_MI ||
     row->load == STARTPLAN_LOAD_GDB_DAP)
  return startplan_load_gdb(p, row, s, in, word);
 startplan_lines_start(&lines, row);
 rc = startplan_load_other_lines(p, row, s, in, word, &lines);
 startplan_lines_reset(&lines);
 return rc;
}

/**
 * wpe_startplan_load_lines - Add what Run it does: load the listed files by
 * the editor's own command.
 * @p: the plan the box showed, its start-up files listed.
 *
 * Each listed file's lines go after the move and before the program's load,
 * so a .gdbinit runs before the program loads as in stock gdb.  The user's own
 * plan ("Use mine") is the same plan without this call.  Called once; for a
 * row that does not start apart it adds nothing, since that debugger still
 * reads the files itself.
 * Return: the number of lines added, or -1 when a listed file's path cannot
 * be given to the debugger safely: Run it is then not offered, and
 * p->load_refused says why.
 */
int wpe_startplan_load_lines(WpeStartPlan *p)
{
 const startplan_row *row;
 int before, i, in = 0, word = 0;

 if (!p || !(row = p->row) || p->loaded || !startplan_row_apart(row))
  return 0;
 p->loaded = 1;
 before = p->ninput + p->nword + p->ninit;
 for (i = 0; i < p->npath; i++)
 {
  if (p->path[i].role != WPE_PATH_READS)
   continue;
  if (startplan_load_one(p, row, &p->path[i], &in, &word) < 0)
  {
   if (!p->load_refused)
    p->load_refused = WPE_STARTPLAN_LOAD_REFUSED;
   return -1;
  }
 }
 free(p->text);
 p->text = NULL;
 return p->ninput + p->nword + p->ninit - before;
}
