#!/usr/bin/env python3
"""Parent against candidate: does a change leave what the editor draws alone?

A refactor that must not change behaviour is proved by identical output.  The
pixels depend on the installed fonts, so no frame is ever stored: this script
takes two BUILT trees (the parent commit's and the candidate's, built by the
caller on this host), replays the same short scripted session under each and
compares what each drew.  It builds nothing and writes no frame into the
repository; the frames go to a scratch folder.

  frames_compare.py PARENT_TREE CANDIDATE_TREE [--sessions x11,wayland,pyte]
                    [--out DIR] [--keep]

A tree is a folder holding the built `xwpe` and `wpe` and the data files the
editor reads (its XWPE_LIB), that is a build folder.  Three sessions:

  x11      a private Xvfb with a window manager; the editor's own frame dump
           (XWPE_X_DUMP) after each step.
  wayland  a private weston on a private Xvfb; the editor's own dump
           (XWPE_WL_DUMP) after each step.
  pyte     the terminal editor under a pty; the screen's characters and every
           cell's colours (display() and cell colours) after each step.

Each step is: send the keys, then take the frame only once it has stopped
changing (a frame whose bytes are the same for a whole quiet period), never
after a fixed sleep.  The servers are started by tests/gui_owner.py's owner on
displays of its own, never the user's, and are ended with the run.

Exit: 0 every compared frame is byte-identical, 1 a frame differs (the first
one is named, with its first differing pixel or cell), 2 bad arguments or a
session that could not run, 77 a tool the session needs is missing.

Run it with the harness's python (tests/.venv/bin/python): the pyte session
needs pyte, and the owner needs pytest.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

SESSIONS = ("x11", "wayland", "pyte")
SKIP = 77
USAGE = 2
DIFFERS = 1

SOURCE = ("int main(void)\n{\n  int a = 1;\n  int b = 2;\n"
          "  return a + b;\n}\n")
# The steps of every session: a name and what to send.  The first is the
# editor as it starts, then the File menu, the menu closed, typed text.
STEPS = (("start", None), ("menu", "alt+f"), ("closed", "Escape"),
         ("typed", "abc"))
PYTE_KEYS = {"menu": "\033f", "closed": "\033", "typed": "abc"}

FRAME_QUIET_S = 1.0
FRAME_S = 30.0
POLL_S = 0.1
PYTE_QUIET_DRAINS = 2
PYTE_DRAIN_S = 0.5
PYTE_SETTLE_TRIES = 12
PYTE_RUN_S = 120
NEEDS = {"x11": ("Xvfb", "xdotool", "xprop", "matchbox-window-manager"),
         "wayland": ("Xvfb", "xdotool", "weston"),
         "pyte": ()}


class SessionFailed(RuntimeError):
    """A session could not run to its end (not a difference)."""


# ---------------------------------------------------------------------------
# Frames


def settled_frame(path, state, quiet_s):
    """The bytes of the dump at path once they have not changed for quiet_s,
    else None.  state remembers the last digest and when it was first seen."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    digest = hashlib.sha256(data).hexdigest()
    now = time.monotonic()
    if state.get("digest") != digest:
        state["digest"] = digest
        state["since"] = now
        return None
    if data and now - state["since"] >= quiet_s:
        return data
    return None


def wait_settled_frame(go, path, what, editor):
    """Wait for the editor's dump to stop changing and return its bytes."""
    state = {}
    quiet = go.scaled(FRAME_QUIET_S)
    return go.wait_for(lambda: settled_frame(path, state, quiet), what,
                       FRAME_S, poll=POLL_S, state_fn=editor.log_text)


def parse_ppm(data):
    """(width, height, pixel bytes) of a binary PPM."""
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    pos += 1
    return int(fields[1]), int(fields[2]), data[pos:]


def first_pixel_difference(one, other):
    """A sentence naming the first differing pixel of two PPM frames."""
    a, b = parse_ppm(one), parse_ppm(other)
    if a[:2] != b[:2]:
        return "size %dx%d against %dx%d" % (a[0], a[1], b[0], b[1])
    width, pixels_a, pixels_b = a[0], a[2], b[2]
    first = None
    differing = 0
    for index in range(0, min(len(pixels_a), len(pixels_b)) - 2, 3):
        if pixels_a[index:index + 3] != pixels_b[index:index + 3]:
            differing += 1
            if first is None:
                first = index
    if first is None:
        return "same pixels, different bytes (a header or a length)"
    pixel = first // 3
    return ("first pixel (%d,%d) parent %s candidate %s, %d pixels differ"
            % (pixel % width, pixel // width, pixels_a[first:first + 3].hex(),
               pixels_b[first:first + 3].hex(), differing))


# ---------------------------------------------------------------------------
# Sessions that draw to a window


def editor_environment(tree):
    """The variables one tree's editor gets beyond the owner's whitelist."""
    return {"XWPE_LIB": tree}


def prepare_workfolder(out, name):
    """A fresh folder for a session, at the same path for both trees so no
    path that reaches the screen differs between them."""
    work = os.path.join(out, "work-" + name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, "home"))
    with open(os.path.join(work, "t.c"), "w") as fh:
        fh.write(SOURCE)
    with open(os.path.join(work, "home", ".Xdefaults"), "w") as fh:
        fh.write("xwpe.altMask: mod1\n")
    return work


def window_id(go, display, window_class, what):
    """The id of the first window of the class on the display."""
    def found():
        out = go.run_tool(["xdotool", "search", "--class", window_class], 10,
                          display=display).stdout.decode().split()
        return out[-1] if out else None
    return go.wait_for(found, what, 20.0)


def send_step(go, display, window, step_name, keys):
    """Send the step's keys to the window (nothing for the first step)."""
    if keys is None:
        return
    if step_name == "typed":
        argv = ["xdotool", "type", "--window", window, "--clearmodifiers", keys]
    else:
        argv = ["xdotool", "key", "--window", window, "--clearmodifiers", keys]
    go.run_tool(argv, 10, display=display)


def record_window_session(go, owner, kind, tree, out, displays):
    """The frames, as {step: bytes}, of one tree's editor on a display."""
    work = prepare_workfolder(out, kind)
    log = os.path.join(work, "logs-" + os.path.basename(tree.rstrip("/")))
    os.makedirs(log, exist_ok=True)
    x_display, editor_display, search_class = displays
    editor = go.Editor.start(owner, os.path.join(tree, "xwpe"), ["t.c"], kind,
                             editor_display, work, os.path.join(work, "home"),
                             log_dir=log, extra_env=editor_environment(tree),
                             label="%s editor of %s" % (kind, tree))
    dump = os.path.join(log, "frame.ppm")
    frames = {}
    try:
        window = window_id(go, x_display, search_class,
                           "the %s window to appear" % kind)
        go.run_tool(["xdotool", "windowfocus", window], 10, display=x_display)
        for name, keys in STEPS:
            send_step(go, x_display, window, name, keys)
            frames[name] = wait_settled_frame(
                go, dump, "the %s frame after step %s to settle" % (kind, name),
                editor)
    finally:
        editor.end()
    return frames


def window_manager_arguments():
    """The window manager's command line: the suite's empty key config, so
    the manager does not take Alt-letter away from the editor."""
    kbd = os.path.join(HERE, "x11", "matchbox-kbdconfig")
    return ["matchbox-window-manager", "-use_titlebar", "no", "-kbdconfig", kbd]


def wait_window_manager(go, display, manager):
    """Wait until the window manager says it manages the display."""
    def ready():
        out = go.run_tool(["xprop", "-root", "_NET_SUPPORTING_WM_CHECK"], 10,
                          display=display).stdout.decode()
        return "window id" in out
    go.wait_for(ready, "the window manager to announce itself", 20.0,
                state_fn=manager.log_text)


def run_x11(go, owner, trees, out):
    """The x11 session of every tree, on one private server."""
    log = os.path.join(out, "x11-server")
    os.makedirs(log, exist_ok=True)
    display = go.start_x_server(owner, None, log, go.SCREEN)
    manager = owner.start_service(window_manager_arguments(), "window manager",
                                  log, log, kind="wm",
                                  extra_env={"DISPLAY": display.name})
    wait_window_manager(go, display, manager)
    return [record_window_session(go, owner, "x11", tree, out,
                                  (display, display, "xwpe"))
            for tree in trees]


def run_wayland(go, owner, trees, out):
    """The wayland session of every tree, on one private weston."""
    log = os.path.join(out, "wayland-server")
    os.makedirs(log, exist_ok=True)
    x_display = go.start_x_server(owner, None, log, go.SCREEN)
    wayland = go.start_compositor(owner, None, x_display, log,
                                  int(go.WL_WIDTH), int(go.WL_HEIGHT))
    return [record_window_session(go, owner, "wayland", tree, out,
                                  (x_display, wayland, "weston"))
            for tree in trees]


# ---------------------------------------------------------------------------
# The terminal session (a child of this script, so WPE_BIN is read at import)


def pyte_cells(screen):
    """Every cell's character and colours, as lists a JSON file can hold."""
    rows = []
    for y in range(screen.lines):
        row = []
        for x in range(screen.columns):
            cell = screen.buffer[y][x]
            row.append([cell.data, cell.fg, cell.bg, bool(cell.bold),
                        bool(cell.reverse)])
        rows.append(row)
    return rows


def pyte_snapshot(session):
    """The screen once two drains in a row left it unchanged."""
    last = None
    quiet = 0
    for _ in range(PYTE_SETTLE_TRIES):
        shown = list(session.display(drain=PYTE_DRAIN_S))
        quiet = quiet + 1 if shown == last else 0
        last = shown
        if quiet >= PYTE_QUIET_DRAINS:
            break
    return {"display": last, "cells": pyte_cells(session.screen)}


def pyte_child(tree, out_file, work):
    """Run the terminal steps of one tree and write the snapshots as JSON."""
    os.environ["WPE_BIN"] = os.path.join(tree, "wpe")
    import wpe_driver
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    snapshots = {}
    with wpe_driver.WpeSession(work, SOURCE, wait=1.5) as session:
        for name, _keys in STEPS:
            if name in PYTE_KEYS:
                session.key(PYTE_KEYS[name], delay=1.0)
            snapshots[name] = pyte_snapshot(session)
    with open(out_file, "w") as fh:
        json.dump(snapshots, fh)


def run_pyte(trees, out):
    """The pyte snapshots of every tree, each from a child process."""
    results = []
    work = os.path.join(out, "work-pyte")
    for number, tree in enumerate(trees):
        out_file = os.path.join(out, "pyte-%d.json" % number)
        env = {k: v for k, v in os.environ.items()
               if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        done = subprocess.run([sys.executable, os.path.abspath(__file__),
                               "--pyte-child", tree, out_file, work],
                              env=env, timeout=PYTE_RUN_S)
        if done.returncode != 0:
            raise SessionFailed("the terminal session of %s ended with %d"
                                % (tree, done.returncode))
        with open(out_file) as fh:
            results.append(json.load(fh))
    return results


def first_cell_difference(one, other):
    """A sentence naming the first differing cell of two snapshots."""
    for y, (ra, rb) in enumerate(zip(one["cells"], other["cells"])):
        for x, (ca, cb) in enumerate(zip(ra, rb)):
            if ca != cb:
                return ("first cell row %d column %d parent %r candidate %r"
                        % (y, x, ca, cb))
    return "the rows differ in length"


# ---------------------------------------------------------------------------
# The comparison


def require_motion(kind, frames):
    """Refuse a session whose keys changed nothing: two trees that both drew
    a blank or a stuck window would compare identical and prove nothing."""
    if frames["start"] == frames["menu"]:
        raise SessionFailed("%s: the menu step left the frame as it was, the "
                            "keys did not reach the editor" % kind)


def compare_frames(kind, parent, candidate, differ):
    """(line, identical) for one session's {step: frame} pair."""
    require_motion(kind, parent)
    require_motion(kind, candidate)
    for name, _keys in STEPS:
        if parent[name] != candidate[name]:
            return ("%s: step %s differs: %s"
                    % (kind, name, differ(parent[name], candidate[name])), False)
    return ("%s: %d frames, byte-identical" % (kind, len(STEPS)), True)


def compare_snapshots(parent, candidate):
    """(line, identical) for the two terminal runs."""
    for snapshots in (parent, candidate):
        if snapshots["start"] == snapshots["menu"]:
            raise SessionFailed("pyte: the menu step left the screen as it was, "
                                "the keys did not reach the editor")
    for name, _keys in STEPS:
        if parent[name] != candidate[name]:
            return ("pyte: step %s differs: %s"
                    % (name, first_cell_difference(parent[name], candidate[name])),
                    False)
    return ("pyte: %d screens, every cell identical" % len(STEPS), True)


def missing_tools(sessions):
    """The tools the chosen sessions need that this machine lacks."""
    need = {tool for s in sessions for tool in NEEDS[s]}
    return sorted(t for t in need if shutil.which(t) is None)


def check_tree(path):
    """The tree's folder, or a SessionFailed naming what it lacks."""
    full = os.path.abspath(path)
    for binary in ("xwpe", "wpe"):
        if not os.access(os.path.join(full, binary), os.X_OK):
            raise SessionFailed("%s has no built %s" % (full, binary))
    return full


def parse_arguments(argv):
    """(parent, candidate, sessions, out, keep) or a usage error."""
    sessions, out, keep, trees = list(SESSIONS), None, False, []
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--sessions" and args:
            sessions = args.pop(0).split(",")
        elif arg == "--out" and args:
            out = args.pop(0)
        elif arg == "--keep":
            keep = True
        elif arg.startswith("-"):
            raise SessionFailed("unknown argument %s" % arg)
        else:
            trees.append(arg)
    if len(trees) != 2 or not sessions or any(s not in SESSIONS for s in sessions):
        raise SessionFailed("usage: frames_compare.py PARENT_TREE CANDIDATE_TREE "
                            "[--sessions %s] [--out DIR] [--keep]"
                            % ",".join(SESSIONS))
    return trees[0], trees[1], sessions, out, keep


def scratch_folder(out):
    """The folder the frames go to: never inside the repository."""
    if out is None:
        return tempfile.mkdtemp(prefix="frames-compare.")
    full = os.path.realpath(out)
    if full == REPO or full.startswith(REPO + os.sep):
        raise SessionFailed("--out %s is inside the repository: frames are "
                            "never stored there" % out)
    os.makedirs(full, exist_ok=True)
    return full


def compare_sessions(sessions, trees, out):
    """[(line, identical)] for the chosen sessions of the two trees."""
    verdicts = []
    if "pyte" in sessions:
        parent, candidate = run_pyte(trees, out)
        verdicts.append(compare_snapshots(parent, candidate))
    window_sessions = [s for s in sessions if s != "pyte"]
    if window_sessions:
        import gui_owner as go
        owner = go.Owner(out)
        owner.begin()
        go.install(owner)
        try:
            runners = {"x11": run_x11, "wayland": run_wayland}
            for kind in window_sessions:
                parent, candidate = runners[kind](go, owner, trees, out)
                verdicts.append(compare_frames(kind, parent, candidate,
                                               first_pixel_difference))
        finally:
            go.install(None)
            for late in owner.finish():
                sys.stderr.write("frames_compare: left over, ended: %s\n"
                                 % late.describe())
    return verdicts


def main(argv):
    if argv[:1] == ["--pyte-child"]:
        pyte_child(*argv[1:4])
        return 0
    try:
        parent, candidate, sessions, out, keep = parse_arguments(argv)
        trees = [check_tree(parent), check_tree(candidate)]
        lacking = missing_tools(sessions)
        if lacking:
            print("frames_compare: SKIP, missing %s" % ", ".join(lacking))
            return SKIP
        made_here = out is None
        out = scratch_folder(out)
    except SessionFailed as problem:
        sys.stderr.write("frames_compare: %s\n" % problem)
        return USAGE
    try:
        verdicts = compare_sessions(sessions, trees, out)
    except (SessionFailed, subprocess.TimeoutExpired) as problem:
        sys.stderr.write("frames_compare: %s (frames and logs in %s)\n"
                         % (problem, out))
        return USAGE
    for line, _same in verdicts:
        print("frames_compare: " + line)
    identical = all(same for _line, same in verdicts)
    if identical and made_here and not keep:
        shutil.rmtree(out, ignore_errors=True)
        print("frames_compare: identical")
    else:
        print("frames_compare: %s (frames and logs in %s)"
              % ("identical" if identical else "DIFFERENT", out))
    return 0 if identical else DIFFERS


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
