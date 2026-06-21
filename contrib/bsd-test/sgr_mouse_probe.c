/*
 * Drive the real wpe under a pty on this BSD box and verify the console
 * mouse: feed an SGR (1006) left-click on the "File" menu-bar entry and
 * check the File menu opens (its "Save"/"New" items appear).  This exercises
 * the platform's actual curses + terminfo, which is the whole point: on a
 * terminal whose terminfo lacks an SGR-capable kmous the report is not folded
 * into KEY_MOUSE and wpe must decode it itself.
 *
 * argv[1] = path to the wpe binary, argv[2] = TERM to give the child.
 * Exit 0 and print "MENU-OPENED" if the menu appeared, else exit 1.
 */
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>          /* forkpty() on OpenBSD/NetBSD/macOS */
#else
#include <libutil.h>       /* forkpty() on FreeBSD */
#endif
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

static void drain(int fd, char *acc, size_t *len, size_t cap, int ms)
{
    struct timespec ts = { 0, 50L * 1000000L };
    int waited = 0;
    for (;;) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (*len + (size_t)n < cap) { memcpy(acc + *len, buf, n); *len += n; }
            waited = 0;
            continue;
        }
        nanosleep(&ts, NULL);
        waited += 50;
        if (waited >= ms) break;
    }
}

int main(int argc, char **argv)
{
    const char *bin  = argc > 1 ? argv[1] : "./we";
    const char *term = argc > 2 ? argv[2] : "xterm";
    int master;
    pid_t pid;
    struct winsize ws = { 25, 80, 0, 0 };
    static char acc[1 << 20];
    size_t len = 0;
    const char *seed = "AAAAA\nBBBBB\nCCCCC\n";
    char tmpl[] = "/tmp/sgrprobe.XXXXXX";
    int sfd = mkstemp(tmpl);
    write(sfd, seed, strlen(seed));
    close(sfd);

    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return 2; }
    if (pid == 0) {
        setenv("TERM", term, 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);
        setenv("XWPE_LSP_NO_EAGER", "1", 1);
        execl(bin, bin, tmpl, (char *)NULL);
        _exit(127);
    }

    fcntl(master, F_SETFL, O_NONBLOCK);
    drain(master, acc, &len, sizeof acc, 1500);   /* let the UI paint */

    /* SGR left-button press+release on the menu bar.  "File" sits a few cells
       in; column 7 (1-based) lands on it in the default 80-col layout. */
    write(master, "\x1b[<0;7;1M", 8);
    write(master, "\x1b[<0;7;1m", 8);
    drain(master, acc, &len, sizeof acc, 1200);

    write(master, "\x18", 1);                      /* Alt-X-ish? no: ^X quit  */
    drain(master, acc, &len, sizeof acc, 400);
    kill(pid, 9);
    waitpid(pid, NULL, 0);
    unlink(tmpl);

    acc[len < sizeof acc ? len : sizeof acc - 1] = 0;
    if (strstr(acc, "Save") || strstr(acc, "New ") || strstr(acc, "New\t")) {
        printf("MENU-OPENED (TERM=%s)\n", term);
        return 0;
    }
    fprintf(stderr, "NO MENU (TERM=%s); last bytes:\n", term);
    fwrite(acc + (len > 600 ? len - 600 : 0), 1,
           len > 600 ? 600 : len, stderr);
    fprintf(stderr, "\n");
    return 1;
}
