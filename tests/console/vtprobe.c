/* vtprobe -- runs as init inside a micro-VM so the console layer can observe
 * what an ssh or terminal-emulator session never can: how the editor's own
 * console test answers on a real Linux virtual console.
 *
 * The predicate under test is NOT copied here.  The build extracts
 * WpeGpmIsConsoleTty() verbatim from we_gpm.c into pred.inc (see conftest.py)
 * and redirects STDIN_FILENO to the descriptor being probed, so this program
 * cannot drift from the editor it is testing.
 *
 * Output is one "CASE" line per descriptor, as key=value pairs, then
 * VTPROBE DONE.  It goes to the serial port, which qemu captures to a file.
 * The last act is a reboot, so qemu with -no-reboot exits on its own.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/major.h>
#include <linux/reboot.h>

static FILE *out;

/* The shipped console test: the pure per-descriptor predicate and the chooser
   that picks which descriptor to ask about.  Extracted verbatim from we_gpm.c
   at build time, so neither can drift from the editor. */
#include "pred.inc"

static void node(const char *path, int maj, int min)
{
	if (mknod(path, S_IFCHR | 0666, makedev(maj, min)) == 0 || errno == EEXIST)
		return;
	fprintf(out, "NOTE mknod=%s errno=%s\n", path, strerror(errno));
}

static void probe(const char *label, int fd)
{
	int mode = -1, kd, vt, pred;
	struct vt_stat vts;
	const char *name = "-";

	if (fd < 0) {
		fprintf(out, "CASE label=%s open=failed errno=%s\n", label,
			strerror(errno));
		fflush(out);
		return;
	}
	memset(&vts, 0, sizeof vts);
	kd = (ioctl(fd, KDGETMODE, &mode) == 0);
	vt = (ioctl(fd, VT_GETSTATE, &vts) == 0);
	if (isatty(fd) && ttyname(fd))
		name = ttyname(fd);
	pred = WpeGpmTtyIsConsole(fd);
	fprintf(out, "CASE label=%s isatty=%d ttyname=%s kdgetmode=%s "
		     "vtgetstate=%s predicate=%d\n",
		label, isatty(fd), name, kd ? "ok" : "enotty",
		vt ? "ok" : "enotty", pred);
	fflush(out);
	/* The descriptor is deliberately left open: closing a pty master
	   destroys its slave, so the next case would probe a dead end. */
}

int main(void)
{
	int master = -1, slave = -1;

	mkdir("/dev", 0755);
	mkdir("/dev/pts", 0755);
	node("/dev/console", TTYAUX_MAJOR, 1);
	node("/dev/ptmx", TTYAUX_MAJOR, 2);
	node("/dev/null", MEM_MAJOR, 3);
	node("/dev/ttyS0", TTY_MAJOR, 64);
	node("/dev/tty0", TTY_MAJOR, 0);
	node("/dev/tty1", TTY_MAJOR, 1);
	node("/dev/tty63", TTY_MAJOR, 63);
	mount("devpts", "/dev/pts", "devpts", 0, NULL);

	out = fopen("/dev/ttyS0", "w");
	if (!out)
		out = stderr;
	setvbuf(out, NULL, _IOLBF, 0);

	fprintf(out, "VTPROBE kernel=%s\n", "linux");
	probe("vt0", open("/dev/tty0", O_RDWR | O_NOCTTY));
	probe("vt1", open("/dev/tty1", O_RDWR | O_NOCTTY));
	probe("vt63", open("/dev/tty63", O_RDWR | O_NOCTTY));
	probe("console", open("/dev/console", O_RDWR | O_NOCTTY));
	probe("serial", open("/dev/ttyS0", O_RDWR | O_NOCTTY));
	if (openpty(&master, &slave, NULL, NULL, NULL) == 0) {
		probe("pty_master", master);
		probe("pty_slave", slave);
	} else {
		fprintf(out, "CASE label=pty open=failed errno=%s\n",
			strerror(errno));
	}
	probe("not_a_tty", open("/dev/null", O_RDWR));

	/* The chooser the editor actually calls.  It prefers stdin and falls back
	   to the controlling terminal when stdin is not one, which is what keeps
	   the mouse on a console session started as `wpe < notes`.  Both halves
	   are reported so the host asserts the relationship instead of guessing
	   which branch ran. */
	fprintf(out, "CASE label=selector isatty0=%d stdin_console=%d selector=%d\n",
		isatty(STDIN_FILENO), WpeGpmTtyIsConsole(STDIN_FILENO),
		WpeGpmIsConsoleTty());
	fflush(out);
	{
		int saved = dup(STDIN_FILENO);
		int nullfd = open("/dev/null", O_RDWR);
		int ttyfd = open("/dev/tty", O_RDONLY | O_NOCTTY);
		int sel;

		if (saved >= 0 && nullfd >= 0)
			dup2(nullfd, STDIN_FILENO);
		sel = WpeGpmIsConsoleTty();
		fprintf(out, "CASE label=selector_redirected selector=%d devtty=%s "
			"devtty_console=%d\n",
			sel, ttyfd >= 0 ? "ok" : "unavailable",
			ttyfd >= 0 ? WpeGpmTtyIsConsole(ttyfd) : 0);
		fflush(out);
		if (saved >= 0) {
			dup2(saved, STDIN_FILENO);
			close(saved);
		}
		if (nullfd >= 0)
			close(nullfd);
		if (ttyfd >= 0)
			close(ttyfd);
	}

	fprintf(out, "VTPROBE DONE\n");
	fflush(out);
	sync();
	reboot(LINUX_REBOOT_CMD_RESTART);
	for (;;)
		pause();
	return 0;
}
