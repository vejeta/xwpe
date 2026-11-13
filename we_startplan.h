/* we_startplan.h -- what the editor decided about a program, before it starts.
 *
 * A start plan holds everything the editor itself decides about a program
 * it starts for the user: the words of its command line with where each
 * came from, the folder it runs in, and every path it writes, removes or is
 * handed, resolved with links followed and judged inside or outside the
 * folder of the file that supplied it.  It is built whole before the fork,
 * for one user action (F9 builds one plan for its compiles and its link),
 * judged once by the rules in we_startplan.c, shown by one box when a value
 * that came with a folder decides something, and remembered by the store of
 * we_startcmd.c.  The children of the action are started only through
 * wpe_startplan_start, which takes no word the judged plan does not hold.
 *
 * Pure C, no display header: the C tests link we_startplan.c with
 * we_startcmd.c and we_child.c.
 */
#ifndef WE_STARTPLAN_H
#define WE_STARTPLAN_H

#include <stddef.h>

#include "we_child.h"
#include "we_debreq.h"

/* Where a value came from: shown in the box, and decides whether to ask. */
#define WPE_FROM_EDITOR 0   /* a literal the editor adds: "-c", "-o", "ar"            */
#define WPE_FROM_USER   1   /* the user's own config, the built-in default, a value
                               typed in Options, the file he opened                  */
#define WPE_FROM_FOLDER 2   /* a start-folder xwperc or a project file                */

/* What a word of a command line is. */
#define WPE_WORD_PROGRAM 0  /* argv[0]                                                */
#define WPE_WORD_OPTION  1  /* read as an option: only the editor or the user make one */
#define WPE_WORD_OPERAND 2  /* a path, joined by wpe_startplan_operand                 */
#define WPE_WORD_TEXT    3  /* a folder's text the tool reads as it likes (options
                               from a folder): shown whole, asked about              */
#define WPE_WORD_CARRIER 4  /* the editor's own program that carries the plan's
                               command to where it runs (the graphical editors'
                               helper script, the terminal it opens): never a
                               folder's, neither shown nor remembered               */

/* What the start does to a path, by the editor's own hand. */
#define WPE_PATH_WRITE  1   /* the tool is told to write it ("-o")                     */
#define WPE_PATH_REMOVE 2   /* the editor removes it before the start                  */
#define WPE_PATH_READS  3   /* the tool reads and runs it from its folder on its own   */
#define WPE_PATH_INPUT  4   /* the editor hands it to the tool, or reads it to build
                               the start (a project file)                            */

/* What a path is to the build that names it: decides whether a file kept
   right beside a project in the home folder is the project's own. */
#define WPE_OUTPUT_NONE    0   /* anything else                                        */
#define WPE_OUTPUT_OBJECT  1   /* <stem>.o the build derives from its own FILES        */
#define WPE_OUTPUT_PRODUCT 2   /* the program or library the build links (EXENAME,
                                  LIBNAME, Executable)                                 */
#define WPE_OUTPUT_SOURCE  3   /* one of the project's own FILES, read                 */

/* The kinds of start this owner plans. */
#define WPE_START_BUILD   1   /* a file's compile and link (F9, Alt-C)                */
#define WPE_START_PROJECT 2   /* a project's compiles, ar, ranlib and link            */
#define WPE_START_PRINT   3   /* File > Print: the print command on a copy of the file */
#define WPE_START_MAN     4   /* a manual page for Help                               */
#define WPE_START_UNPACK  5   /* gunzip of a compressed help file                     */
#define WPE_START_PROBE   6   /* a question the editor asks a tool (a Java's version,
                                 the desktop's font)                                  */
#define WPE_START_DEBUGGER      7   /* a debugger on the program (gdb, pdb, jdb, dbx,
                                       xdb, sdb, a68g --monitor)                      */
#define WPE_START_DEBUGGEE      8   /* the program a debug session runs, started by the
                                       editor itself (jdb's VM)                       */
#define WPE_START_DEBUG_COMPILE 9   /* the compile of a program for its debug session */
#define WPE_START_SETUP_IDE    10   /* scala-cli setup-ide, for a Scala session       */
#define WPE_START_BUILD_SERVER 11   /* the build server a project's .bsp file names   */
#define WPE_START_DEBUG_ADAPTER 12  /* a debug adapter (dlv, gdb, lldb-dap)           */
#define WPE_START_RUN          13   /* Run (Ctrl-F9): the program just built, or the
                                       interpreter on the file                        */
#define WPE_START_SCRIPT       14   /* Run on a shell script: the script itself       */
#define WPE_START_MAKE         15   /* Execute Make: make in the file's folder        */
#define WPE_START_SHELL        16   /* File > Shell, and Execute in the file manager  */
#define WPE_START_INSTALL      17   /* a project's install lines                      */
#define WPE_START_LANGUAGE_SERVER 18 /* a language server, for the file's folder      */

/* What the judge, and then the user, decided. */
#define WPE_PLAN_UNJUDGED 0
#define WPE_PLAN_CLEAN    1   /* nothing a folder chose: runs, no store consulted     */
#define WPE_PLAN_RUN      2   /* the user's own plan, or one he said yes to           */
#define WPE_PLAN_USE_OWN  3   /* he said "Use mine": his own plan runs instead        */
#define WPE_PLAN_ASK      4   /* nothing known: the box asks                          */
#define WPE_PLAN_REFUSED  5   /* a rule refused it: nothing runs                      */
#define WPE_PLAN_CANCEL   6   /* he said neither: nothing runs                        */

/* What wpe_startplan_remove did. */
#define WPE_STARTPLAN_REMOVED     0   /* removed, or there was nothing to remove     */
#define WPE_STARTPLAN_NOT_REMOVED 1   /* outside the folder: left as it is           */
#define WPE_STARTPLAN_NOT_PLANNED 2   /* not a removal of this plan: left as it is   */

/* Sizes. */
#define WPE_STARTPLAN_PATH_MAX  E_D_REQ_PATH_MAX   /* the debugger request engine's path, one number */
#define WPE_STARTPLAN_KEY_MAX   256
#define WPE_STARTPLAN_WORDS_MAX 1024     /* a project with many FILES               */
#define WPE_STARTPLAN_TEXT_MAX  65536    /* past it the plan cannot be remembered   */
#define WPE_STARTPLAN_READS_MAX 16384    /* of one start-up file: past it the file is
                                            shown in part and Run it is not offered */

/* The shell a planned shell line runs in, and the switch that hands it the
   line; the line's file names follow as its $1, $2 ... never as its text. */
#define WPE_STARTPLAN_SHELL     "/bin/sh"
#define WPE_STARTPLAN_SHELL_OPT "-c"

/* The section of a program that carries gdb scripts inside it. */
#define WPE_STARTPLAN_GDB_SECTION ".debug_gdb_scripts"

/* The name a planned shell line gets as its $0: the editor's, never shown. */
#define WPE_STARTPLAN_SHELL_NAME "sh"

/* The canonical text's first line. */
#define WPE_STARTPLAN_TEXT_MAGIC "xwpe-start 2"

/* What a refusal says. */
#define WPE_STARTPLAN_REFUSE_OPTION "a folder value cannot be an option"
#define WPE_STARTPLAN_REFUSE_WORD   "a word the plan did not show"
#define WPE_STARTPLAN_REFUSE_JUDGE  "the plan was not approved"
#define WPE_STARTPLAN_REFUSE_INPUT  "not sent: a value with a line break"
#define WPE_STARTPLAN_REFUSE_CARRIER "a folder value cannot carry a command"
#define WPE_STARTPLAN_REFUSE_DEBUG_CWD \
 "a debugger may not start in the program's folder"
#define WPE_STARTPLAN_REFUSE_DEBUG_PIN \
 "a debugger start without its containment lines"
#define WPE_STARTPLAN_REFUSE_DEBUG_UNKNOWN "a debugger the plan does not know"
#define WPE_STARTPLAN_REFUSE_DEBUG_PATH \
 "a folder or program the debugger cannot be given safely"
#define WPE_STARTPLAN_REFUSE_DEBUG_LONG \
 "a folder or program whose path is longer than the system allows"
#define WPE_STARTPLAN_REFUSE_DEBUG_MEMORY \
 "not enough memory to plan the debugger's start"

/* Why Run it is not offered for a plan whose start-up files cannot be sent
   to the debugger by the editor's own command. */
#define WPE_STARTPLAN_LOAD_REFUSED "a start-up file whose path the debugger cannot be given safely"
#define WPE_STARTPLAN_LOAD_REFUSED_PDBRC \
 "a .pdbrc line that pdb cannot be given one at a time (a commands block)"

/* One word of a command line. */
typedef struct {
 char *text;                 /* as it reaches execvp                                  */
 int role;                   /* WPE_WORD_*                                            */
 int from;                   /* WPE_FROM_*                                            */
 char *source;               /* "./xwperc: Compiler", or ""                           */
} WpeStartWord;

/* One path the start touches by the editor's hand. */
typedef struct {
 char *path;                 /* as given to the tool                                  */
 char resolved[WPE_STARTPLAN_PATH_MAX]; /* links followed, a missing tail kept as written */
 int role;                   /* WPE_PATH_*                                            */
 int from;                   /* WPE_FROM_*                                            */
 char *root;                 /* the resolved folder of the file that supplied it, or
                                NULL: the plan's working folder                      */
 int inside;                 /* resolved lies under a usable root                     */
 int dropped;                /* a removal outside: never done                         */
 int output;                 /* WPE_OUTPUT_*: what the build makes of it              */
 char *source;               /* "p.prj: EXENAME", for the box                          */
 char *content;              /* WPE_PATH_READS: the file's text (at most
                                WPE_STARTPLAN_READS_MAX bytes), shown and remembered  */
 long length;                /* WPE_PATH_READS: the file's whole size in bytes        */
 int too_long;               /* WPE_PATH_READS: not shown whole, so not run unseen   */
 const char *note;           /* WPE_PATH_READS: when the tool runs it, for the box    */
 int embedded;               /* WPE_PATH_READS: not a file of its own: the scripts the
                                program carries inside itself (gdb's
                                .debug_gdb_scripts); content is what they hold       */
 char **admit;               /* embedded: the paths gdb checks against its safe-path
                                for these scripts (a file entry's resolved script,
                                the program for a text entry)                         */
 int nadmit;
} WpeStartPath;

struct startplan_row;        /* one debugger's containment, in we_startplan.c         */

/* One value a folder file set, as the box lists it. */
typedef struct {
 char *file;                 /* "./xwperc", "p.prj"                                   */
 char *key;                  /* "Compiler", "EXENAME"                                 */
 char *raw;                  /* as written in the file                                */
 char *expanded;             /* as the build uses it, or NULL when the same           */
} WpeStartValue;

/* The I/O shape of one child: everything of a WpeChildSpec but what the
   plan decides (the command line, the working folder, the origin). */
typedef struct {
 WpeChildIo io;
 WpeChildGroup group;
 int stdin_null;
 int stdout_to_null;
 const char *stdout_path;
 int stderr_to_null;
 int stderr_to_stdout;
 int stderr_inherit;
 int pty_raw;
 int die_with_editor;
 int signal_leader_only;
 int stderr_nonblocking;
 int side_pty_slave;
 int tty_fd;
 const char *tty_path;
 int say_failed;
 int echo_argv;
 const char *exec_failed;
 const char *what;
} WpeStartIo;

typedef struct WpeStartPlan {
 int kind;                   /* WPE_START_*                                           */
 int origin;                 /* WPE_CHILD_ORIGIN_USER                                 */
 char *name;                 /* the key's second part: "C", "demo.prj"               */
 char *file;                 /* the folder file that supplied values, or NULL        */
 char *folder;               /* that file's folder, resolved: the store's folder     */
 char *cwd;                  /* the working folder, resolved: where the process
                                starts, nothing else                                 */
 char *program_dir;          /* the program's folder, resolved: what is listed from,
                                what relative paths mean, what the canonical text
                                names; NULL means cwd                                */
 char *debug_dir;            /* the folder the owner made for this start, or NULL    */
 int program_from;           /* WPE_FROM_*: whose word the program is (contain)      */
 char *program_source;       /* what the box calls it, or NULL for "the program"      */
 const struct startplan_row *row; /* the debugger row contain() chose, or NULL      */
 const char *contain_refused; /* contain() could not make this start: why           */
 int load_at;                /* where Run it's lines go in input                     */
 int load_word_at;           /* and in word                                          */
 int loaded;                 /* load_lines has run                                   */
 const char *load_refused;   /* why Run it is not offered, or NULL                   */
 char *launch_cwd;           /* the program's folder for an adapter's launch request */
 WpeStartWord *init; int ninit; /* an adapter's launch init commands                 */
 WpeStartWord *word;  int nword;
 WpeStartPath *path;  int npath;
 WpeStartValue *value; int nvalue;
 WpeStartWord *input; int ninput; /* what the editor writes to the tool's input after
                                the start, built from values ("r <Arguments>")      */
 int honest;                 /* the start runs the folder's own program: the box says
                                so, since what that program does is not vetted     */
 const char *runs_code;      /* what of the folder's own code the program runs on its
                                own as it starts (a language server's build scripts),
                                said in the box; the plan then asks once per folder */
 int list_inside;            /* the inside operands are part of the text (a project) */
 struct WpeStartPlan *own;   /* what "Use mine" starts instead, or NULL               */
 int verdict;                /* WPE_PLAN_*                                            */
 const char *refused;        /* why, when WPE_PLAN_REFUSED                            */
 char *text;                 /* the canonical text, once judged                      */
} WpeStartPlan;

WpeStartPlan *wpe_startplan_new(int kind, const char *name);
void wpe_startplan_free(WpeStartPlan *p);
const char *wpe_startplan_kind_name(int kind);
int wpe_startplan_key(const WpeStartPlan *p, char *out, size_t sz);
int wpe_startplan_set_file(WpeStartPlan *p, const char *file, const char *folder);
int wpe_startplan_set_cwd(WpeStartPlan *p, const char *dir);
int wpe_startplan_word(WpeStartPlan *p, const char *text, int role, int from,
                       const char *source);
int wpe_startplan_words(WpeStartPlan *p, const char *text, int role, int from,
                        const char *source);
int wpe_startplan_path(WpeStartPlan *p, const char *path, int role, int from,
                       const char *source, const char *root);
int wpe_startplan_value(WpeStartPlan *p, const char *file, const char *key,
                        const char *raw, const char *expanded);
void wpe_startplan_set_own(WpeStartPlan *p, WpeStartPlan *own);
int wpe_startplan_operand(char *out, size_t n, const char *dir, const char *name);
int wpe_startplan_operand_in(char *out, size_t n, const char *dir, const char *name);
int wpe_startplan_resolve(const char *path, const char *cwd, char *out, size_t sz);
int wpe_startplan_root_usable(const char *root);
int wpe_startplan_judge(WpeStartPlan *p);
int wpe_startplan_own_usable(WpeStartPlan *p);
int wpe_startplan_counts(const WpeStartPlan *p, int *outside, int *dropped, int *inside);
const char *wpe_startplan_text(WpeStartPlan *p);
int wpe_startplan_remember(WpeStartPlan *p, int verdict);
int wpe_startplan_covers(const WpeStartPlan *p, char *const argv[], const char **bad);
int wpe_startplan_start(WpeStartPlan *p, char *const argv[], const WpeStartIo *io,
                        WpeChild *c, WpeChildEndedFn ended, void *data);
int wpe_startplan_remove(WpeStartPlan *p, const char *path);
void wpe_startplan_mark_output(WpeStartPlan *p, int output);
int wpe_startplan_relabel(WpeStartPlan *p, int index, int role, int from);
void wpe_startplan_answered(WpeStartPlan *p, int verdict);
WpeStartPlan *wpe_startplan_fixed(int kind, const char *name, char *const argv[]);
int wpe_startplan_capture(WpeStartPlan *p, char *out, size_t max, long deadline_ms,
                          int *status, const char *what);
int wpe_startplan_probe(WpeStartPlan *p, char *out, size_t max, long deadline_ms,
                        int *status, const char *what);
int wpe_startplan_run_to_file(WpeStartPlan *p, const char *path, long deadline_ms,
                              int *status, const char *what);
int wpe_startplan_env_name_allowed(const char *name);
void wpe_startplan_trace_set(void (*trace)(const char *line));
int wpe_startplan_tool_reads(WpeStartPlan *p, const char *tool);
int wpe_startplan_tool_reads_of(WpeStartPlan *p, const char *tool, const char *program);
int wpe_startplan_tool_reads_known(const char *tool);
int wpe_startplan_elf_section(const char *file, const char *name, unsigned char *out,
                              size_t max, size_t *size);
int wpe_startplan_input(WpeStartPlan *p, const char *text, int from, const char *source);
int wpe_startplan_line_ok(const char *value);
int wpe_startplan_reads_too_long(const WpeStartPlan *p);
WpeStartPlan *wpe_startplan_running(WpeStartPlan *p);
char **wpe_startplan_argv(WpeStartPlan *p);
int wpe_startplan_start_words(WpeStartPlan *p, const WpeStartIo *io, WpeChild *c,
                              WpeChildEndedFn ended, void *data);
int wpe_startplan_shell(WpeStartPlan *p, const char *script, int from, const char *source);
int wpe_startplan_carry(WpeStartPlan *p, char *const carrier[], int from);
int wpe_startplan_start_carried(WpeStartPlan *p, const WpeStartIo *io, WpeChild *c,
                                WpeChildEndedFn ended, void *data);
void wpe_startplan_runs_code(WpeStartPlan *p, const char *what);

/* The debugger's own empty folder, and the one call a debugger site makes. */
void wpe_startplan_debug_base_set(const char *base);
char *wpe_startplan_debug_dir_new(const char *base);
int wpe_startplan_debug_dir_end(const char *dir);
char *wpe_startplan_debug_dir_take(WpeStartPlan *p);
void wpe_startplan_program_origin(WpeStartPlan *p, int from, const char *source);
int wpe_startplan_contain(WpeStartPlan *p, const char *key, const char *program_tool,
                          const char *folder, const char *program);
int wpe_startplan_load_lines(WpeStartPlan *p);
int wpe_startplan_is_apart(const WpeStartPlan *p);
const char *wpe_startplan_debugger_of(const WpeStartPlan *p);
const char *wpe_startplan_program_path(const WpeStartPlan *p);
int wpe_startplan_row_is_apart(const char *key, const char *program_tool);
int wpe_startplan_editor_inputs(const WpeStartPlan *p);
int wpe_startplan_history_lines(const WpeStartPlan *p, char *ask, size_t ask_n, char *fix,
                                size_t fix_n);
void wpe_startplan_rows_designed(int on);
const char *wpe_startplan_row_info(int index, const char **variant, int *apart, int *designed,
                                   const char **reason);

/* How a path reaches each debugger's command: a resolved absolute path in, the
   text to put after the command word out; -1 when the debugger cannot be given
   that path safely (the reason is the form's, not a message). */
#define WPE_GDB_ARG_RAW   0   /* the rest of the line: cd, source, add-auto-load-safe-path */
#define WPE_GDB_ARG_SPLIT 1   /* split by gdb's argument splitter: file                    */
int wpe_startplan_gdb_arg(char *out, size_t n, const char *path, int form);
int wpe_startplan_gdb_safe_path_arg(char *out, size_t n, const char *path);
int wpe_startplan_mi_cstring(char *out, size_t n, const char *path);
int wpe_startplan_lldb_source_arg(char *out, size_t n, const char *path);
int wpe_startplan_jdb_read_arg(char *out, size_t n, const char *path);
int wpe_startplan_python_bytes(char *out, size_t n, const char *path);

/* How many bytes the widest escape of wpe_startplan_python_bytes writes for
   one byte of a path: \xNN.  The request engine's command length is derived
   from it (we_debreq.h, checked in we_startplan.c). */
#define WPE_STARTPLAN_PYTHON_ESCAPE 4

#endif
