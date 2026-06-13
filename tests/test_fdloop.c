/* test_fdloop.c -- the fd-loop must reap a hung-up fd, not spin on it.
 *
 * Regression for the 100% CPU spin: when a registered fd (an LSP / debug
 * server that exited) reaches EOF, poll() reports POLLHUP -- and, once the
 * last bytes are drained, POLLHUP WITHOUT POLLIN.  wpe_fd_poll() used to
 * dispatch only on POLLIN|POLLPRI, so the read callback that reaps the dead
 * fd never ran; poll() then returned instantly forever and the input loop
 * burned a core.  wpe_fd_poll() now dispatches on POLLHUP/POLLERR too (and
 * drops wake-only fds that hang up), so the fd is reaped on the first poll.
 */
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include "we_fdloop.h"

static int cb_calls = 0;
static int reaped_fd = -1;

/* Mimics e_lsp_on_fd_readable / e_d_on_gdb_readable: drain, and on EOF reap. */
static void on_ready(int fd, void *data)
{
 char buf[64];
 ssize_t n = read(fd, buf, sizeof buf);
 (void)data;
 cb_calls++;
 if (n <= 0) { reaped_fd = fd; wpe_fd_del(fd); }
}

static int eof_pipe(void)
{
 int p[2];
 if (pipe(p) != 0) return -1;
 close(p[1]);              /* write end closed -> read end is at EOF (POLLHUP) */
 return p[0];
}

/* A callback fd at EOF is dispatched (reaped) once, then the loop is quiet. */
static int test_callback_fd_reaped_on_hup(void)
{
 int fd = eof_pipe();
 int r1, r2;
 if (fd < 0) return 1;
 cb_calls = 0; reaped_fd = -1;
 wpe_fd_add(fd, POLLIN, on_ready, NULL);
 r1 = wpe_fd_poll(0);     /* HUP must dispatch the callback, which reaps fd */
 r2 = wpe_fd_poll(0);     /* fd gone -> no work, NO spin */
 if (cb_calls != 1)  { printf("FAIL: callback not dispatched on HUP (calls=%d)\n", cb_calls); return 1; }
 if (reaped_fd != fd){ printf("FAIL: fd not reaped (%d)\n", reaped_fd); return 1; }
 if (r1 <= 0)        { printf("FAIL: poll did not report the hung-up fd (r1=%d)\n", r1); return 1; }
 if (r2 != 0)        { printf("FAIL: loop still busy after reap (r2=%d -> spin)\n", r2); return 1; }
 return 0;
}

/* A wake-only fd (NULL callback, like stdin/X11) that hangs up is dropped,
 * not left to spin. */
static int test_wakeonly_fd_dropped_on_hup(void)
{
 int fd = eof_pipe();
 int r1, r2;
 if (fd < 0) return 1;
 wpe_fd_add(fd, POLLIN, NULL, NULL);
 r1 = wpe_fd_poll(0);     /* HUP on a NULL-callback fd -> drop it */
 r2 = wpe_fd_poll(0);     /* dropped -> quiet */
 close(fd);
 if (r1 <= 0) { printf("FAIL: poll did not report wake-only HUP (r1=%d)\n", r1); return 1; }
 if (r2 != 0) { printf("FAIL: wake-only hung-up fd not dropped (r2=%d -> spin)\n", r2); return 1; }
 return 0;
}

int main(void)
{
 int rc = 0;
 rc |= test_callback_fd_reaped_on_hup();
 rc |= test_wakeonly_fd_dropped_on_hup();
 if (rc == 0) printf("test_fdloop: OK\n");
 return rc;
}
