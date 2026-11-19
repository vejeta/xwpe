#!/bin/sh
# ci-run.sh OS
#
# The build-and-test body of the BSD CI job (.github/workflows/bsd.yml), one
# file for FreeBSD, OpenBSD and NetBSD so the three jobs cannot drift apart.
#
#   OS   freebsd, openbsd or netbsd
#
# It runs as the unprivileged user "tester", never as root: the editor is not
# run as root by its users, and root ignores file modes, so every permission
# row of the tests would be skipped or wrong.  The workflow step, which is
# root, makes the user, the output folder and the ownership of the checkout,
# does anything that needs root (OpenBSD's ldconfig), then runs
#     su -m tester -c "sh contrib/bsd-test/ci-run.sh OS"
# `su -m` keeps the environment (SELECTED_TESTS, WPE_EXPECT_BUILT, PATH) but
# also keeps root's HOME, which tester cannot read, so HOME, USER and LOGNAME
# are set here.
#
# Exit status: 0 everything passed, 1 a build or test step failed.  Logs go to
# bsd-out/ next to the checkout; the workflow step copies them back.

usage() {
  echo "usage: ci-run.sh freebsd|openbsd|netbsd" >&2
  exit 2
}

[ $# -eq 1 ] || usage
os=$1
case $os in
  freebsd | openbsd | netbsd) ;;
  *) usage ;;
esac

HOME=/home/tester
USER=tester
LOGNAME=tester
# Tells bsd-known-failures.sh that file modes are honoured in this run, so a
# permission row the tests skipped is a finding, not a pass.
WPE_TEST_UNPRIVILEGED=1
export HOME USER LOGNAME WPE_TEST_UNPRIVILEGED

mkdir -p bsd-out

# find_python: print the Python interpreter the packages installed.  The
# workflow names the Python packages in one place (its prepare script); this
# finds whichever minor version they brought, so a release bump there needs
# no edit here.  Takes python3 when a link by that name exists, otherwise the
# highest python3.N in the package prefixes.  Prints nothing when none is
# installed, and the caller stops with a message instead of a vague pytest
# error later.
find_python() {
  command -v python3 && return 0
  ls /usr/local/bin/python3.[0-9]* /usr/pkg/bin/python3.[0-9]* 2>/dev/null \
    | grep -E 'python3\.[0-9]+$' | sort -t. -k2,2n | tail -1
}

# os_environment OS: set PY (the Python interpreter the packages installed)
# and the variables the system's toolchain needs to find its libraries.
os_environment() {
  case $1 in
    freebsd)
      PY=$(find_python)
      ;;
    openbsd)
      PY=$(find_python)
      AUTOCONF_VERSION="$(ls /usr/local/bin/autoconf-* 2>/dev/null | sed 's/.*autoconf-//' | sort -V | tail -1)"
      AUTOMAKE_VERSION="$(ls -d /usr/local/share/automake-* 2>/dev/null | sed 's/.*automake-//' | sort -V | tail -1)"
      export AUTOCONF_VERSION AUTOMAKE_VERSION
      PKG_CONFIG_PATH=/usr/X11R6/lib/pkgconfig:/usr/local/lib/pkgconfig
      export PKG_CONFIG_PATH
      ;;
    netbsd)
      PATH=/usr/pkg/bin:/usr/pkg/sbin:$PATH
      PY=$(find_python)
      PKG_CONFIG_PATH=/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig:/usr/lib/pkgconfig
      export PATH PKG_CONFIG_PATH
      ;;
  esac
}

# write_env_log: record what the next classification of a failure needs to
# know about this machine and this user: the path limit, the resource limits
# the user got from its login class, the awk, the identity.
write_env_log() {
  {
    echo "== getconf PATH_MAX /"
    getconf PATH_MAX /
    echo "== ulimit -a"
    ulimit -a
    echo "== awk version"
    awk --version 2>&1 | head -1
    echo "== id"
    id
  } > bsd-out/env.log 2>&1
}

# append_nm_to_env_log: the head of this system's own `nm -g` listing of one
# object, so a symbol-reader row can be judged against the real format.
append_nm_to_env_log() {
  {
    echo "== nm -g we_paint.o (head)"
    nm -g we_paint.o 2>&1 | head -20
  } >> bsd-out/env.log 2>&1
}

# run_selected_tests: run the pytest ids the caller chose, alone, with full
# tracebacks.  Returns pytest's status.
run_selected_tests() {
  # Chosen ids: split on blanks, never glob.
  set -f
  getconf PATH_MAX /
  getconf NAME_MAX /
  WPE_BIN="$PWD/wpe" tests/with-deadline.sh 2400 $PY -m pytest -v --tb=long -p no:cacheprovider $SELECTED_TESTS
}

# run_xvfb_smoke: the X11 smoke test.  FreeBSD installs the server as a
# package, so a missing one is a failure there; the other two ship it in
# their base sets and skip the step when it is not on the PATH.
run_xvfb_smoke() {
  if [ "$os" = freebsd ] || command -v Xvfb >/dev/null 2>&1; then
    tests/with-deadline.sh 300 sh contrib/bsd-test/xvfb_smoke.sh "$PWD" > bsd-out/xvfb.log 2>&1 \
      || { rc=1; tail -20 bsd-out/xvfb.log; }
  fi
}

rc=0
os_environment "$os"
[ -n "$PY" ] || { echo "ci-run: no python3 installed by the prepare step"; exit 1; }
write_env_log

autoreconf -fi > bsd-out/autoreconf.log 2>&1 || { tail -20 bsd-out/autoreconf.log; exit 1; }
./configure --without-gpm --disable-dependency-tracking \
  --enable-ai --enable-ai-agent-host > bsd-out/configure.log 2>&1 \
  || { tail -30 bsd-out/configure.log; exit 1; }
grep -E 'math library|libvterm|X11|ncursesw|AI' bsd-out/configure.log
tests/with-deadline.sh 600 gmake -j"$(sysctl -n hw.ncpu)" > bsd-out/build.log 2>&1 \
  || { grep -nE 'error:|undefined|ld:' bsd-out/build.log | head -40; exit 1; }
append_nm_to_env_log

if [ -n "$SELECTED_TESTS" ]; then
  run_selected_tests
  exit $?
fi

# Every test runs under with-deadline.sh: base timeout(1) has no KILL.
tests/with-deadline.sh 2700 gmake check LOG_COMPILER="$PWD/tests/with-deadline.sh 300" > bsd-out/check.log 2>&1
check_rc=$?
echo "gmake check exit status: $check_rc"
grep -cE '^(PASS|FAIL|SKIP|ERROR|TIMEOUT):' bsd-out/check.log
# bsd-known-failures.sh reads each failing test's own log from the folder of
# check.log, so the logs must be there before it runs; the copy at the end
# picks up whatever the probes and pytest wrote after.
cp test-suite.log test_*.log tests/*.log bsd-out/ 2>/dev/null
# The verdict goes to a file first so the script's exit status is not lost in
# a pipe; the file is then shown, and the artifact carries it.
sh tests/bsd-known-failures.sh "$os" bsd-out/check.log "$check_rc" > bsd-out/known-failures.log 2>&1 || rc=1
cat bsd-out/known-failures.log
{ cc -O2 -o /tmp/probe contrib/bsd-test/sgr_mouse_probe.c -lutil \
  && tests/with-deadline.sh 300 /tmp/probe ./we xterm-256color; } \
  > bsd-out/probe.log 2>&1 || { rc=1; tail -20 bsd-out/probe.log; }
run_xvfb_smoke
WPE_BIN="$PWD/wpe" tests/with-deadline.sh 600 $PY -m pytest -q -p no:cacheprovider \
  tests/test_menu_file.py tests/test_option_file_sections.py tests/test_file_manager_folder.py > bsd-out/pytest.log 2>&1 \
  || { rc=1; tail -20 bsd-out/pytest.log; }
cp test-suite.log test_*.log tests/*.log bsd-out/ 2>/dev/null
exit $rc
