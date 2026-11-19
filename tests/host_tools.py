"""The host tools a row needs, declared by the row and resolved in one place.

Invariant: no row decides on its own whether a host tool is usable.  A row
declares what it needs with needs("gcc", "gdb"), and tests/conftest.py is the
only place that turns a declaration into a skip.  A tool is usable when it is
on PATH and it runs (a rustup shim, a macOS stub or a launcher with no
toolchain behind it is on PATH and does not run), and when the job has not
switched it off.

This is the sibling of build_config.requires() (what the binary under test was
built with) and procinfo (what the platform is): those answer for the editor
and the system, this answers for the programs the rows start around it.

Two skip reasons, both naming the tool, so a reader can tell them apart:

    no gdb on this host                       the tool is missing or broken
    gdb: not exercised by this job            the job switched it off

A job switches tools off by name with XWPE_TEST_TOOLS_OFF (blank-separated),
for the tools whose servers are too slow or too flaky on its runners.  That
replaces a -k expression that dropped rows by substring: `dap` matched
`adapter`, `rust` matched `trust`, and `compile` matched `compiler`.  Each job
prints the variable (conftest.py's report header), so a job that stopped
exercising a capability says so in its own log.

A name may list alternatives with "|": needs("pyright-langserver|pylsp") is
met when any one of them is usable and not switched off.

The one named exception: the display-tool gates of tests/x11/conftest.py,
tests/wayland/conftest.py and tests/console/conftest.py (Xvfb, weston, a
window manager, qemu).  They decide whether a whole graphical or console
layer can run at all, once, for every row of that layer; they are layer
gates, not the skip of a row, and they stay where they are.
"""
import os
import shutil
import subprocess

import pytest

import engine_guard

TOOLS_OFF_VARIABLE = "XWPE_TEST_TOOLS_OFF"
NOT_EXERCISED = "not exercised by this job"   # the reason's tail; the harness reads it
MISSING_FORMAT = "no %s on this host"
PROBE_TIMEOUT_S = 30
DEFAULT_PROBE = ("--version",)
PRESENCE_ONLY = None      # a tool with no cheap, side-effect-free way to run it

# How each tool is asked whether it runs.  A tool not listed answers --version.
# The version flag is not the same everywhere (javac -version, fpc -iV, a
# shell has none), and a few tools start a server or a window when run bare.
PROBES = {
    "javac": ("-version",),
    "java": ("-version",),
    "jdb": ("-version",),
    "scalac": ("-version",),
    "scala": ("-version",),
    "fpc": ("-iV",),
    "sh": ("-c", "exit 0"),
    "go": ("version",),
    "gopls": ("version",),
    "dlv": ("version",),
    "metals": PRESENCE_ONLY,             # a launcher that starts a server
    "pyright-langserver": PRESENCE_ONLY,  # starts a server when run bare
    "man": PRESENCE_ONLY,                # --version differs between man implementations
    "tput": PRESENCE_ONLY,
    "infocmp": PRESENCE_ONLY,
    "tic": PRESENCE_ONLY,
    "ldd": PRESENCE_ONLY,
    "lsof": PRESENCE_ONLY,              # -v prints to stderr and exits 1
    "openssl": PRESENCE_ONLY,
    "sdb": PRESENCE_ONLY,
    "Xvfb": PRESENCE_ONLY,
    "weston": PRESENCE_ONLY,
    "xdotool": PRESENCE_ONLY,
    "xterm": PRESENCE_ONLY,             # a window program: -version, not --version
    "xlogo": PRESENCE_ONLY,
    "xclip": PRESENCE_ONLY,
    "wl-copy": PRESENCE_ONLY,
    "wl-paste": PRESENCE_ONLY,
}

_answers = {}


def probe_of(name):
    """The arguments that ask `name` whether it runs, or PRESENCE_ONLY."""
    return PROBES.get(name, DEFAULT_PROBE)


def tool_usable(name, *args):
    """True only if `name` is on PATH AND actually runs (exit 0).

    shutil.which() finds an executable file, but a rustup / asdf / mise shim
    can sit on PATH and still fail when run: no default toolchain, a
    rust-toolchain.toml pinning one that is not installed, a component not
    added.  A which()-only guard then lets the row start and fail with a
    confusing toolchain error.  Running `name --version` (or the given
    arguments, or the tool's own entry in PROBES) tells an installed tool
    from a shim that errors, so a misconfigured toolchain skips with its name.
    """
    path = shutil.which(name)
    if path is None:
        return False
    if not engine_guard.is_baseline_tool(name):
        # Only a tool the farm provides is ever run here.  Any other name
        # (a new AI engine's `--version` may call out to its account) is
        # checked for presence and nothing more.
        return True
    probe = list(args) if args else probe_of(name)
    if probe is PRESENCE_ONLY:
        return True
    try:
        return subprocess.run(
            [path, *probe],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL, timeout=PROBE_TIMEOUT_S,
        ).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def switched_off():
    """The tools this job switched off, from XWPE_TEST_TOOLS_OFF."""
    return os.environ.get(TOOLS_OFF_VARIABLE, "").split()


def _usable_once(name):
    """tool_usable(name), asked once per tool for the whole session."""
    if name not in _answers:
        _answers[name] = tool_usable(name)
    return _answers[name]


def _alternatives(spec):
    return spec.split("|")


def unmet(tools):
    """The skip reason for the first tool of `tools` that cannot be used, or None.

    A tool the job switched off reads "<tool>: not exercised by this job";
    a tool that is absent or does not run reads "no <tool> on this host".  For
    alternatives ("a|b") the reason names all of them.
    """
    off = switched_off()
    for spec in tools:
        names = _alternatives(spec)
        on = [n for n in names if n not in off]
        if not on:
            return "%s: %s" % ("|".join(names), NOT_EXERCISED)
        if not any(_usable_once(n) for n in on):
            return MISSING_FORMAT % "|".join(on)
    return None


def usable(*tools):
    """True when every tool is usable and not switched off (no skip, no mark).

    For a row that BUILDS its own data from what the host has (a parameter
    list of the installed tools), never to decide a skip: a skip is declared
    with needs().
    """
    return unmet(tools) is None


def needs(*tools):
    """Mark a module, a class or a test as needing the host tools named.

    Use as `pytestmark = needs("gcc", "gdb")` or as a decorator.  The row
    skips, naming the tool, where it is missing, broken or switched off.
    """
    return pytest.mark.wpe_needs(*tools)


def require(*tools):
    """Skip the running row, naming the tool, when `tools` are not usable.

    The same resolver as the marker, for the few rows that learn their tool
    only while running (a tool chosen by a parameter or by a helper's
    argument).  A row that knows its tools up front uses needs().
    """
    reason = unmet(tools)
    if reason:
        pytest.skip(reason)


def require_absent(*tools):
    """Skip the running row when any of `tools` is installed on this host.

    For the rows that need a debugger or a compiler to be MISSING (the editor
    must name the one it cannot start).  The skip names the tool, as the
    others do."""
    for tool in tools:
        if shutil.which(tool) is not None:
            pytest.skip("%s is installed on this host" % tool)


def report_line():
    """The line a job prints about what it switched off, or None."""
    off = switched_off()
    if not off:
        return None
    return "host tools switched off by %s: %s" % (TOOLS_OFF_VARIABLE, " ".join(off))
