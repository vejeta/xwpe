/* we_dap.h -- DAP debug session engine (spawn adapter, drive a session).
 *
 * Layer above we_dap_proto.c (the wire format).  It spawns a debug adapter
 * (Delve `dlv dap`, lldb-dap, Metals, ...), connects, and runs a source-level
 * debug session.  Every message read goes through one dispatcher that hands
 * each response to the owner of its request, so the host can either wait
 * (the synchronous calls) or follow the run from its event loop (the run
 * pump below), and an answer never reaches the wrong wait or none.  The host
 * (xwpe, or a test) supplies callbacks
 * for the UI side-effects, so the engine itself stays free of editor globals
 * and can be integration-tested against a real adapter.
 *
 * Transport for Go/Delve is reverse-connect TCP: the engine binds a local
 * port, launches `dlv dap --client-addr=127.0.0.1:PORT`, and the adapter dials
 * back in.  See FEATURE-dap-client.md (xwpe-dev) for the validated sequence.
 */
#ifndef WE_DAP_H
#define WE_DAP_H

#include <sys/types.h>

/* Side-effects the session asks the host to perform.  The callbacks only
 * record what they are told: they are called from inside the engine's reads,
 * so none of them may call back into a waiting e_dap_ function or close the
 * session.  Unused callbacks are NULL (zero the struct before filling it).
 * on_stopped gets the top frame's source; @file is NULL and @line 0 when
 * that frame has none (dlv's runtime, Go's pre-main entry stop) or the
 * adapter refused the stackTrace: the stop is told all the same, so the
 * host never waits for one that already came. */
typedef struct {
 void (*on_stopped)(const char *file, int line, const char *reason, void *ud);
 void (*on_output)(const char *text, void *ud);
 void (*on_terminated)(void *ud);
 /* The adapter's word on breakpoints: @n entries of one source @file (its
  * absolute path), each the @line the host asked for, whether the adapter
  * @verified it, and the adapter's @message (NULL when it gave none; for an
  * unverified one, why, e.g. "pending").  Fired for the answer to every
  * setBreakpoints and for each `breakpoint` event (n = 1), so a breakpoint
  * gdb first reports pending turns verified when gdb later says so.  The
  * arrays live for the call only. */
 void (*on_breakpoints)(const char *file, const int *line, const int *verified,
                        const char *const *message, int n, void *ud);
 void *ud;
} e_dap_host;

typedef struct e_dap_session e_dap_session;

/* Spawn the adapter (argv NULL-terminated, e.g. {"dlv","dap","--client-addr=...",0}
 * -- but the engine appends --client-addr itself, so pass just {"dlv","dap",0}),
 * reverse-TCP connect, run the `initialize` handshake.  `program` is the dir or
 * file to debug; `entry_func` (may be NULL) is a function the program stops at
 * on Run, like gdb's "break main" -- "main.main" for Go.  `cwd` is the adapter's
 * working directory (NULL = inherit).  Returns a session or NULL on failure. */
e_dap_session *e_dap_open(char *const argv[], const char *program,
                          const char *entry_func, const char *cwd,
                          const e_dap_host *host);

/* Same, but over the STDIO transport: the adapter (e.g. {"gdb","--interpreter=dap"}
 * or {"lldb-dap"}) speaks DAP on its own stdin/stdout.  `program` is a PREBUILT
 * binary the adapter launches (it does not build it), so the caller compiles
 * first.  The debuggee's output arrives as DAP "output" events (no pty). */
e_dap_session *e_dap_open_stdio(char *const argv[], const char *program,
                                const char *entry_func, const char *cwd,
                                const e_dap_host *host);

/* Connect to an ALREADY-RUNNING DAP server at host:port (a forward TCP client,
 * no adapter spawned, no pty).  Used for the JVM/Scala path: a build server
 * (Bloop, driven over BSP -- see we_bsp.c) starts the debug session and hands
 * back a "tcp://HOST:PORT" endpoint; we connect to it here.  The launch
 * handshake follows the strict DAP order (launch -> wait the `initialized`
 * event -> setBreakpoints -> configurationDone), and the launch carries no
 * program/mode (the build server already bound the main class).  The debuggee's
 * output arrives as DAP "output" events.  Returns a session or NULL. */
e_dap_session *e_dap_open_tcp(const char *host, int port, const char *program,
                              const char *entry_func, const e_dap_host *host_cb);

/* The same three transports, started without waiting for the adapter.  Each
 * spawns or connects and returns at once (NULL when that failed); the whole
 * handshake (initialize, launch, breakpoints, configurationDone) then runs as
 * the host feeds it: the host adds its breakpoints, watches e_dap_start_fd()
 * (and e_dap_output_fd(), the debuggee's pty, when >= 0) on its event loop,
 * calls e_dap_start_pump() whenever the first is readable and
 * e_dap_drain_output() for the second, and once the pump answers
 * E_DAP_START_READY calls e_dap_run_to_first_stop().  The start descriptor
 * changes once, when a reverse-TCP adapter dials in: re-read it after each
 * pump.  An adapter that never answers is ended with e_dap_signal() and
 * reaped with e_dap_adapter_gone(); e_dap_close() then frees the session. */
#define E_DAP_START_WAITING 0
#define E_DAP_START_READY   1
#define E_DAP_START_FAILED  (-1)
e_dap_session *e_dap_start(char *const argv[], const char *program,
                           const char *entry_func, const char *cwd,
                           const e_dap_host *host);
e_dap_session *e_dap_start_stdio(char *const argv[], const char *program,
                                 const char *entry_func, const char *cwd,
                                 const e_dap_host *host);
e_dap_session *e_dap_start_tcp(const char *host, int port, const char *program,
                               const char *entry_func, const e_dap_host *host_cb);
int  e_dap_start_fd(const e_dap_session *s);
int  e_dap_output_fd(const e_dap_session *s);
int  e_dap_start_pump(e_dap_session *s);
void e_dap_drain_output(e_dap_session *s);
int  e_dap_run_to_first_stop(e_dap_session *s);
/* Once the adapter has answered initialize it is alive, and a host stops
 * counting a start deadline.  e_dap_start_waits_for() names the step for the
 * waiting line ("starting", "building", "launching", "configuring").  A
 * refused request ends the start with E_DAP_START_FAILED, and
 * e_dap_start_error() gives the adapter's words (dlv's build error). */
int  e_dap_start_answered(const e_dap_session *s);
const char *e_dap_start_waits_for(const e_dap_session *s);
const char *e_dap_start_error(const e_dap_session *s);
/* The adapter's PID when it is this process's child, else 0 (Scala: the
 * build server hosts it). */
pid_t e_dap_adapter_pid(const e_dap_session *s);
void e_dap_signal(e_dap_session *s, int sig);
int  e_dap_adapter_gone(e_dap_session *s);

/* Record a source breakpoint to install on Run (DAP sets breakpoints per file
 * just before the program starts). */
int e_dap_add_breakpoint(e_dap_session *s, const char *file, int line);

/* Replace one file's breakpoints while stopped (setBreakpoints, waited for):
 * Go to cursor's temporary breakpoint is added and removed this way. */
int e_dap_set_file_breakpoints(e_dap_session *s, const char *file,
                               const int *lines, int n);

/* launch + setBreakpoints + setFunctionBreakpoints(entry_func) +
 * configurationDone, then run to the first stop (fires host->on_stopped with
 * the source line, or on_terminated if the program ended). */
int e_dap_run(e_dap_session *s);

/* Resume execution: verb is "continue", "next", "stepIn" or "stepOut".  Pumps
 * to the next stop and fires host->on_stopped (or on_terminated).  The
 * synchronous form of e_dap_resume + e_dap_run_pump, for tests and callers
 * that have nothing else to do meanwhile. */
int e_dap_step(e_dap_session *s, const char *verb);

/* Evaluate `expr` in the current top frame (a watch).  Returns a malloc'd
 * value string (caller frees), or NULL if it cannot be evaluated. */
char *e_dap_evaluate(e_dap_session *s, const char *expr);

/* One frame of the stopped thread's call stack, as stackTrace reports it. */
#define E_DAP_FRAME_NAME_MAX 128
#define E_DAP_FRAME_PATH_MAX 1024
typedef struct {
 int  id;                           /* the adapter's frameId, for evaluate  */
 int  line;                         /* 1-based, 0 when the frame has none   */
 char name[E_DAP_FRAME_NAME_MAX];   /* the function                         */
 char path[E_DAP_FRAME_PATH_MAX];   /* its source file, "" when it has none */
} e_dap_frame;

/* Every frame of the stopped thread, innermost first, in one stackTrace
 * request; at most `max` are written.  Returns how many, or -1 when the
 * session has ended or the adapter did not answer.  The Stack window and
 * Finish function read it. */
int e_dap_stack_trace(e_dap_session *s, e_dap_frame *frames, int max);

/* Evaluate later watches in frame `frame_id` (an id e_dap_stack_trace gave)
 * instead of the top one, until the next stop selects the top again. */
void e_dap_select_frame(e_dap_session *s, int frame_id);

/* The exit code the debuggee's `exited` event carried: 1 with *code set
 * when the adapter reported one, else 0 (`terminated` carries none). */
int e_dap_exit_code(const e_dap_session *s, int *code);

/* True once the debuggee has terminated/exited. */
int e_dap_ended(const e_dap_session *s);

/* ---- the run, pumped by the host's event loop ---------------------------
 *
 * Once the start is ready the program runs, and the host can follow it
 * without waiting: it watches e_dap_fd() (and e_dap_output_fd() for dlv's
 * pty) and calls e_dap_run_pump() each time the first is readable.  The
 * pump reads once and answers:
 *   E_DAP_RUN_GOING    nothing that ends the run yet (output and
 *                      breakpoint answers were passed on meanwhile)
 *   E_DAP_RUN_STOPPED  the program stopped and on_stopped has fired with
 *                      its source line; the run is over until a resume
 *   E_DAP_RUN_ENDED    the program or the adapter ended (on_terminated
 *                      fired for the former), or a disconnect is done
 * At a stop the host resumes with e_dap_resume() and goes on pumping.  The
 * synchronous calls (e_dap_step, e_dap_evaluate, e_dap_stack_trace, ...)
 * use the same reader, so an answer or event that comes during one of them
 * reaches its owner instead of being dropped.
 *
 * Every engine call hands on all it has read before it returns, so nothing
 * waits in the engine for the adapter to write again.  The pump never
 * blocks: it reads only what is there, and may be called at any time.  The
 * start's answer cannot carry a stop, so once e_dap_start_pump() answers
 * E_DAP_START_READY the host calls e_dap_run_pump() once, at once: a stop
 * read together with the start's last answer is answered there.
 *
 * On E_DAP_RUN_ENDED the host stops watching e_dap_fd() and calls
 * e_dap_close(): after a failed start, or a Scala session whose disconnect
 * was answered, the descriptor can still be open and readable. */
#define E_DAP_RUN_GOING   0
#define E_DAP_RUN_STOPPED 1
#define E_DAP_RUN_ENDED   2
int  e_dap_run_pump(e_dap_session *s);
/* The DAP transport the run is read from, or -1 when there is none. */
int  e_dap_fd(const e_dap_session *s);
/* True while the program runs (resumed, its stop not yet reported). */
int  e_dap_running(const e_dap_session *s);
/* At a stop: send "continue", "next", "stepIn" or "stepOut" and return at
 * once.  Returns 0, or -1 when the program is not stopped or has ended. */
int  e_dap_resume(e_dap_session *s, const char *verb);
/* While running: ask the adapter to pause.  The next stop is reported with
 * the reason "pause", whatever reason the adapter gives (lldb-dap says
 * "exception"/SIGSTOP).  Returns 0, or -1 when the program is not running. */
int  e_dap_pause(e_dap_session *s);
/* Replace one file's breakpoints without waiting, running or stopped; the
 * answer comes through the pump (or any later wait) as on_breakpoints.
 * Returns 0, or -1 when the session has ended, its start is not done, @n is
 * over the engine's limit or too many requests are unanswered. */
int  e_dap_set_file_breakpoints_async(e_dap_session *s, const char *file,
                                      const int *lines, int n);
/* Quit without waiting: send `disconnect` (terminate the program) and
 * return.  The pump answers E_DAP_RUN_ENDED once the adapter has answered
 * (or its transport ended); the host counts its own deadline and shows its
 * own status line meanwhile, then calls e_dap_close().  Returns 0, or -1
 * when there is no adapter to speak to (close at once). */
int  e_dap_disconnect(e_dap_session *s);

/* Disconnect and reap the adapter; frees the session.  It WAITS, outside
 * any event loop and with nothing on the screen, each wait a poll bounded
 * by its own deadline:
 *   - when no disconnect was sent yet: for its answer, up to 2 s;
 *   - after an answer: for the adapter to exit by itself, up to 2 s
 *     (lldb-dap takes about a second);
 *   - for an adapter still there: SIGTERM and up to 1 s for it to exit,
 *     then SIGKILL.
 * The editor's end request does not cut these waits short: lldb-dap ends a
 * running program only on `disconnect` (killed by SIGTERM or SIGKILL it
 * leaves the program running), and giving up sends it SIGTERM before it
 * has acted on `disconnect`, which leaves the program behind (measured).
 * Systems with neither pidfd nor kqueue may wait out the 2 s exit bound on
 * an adapter that closed its descriptors before it exited.
 * When the end request comes during them, the save given to
 * e_dap_set_end_watch runs at once instead.  Either way the adapter is
 * reaped, so no adapter and no program is left behind.  A host that must
 * not wait (the editor while a program runs) disconnects first with
 * e_dap_disconnect() and pumps to ENDED. */
void e_dap_close(e_dap_session *s);

/* Let every blocking wait of every session also watch @fd, and give up as
 * soon as @requested() answers non-zero: the editor's end was asked for
 * (SIGTERM, SIGHUP), so a program that never stops cannot hold the editor.
 * Quit's own waits inside e_dap_close are the exception: they are bounded
 * and go on, so the adapter still ends its program; but when the end comes
 * during them, @save (may be NULL) is called once, at once, so the user's
 * work is on disk before anything else can end the editor (a second
 * signal ends it without a save).  @save must be safe to call again later
 * (the editor's own end calls its save too).  Without this call (a test
 * program) the waits are unchanged. */
void e_dap_set_end_watch(int fd, int (*requested)(void), void (*save)(void));

#endif /* WE_DAP_H */
