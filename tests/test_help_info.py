"""Help -> Info opens xwpe's OWN Texinfo manual (wpe, terminal mode).

Borland's Help > Info historically dropped the user into the system-wide Info
directory (every package on the machine).  xwpe should instead land in ITS OWN
manual (the installed @file{xwpe.info}); e_read_xwpe_info loads the Top node of
xwpe.info, falling back to the system directory only if xwpe.info is not on the
Info path.

We point @env{INFOPATH} at the built @file{docs/} so the test works from the
build tree (uninstalled).  If @file{docs/xwpe.info} has not been built, the test
skips cleanly.

Run: tests/.venv/bin/python -m pytest -v tests/test_help_info.py
"""
import os
import pty
import select
import subprocess
import time

import pytest
import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get('WPE_BIN') or os.path.join(os.path.dirname(__file__), '..', 'wpe')
DOCS = os.path.join(os.path.dirname(__file__), '..', 'docs')
INFO = os.path.join(DOCS, 'xwpe.info')

ALT_H = '\033h'          # open the Help menu
INFO_ITEM = 'i'          # Help -> Info


def _open_info(workdir, with_infopath=True):
    """Launch wpe, open Help -> Info, return the (screen text, alive).

    with_infopath=True points INFOPATH at the built docs/ so an UNINSTALLED
    build tree finds the manual; with_infopath=False unsets INFOPATH (and
    XWPE_LIB) so the binary must locate xwpe.info via the infodir compiled in
    at build time -- the install path that was broken for any --prefix != /usr.
    """
    with open(os.path.join(workdir, 't.c'), 'w') as fh:
        fh.write('int main(void){ return 0; }\n')
    screen = SafeScreen(80, 30)
    stream = pyte.Stream(screen)
    master_fd, slave_fd = pty.openpty()
    env = os.environ.copy()
    env.update(TERM='xterm-256color', COLUMNS='80', LINES='30',
               LC_ALL='en_US.UTF-8', HOME=workdir)
    if with_infopath:
        env['INFOPATH'] = os.path.abspath(DOCS)
    else:
        env.pop('INFOPATH', None)
        env.pop('XWPE_LIB', None)
    proc = subprocess.Popen([WPE_BIN, 't.c'], stdin=slave_fd, stdout=slave_fd,
                            stderr=slave_fd, cwd=workdir, env=env,
                            preexec_fn=os.setsid)
    os.close(slave_fd)

    def drain(timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([master_fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(master_fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                stream.feed(data.decode('utf-8', 'replace'))

    try:
        drain(1.3)
        for key in (ALT_H, INFO_ITEM):
            os.write(master_fd, key.encode())
            drain(0.9)
        text = "\n".join(screen.display)
        alive = proc.poll() is None
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        os.close(master_fd)
        proc.wait()
    return text, alive


@pytest.mark.skipif(not os.path.exists(INFO),
                    reason="docs/xwpe.info not built (run 'make info' or a full build)")
def test_help_info_opens_xwpe_manual(tmp_path):
    """Help -> Info lands in xwpe's own manual, not the system Info directory."""
    text, alive = _open_info(str(tmp_path))
    assert alive, "wpe died opening Help -> Info:\n%s" % text
    # Content that exists only in xwpe's manual Top node, never in the system dir.
    assert "manual for xwpe" in text or "Introduction" in text, \
        "Help -> Info should show xwpe's own manual, got:\n%s" % text


def _installed_manual(wpe_bin):
    """xwpe.info beside an INSTALLED binary: <prefix>/bin/we -> <prefix>/share/info."""
    prefix = os.path.dirname(os.path.dirname(os.path.realpath(wpe_bin)))
    return os.path.join(prefix, 'share', 'info', 'xwpe.info')


@pytest.mark.skipif(not os.path.exists(_installed_manual(WPE_BIN)),
                    reason="wpe not installed beside its manual; the compiled-in "
                           "infodir path is only testable against an installed build "
                           "(set WPE_BIN=<prefix>/bin/wpe after 'make install')")
def test_help_info_finds_installed_manual_without_infopath(tmp_path):
    """An INSTALLED wpe opens its manual with NO $INFOPATH -- it locates xwpe.info
    via the infodir compiled in at build time ($(infodir)).

    Regression: the Info search path was a fixed
    "/usr/share/info:/usr/local/info:/usr/info", so Help -> Info found the manual
    ONLY when --prefix happened to be /usr.  Every other install -- ~/.local or a
    Homebrew prefix on macOS, even the default /usr/local (whose share/info was
    not on that list) -- silently showed the system dir node instead.  The fix
    bakes $(infodir) into the path; this proves it WITHOUT the INFOPATH crutch the
    test above leans on."""
    text, alive = _open_info(str(tmp_path), with_infopath=False)
    assert alive, "wpe died opening Help -> Info (installed, no INFOPATH):\n%s" % text
    assert "manual for xwpe" in text or "Introduction" in text, \
        "installed Help -> Info did not find xwpe.info via the compiled-in " \
        "infodir:\n%s" % text
