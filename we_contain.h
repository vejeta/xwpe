/* we_contain.h -- how the assistant's children are held.
 *
 * Every program the assistant starts (a tool's command, the edit hook, git for
 * the checkpoint and the review, the `claude` CLI, the hosted engine) goes
 * through wpe_contain_start with the profile of what it is.  The profile
 * decides what the child is given: an environment built from a list instead
 * of the editor's own (the provider keys, the agent sockets and the desktop
 * never reach a command), a program path resolved in the parent from a PATH
 * without hostile entries, a file-size limit, no core file and no way to gain
 * privileges.  The refusal of a start with no profile, and of every start
 * under Strict while the system holds neither files nor network, is the start
 * gate of we_child.c; this file only prepares what the gate and the child
 * read.
 *
 * What this holds today, and what it does not: the environment, the limits
 * and no_new_privs.  The files the user can read, the network, the other
 * processes and the desktop are NOT held yet, and the report says so
 * (fs=none net=uncontrolled).
 *
 * Pure C, no display header: the C tests link it with we_child.c and the fd
 * loop.
 */
#ifndef WE_CONTAIN_H
#define WE_CONTAIN_H

#include <stddef.h>
#include "we_child.h"
#include "we_path.h"

/* Room for the sentence that explains an engine's refusal: the fixed words
   (about 170 characters), the program's name and a folder as long as a path
   can be, so the folder is never cut off. */
#define WPE_CONTAIN_NOTE_MAX (WPE_PATH_SIZE + 512)

/* How strict the user asked the containment to be (Options > AI, AIContain,
   XWPE_AI_CONTAIN). */
#define WPE_CONTAIN_MODE_STANDARD 0   /* contain as far as this system allows, say what is missing */
#define WPE_CONTAIN_MODE_STRICT   1   /* start nothing the system cannot hold as asked */
#define WPE_CONTAIN_MODE_OFF      2   /* the environment and the limits only; the header says so */

/* Which engine's own login variables a profile keeps. */
#define WPE_CONTAIN_ENGINE_NONE    0  /* a command, the hook, git: no login variable at all */
#define WPE_CONTAIN_ENGINE_CLAUDE  1  /* the claude CLI: ANTHROPIC_*, CLAUDE_CODE_* */
#define WPE_CONTAIN_ENGINE_COMMAND 2  /* the user's own hosted command: OPENAI_* and AIHostEnv */

/* One kind of assistant child. */
typedef struct WpeContainProfile {
 const char *name;               /* "tool", "hook", "git", "git-commit", ... */
 int engine;                     /* WPE_CONTAIN_ENGINE_*: whose login variables it keeps */
 int keeps_ssh_agent;            /* SSH_AUTH_SOCK is kept: a commit signed with ssh */
 int path_skips_workspace;       /* PATH entries inside the workspace are dropped: an
                                    engine must never run a program the workspace planted */
} WpeContainProfile;

extern const WpeContainProfile wpe_contain_tool;          /* run_command, list_dir, grep, glob, Build & fix */
extern const WpeContainProfile wpe_contain_hook;          /* the post-edit hook */
extern const WpeContainProfile wpe_contain_git;           /* the checkpoint and the review's git reads */
extern const WpeContainProfile wpe_contain_git_commit;    /* the review's stage and commit */
extern const WpeContainProfile wpe_contain_git_commit_ssh;/* the same, signing with ssh: the agent's socket is kept */
extern const WpeContainProfile wpe_contain_cli_text;      /* the `claude -p` backend */
extern const WpeContainProfile wpe_contain_host_claude;   /* the hosted engine, built-in argv */
extern const WpeContainProfile wpe_contain_host_command;  /* the hosted engine, the user's command */

/* The names of the environment variables every profile drops, whatever the
   lists say, are in we_contain.c; the engines keep their own login. */

/* The one test seam, read from the environment only (never from a config
   file, so a project folder cannot set it).  It keeps variables whose names
   begin with the suites' stub prefix (XWPE_STUB_) and nothing else, and the
   report names it when set. */
#define WPE_CONTAIN_ENV_PASS     "XWPE_AI_CONTAIN_PASS_ENV"   /* stub name prefixes the list keeps */
/* The user's own settings, each shadowing its config key. */
#define WPE_CONTAIN_ENV_PATH_OK  "XWPE_AI_CONTAIN_PATH_ALLOW" /* AIContainPathAllow: folders an engine's PATH keeps inside the workspace */
#define WPE_CONTAIN_ENV_MODE     "XWPE_AI_CONTAIN"            /* strict | standard | off */
#define WPE_CONTAIN_ENV_KEEP     "XWPE_AI_CONTAIN_ENV"        /* names every profile keeps */
#define WPE_CONTAIN_ENV_HOST     "XWPE_AI_HOST_ENV"           /* names the host command keeps */
#define WPE_CONTAIN_ENV_FSIZE    "XWPE_AI_CONTAIN_FSIZE"      /* the file-size limit in bytes (K, M, G) */

/* The file-size limit: large enough for a big link or a debug build, small
   enough to stop `yes > f`.  0 given by the user means no limit. */
#define WPE_CONTAIN_FSIZE_DEFAULT (8ULL * 1024 * 1024 * 1024)

/* Settings from the user's own config file (the keys are user-only). */
void wpe_contain_mode_set(int mode);
int  wpe_contain_mode_from_name(const char *name);
const char *wpe_contain_mode_name(int mode);
int  wpe_contain_mode(void);               /* the effective mode: the environment wins */
int  wpe_contain_mode_file(void);          /* the mode the file chose, for saving */
void wpe_contain_keep_names_set(const char *names);   /* AIContainEnv, comma separated, or NULL */
void wpe_contain_host_names_set(const char *names);   /* AIHostEnv, comma separated, or NULL */
void wpe_contain_path_allow_set(const char *folders); /* AIContainPathAllow, colon separated, or NULL */
const char *wpe_contain_path_allow_file(void);
/* Why an engine's program was not found when it sits in a workspace folder
   its PATH drops: 1 and the sentence in @buf, else 0. */
int  wpe_contain_engine_note(const WpeContainProfile *p, const char *name,
                             const char *workspace, char *buf, size_t n);
const char *wpe_contain_keep_names_file(void);        /* what the file said, for saving */
const char *wpe_contain_host_names_file(void);
unsigned long long wpe_contain_fsize_from_text(const char *text, int *ok);

/* What a profile holds on this system, printable: the same struct the status
   line, the notice and the tests read. */
typedef struct {
 const char *fs;                 /* none | namespaces | landlock | seatbelt(deny-list) */
 const char *net;                /* uncontrolled | off | endpoint | open */
 const char *ipc;                /* none | netns | ... */
 const char *pid;                /* none | ns */
 const char *privs;              /* nonewprivs | none */
 const char *landlock;           /* none | <abi> */
 const char *seccomp;            /* none | libseccomp | table */
 const char *credentials;        /* reachable(files) | inside(env) | none */
 char pass[96];                  /* the PASS_ENV prefixes in force, or "" */
 char refused[96];               /* the PASS_ENV prefixes the builder refused, or "" */
 int held;                       /* 1 when the files and the network are both held */
} WpeContainReport;

void wpe_contain_report(const WpeContainProfile *p, WpeContainReport *r);
void wpe_contain_report_line(const WpeContainProfile *p, char *buf, size_t n);
const char *wpe_contain_word(void);       /* the pane header's word */
/* The one sentence the pane says, in the error colour, before the first
   command of a session: what is held and what is not.  Returned once; NULL
   afterwards (and for a session that already said it). */
const char *wpe_contain_notice_once(char *buf, size_t n);
void wpe_contain_notice_reset(void);      /* the tests: say it again */

/* What a prepared child carries, and what frees it. */
typedef struct {
 WpeChildContain child;          /* what wpe_child_start reads; first member */
 char **vec;                     /* the environment, NULL-terminated, each string owned */
 char *prog;                     /* the resolved program path, owned */
 char refusal[WPE_CONTAIN_NOTE_MAX]; /* storage of child.refusal when the start is refused */
} WpeContainPlan;

/* The environment a profile gives its child, from @from (the editor's
   environment): a NULL-terminated vector of malloc'd strings, freed with
   wpe_contain_env_free.  @workspace is the folder the child works in (for an
   engine's PATH).  NULL on out of memory. */
char **wpe_contain_env_make(const WpeContainProfile *p, char *const *from,
                            const char *workspace);
void wpe_contain_env_free(char **vec);

/* The program @name resolves to along @path (colon separated), the way a
   shell finds it, without trusting an empty, "." or relative entry: a malloc'd
   absolute path, or NULL when none is there. */
char *wpe_contain_resolve(const char *name, const char *path);
/* The PATH a profile's child gets: @path with the entries it may not trust
   removed (empty, "." and relative ones; for an engine also those inside
   @workspace).  malloc'd. */
char *wpe_contain_path_make(const WpeContainProfile *p, const char *path,
                            const char *workspace);

/* Prepare @plan for @p and the start @spec describes (argv, cwd).  0, or -1
   with errno (ENOMEM). */
int  wpe_contain_plan_make(WpeContainPlan *plan, const WpeContainProfile *p,
                           const WpeChildSpec *spec);
void wpe_contain_plan_free(WpeContainPlan *plan);

/* The one way an assistant child is started: the profile's plan is made in the
   parent, the start is made as the assistant's (origin and containment set
   here and nowhere else), and the plan is freed once the child is forked.
   @p NULL is a start that names no profile, which the start gate refuses.
   Return: as wpe_child_start. */
int  wpe_contain_start(const WpeContainProfile *p, WpeChildSpec *spec, WpeChild *c,
                       WpeChildEndedFn ended, void *data);

#endif /* WE_CONTAIN_H */
