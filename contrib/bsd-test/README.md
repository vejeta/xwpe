# BSD build + console-mouse test harness

Reproducible Vagrant VMs that verify xwpe **builds and RUNS** on **FreeBSD 14**,
**OpenBSD 7.x** and **NetBSD 9**: after the build each `provision.sh` runs the
shared `run_tests.sh`, which drives the unit tests, the `wpe` console mouse, and
`xwpe` rendering under Xvfb. Developer/maintainer tooling (like `tests/x11/`),
not part of a normal build.

Shared, single-copy test code lives in this directory (never duplicated per
platform); the per-platform folders hold only the `Vagrantfile` + `provision.sh`
that install deps and build:
- `sgr_mouse_probe.c` — console-mouse probe for `wpe`
- `xvfb_smoke.sh` — headless X11 render smoke for `xwpe`
- `run_tests.sh` — runs all three checks in order

## What it checks

1. **Build + link.** Each `provision.sh` installs the deps with the platform's
   package manager, fetches the latest `main` archive from Codeberg, runs
   `autoreconf -fi && ./configure --without-gpm --disable-dependency-tracking`
   and builds with **gmake** (`gmake -j$(ncpu)`), then reports the resulting
   `./we` ELF. It exercises the BSD portability that lives in `configure.ac`
   (libexecinfo for `backtrace()`; base `curses` / `ncurses.pc` fallback; `-std`
   flag detection; the `vterm03` pkgsrc module name) plus the `<sys/ioctl.h>`
   include — all no-ops on Linux.

   Two BSD-only build gotchas the harness handles (both are why a plain
   `./configure && make` from a tarball fails on the BSDs but not on Linux):
   - **`--disable-dependency-tracking`.** `config.status` bootstraps the automake
     `.deps` fragments by running the *default* make, which on the BSDs is BSD
     make; it chokes on the GNU-make dependency syntax ("Something went wrong
     bootstrapping makefile fragments"). We build with `gmake`, so the harness
     disables dep-tracking to skip that bootstrap. (Alternatively
     `MAKE=gmake ./configure`.)
   - **`SOURCE_DATE_EPOCH` must not be empty.** Building from the archive/release
     tarball there is no `.git`, so `Makefile.am`'s git-timestamp lookup is empty;
     a modern clang/gcc rejects an empty `SOURCE_DATE_EPOCH`. Fixed upstream
     (exported only when non-empty); packager builds set it in the environment.

2. **Console mouse.** `sgr_mouse_probe.c` is a tiny `forkpty()` harness: it
   launches the real `wpe` under a pty, injects an SGR (1006) mouse click on the
   File menu, and checks the menu opens. It exercises each platform's *actual*
   curses (FreeBSD termcap console, OpenBSD base curses) — including under
   `TERM=vt220`, whose terminfo has no SGR-capable `kmous`, so only xwpe's own
   terminfo-independent SGR decoder can have opened the menu. Build and run it on
   the VM:

   ```sh
   cc -O2 -o /tmp/probe contrib/bsd-test/sgr_mouse_probe.c -lutil   # -l util on all BSDs
   /tmp/probe /path/to/we xterm-direct        # also try: xterm-256color, vt220, wsvt25
   ```

3. **X11 render smoke.** `xvfb_smoke.sh` runs the built `xwpe` under Xvfb + a tiny
   WM (`twm` with a `NoTitle/RandomPlacement/NoGrabServer` rc, or `matchbox`),
   captures its window through xwpe's own `XWPE_X_DUMP` hook (a PPM — no external
   screenshot tool) and asserts it painted a real multi-colour UI, not a blank
   frame. This proves the X11 build actually *renders* on the platform, the
   analogue of driving `wpe` under a pty. It **skips cleanly** if `Xvfb`, a WM or
   `python3` is unavailable, so a bare box never fails the build over it.

   ```sh
   sh contrib/bsd-test/xvfb_smoke.sh /path/to/build-tree   # PASS / FAIL / SKIP
   ```

## Running it

Needs `libvirt` + `qemu` + Vagrant on the host (Docker cannot run a BSD kernel).

```sh
cd contrib/bsd-test/freebsd      # or openbsd / netbsd
VAGRANT_DEFAULT_PROVIDER=libvirt vagrant up         # builds from Codeberg main
VAGRANT_DEFAULT_PROVIDER=libvirt vagrant ssh        # poke around; run the probe
vagrant destroy -f               # when done
```

Each `provision.sh` builds from the **Codeberg `main` archive** (not a git
clone: an aged box may pull a `git` needing a newer `libpcre2` than base), so a
fresh `vagrant up` always tests the current `main`. On failure it prints only the
`error:` lines (full log at `/tmp/xwpe-build.log` in the VM).

### Box / version quirks captured in the provision scripts

- **OpenBSD** uses `generic/openbsd7` (= 7.4, EOL): the EOL package set is pruned
  from the CDN, so `PKG_PATH` points at the leaseweb archive; the X sets
  (`xbase`/`xshare`/`xfont`) are fetched and `ldconfig` is run; wide-char curses
  comes from the base system (no `ncursesw.pc`); `tar` has no
  `--strip-components`.
- **NetBSD** uses `generic/netbsd9` (= 9.3): fetch the X sets (`xbase`/`xcomp`);
  pkgsrc names are `libvterm03` and `ncurses`; `PKG_CONFIG_PATH` must put
  `/usr/pkg` FIRST so pkgsrc's newer `fontconfig.pc` wins over the base X set's.
- **FreeBSD** (`generic/freebsd14`) needs the fewest tweaks: the X sets ship in
  base and `ncursesw.pc` is present.

## Not here: the port-submission files

The actual *ports* (FreeBSD `devel/xwpe`, OpenBSD `editors/xwpe`, NetBSD pkgsrc
`editors/xwpe`) live in their respective ports trees, and the update artefacts
(`Makefile`, `distinfo`, `pkg-descr`, `pkg-plist`) are submitted there
(bugs.freebsd.org, ports@openbsd.org, pkgsrc) — they are not xwpe source, just as
the Debian packaging lives on Salsa. This directory only proves the source
builds and runs on each BSD.
