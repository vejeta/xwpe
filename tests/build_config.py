"""What the binary under test was built with, asked of the binary.

The one reader.  `wpe --version` prints, after the version, a line

    built with: enable-ai=yes with-x=no libvterm=no ...

which configure decides and the editor compiles in (we_features.c).  The words
are configure's options without their dashes (enable-ai, with-wayland) or a
library's name (libvterm, cairo).  A test that needs one of them says so with
requires("enable-ai"); it never runs strings, ldd or otool on the binary and
never reads config.h to learn a configure answer, because those answers differ
between an in-tree, an installed and a sanitizer binary and this one does not.

requires() is lazy: it returns a pytest marker, and tests/conftest.py asks the
binary when the row is about to run, never at collection.  A tree without a
binary can therefore still be collected (the harness does that), and a binary
that prints no "built with:" line is an error of the row that needs it, not a
skip: a skip that hides a broken build is the failure this module exists to
prevent.
"""
import os
import subprocess

import pytest

NOT_BUILT = "not built here: "     # every skip reason of this kind starts so
LINE_PREFIX = "built with:"
ASK_TIMEOUT_S = 30

_asked = {}


class BuildUnknown(RuntimeError):
    """The binary could not say what it was built with."""


def default_binary():
    """The editor the pyte suite drives: $WPE_BIN, or ../wpe beside tests/."""
    return os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")


def _probe_env():
    env = dict(os.environ)
    asan = env.get("ASAN_OPTIONS", "")
    env["ASAN_OPTIONS"] = (asan + ":" if asan else "") + "detect_leaks=0"
    return env


def _ask(path):
    """Run `path --version` and return the words of its "built with:" line."""
    try:
        out = subprocess.run([path, "--version"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                             timeout=ASK_TIMEOUT_S, env=_probe_env()).stdout
    except (OSError, subprocess.TimeoutExpired) as err:
        raise BuildUnknown("cannot ask %s what it was built with: %s" % (path, err))
    for line in out.decode("utf-8", "replace").splitlines():
        if line.startswith(LINE_PREFIX):
            words = {}
            for pair in line[len(LINE_PREFIX):].split():
                word, _, value = pair.partition("=")
                words[word] = (value == "yes")
            if words:
                return words
    raise BuildUnknown("%s printed no '%s' line from --version "
                       "(not built, or not this tree's editor)" % (path, LINE_PREFIX))


def features(binary=None):
    """{word: bool} for the binary, asked once per binary and kept.

    Raises BuildUnknown, naming the binary, when it cannot say.
    """
    path = os.path.abspath(binary or default_binary())
    if path not in _asked:
        _asked[path] = _ask(path)
    return _asked[path]


def built(word, binary=None):
    """True when the binary has `word` (for example "enable-ai")."""
    have = features(binary)
    if word not in have:
        raise BuildUnknown("%s does not say whether it has '%s'"
                           % (os.path.abspath(binary or default_binary()), word))
    return have[word]


def option_of(word):
    """What the user typed to get `word`: --enable-ai, --with-x; a library keeps its name."""
    if word.startswith(("enable-", "with-")):
        return "--" + word
    return word


def missing(words, binary=None):
    """The words of `words` the binary does not have, in order."""
    return [w for w in words if not built(w, binary)]


def skip_reason(words):
    """The reason of a skip for the words the build lacks."""
    return NOT_BUILT + ", ".join(option_of(w) for w in words)


def requires(*words):
    """Mark a module, a class or a test as needing what `words` name.

    Use as `pytestmark = requires("enable-ai")` or as a decorator.  The row
    skips, by name ("not built here: --enable-ai"), on a build without it.
    """
    return pytest.mark.wpe_requires(*words)
