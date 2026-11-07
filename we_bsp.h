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

typedef struct e_bsp_session e_bsp_session;

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
 * NULL and writes a short reason into errbuf (for the Messages window). */
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
