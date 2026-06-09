/* we_dap.h -- DAP debug session engine (spawn adapter, drive a session).
 *
 * Layer above we_dap_proto.c (the wire format).  It spawns a debug adapter
 * (Delve `dlv dap`, lldb-dap, Metals, ...), connects, and runs a source-level
 * debug session with a SYNCHRONOUS request/response model -- the same shape as
 * xwpe's text backends (send a command, read until the matching reply, while
 * surfacing intervening events).  The host (xwpe, or a test) supplies callbacks
 * for the UI side-effects, so the engine itself stays free of editor globals
 * and can be integration-tested against a real adapter.
 *
 * Transport for Go/Delve is reverse-connect TCP: the engine binds a local
 * port, launches `dlv dap --client-addr=127.0.0.1:PORT`, and the adapter dials
 * back in.  See FEATURE-dap-client.md (xwpe-dev) for the validated sequence.
 */
#ifndef WE_DAP_H
#define WE_DAP_H

/* Side-effects the session asks the host to perform. */
typedef struct {
 void (*on_stopped)(const char *file, int line, const char *reason, void *ud);
 void (*on_output)(const char *text, void *ud);
 void (*on_terminated)(void *ud);
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

/* Record a source breakpoint to install on Run (DAP sets breakpoints per file
 * just before the program starts). */
int e_dap_add_breakpoint(e_dap_session *s, const char *file, int line);

/* launch + setBreakpoints + setFunctionBreakpoints(entry_func) +
 * configurationDone, then run to the first stop (fires host->on_stopped with
 * the source line, or on_terminated if the program ended). */
int e_dap_run(e_dap_session *s);

/* Resume execution: verb is "continue", "next", "stepIn" or "stepOut".  Pumps
 * to the next stop and fires host->on_stopped (or on_terminated). */
int e_dap_step(e_dap_session *s, const char *verb);

/* Evaluate `expr` in the current top frame (a watch).  Returns a malloc'd
 * value string (caller frees), or NULL if it cannot be evaluated. */
char *e_dap_evaluate(e_dap_session *s, const char *expr);

/* True once the debuggee has terminated/exited. */
int e_dap_ended(const e_dap_session *s);

/* Disconnect and reap the adapter; frees the session. */
void e_dap_close(e_dap_session *s);

#endif /* WE_DAP_H */
