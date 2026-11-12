/* we_bsp.h -- Build Server Protocol bootstrap for JVM/Scala debugging.
 *
 * The JVM has no standalone "spawn-a-binary" DAP server: the Scala debug
 * adapter (scala-debug-adapter) is a library hosted by a build server.  Bloop
 * (bundled in scala-cli) hosts it and exposes it over BSP -- a JSON-RPC
 * protocol with the SAME Content-Length framing as DAP/LSP, so we reuse
 * we_dap_proto.c's reader for the wire.
 *
 * This module drives the small BSP handshake that ends in a DAP endpoint:
 *
 *     build/initialize -> build/initialized -> workspace/buildTargets
 *     -> buildTarget/compile -> buildTarget/scalaMainClasses
 *     -> debugSession/start  ==>  "tcp://HOST:PORT"
 *
 * The caller then connects the DAP engine (e_dap_open_tcp) to that endpoint.
 * The BSP session MUST stay open for the lifetime of the debug session: the
 * build server hosts the debug adapter, so closing BSP tears the adapter down.
 */
#ifndef WE_BSP_H
#define WE_BSP_H

#include <stddef.h>
#include "we_debreq.h"   /* E_D_REQ_IO: the start's clock, timers and lines */

typedef struct e_bsp_session e_bsp_session;

/* ---- the start on the loop ---------------------------------------------
 *
 * e_bsp_start_async() launches the build server and sends the first request
 * of the handshake above, then returns: each answer, read by
 * e_bsp_read_now() whenever the loop finds e_bsp_fd() readable, sends the
 * next request, so the editor keeps its keys while Bloop compiles.  @io
 * (the line engine's E_D_REQ_IO: now_ms, arm_timer, waiting, notice, name;
 * may be NULL) gives the request out its waiting line after
 * E_D_REQ_QUIET_MS and a deadline of its own (a first compile may fetch the
 * Scala toolchain); the host calls e_bsp_request_timer() when a timer it
 * asked for goes off.  @done is told once, from inside e_bsp_read_now() or
 * e_bsp_request_timer(), with @ok 1 when the endpoint is known
 * (e_bsp_endpoint, e_bsp_main_class), else 0 with the reason in
 * e_bsp_error().  e_bsp_close() during the start gives it up and tells
 * nobody.  Returns NULL (reason in @errbuf) when the server could not be
 * started. */
typedef void (*e_bsp_started_fn)(int ok, void *ctx);

/* Asked, when set, before the server a project's .bsp/<file>.json names is
 * started: @rootdir the project, @file the connection file's name, @argv
 * the command it names.  0 starts it; anything else starts nothing and the
 * start fails with E_BSP_NOT_APPROVED.  The editor sets it (a project
 * folder's command runs only after the user's yes); a test may leave it
 * unset. */
typedef int (*e_bsp_approve_fn)(const char *rootdir, const char *file,
                                char *const *argv);
void e_bsp_set_approver(e_bsp_approve_fn fn);
/* The connection file a start would read (scala-cli.json by name first)
 * and the command it names; free with e_bsp_free_argv.  NULL if none. */
char **e_bsp_connection_argv(const char *workdir, char *file, size_t filesz);
void e_bsp_free_argv(char **argv);
#define E_BSP_NOT_APPROVED "the build server's command was not approved"

e_bsp_session *e_bsp_start_async(const char *workdir, const E_D_REQ_IO *io,
                                 e_bsp_started_fn done, void *ctx,
                                 char *errbuf, size_t errsz);
int  e_bsp_fd(const e_bsp_session *b);
/* Read what the build server has sent, without waiting; returns 0 once its
 * output has ended. */
int  e_bsp_read_now(e_bsp_session *b);
void e_bsp_request_timer(e_bsp_session *b, int which);
/* Is the start still running?  The request it waits for, and since when. */
int  e_bsp_starting(const e_bsp_session *b);
const char *e_bsp_request_waited(const e_bsp_session *b, long *secs);
const char *e_bsp_endpoint(const e_bsp_session *b, int *port);
const char *e_bsp_main_class(const e_bsp_session *b);
const char *e_bsp_error(const e_bsp_session *b);

/* Does the project lack its `.bsp` connection file?  Then `scala-cli setup-ide
 * <workdir>` must write it before e_bsp_start; the editor runs that command as
 * a child of its own, without waiting for it. */
int e_bsp_needs_setup(const char *workdir);

/* Start a BSP session for the scala-cli/Bloop project rooted at `workdir`, drive
 * it to a debug session, and report the DAP endpoint.  Reads the `.bsp`
 * connection file (see e_bsp_needs_setup), launches the BSP server, and runs
 * the handshake above.  On success returns a live session
 * (KEEP IT OPEN until debugging ends) and fills *host/*port with the tcp:// DAP
 * endpoint and *mainclass with the discovered main class.  On failure returns
 * NULL and writes a short reason into errbuf (for the Messages window).
 * Waits for every answer: we_bsp_sync.c, the C tests only. */
e_bsp_session *e_bsp_start(const char *workdir, const char *srcfile,
                           char *host, size_t hostsz, int *port,
                           char *mainclass, size_t mcsz,
                           char *errbuf, size_t errsz);

/* End the BSP server's launcher and free the session, without waiting: SIGTERM
 * now, SIGKILL after the grace of the editor's children's owner (we_child.c),
 * on its loop.  Only the launcher is signalled, never its process group: the
 * shared build server it may have started goes on.  The program being
 * debugged is ended by the DAP disconnect, before this. */
void e_bsp_close(e_bsp_session *b);

#endif /* WE_BSP_H */
