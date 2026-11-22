/* we_contain.c -- how the assistant's children are held.  See we_contain.h.
 *
 * The one place that decides what an assistant child is given: its
 * environment, the program it runs, its limits.  Everything here runs in the
 * editor's process before the fork, so it may allocate; the child only reads
 * the result (we_child.c).
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "we_path.h"
#include "we_platform.h"
#include "we_contain.h"

extern char **environ;

/* ---- the profiles --------------------------------------------------------- */

const WpeContainProfile wpe_contain_tool =
 { "tool", WPE_CONTAIN_ENGINE_NONE, 0, 0 };
const WpeContainProfile wpe_contain_hook =
 { "hook", WPE_CONTAIN_ENGINE_NONE, 0, 0 };
const WpeContainProfile wpe_contain_git =
 { "git", WPE_CONTAIN_ENGINE_NONE, 0, 0 };
const WpeContainProfile wpe_contain_git_commit =
 { "git-commit", WPE_CONTAIN_ENGINE_NONE, 0, 0 };
const WpeContainProfile wpe_contain_git_commit_ssh =
 { "git-commit", WPE_CONTAIN_ENGINE_NONE, 1, 0 };
const WpeContainProfile wpe_contain_cli_text =
 { "cli-text", WPE_CONTAIN_ENGINE_CLAUDE, 0, 1 };
const WpeContainProfile wpe_contain_host_claude =
 { "host-claude", WPE_CONTAIN_ENGINE_CLAUDE, 0, 1 };
const WpeContainProfile wpe_contain_host_command =
 { "host-command", WPE_CONTAIN_ENGINE_COMMAND, 0, 1 };

/* ---- the lists the environment is built from ------------------------------ */

/* Always given: the base of a working shell.  PATH and TERM are set apart. */
static const char *const contain_base_names[] = {
 "HOME", "LANG", "LANGUAGE", "TMPDIR", "USER", "LOGNAME", "TZ", NULL
};
#define CONTAIN_LOCALE_PREFIX "LC_"

/* Given when set: what the toolchains under a home need to find themselves. */
static const char *const contain_toolchain_names[] = {
 "JAVA_HOME", "GOPATH", "GOMODCACHE", "CARGO_HOME", "RUSTUP_HOME",
 "VIRTUAL_ENV", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS",
 "PKG_CONFIG_PATH", "SDKMAN_DIR", "NVM_DIR", NULL
};

/* Never given to any program of any profile, whatever a list says: they make
   the loader or the shell run code the user did not choose. */
static const char *const contain_injection_names[] = {
 "LD_PRELOAD", "LD_AUDIT", "BASH_ENV", "ENV", NULL
};

/* Given to no profile unless it is that profile's own login (an engine), the
   agent socket of a signed commit, or the user's own AIHostEnv for the host
   command: the provider keys, the pointers to key files, the agent sockets,
   the desktop, the container daemon. */
static const char *const contain_secret_names[] = {
 "ANTHROPIC_API_KEY", "OPENAI_API_KEY", "XWPE_AI_KEY", "SSH_AUTH_SOCK",
 "GPG_AGENT_INFO", "DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY",
 "XAUTHORITY", "XDG_RUNTIME_DIR", "DOCKER_HOST", NULL
};
static const char *const contain_secret_prefixes[] = {
 "ANTHROPIC_", "OPENAI_", "XWPE_AI_KEY", "GIT_", NULL
};
#define CONTAIN_LOADER_PATH "LD_LIBRARY_PATH"

/* The identity git reads from the environment: a commit the user's own
   setup signs with these must still be made. */
static const char *const contain_git_identity[] = {
 "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_AUTHOR_DATE",
 "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL", "GIT_COMMITTER_DATE", NULL
};

/* The editor's own pointers to a key file (ANTHROPIC_API_KEY_FILE, ...): an
   engine does not read them, and they name where a key lives. */
#define CONTAIN_KEY_POINTER_TAIL "_FILE"
static const char *const contain_pointer_prefixes[] = {
 "ANTHROPIC_", "OPENAI_", "XWPE_AI_", NULL
};

/* An engine keeps what it needs to reach its provider: its login and, for a
   user behind a proxy or a private authority, the way out. */
static const char *const contain_claude_prefixes[] = {
 "ANTHROPIC_", "CLAUDE_CODE_", "CLAUDE_CONFIG_DIR", NULL
};
static const char *const contain_command_prefixes[] = {
 "OPENAI_", NULL
};
static const char *const contain_route_names[] = {
 "HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY", "ALL_PROXY", "https_proxy",
 "http_proxy", "no_proxy", "all_proxy", "SSL_CERT_FILE", "SSL_CERT_DIR",
 "NODE_EXTRA_CA_CERTS", NULL
};

/* The prefixes the test seam may never name, so that it cannot be used to
   pass a key: the ones the lists above drop, and the ones that reach a
   desktop, an agent or the loader. */
static const char *const contain_pass_refused[] = {
 "ANTHROPIC_", "OPENAI_", "XWPE_AI_KEY", "SSH_", "DISPLAY", "WAYLAND_",
 "DBUS_", "XDG_RUNTIME", "LD_", "GIT_", "GPG_", "DOCKER_", "XAUTHORITY",
 "BASH_ENV", "ENV", NULL
};

/* The only prefix the test seam may name: the suites' stub variables. */
#define CONTAIN_PASS_PREFIX "XWPE_STUB_"
#define CONTAIN_DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin"
#define CONTAIN_TERM_VALUE   "TERM=dumb"
#define CONTAIN_VARNAME_LIMIT 256   /* the longest environment name read from an entry */

/* What a list reader says (contain_word_next, contain_folder_next). */
#define CONTAIN_WORD_TOO_LONG (-1)  /* a word that does not fit: skipped, never cut */
#define CONTAIN_WORD_END      0     /* no word is left */
#define CONTAIN_WORD_READ     1     /* a word was read whole */

/* What separates the words of a list of names, and the folders of the list of folders. */
#define CONTAIN_NAME_SEPARATORS   ",: \t"
#define CONTAIN_FOLDER_SEPARATORS ":"

/* ---- settings ------------------------------------------------------------- */

static int   contain_mode = WPE_CONTAIN_MODE_STANDARD;
static char *contain_keep_names;        /* AIContainEnv */
static char *contain_host_names;        /* AIHostEnv */
static char *contain_path_allow;        /* AIContainPathAllow */
static int   contain_notice_said;

/**
 * wpe_contain_mode_from_name - Read a containment mode as the user wrote it.
 * @name: "strict", "standard" or "off" (any case); anything else is standard.
 */
int wpe_contain_mode_from_name(const char *name)
{
 if (name && strcasecmp(name, "strict") == 0)
  return WPE_CONTAIN_MODE_STRICT;
 if (name && strcasecmp(name, "off") == 0)
  return WPE_CONTAIN_MODE_OFF;
 return WPE_CONTAIN_MODE_STANDARD;
}

/** wpe_contain_mode_name - A mode as it is written in the config file. */
const char *wpe_contain_mode_name(int mode)
{
 if (mode == WPE_CONTAIN_MODE_STRICT)
  return "strict";
 if (mode == WPE_CONTAIN_MODE_OFF)
  return "off";
 return "standard";
}

/** wpe_contain_mode_set - The mode the user's config file chose. */
void wpe_contain_mode_set(int mode)
{
 contain_mode = mode;
}

/**
 * wpe_contain_mode - The mode in force: XWPE_AI_CONTAIN wins over the file.
 *
 * Lets a user shut a run down to Strict, or look at one with Off, for one
 * session without editing the config.
 */
int wpe_contain_mode(void)
{
 const char *e = getenv(WPE_CONTAIN_ENV_MODE);

 if (e && *e)
  return wpe_contain_mode_from_name(e);
 return contain_mode;
}

/** wpe_contain_mode_file - The mode the config file chose, for saving it back. */
int wpe_contain_mode_file(void)
{
 return contain_mode;
}

/**
 * contain_names_replace - Keep a copy of a list the config file gave.
 * @slot: where it is kept; @names: the new text, or NULL or empty for none.
 */
static void contain_names_replace(char **slot, const char *names)
{
 free(*slot);
 *slot = (names && *names) ? strdup(names) : NULL;
}

/** wpe_contain_keep_names_set - AIContainEnv: the names every profile keeps. */
void wpe_contain_keep_names_set(const char *names)
{
 contain_names_replace(&contain_keep_names, names);
}

/** wpe_contain_host_names_set - AIHostEnv: the names the host command keeps. */
void wpe_contain_host_names_set(const char *names)
{
 contain_names_replace(&contain_host_names, names);
}

/** wpe_contain_keep_names_file - What the file said for AIContainEnv, for saving. */
const char *wpe_contain_keep_names_file(void)
{
 return contain_keep_names;
}

/** wpe_contain_path_allow_set - AIContainPathAllow: folders an engine's PATH keeps inside the workspace. */
void wpe_contain_path_allow_set(const char *folders)
{
 contain_names_replace(&contain_path_allow, folders);
}

/** wpe_contain_path_allow_file - What the file said for AIContainPathAllow, for saving. */
const char *wpe_contain_path_allow_file(void)
{
 return contain_path_allow;
}

/** wpe_contain_host_names_file - What the file said for AIHostEnv, for saving. */
const char *wpe_contain_host_names_file(void)
{
 return contain_host_names;
}

/**
 * contain_names_in_force - The list a key and its environment shadow give.
 * @shadow: the variable's name; @file: what the config file said.
 * Return: the shadow when it is set and not empty, else the file's list.
 */
static const char *contain_names_in_force(const char *shadow, const char *file)
{
 const char *e = getenv(shadow);

 if (e && *e)
  return e;
 return file;
}

/* ---- small text helpers --------------------------------------------------- */

/**
 * contain_list_next - The next word of a list, whole or not at all.
 * @cursor: where the list is read (moved past the word, however long);
 * @seps: the characters that separate the words; @tok, @n: receive the word.
 *
 * A word that does not fit @tok is never written as a prefix of itself: a
 * folder cut short names another folder, and a name cut short names another
 * variable.  The word is skipped and the caller reads on.
 * Return: CONTAIN_WORD_READ, CONTAIN_WORD_END or CONTAIN_WORD_TOO_LONG (@tok
 * is then empty).
 */
static int contain_list_next(const char **cursor, const char *seps, char *tok, size_t n)
{
 const char *p = *cursor;
 size_t len;

 tok[0] = '\0';
 while (p && *p && strchr(seps, *p))
  p++;
 if (!p || !*p)
  return CONTAIN_WORD_END;
 len = 0;
 while (p[len] && !strchr(seps, p[len]))
  len++;
 *cursor = p + len;
 if (len >= n)
  return CONTAIN_WORD_TOO_LONG;
 memcpy(tok, p, len);
 tok[len] = '\0';
 return CONTAIN_WORD_READ;
}

/**
 * contain_word_next - The next name, or name prefix, of a list written with
 * commas, colons or spaces between the words (AIContainEnv, AIHostEnv, the
 * test seam's prefixes).
 * @cursor: where the list is read; @tok: receives the word, room for
 * CONTAIN_VARNAME_LIMIT bytes.
 * Return: as contain_list_next.
 */
static int contain_word_next(const char **cursor, char *tok, size_t n)
{
 return contain_list_next(cursor, CONTAIN_NAME_SEPARATORS, tok, n);
}

/**
 * contain_folder_next - The next folder of AIContainPathAllow, which is
 * colon separated: a folder may hold a space, so "/work/My Tools" is one
 * folder.
 * @cursor: where the list is read; @tok: receives the folder, room for
 * WPE_PATH_SIZE bytes.
 * Return: as contain_list_next; a folder the system could not open is
 * CONTAIN_WORD_TOO_LONG and the user's other folders are still read.
 */
static int contain_folder_next(const char **cursor, char *tok, size_t n)
{
 return contain_list_next(cursor, CONTAIN_FOLDER_SEPARATORS, tok, n);
}

/** contain_list_has - Is @name one of the words of the list @csv? */
static int contain_list_has(const char *csv, const char *name)
{
 char tok[CONTAIN_VARNAME_LIMIT];
 const char *c = csv;
 int rc;

 while (c && (rc = contain_word_next(&c, tok, sizeof tok)) != CONTAIN_WORD_END)
  if (rc == CONTAIN_WORD_READ && strcmp(tok, name) == 0)
   return 1;
 return 0;
}

/** contain_in_array - Is @name one of the strings of the NULL-terminated @list? */
static int contain_in_array(const char *const *list, const char *name)
{
 int i;

 for (i = 0; list[i]; i++)
  if (strcmp(list[i], name) == 0)
   return 1;
 return 0;
}

/** contain_has_prefix_in - Does @name begin with one of the strings of @list? */
static int contain_has_prefix_in(const char *const *list, const char *name)
{
 int i;

 for (i = 0; list[i]; i++)
  if (strncmp(name, list[i], strlen(list[i])) == 0)
   return 1;
 return 0;
}

/**
 * contain_name_of - The name of an environment entry.
 * @entry: "NAME=value"; @name: receives NAME.
 * Return: 1, or 0 when the entry has no '=' or its name does not fit.
 */
static int contain_name_of(const char *entry, char *name, size_t n)
{
 const char *eq = strchr(entry, '=');
 size_t len;

 if (!eq)
  return 0;
 len = (size_t)(eq - entry);
 if (len == 0 || len >= n)
  return 0;
 memcpy(name, entry, len);
 name[len] = '\0';
 return 1;
}

/* ---- the environment ------------------------------------------------------ */

/**
 * contain_is_key_pointer - Is @name the editor's pointer to a key file?
 * Those (ANTHROPIC_API_KEY_FILE, XWPE_AI_..._FILE) say where a key is kept,
 * and no program but the editor reads them.
 */
static int contain_is_key_pointer(const char *name)
{
 size_t n = strlen(name), t = strlen(CONTAIN_KEY_POINTER_TAIL);

 if (n < t || strcmp(name + n - t, CONTAIN_KEY_POINTER_TAIL) != 0)
  return 0;
 return contain_has_prefix_in(contain_pointer_prefixes, name);
}

/** contain_is_git_identity - Is @name the author or committer git reads from the environment? */
static int contain_is_git_identity(const char *name)
{
 return contain_in_array(contain_git_identity, name);
}

/**
 * contain_is_secret - Is @name one no profile gets unless it is its own?
 * @name: an environment name.
 */
static int contain_is_secret(const char *name)
{
 if (contain_is_git_identity(name))
  return 0;
 if (contain_in_array(contain_secret_names, name))
  return 1;
 if (contain_is_key_pointer(name))
  return 1;
 return contain_has_prefix_in(contain_secret_prefixes, name);
}

/**
 * contain_is_engine_login - Is @name what @p's engine needs to reach its provider?
 * @p: the profile; @name: an environment name.
 * Return: 1 for the engine's own login and route variables, never for a key pointer.
 */
static int contain_is_engine_login(const WpeContainProfile *p, const char *name)
{
 if (p->engine == WPE_CONTAIN_ENGINE_NONE || contain_is_key_pointer(name))
  return 0;
 if (contain_in_array(contain_route_names, name))
  return 1;
 if (p->engine == WPE_CONTAIN_ENGINE_CLAUDE)
  return contain_has_prefix_in(contain_claude_prefixes, name);
 return contain_has_prefix_in(contain_command_prefixes, name);
}

/**
 * contain_pass_prefix_refused - May the test seam name this prefix?
 * @prefix: one word of XWPE_AI_CONTAIN_PASS_ENV.
 * Return: 1 unless it begins with the suites' own prefix (CONTAIN_PASS_PREFIX):
 * the seam passes stub variables and nothing else, so it can never pass a
 * cloud key or a token whatever a test names.  A prefix that also overlaps a
 * refused one is refused as well.
 */
static int contain_pass_prefix_refused(const char *prefix)
{
 size_t i, a, b;

 if (strncmp(prefix, CONTAIN_PASS_PREFIX, strlen(CONTAIN_PASS_PREFIX)) != 0)
  return 1;
 for (i = 0; contain_pass_refused[i]; i++) {
  a = strlen(prefix);
  b = strlen(contain_pass_refused[i]);
  if (strncmp(prefix, contain_pass_refused[i], a < b ? a : b) == 0)
   return 1;
 }
 return 0;
}

/** contain_passes_by_seam - Does the test seam keep @name?  (Refused prefixes never match.) */
static int contain_passes_by_seam(const char *name)
{
 const char *c = getenv(WPE_CONTAIN_ENV_PASS);
 char tok[CONTAIN_VARNAME_LIMIT];
 int rc;

 while (c && (rc = contain_word_next(&c, tok, sizeof tok)) != CONTAIN_WORD_END)
  if (rc == CONTAIN_WORD_READ && !contain_pass_prefix_refused(tok) &&
      strncmp(name, tok, strlen(tok)) == 0)
   return 1;
 return 0;
}

/** contain_is_base - Is @name part of the base every profile gets? */
static int contain_is_base(const char *name)
{
 if (strncmp(name, CONTAIN_LOCALE_PREFIX, strlen(CONTAIN_LOCALE_PREFIX)) == 0)
  return 1;
 if (contain_is_git_identity(name))
  return 1;
 return contain_in_array(contain_base_names, name) ||
        contain_in_array(contain_toolchain_names, name);
}

/**
 * contain_names_loader_path - Does a list the user wrote name LD_LIBRARY_PATH?
 * It is the one name of the drop list the user may keep, because a program in
 * a private toolchain can need it.
 */
static int contain_names_loader_path(const WpeContainProfile *p)
{
 const char *keep = contain_names_in_force(WPE_CONTAIN_ENV_KEEP, contain_keep_names);
 const char *host = contain_names_in_force(WPE_CONTAIN_ENV_HOST, contain_host_names);

 if (contain_list_has(keep, CONTAIN_LOADER_PATH))
  return 1;
 return p->engine == WPE_CONTAIN_ENGINE_COMMAND && contain_list_has(host, CONTAIN_LOADER_PATH);
}

/**
 * contain_keeps - Does @p's child get the variable @name?
 * @p: the profile; @name: an environment name.
 *
 * The one judge of the list.  Never the names that make the loader or the
 * shell run code; the engine's own login and route; the agent socket of a
 * signed commit; the user's AIHostEnv for the host command; then, unless the
 * name is one of the secrets, the base, the toolchains, the user's AIContainEnv
 * and the test seam.
 * Return: 1 to keep it.
 */
static int contain_keeps(const WpeContainProfile *p, const char *name)
{
 const char *keep = contain_names_in_force(WPE_CONTAIN_ENV_KEEP, contain_keep_names);
 const char *host = contain_names_in_force(WPE_CONTAIN_ENV_HOST, contain_host_names);

 if (contain_in_array(contain_injection_names, name))
  return 0;
 if (strcmp(name, CONTAIN_LOADER_PATH) == 0)
  return contain_names_loader_path(p);
 if (contain_is_engine_login(p, name))
  return 1;
 if (p->keeps_ssh_agent && strcmp(name, "SSH_AUTH_SOCK") == 0)
  return 1;
 if (p->engine == WPE_CONTAIN_ENGINE_COMMAND && contain_list_has(host, name))
  return 1;
 if (contain_is_secret(name))
  return 0;
 if (contain_is_base(name) || contain_list_has(keep, name))
  return 1;
 return contain_passes_by_seam(name);
}

/** contain_env_count - How many entries the vector @from has. */
static size_t contain_env_count(char *const *from)
{
 size_t n = 0;

 while (from && from[n])
  n++;
 return n;
}

/**
 * contain_env_value - The value of @name in the vector @from.
 * Return: it (inside @from), or NULL when it is not there.
 */
static const char *contain_env_value(char *const *from, const char *name)
{
 size_t i, len = strlen(name);

 for (i = 0; from && from[i]; i++)
  if (strncmp(from[i], name, len) == 0 && from[i][len] == '=')
   return from[i] + len + 1;
 return NULL;
}

/**
 * contain_env_append - Add one entry to the vector under construction.
 * @vec: the vector; @n: its length so far (moved on); @entry: the text, copied.
 * Return: 0, or -1 out of memory.
 */
static int contain_env_append(char **vec, size_t *n, const char *entry)
{
 char *copy = strdup(entry);

 if (!copy)
  return -1;
 vec[(*n)++] = copy;
 return 0;
}

/**
 * contain_env_add_path - Add PATH, from the editor's PATH without the
 * entries the child may not trust.
 * @vec, @n: the vector; @from: the editor's environment; @p, @workspace as
 * for wpe_contain_path_make.
 * Return: 0, or -1 out of memory.
 */
static int contain_env_add_path(char **vec, size_t *n, char *const *from,
                                const WpeContainProfile *p, const char *workspace)
{
 const char *have = contain_env_value(from, "PATH");
 char *path = wpe_contain_path_make(p, have ? have : CONTAIN_DEFAULT_PATH, workspace);
 char *entry;
 size_t room;
 int rc;

 if (!path)
  return -1;
 room = strlen(path) + sizeof "PATH=";
 entry = malloc(room);
 if (!entry) {
  free(path);
  return -1;
 }
 snprintf(entry, room, "PATH=%s", path);
 free(path);
 rc = contain_env_append(vec, n, entry);
 free(entry);
 return rc;
}

/**
 * wpe_contain_env_make - The environment a profile gives its child.
 * @p: the profile; @from: the editor's environment; @workspace: the folder
 * the child works in, or NULL.
 *
 * Built from a list, never inherited: a command the assistant runs does not
 * see the provider key, the agent sockets or the desktop, whatever the
 * editor's own environment holds.  PATH is the editor's without the entries
 * a hostile folder could use; TERM is dumb.
 * Return: the vector, NULL-terminated, to free with wpe_contain_env_free;
 * NULL when out of memory.
 */
char **wpe_contain_env_make(const WpeContainProfile *p, char *const *from,
                            const char *workspace)
{
 char **vec = calloc(contain_env_count(from) + 3, sizeof *vec);
 char name[CONTAIN_VARNAME_LIMIT];
 size_t i, n = 0;

 if (!vec)
  return NULL;
 for (i = 0; from && from[i]; i++) {
  if (!contain_name_of(from[i], name, sizeof name))
   continue;
  if (strcmp(name, "PATH") == 0 || strcmp(name, "TERM") == 0)
   continue;
  if (contain_keeps(p, name) && contain_env_append(vec, &n, from[i]) != 0)
   goto fail;
 }
 if (contain_env_add_path(vec, &n, from, p, workspace) != 0 ||
     contain_env_append(vec, &n, CONTAIN_TERM_VALUE) != 0)
  goto fail;
 return vec;
fail:
 wpe_contain_env_free(vec);
 return NULL;
}

/** wpe_contain_env_free - Free what wpe_contain_env_make made. */
void wpe_contain_env_free(char **vec)
{
 size_t i;

 for (i = 0; vec && vec[i]; i++)
  free(vec[i]);
 free(vec);
}

/* ---- PATH and the program ------------------------------------------------- */

/**
 * contain_inside - Does @dir lie at or under @root?
 * @dir, @root: absolute paths.
 */
static int contain_inside(const char *dir, const char *root)
{
 size_t n = strlen(root);

 if (n > 1 && root[n - 1] == '/')
  n--;
 if (strncmp(dir, root, n) != 0)
  return 0;
 return dir[n] == '\0' || dir[n] == '/' || n == 1;
}

/**
 * contain_real - A folder's real path, or the text as given when it has none.
 * @dir: a path; @out, @n: receive it.
 *
 * A folder the system could not open (longer than a path may be) has no real
 * path and no text that fits: it is refused, never cut into the path of a
 * folder it only begins with.
 * Return: 0, or -1 with @out empty when @dir does not fit.
 */
static int contain_real(const char *dir, char *out, size_t n)
{
 char real[WPE_PATH_SIZE];

 if (realpath(dir, real) && strlen(real) < n)
  return wpe_path_copy(out, n, real);
 return wpe_path_copy(out, n, dir);
}

/**
 * contain_path_allowed_inside - Did the user name this folder (AIContainPathAllow,
 * or its environment shadow) as one an engine's PATH may keep although it lies
 * inside the workspace?
 * @dir: its real path.
 */
static int contain_path_allowed_inside(const char *dir)
{
 const char *c = contain_names_in_force(WPE_CONTAIN_ENV_PATH_OK, contain_path_allow);
 char tok[WPE_PATH_SIZE], real[WPE_PATH_SIZE];
 int rc;

 while (c && (rc = contain_folder_next(&c, tok, sizeof tok)) != CONTAIN_WORD_END) {
  if (rc == CONTAIN_WORD_TOO_LONG || contain_real(tok, real, sizeof real) < 0)
   continue;
  if (strcmp(real, dir) == 0)
   return 1;
 }
 return 0;
}

/**
 * contain_path_entry_ok - May @entry stay on @p's PATH?
 * @p, @entry, @root: the profile, one PATH entry, the workspace's real path
 * (or NULL).
 *
 * An empty, "." or relative entry means "the folder the program runs in",
 * which for the assistant is the workspace: a `make` or a `claude` the model
 * wrote there would win over the real one.  An engine's PATH also drops every
 * entry inside the workspace, and an entry too long for the system to open,
 * which cannot be judged inside or outside and holds no program the kernel
 * could run.
 */
static int contain_path_entry_ok(const WpeContainProfile *p, const char *entry,
                                 const char *root)
{
 char real[WPE_PATH_SIZE];

 if (entry[0] != '/')
  return 0;
 if (!p->path_skips_workspace || !root)
  return 1;
 if (contain_real(entry, real, sizeof real) < 0)
  return 0;
 if (!contain_inside(real, root))
  return 1;
 return contain_path_allowed_inside(real);
}

/**
 * wpe_contain_path_make - The PATH a profile's child gets.
 * @p: the profile; @path: the editor's PATH; @workspace: the folder the child
 * works in, or NULL.
 * Return: malloc'd, entries in their order; NULL out of memory.
 */
char *wpe_contain_path_make(const WpeContainProfile *p, const char *path,
                            const char *workspace)
{
 char *out = calloc(1, strlen(path) + 1);
 char *copy = strdup(path), *entry, *save = NULL, root[WPE_PATH_SIZE];
 const char *rootp = NULL;
 size_t len = 0;

 if (!out || !copy) {
  free(out);
  free(copy);
  return NULL;
 }
 if (workspace && *workspace) {
  if (contain_real(workspace, root, sizeof root) < 0 && p->path_skips_workspace) {
   free(copy);      /* no folder can be judged against a workspace that is not known */
   return out;
  }
  rootp = root;
 }
 for (entry = strtok_r(copy, ":", &save); entry; entry = strtok_r(NULL, ":", &save)) {
  if (!contain_path_entry_ok(p, entry, rootp))
   continue;
  if (len)
   out[len++] = ':';
  memcpy(out + len, entry, strlen(entry));    /* out holds all of path, so it fits */
  len += strlen(entry);
 }
 free(copy);
 return out;
}

/** contain_is_runnable - Is @file a regular file the user may run? */
static int contain_is_runnable(const char *file)
{
 struct stat st;

 return stat(file, &st) == 0 && S_ISREG(st.st_mode) && access(file, X_OK) == 0;
}

/**
 * wpe_contain_resolve - Where a program is, the way a shell finds it.
 * @name: a bare program name; @path: the PATH to look along.
 * Return: the absolute path, malloc'd; NULL when it is not there.
 */
char *wpe_contain_resolve(const char *name, const char *path)
{
 char *copy = strdup(path), *entry, *save = NULL, *found = NULL;
 size_t need;

 if (!copy)
  return NULL;
 for (entry = strtok_r(copy, ":", &save); entry && !found;
      entry = strtok_r(NULL, ":", &save)) {
  char *cand;

  if (entry[0] != '/')
   continue;
  need = strlen(entry) + strlen(name) + 2;
  cand = malloc(need);
  if (!cand)
   break;
  snprintf(cand, need, "%s/%s", entry, name);
  if (contain_is_runnable(cand))
   found = cand;
  else
   free(cand);
 }
 free(copy);
 return found;
}

/**
 * contain_note_make - The sentence for an engine whose program sits in a
 * folder its PATH does not trust.
 * @name: the program; @dir: the folder it was found in; @buf, @n: the text.
 */
static void contain_note_make(const char *name, const char *dir, char *buf, size_t n)
{
 snprintf(buf, n, "%s is in %s, which is inside the workspace: an engine does not run "
          "programs from there. Name the folder in AIContainPathAllow, or start the "
          "editor in a project folder", name, dir);
}

/**
 * wpe_contain_engine_note - Explain why an engine's program was not found.
 * @p: the engine's profile; @name: its program ("claude"); @workspace: where
 * it works, or NULL for the editor's folder; @buf, @n: receives the sentence.
 *
 * When the editor itself finds the program in a PATH folder inside the
 * workspace (the workspace is the home folder and the program is in
 * ~/.local/bin) the engine's PATH drops that folder, and without this the user
 * would read only "produced no output".
 * Return: 1 and the sentence naming the folder and the reason, when the
 * program is not found on the engine's PATH but is in a dropped folder of the
 * workspace; 0 otherwise.
 */
int wpe_contain_engine_note(const WpeContainProfile *p, const char *name,
                            const char *workspace, char *buf, size_t n)
{
 const char *have = getenv("PATH");
 char here[WPE_PATH_SIZE], root[WPE_PATH_SIZE], real[WPE_PATH_SIZE], cand[WPE_PATH_SIZE];
 char *copy, *entry, *save = NULL, *safe;
 int found = 0;

 if (!have || !p->path_skips_workspace)
  return 0;
 if (!workspace && !getcwd(here, sizeof here))
  return 0;
 if (contain_real(workspace ? workspace : here, root, sizeof root) < 0)
  return 0;
 safe = wpe_contain_path_make(p, have, root);
 if (safe && (copy = wpe_contain_resolve(name, safe)) != NULL) {
  free(copy);
  free(safe);
  return 0;
 }
 free(safe);
 copy = strdup(have);
 for (entry = copy ? strtok_r(copy, ":", &save) : NULL; entry && !found;
      entry = strtok_r(NULL, ":", &save)) {
  if (entry[0] != '/')
   continue;
  if (contain_real(entry, real, sizeof real) < 0 ||
      wpe_path_join(cand, sizeof cand, real, name) < 0)
   continue;
  if (contain_inside(real, root) && contain_is_runnable(cand)) {
   contain_note_make(name, real, buf, n);
   found = 1;
  }
 }
 free(copy);
 return found;
}

/* ---- limits --------------------------------------------------------------- */

/**
 * contain_size_shift - How far a size unit shifts a count.
 * @unit: 'k', 'm' or 'g' in either case.
 * Return: 10, 20 or 30; 0 for any other letter.
 */
static int contain_size_shift(int unit)
{
 switch (tolower(unit)) {
 case 'k': return 10;
 case 'm': return 20;
 case 'g': return 30;
 default:  return 0;
 }
}

/**
 * wpe_contain_fsize_from_text - Read a file-size limit.
 * @text: digits with an optional K, M or G (powers of 1024); @ok: receives 1
 * when the text was one.
 * Return: the bytes (0 = no limit); the default when the text is not a size.
 */
unsigned long long wpe_contain_fsize_from_text(const char *text, int *ok)
{
 char *end;
 unsigned long long v;
 int shift;

 if (ok)
  *ok = 0;
 if (!text || !isdigit((unsigned char)*text))
  return WPE_CONTAIN_FSIZE_DEFAULT;
 v = strtoull(text, &end, 10);
 shift = contain_size_shift(*end);
 if (shift) {
  v <<= shift;
  end++;
 }
 if (*end)
  return WPE_CONTAIN_FSIZE_DEFAULT;
 if (ok)
  *ok = 1;
 return v;
}

/** contain_fsize_in_force - The file-size limit: XWPE_AI_CONTAIN_FSIZE, else the default. */
static unsigned long long contain_fsize_in_force(void)
{
 return wpe_contain_fsize_from_text(getenv(WPE_CONTAIN_ENV_FSIZE), NULL);
}

/* ---- the report ----------------------------------------------------------- */

/**
 * contain_collect_pass - Which test-seam prefixes are in force and which were refused.
 * @r: the report to fill.
 */
static void contain_collect_pass(WpeContainReport *r)
{
 const char *c = getenv(WPE_CONTAIN_ENV_PASS);
 char tok[CONTAIN_VARNAME_LIMIT];
 int rc;

 r->pass[0] = '\0';
 r->refused[0] = '\0';
 while (c && (rc = contain_word_next(&c, tok, sizeof tok)) != CONTAIN_WORD_END) {
  char *dst = contain_pass_prefix_refused(tok) ? r->refused : r->pass;
  size_t have = strlen(dst);

  if (rc == CONTAIN_WORD_TOO_LONG)
   continue;
  if (have + strlen(tok) + 2 > sizeof r->pass)
   continue;
  snprintf(dst + have, sizeof r->pass - have, "%s%s", have ? "," : "", tok);
 }
}

/**
 * wpe_contain_report - What @p holds on this system.
 * @p: the profile; @r: receives it.
 *
 * Today the environment, the file-size limit and (where the system has it)
 * no_new_privs.  The file system view, the network, the processes and the
 * desktop are not held, and the report says so.
 */
void wpe_contain_report(const WpeContainProfile *p, WpeContainReport *r)
{
 memset(r, 0, sizeof *r);
 r->fs = "none";
 r->net = "uncontrolled";
 r->ipc = "none";
 r->pid = "none";
 r->landlock = "none";
 r->seccomp = "none";
 r->privs = "none";
#if WPE_HAS_NO_NEW_PRIVS
 if (wpe_contain_mode() != WPE_CONTAIN_MODE_OFF)
  r->privs = "nonewprivs";
#endif
 r->credentials = (p->engine == WPE_CONTAIN_ENGINE_NONE) ? "reachable(files)" : "inside(env)";
 contain_collect_pass(r);
 r->held = 0;
}

/**
 * wpe_contain_report_line - The report as one line.
 * @p: the profile; @buf, @n: where the line goes.
 */
void wpe_contain_report_line(const WpeContainProfile *p, char *buf, size_t n)
{
 WpeContainReport r;
 char env[200];

 wpe_contain_report(p, &r);
 snprintf(env, sizeof env, "whitelist%s%s%s%s%s", r.pass[0] ? "+pass:" : "", r.pass,
          r.refused[0] ? " (refused:" : "", r.refused, r.refused[0] ? ")" : "");
 snprintf(buf, n, "fs=%s net=%s ipc=%s pid=%s env=%s privs=%s landlock=%s seccomp=%s credentials=%s",
          r.fs, r.net, r.ipc, r.pid, env, r.privs, r.landlock, r.seccomp, r.credentials);
}

/**
 * wpe_contain_word - The word the pane header carries for the containment.
 *
 * UNCONTAINED while the files and the network are not held, which is every
 * system today: the header never says "contained" for a start that can still
 * read the user's files and use the network.
 */
const char *wpe_contain_word(void)
{
 WpeContainReport r;

 wpe_contain_report(&wpe_contain_tool, &r);
 return r.held ? "contained" : "UNCONTAINED";
}

#define CONTAIN_NOTICE_TAIL \
 "; held today: the environment and the limits; NOT held: the files you can read, the network, " \
 "other processes, the desktop"

/**
 * wpe_contain_notice_once - The sentence for the first command of a session.
 * @buf, @n: where it goes.
 *
 * Says what the assistant's commands still reach on this system, in the words
 * the user needs before trusting the dial: it is the honest line the
 * containment owes.  Returned once per session.
 * Return: @buf, or NULL when the session already said it.
 */
const char *wpe_contain_notice_once(char *buf, size_t n)
{
 char line[400];

 if (contain_notice_said)
  return NULL;
 contain_notice_said = 1;
 wpe_contain_report_line(&wpe_contain_tool, line, sizeof line);
 snprintf(buf, n, "[AI] containment on this system: %s%s", line, CONTAIN_NOTICE_TAIL);
 return buf;
}

/** wpe_contain_notice_reset - Let the notice be said again (the tests). */
void wpe_contain_notice_reset(void)
{
 contain_notice_said = 0;
}

/* ---- the plan, and the one way to start ------------------------------------ */

/* The reason leads with the key's name, so it survives a status row cut at 80 columns. */
#define CONTAIN_STRICT_REFUSAL \
 "AIContain strict: not started, the files and the network are not held yet (fs=none net=uncontrolled)"

/**
 * contain_workspace_of - The folder the child works in.
 * @spec: the start; @buf: storage when the editor's own folder is used.
 * Return: @spec's folder, else the editor's current one; NULL when neither is known.
 */
static const char *contain_workspace_of(const WpeChildSpec *spec, char *buf, size_t n)
{
 if (spec->cwd && *spec->cwd)
  return spec->cwd;
 return getcwd(buf, n);
}

/**
 * contain_workspace_too_long - Is the folder the child works in one the system
 * could not open?
 * @workspace: the folder, or NULL.
 *
 * Such a start is refused with the owner's sentence: the child could not
 * change into it, and a PATH judged against a cut folder would be judged
 * against the wrong one.
 * Return: 1 when it does not fit a path buffer, 0 otherwise.
 */
static int contain_workspace_too_long(const char *workspace)
{
 char real[WPE_PATH_SIZE];

 if (!workspace || !*workspace)
  return 0;
 return contain_real(workspace, real, sizeof real) < 0;
}

/**
 * contain_resolve_program - The path execve runs.
 * @spec: the start; @env: the child's environment (its PATH is searched).
 * Return: malloc'd.  A name with a '/' is used as written; a bare name is
 * looked for along the child's PATH; one that is not there becomes "", which
 * execve refuses with ENOENT, so a program in the working folder is never run
 * by accident.
 */
static char *contain_resolve_program(const WpeChildSpec *spec, char *const *env)
{
 const char *name = spec->argv[0];
 const char *path = contain_env_value(env, "PATH");
 char *found;

 if (strchr(name, '/'))
  return strdup(name);
 found = path ? wpe_contain_resolve(name, path) : NULL;
 return found ? found : strdup("");
}

/**
 * contain_strict_refuses - Must this start be refused because the user asked
 * for Strict and this system cannot hold what Strict needs?
 * @p: the profile.
 */
static int contain_strict_refuses(const WpeContainProfile *p)
{
 WpeContainReport r;

 if (wpe_contain_mode() != WPE_CONTAIN_MODE_STRICT)
  return 0;
 wpe_contain_report(p, &r);
 return !r.held;
}

static unsigned long long contain_fsize_clamped(unsigned long long wanted);

/**
 * contain_plan_limits - Set the limits the mode allows.
 * @plan: the plan being made.
 *
 * Off is the environment alone.
 */
static void contain_plan_limits(WpeContainPlan *plan)
{
 if (wpe_contain_mode() == WPE_CONTAIN_MODE_OFF)
  return;
 plan->child.core_zero = 1;
 plan->child.no_new_privs = 1;
 plan->child.fsize_limit = contain_fsize_clamped(contain_fsize_in_force());
}

/**
 * contain_fsize_clamped - Keep the file-size limit within what the editor
 * itself inherited.
 * @wanted: the limit asked for in bytes (0 = none).
 *
 * A child may lower its hard limit but never raise it, so when the editor
 * runs under a smaller hard limit (a login shell's, a container's) asking for
 * more would fail every assistant command at its start.  The child gets the
 * smaller of the two.
 * Return: the limit to set; 0 when none was asked for.
 */
static unsigned long long contain_fsize_clamped(unsigned long long wanted)
{
 struct rlimit now;

 if (wanted == 0 || getrlimit(RLIMIT_FSIZE, &now) != 0)
  return wanted;
 if (now.rlim_max != RLIM_INFINITY && wanted > (unsigned long long)now.rlim_max)
  return (unsigned long long)now.rlim_max;
 return wanted;
}

/**
 * wpe_contain_plan_make - Prepare how the child of @spec is held.
 * @plan: receives it (to free with wpe_contain_plan_free); @p: the profile;
 * @spec: the start (argv, cwd).
 * Return: 0, or -1 with errno ENOMEM.
 */
int wpe_contain_plan_make(WpeContainPlan *plan, const WpeContainProfile *p,
                          const WpeChildSpec *spec)
{
 char here[WPE_PATH_SIZE];
 const char *workspace = contain_workspace_of(spec, here, sizeof here);

 memset(plan, 0, sizeof *plan);
 plan->vec = wpe_contain_env_make(p, environ, workspace);
 if (!plan->vec) {
  errno = ENOMEM;
  return -1;
 }
 plan->prog = contain_resolve_program(spec, plan->vec);
 if (!plan->prog) {
  wpe_contain_env_free(plan->vec);
  errno = ENOMEM;
  return -1;
 }
 plan->child.name = p->name;
 plan->child.prog = plan->prog;
 plan->child.envp = plan->vec;
 contain_plan_limits(plan);
 if (contain_workspace_too_long(workspace)) {
  wpe_path_say_too_long(plan->refusal, sizeof plan->refusal, "workspace");
  plan->child.refusal = plan->refusal;
 } else if (!plan->prog[0] &&
     wpe_contain_engine_note(p, spec->argv[0], workspace, plan->refusal, sizeof plan->refusal))
  plan->child.refusal = plan->refusal;
 if (contain_strict_refuses(p)) {
  snprintf(plan->refusal, sizeof plan->refusal, "%s", CONTAIN_STRICT_REFUSAL);
  plan->child.refusal = plan->refusal;
 }
 return 0;
}

/** wpe_contain_plan_free - Free what wpe_contain_plan_make made. */
void wpe_contain_plan_free(WpeContainPlan *plan)
{
 wpe_contain_env_free(plan->vec);
 free(plan->prog);
 memset(plan, 0, sizeof *plan);
}

/**
 * wpe_contain_start - Start an assistant child as its profile says.
 * @p: the profile, or NULL for a start that names none (refused by the
 * start gate); @spec: the child; @c, @ended, @data: as wpe_child_start.
 *
 * The only place that marks a start as the assistant's and gives it its
 * containment, so no assistant command can run with the editor's whole
 * environment and no limit.  The plan lives only as long as the fork needs
 * it.
 * Return: as wpe_child_start (-1 with errno EPERM when the gate refused it).
 */
int wpe_contain_start(const WpeContainProfile *p, WpeChildSpec *spec, WpeChild *c,
                      WpeChildEndedFn ended, void *data)
{
 WpeContainPlan plan;
 int rc, err;

 spec->origin = WPE_CHILD_ORIGIN_AI;
 spec->contain = NULL;
 if (p && wpe_contain_plan_make(&plan, p, spec) != 0)
  return -1;
 if (p)
  spec->contain = &plan.child;
 rc = wpe_child_start(spec, c, ended, data);
 err = errno;
 if (p)
  wpe_contain_plan_free(&plan);
 spec->contain = NULL;
 errno = err;
 return rc;
}
