/* we_bsp_sync.c -- the build server's start, waited for, for the C tests only.
 *
 * The editor starts the build server on its loop (e_bsp_start_async: each
 * answer sends the next request) and never waits.  The C tests drive it
 * without an editor and want the endpoint in hand: e_bsp_start runs the loop
 * (we_fdloop.c) until the start has ended.  This file is not in the
 * editor's link set.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>

#include "we_bsp.h"
#include "we_fdloop.h"

#define BSP_SYNC_STEPS    5        /* the requests of the start               */
#define BSP_SYNC_STEP_MS  240000   /* each one's bound, as the editor's deadline */

/* What the start told. */
typedef struct {
 int done;
 int ok;
} bsp_sync_wait;

/** bsp_sync_now_ms - A monotonic instant, for the wait's deadline. */
static long bsp_sync_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** bsp_sync_told - The start ended. */
static void bsp_sync_told(int ok, void *ctx)
{
 bsp_sync_wait *w = ctx;

 w->done = 1;
 w->ok = ok;
}

/** bsp_sync_on_readable - The loop found the build server's output readable. */
static void bsp_sync_on_readable(int fd, void *data)
{
 (void)fd;
 e_bsp_read_now(data);
}

/**
 * bsp_sync_run - Run the loop until the start of @b has ended or the
 * bound passed.
 * Return: 1 when it ended.
 */
static int bsp_sync_run(e_bsp_session *b, bsp_sync_wait *w)
{
 long deadline = bsp_sync_now_ms() + (long)BSP_SYNC_STEPS * BSP_SYNC_STEP_MS;
 int fd = e_bsp_fd(b);

 if (fd < 0 || wpe_fd_add(fd, POLLIN, bsp_sync_on_readable, b) != 0)
  return 0;
 while (!w->done)
 {
  long left = deadline - bsp_sync_now_ms();

  if (left <= 0)
   break;
  wpe_fd_poll((int)left);
 }
 wpe_fd_del(fd);
 return w->done;
}

e_bsp_session *e_bsp_start(const char *workdir, const char *srcfile,
                           char *host, size_t hostsz, int *port,
                           char *mainclass, size_t mcsz,
                           char *errbuf, size_t errsz)
{
 bsp_sync_wait w;
 e_bsp_session *b;
 const char *h;

 (void)srcfile;
 memset(&w, 0, sizeof w);
 b = e_bsp_start_async(workdir, NULL, bsp_sync_told, &w, errbuf, errsz);
 if (!b)
  return NULL;
 if (!bsp_sync_run(b, &w) || !w.ok)
 {
  if (errbuf && errsz)
  {
   const char *why = e_bsp_error(b);
   long secs = 0;
   const char *method = e_bsp_request_waited(b, &secs);

   if (method && !w.done)
    snprintf(errbuf, errsz, "BSP timeout/EOF on %s", method);
   else
    snprintf(errbuf, errsz, "%s", why ? why : "");
  }
  e_bsp_close(b);
  return NULL;
 }
 h = e_bsp_endpoint(b, port);
 snprintf(host, hostsz, "%s", h ? h : "");
 if (mainclass && mcsz)
  snprintf(mainclass, mcsz, "%s", e_bsp_main_class(b));
 return b;
}
