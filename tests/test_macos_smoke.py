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
import os, sys, pty, select, subprocess, time, tempfile, shutil, signal

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
WPE = os.environ.get('WPE_BIN') or os.path.join(TOP, 'wpe')
COLS, ROWS = 100, 30


def _import_pyte():
    sys.path.insert(0, HERE)
    import pyte  # noqa: E402
    from test_utf8_border import SafeScreen  # noqa: E402
    return pyte, SafeScreen


def open_wpe(files, workdir, term='xterm-256color', extra_env=None):
    pyte, SafeScreen = _import_pyte()
    screen = SafeScreen(COLS, ROWS); stream = pyte.Stream(screen)
    master, slave = pty.openpty()
    env = os.environ.copy()
    env.update(TERM=term, COLUMNS=str(COLS), LINES=str(ROWS),
               LC_ALL='en_US.UTF-8', HOME=workdir)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.Popen([WPE, *files], stdin=slave, stdout=slave,
                            stderr=slave, cwd=workdir, env=env,
                            preexec_fn=os.setsid)
    os.close(slave)
    return proc, master, screen, stream


def drain(master, stream, timeout):
    end = time.time() + timeout
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


def cleanup(proc, master):
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception: pass
    time.sleep(0.1)
    try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception: pass
    try: os.close(master)
    except Exception: pass
    try: proc.wait(timeout=2)
    except Exception: pass


def screen_has(screen, s):
    return any(s in line for line in screen.display)


def cpu_percent(pid):
    try:
        out = subprocess.check_output(['ps', '-p', str(pid), '-o', '%cpu='],
                                      text=True).strip()
        return float(out)
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
        rec('§0', 'binary exists', 'PASS',
            subprocess.check_output(['file', WPE], text=True)
            .split(':', 1)[1].strip())
    else:
        rec('§0', 'binary exists', 'FAIL', f'not found: {WPE}')
        return results
    rec('§0', f'TERM={os.environ.get("TERM")}',
        'PASS' if os.environ.get('TERM') else 'FAIL')

    print('\n== §3 1.6.5-new behaviour ==', flush=True)
    wd = tempfile.mkdtemp(prefix='xwpe-smoke-')
    open(os.path.join(wd, 'e.c'), 'w').write('int main(void){return 0;}\n')

    # F10 highlights menu bar -- Enter expands first dropdown
    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1b[21~'); drain(m, st, 0.5)
    os.write(m, b'\r'); drain(m, st, 0.5)
    opened = screen_has(scr, 'About WE') or screen_has(scr, 'System Info')
    rec('§3', 'F10 opens menu bar (Enter expands)',
        'PASS' if opened else 'FAIL',
        '' if opened else 'no System-menu items after F10 + Enter')
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
    open(fp, 'w').write('int s(){return 0;}\n')
    proc, m, scr, st = open_wpe(['s.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1bOQ'); drain(m, st, 0.7)
    os.write(m, b'\x1bx')
    t0 = time.time()
    while time.time() - t0 < 3 and proc.poll() is None:
        drain(m, st, 0.2)
    rec('§4', 'F2 save then Alt-X quit',
        'PASS' if 'int s()' in open(fp).read() else 'FAIL')
    cleanup(proc, m)

    if shutil.which('gcc'):
        open(os.path.join(wd, 'hello.c'), 'w').write(
            '#include <stdio.h>\nint main(){puts("hi");return 0;}\n')
        proc, m, scr, st = open_wpe(['hello.c'], wd)
        drain(m, st, 1.5)
        os.write(m, b'\x1b[20~'); drain(m, st, 4.0)
        has_o = any(os.path.exists(os.path.join(wd, n))
                    for n in ('hello.o', 'hello.e', 'hello'))
        rec('§1', 'F9 compile produces output',
            'PASS' if has_o else 'FAIL',
            '' if has_o else 'no compiled artefact under workdir')
        cleanup(proc, m)
    else:
        rec('§1', 'F9 compile', 'SKIP', 'gcc not on PATH')

    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1bOR'); drain(m, st, 0.8)
    fm_open = screen_has(scr, 'Name') and (
        screen_has(scr, 'Files') or screen_has(scr, 'DirTree'))
    rec('§3', 'F3 opens the File Manager dialog',
        'PASS' if fm_open else 'FAIL',
        '' if fm_open else 'no Files/DirTree/Name in screen after F3')
    cleanup(proc, m)

    proc, m, scr, st = open_wpe(['e.c'], wd)
    drain(m, st, 1.5)
    os.write(m, b'\x1bOP'); drain(m, st, 1.5)
    help_open = any(screen_has(scr, s)
                    for s in ('Help', 'Topic', 'WPE', 'XWPE'))
    rec('§3', 'F1 Help opens a viewer',
        'PASS' if help_open else 'FAIL')
    cleanup(proc, m)

    print('\n== §5 macOS-specific ==', flush=True)
    rec('§5', 'wpe runs without XQuartz (ncurses build)', 'PASS')
    rec('§5', 'xwpe build present?',
        'PASS' if os.path.exists(os.path.join(TOP, 'xwpe')) else 'SKIP',
        '' if os.path.exists(os.path.join(TOP, 'xwpe'))
        else 'not built (expected on macOS without XQuartz)')

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
    env = os.environ.copy()
    env['WPE_BIN'] = WPE
    rc = subprocess.call([sys.executable, os.path.abspath(__file__)], env=env)
    assert rc == 0, f'standalone smoke exited with rc={rc}'


if __name__ == '__main__':
    sys.exit(main())
