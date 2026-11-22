"""macOS / kitty smoke for ./wpe.

Drives wpe directly in a pty (the proven-stable path -- the pytest+pyte
harness has a separate, macOS-only EIO race we are not chasing here).
One check per prerelease-checklist item, prints PASS/FAIL/SKIP with a
short reason.

Two entry points:
  * standalone script (the one we trust on Darwin):
        tests/.venv/bin/python tests/test_macos_smoke.py
  * pytest collection: runs the script as a subprocess and asserts
    exit==0, so pytest's own EIO race cannot poison it.
"""
from __future__ import annotations
import os, sys, pty, select, time, tempfile, shutil

import gui_owner
from host_tools import tool_usable

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
WPE = os.environ.get('WPE_BIN') or os.path.join(TOP, 'wpe')
COLS, ROWS = 100, 30
# The whole script, started by the pytest entry point, runs at most this long
# (before the scale): its own waits add up to a few minutes on a slow runner.
SMOKE_TIMEOUT_S = 900
# A short probe of the system (ps, file) is bounded by this (before the scale).
PROBE_TIMEOUT_S = 30


def _import_pyte():
    sys.path.insert(0, HERE)
    import pyte  # noqa: E402
    from test_utf8_border import SafeScreen  # noqa: E402
    return pyte, SafeScreen


def open_wpe(files, workdir, term='xterm-256color', extra_env=None):
    pyte, SafeScreen = _import_pyte()
    screen = SafeScreen(COLS, ROWS); stream = pyte.Stream(screen)
    master, slave = pty.openpty()
    # Load the in-app help from the checkout so the F1/Help check works from a
    # plain build tree (no `make install`), via the editor's "_eng" fallback.
    # The owner starts the editor: the pty is sized and is its controlling
    # terminal, the environment is its allow-list (no display variable) and
    # these variables, None removes one.
    env = {'TERM': term, 'XWPE_LIB': os.path.dirname(os.path.abspath(WPE))}
    env.update(extra_env or {})
    owned = gui_owner.Terminal.start(
        gui_owner.current(), WPE, list(files), workdir, workdir, master, slave,
        (COLS, ROWS), extra_env=env)
    os.close(slave)
    return owned.proc, master, screen, stream


# A loaded CI runner is slow; XWPE_TEST_WAIT_SCALE stretches every wait so the
# scenarios don't flake (default 1.0; the Debian autopkgtest sets 3). All
# scenario waits funnel through drain(), so scaling its deadline covers them.
WAIT_SCALE = gui_owner.wait_scale()

# Every action is checked by both of its routes on every system: the function
# key, which this script writes to the pty as bytes (no keyboard layer lies
# between it and the editor), and the menu route, which is how a user whose
# system keeps F1-F12 for itself reaches the same action.
KEY_ROUTE = 'function key'
MENU_ROUTE = 'menu route'
ROUTES = (KEY_ROUTE, MENU_ROUTE)


def drain(master, stream, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.1)
        if r:
            try:
                data = os.read(master, 65536)
            except OSError:
                return
            if not data:
                return
            stream.feed(data.decode('utf-8', 'replace'))


def _owned_by(proc):
    """The owner's entry for the editor process open_wpe returned."""
    for owned in gui_owner.current().ledger.entries():
        if owned.proc is proc:
            return owned
    return None


def cleanup(proc, master):
    """End the editor through the owner (SIGTERM, its own ladder, SIGKILL,
    what it left found and ended) and close its terminal."""
    owned = _owned_by(proc)
    if owned is not None:
        owned.end_for_row()
    try: os.close(master)
    except Exception: pass


def screen_has(screen, s):
    return any(s in line for line in screen.display)


def action(m, st, route, fkey, *menu, wait=0.7):
    """Trigger an action by one of its two routes: its function key, or its
    menu route (the strokes are sent one at a time with a drain so each
    dropdown renders before the next keypress)."""
    if route == MENU_ROUTE:
        for b in menu:
            os.write(m, b); drain(m, st, wait)
    else:
        os.write(m, fkey); drain(m, st, wait)


def cpu_percent(pid):
    try:
        out = gui_owner.run_tool(['ps', '-p', str(pid), '-o', '%cpu='],
                                 PROBE_TIMEOUT_S, check=True).stdout
        return float(out.decode().strip())
    except Exception:
        return None


def _run_checks():
    results = []  # list of (section, item, status, note)

    def rec(section, item, status, note=''):
        results.append((section, item, status, note))
        print(f'  [{status:4}] {item}  {("-- " + note) if note else ""}',
              flush=True)

    print('== §0 setup / sanity ==', flush=True)
    if os.path.exists(WPE):
        kind = gui_owner.run_tool(['file', WPE], PROBE_TIMEOUT_S,
                                  check=True).stdout.decode()
        rec('§0', 'binary exists', 'PASS', kind.split(':', 1)[1].strip())
    else:
        rec('§0', 'binary exists', 'FAIL', f'not found: {WPE}')
        return results
    rec('§0', f'TERM={os.environ.get("TERM")}',
        'PASS' if os.environ.get('TERM') else 'FAIL')

    print('\n== §3 1.6.5-new behaviour ==', flush=True)
    wd = tempfile.mkdtemp(prefix='xwpe-smoke-')
    open(os.path.join(wd, 'e.c'), 'w').write('int main(void){return 0;}\n')

    # System menu items appear by both routes: F10 highlights the menu bar
    # (Enter expands the first dropdown); Alt-# opens the System menu directly.
    for route in ROUTES:
        proc, m, scr, st = open_wpe(['e.c'], wd)
        drain(m, st, 1.5)
        if route == MENU_ROUTE:
            os.write(m, b'\x1b#'); drain(m, st, 0.5)        # Alt-# -> System menu
            label = 'Alt-# opens System menu'
        else:
            os.write(m, b'\x1b[21~'); drain(m, st, 0.5)     # F10 highlights menu bar
            os.write(m, b'\r'); drain(m, st, 0.5)           # Enter expands first dropdown
            label = 'F10 opens menu bar (Enter expands)'
        opened = screen_has(scr, 'About WE') or screen_has(scr, 'System Info')
        rec('§3', label,
            'PASS' if opened else 'FAIL',
            '' if opened else 'no System-menu items found')
        cleanup(proc, m)

    # bare Esc is a no-op
    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1b'); drain(m, st, 0.7)
    menu_open = screen_has(scr, 'Save As') or screen_has(scr, 'About WE')
    rec('§3', 'lone Esc is a no-op (does not open menu)',
        'PASS' if not menu_open else 'FAIL',
        '' if not menu_open else 'menu opened after bare ESC')
    cleanup(proc, m)

    # Alt-F opens the File menu
    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1bf'); drain(m, st, 0.7)
    rec('§3', 'Alt-F opens File menu',
        'PASS' if screen_has(scr, 'Save As') else 'FAIL')
    cleanup(proc, m)

    # no 100% CPU when an LSP server is missing
    fake = os.path.join(wd, 'bin'); os.makedirs(fake, exist_ok=True)
    open(os.path.join(wd, 'dead.scala'), 'w').write(
        'object X{def main(args:Array[String])={println("h")}}\n')
    proc, m, scr, st = open_wpe(['dead.scala'], wd, extra_env={'PATH': fake})
    drain(m, st, 1.5)
    os.write(m, b'\x1bqe'); drain(m, st, 1.0)
    samples = []
    for _ in range(4):
        time.sleep(0.5); c = cpu_percent(proc.pid)
        if c is not None: samples.append(c)
    peak = max(samples) if samples else 0.0
    rec('§3', 'No 100% CPU spin when LSP server is unavailable',
        'PASS' if peak < 50 else 'FAIL', f'peak %CPU = {peak:.1f}')
    cleanup(proc, m)

    print('\n== §4 general editing & UX ==', flush=True)

    open(os.path.join(wd, 'u.txt'), 'w').write('café пример 🦀\n')
    proc, m, scr, st = open_wpe(['u.txt'], wd)
    drain(m, st, 1.5)
    shown = ' '.join(scr.display)
    rec('§4', 'UTF-8: latin accent (é) visible',
        'PASS' if 'café' in shown else 'FAIL')
    rec('§4', 'UTF-8: Cyrillic visible',
        'PASS' if 'пример' in shown else 'FAIL')
    rec('§4', 'UTF-8: emoji visible',
        'PASS' if '🦀' in shown else 'FAIL')
    cleanup(proc, m)

    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1bx')
    t0 = time.time()
    while time.time() - t0 < 3 and proc.poll() is None:
        drain(m, st, 0.2)
    rec('§4', 'Alt-X quits cleanly',
        'PASS' if proc.poll() is not None else 'FAIL',
        f'poll={proc.poll()}')
    cleanup(proc, m)

    fp = os.path.join(wd, 's.c')
    for route in ROUTES:
        open(fp, 'w').write('int s(){return 0;}\n')
        proc, m, scr, st = open_wpe(['s.c'], wd)
        drain(m, st, 1.5)
        os.write(m, b'zz'); drain(m, st, 0.3)      # change the file so Save has work
        action(m, st, route, b'\x1bOQ', b'\x1bf', b's')   # F2 / File -> Save
        os.write(m, b'\x1bx')
        t0 = time.time()
        while time.time() - t0 < 3 and proc.poll() is None:
            drain(m, st, 0.2)
        rec('§4', f'F2 save then Alt-X quit ({route})',
            'PASS' if open(fp).read().startswith('zzint s()') else 'FAIL')
        cleanup(proc, m)

    for route in ROUTES:
        if tool_usable('gcc'):
            for n in ('hello.o', 'hello.e', 'hello'):
                if os.path.exists(os.path.join(wd, n)):
                    os.unlink(os.path.join(wd, n))
            open(os.path.join(wd, 'hello.c'), 'w').write(
                '#include <stdio.h>\nint main(){puts("hi");return 0;}\n')
            proc, m, scr, st = open_wpe(['hello.c'], wd)
            drain(m, st, 1.5)
            action(m, st, route, b'\x1b[20~', b'\x1br', b'm', wait=4.0)   # F9 / Run -> Make
            has_o = any(os.path.exists(os.path.join(wd, n))
                        for n in ('hello.o', 'hello.e', 'hello'))
            rec('§1', f'F9 compile produces output ({route})',
                'PASS' if has_o else 'FAIL',
                '' if has_o else 'no compiled artefact under workdir')
            cleanup(proc, m)
        else:
            rec('§1', f'F9 compile ({route})', 'SKIP', 'gcc not on PATH')

    for route in ROUTES:
        proc, m, scr, st = open_wpe(['e.c'], wd)
        drain(m, st, 1.5)
        action(m, st, route, b'\x1bOR', b'\x1bf', b'm', wait=0.8)   # F3 / File -> File-Manager
        fm_open = screen_has(scr, 'Name') and (
            screen_has(scr, 'Files') or screen_has(scr, 'DirTree'))
        rec('§3', f'F3 opens the File Manager dialog ({route})',
            'PASS' if fm_open else 'FAIL',
            '' if fm_open else 'no Files/DirTree/Name in screen after F3')
        cleanup(proc, m)

    for route in ROUTES:
        proc, m, scr, st = open_wpe(['e.c'], wd)
        drain(m, st, 1.5)
        action(m, st, route, b'\x1bOP', b'\x1bh', b'e', wait=1.5)   # F1 / Help -> Editor
        help_open = any(screen_has(scr, s)
                        for s in ('Help', 'Topic', 'WPE', 'XWPE'))
        rec('§3', f'F1 Help opens a viewer ({route})',
            'PASS' if help_open else 'FAIL')
        cleanup(proc, m)

    print('\n== §5 macOS-specific ==', flush=True)
    # Measured, not assumed: with no display variable at all the editor
    # starts, draws the file, and quits on Alt-X with status 0.
    # The owner passes no display variable to an editor on a terminal.
    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    drew = screen_has(scr, 'int main')
    os.write(m, b'\x1bx')
    t0 = time.time()
    while time.time() - t0 < 3 and proc.poll() is None:
        drain(m, st, 0.2)
    code = proc.poll()
    rec('§5', 'wpe runs, draws the file and quits with DISPLAY unset',
        'PASS' if drew and code == 0 else 'FAIL',
        f'drew={drew} exit={code}')
    cleanup(proc, m)

    graphical = os.path.exists(os.path.join(TOP, 'xwpe'))
    rec('§5', 'xwpe build present?',
        'PASS' if graphical else 'SKIP',
        '' if graphical
        else 'no graphical build here; configure with --with-libx11-compat '
             'or --with-x to get one')

    shutil.rmtree(wd, ignore_errors=True)
    return results


def main():
    results = _run_checks()
    print('\n' + '=' * 72)
    n_pass = sum(1 for r in results if r[2] == 'PASS')
    n_fail = sum(1 for r in results if r[2] == 'FAIL')
    n_skip = sum(1 for r in results if r[2] == 'SKIP')
    print(f'  totals: PASS={n_pass}  FAIL={n_fail}  SKIP={n_skip}')
    if n_fail:
        print('\nFAIL details:')
        for s, i, st, note in results:
            if st == 'FAIL':
                print(f'  {s}  {i}  -- {note}')
    return 0 if n_fail == 0 else 1


# pytest entry point -- runs the standalone script in a subprocess so
# pytest's own pty/EIO race cannot affect it.  Skipped when WPE isn't built.
def test_macos_smoke():  # noqa: D401
    import pytest
    if not os.path.exists(WPE):
        pytest.skip(f'wpe binary not built at {WPE}')
    done = gui_owner.run_nested([sys.executable, os.path.abspath(__file__)],
                                SMOKE_TIMEOUT_S,
                                extra_env={'WPE_BIN': WPE,
                                           'TERM': os.environ.get('TERM')})
    assert done.returncode == 0, (f'standalone smoke exited with '
                                  f'rc={done.returncode}:\n'
                                  f'{done.stdout.decode(errors="replace")[-2000:]}')


if __name__ == '__main__':
    sys.exit(main())
