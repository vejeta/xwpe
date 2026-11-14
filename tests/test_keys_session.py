"""The key style of one run (--keys, XWPE_KEYS) and the one-time upgrade of a
1.6 CUA profile.

* "--keys borland|modern" and $XWPE_KEYS choose the keys for this run only;
  the command line wins over the variable, and either wins over xwperc.
  Neither writes xwperc, and neither opens its value as a file.
* A profile whose Options2 has the old CUA bit and none of the three bits
  Modern added is upgraded once: it gets those bits, a notice on the status
  row, and KeysNotice : 1 in xwperc so it is said once.  A run that was given
  a style does not upgrade, and a profile that already says KeysNotice does
  not either.

The rows read the style from the bottom bar: Borland names F2 Save.
"""
import os
import subprocess

import pytest

from wpe_driver import WPE_BIN, WpeSession

CUA_BIT = 1
OPTIONS2_CUA = 153                      # CUA bit, syntax, errors, source indent
MODERN_BITS = 0x400 + 0x800 + 0x1000    # selection, trim, keep final newline
NOTICE = "Keys are now Modern"


def _config(tmp_path, text):
    folder = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(folder)
    path = os.path.join(folder, "xwperc")
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _screen(s):
    return "\n".join(s.display())


def _borland(s):
    """Does the bottom bar name the Borland Save key?"""
    return " F2 Save" in _screen(s)


def _run(tmp_path, extra=None, args=(), seed="abc\n", rc=None):
    if rc is not None:
        _config(tmp_path, rc)
    return WpeSession(str(tmp_path), seed, env_extra=extra, args=args)


def _options2(path):
    with open(path) as fh:
        for line in fh:
            if line.startswith("Options2"):
                return int(line.split(":")[1])
    return None


def test_the_harness_default_is_borland(tmp_path):
    s = _run(tmp_path)
    try:
        assert _borland(s), _screen(s)
    finally:
        s.close()


# @scenario SC-modern-keymap-01
def test_keys_option_takes_the_style(tmp_path):
    s = _run(tmp_path, args=["--keys", "modern"])
    try:
        assert not _borland(s), _screen(s)
    finally:
        s.close()


def test_keys_option_with_equals(tmp_path):
    s = _run(tmp_path, args=["--keys=modern"])
    try:
        assert not _borland(s), _screen(s)
    finally:
        s.close()


def test_the_style_word_is_not_opened_as_a_file(tmp_path):
    s = _run(tmp_path, args=["--keys", "modern"])
    try:
        assert "modern" not in "\n".join(s.display()[:3]), _screen(s)
        assert "Window" in _screen(s)
    finally:
        s.close()


def test_environment_takes_the_style(tmp_path):
    s = _run(tmp_path, extra={"XWPE_KEYS": "modern"})
    try:
        assert not _borland(s), _screen(s)
    finally:
        s.close()


def test_the_option_wins_over_the_environment(tmp_path):
    s = _run(tmp_path, extra={"XWPE_KEYS": "modern"}, args=["--keys", "borland"])
    try:
        assert _borland(s), _screen(s)
    finally:
        s.close()
    t = os.path.join(str(tmp_path), "again")
    os.makedirs(t)
    s = _run(t, extra={"XWPE_KEYS": "borland"}, args=["--keys", "modern"])
    try:
        assert not _borland(s), _screen(s)
    finally:
        s.close()


def test_the_style_wins_over_the_file(tmp_path):
    s = _run(tmp_path, extra={"XWPE_KEYS": "borland"}, rc="[General]\nOptions2 : %d\n"
             % (OPTIONS2_CUA + MODERN_BITS + 0))
    try:
        assert _borland(s), _screen(s)
    finally:
        s.close()


def test_the_style_never_changes_the_file(tmp_path):
    rc = "[General]\nOptions2 : 24\n"
    path = _config(tmp_path, rc)
    s = WpeSession(str(tmp_path), "abc\n", args=["--keys", "modern"])
    try:
        assert not _borland(s), _screen(s)
        s.key("\033x")                       # quit
    finally:
        s.close()
    assert _options2(path) == 24
    with open(path) as fh:
        assert fh.read() == rc


@pytest.mark.parametrize("how", ["option", "environment"])
def test_save_options_in_a_styled_run_keeps_the_files_style(tmp_path, how):
    """Options > Save Options writes xwperc, and a run given a style writes
    the file's own style bits there, not the session's: the Borland profile
    stays Borland after a Modern run saved its options."""
    path = _config(tmp_path, "[General]\nOptions2 : 24\n")
    if how == "option":
        s = WpeSession(str(tmp_path), "abc\n", args=["--keys", "modern"],
                       env_extra={"XWPE_KEYS": None})
    else:
        s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": "modern"})
    try:
        assert not _borland(s), _screen(s)
        s.key("\033o", delay=0.7)            # Options
        s.key("s", delay=0.9)                # Save Options
        s.key("\r", delay=1.0)               # the file the box names
    finally:
        s.close()
    assert _options2(path) == 24, "the run's style was written to xwperc"


# @scenario SC-modern-keymap-02
def test_an_unknown_style_is_refused():
    done = subprocess.run([WPE_BIN, "--keys", "nonsense"], capture_output=True,
                          text=True, timeout=20, stdin=subprocess.DEVNULL)
    assert done.returncode == 2
    assert "unknown key style 'nonsense'" in done.stderr


def test_an_unknown_style_in_the_environment_is_refused():
    env = dict(os.environ, XWPE_KEYS="vi")
    done = subprocess.run([WPE_BIN], capture_output=True, text=True, timeout=20,
                          stdin=subprocess.DEVNULL, env=env)
    assert done.returncode == 2
    assert "unknown key style 'vi'" in done.stderr


# @scenario SC-modern-keymap-03
def test_a_cua_profile_is_upgraded_once_with_a_notice(tmp_path):
    path = _config(tmp_path, "[General]\nOptions2 : %d\n" % OPTIONS2_CUA)
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": None})
    try:
        assert NOTICE in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == OPTIONS2_CUA + MODERN_BITS
    with open(path) as fh:
        assert "KeysNotice : 1" in fh.read()
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": None})
    try:
        assert NOTICE not in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == OPTIONS2_CUA + MODERN_BITS


def test_a_profile_that_chose_its_bits_is_not_upgraded(tmp_path):
    rc = "[General]\nOptions2 : %d\n" % (OPTIONS2_CUA + 0x400)
    path = _config(tmp_path, rc)
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": None})
    try:
        assert NOTICE not in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == OPTIONS2_CUA + 0x400


def test_a_profile_that_was_told_once_is_not_upgraded_again(tmp_path):
    """A user who was upgraded, then cleared the three boxes and kept the
    Modern keys, is not upgraded a second time: KeysNotice : 1 says the
    notice was given."""
    rc = "[General]\nOptions2 : %d\nKeysNotice : 1\n" % OPTIONS2_CUA
    path = _config(tmp_path, rc)
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": None})
    try:
        assert NOTICE not in _screen(s), _screen(s)
        assert not _borland(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == OPTIONS2_CUA


def test_a_borland_profile_is_not_upgraded(tmp_path):
    path = _config(tmp_path, "[General]\nOptions2 : 24\n")
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": None})
    try:
        assert NOTICE not in _screen(s), _screen(s)
        assert _borland(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == 24


def test_a_run_given_a_style_does_not_upgrade(tmp_path):
    path = _config(tmp_path, "[General]\nOptions2 : %d\n" % OPTIONS2_CUA)
    s = WpeSession(str(tmp_path), "abc\n", env_extra={"XWPE_KEYS": "modern"})
    try:
        assert NOTICE not in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(path) == OPTIONS2_CUA


ALT_O, ALT_C = "\033o", "\033c"
CTRL_A, CTRL_S = chr(1), chr(19)


# @scenario SC-modern-keymap-12
def test_the_keys_radio_brings_the_modern_options_with_it(tmp_path):
    """Options > Editor > Keys > Modern also sets the selection mode and Keep
    Final newline: Ctrl-A then a letter replaces the text and the file keeps
    its final line break when saved with Ctrl-S."""
    s = _run(tmp_path, seed="abc\n")
    try:
        s.key(ALT_O, delay=0.7)
        s.key("e", delay=0.9)
        s.key(ALT_C, delay=0.5)
        s.key("\r", delay=1.0)
        s.key(CTRL_A, "Z", CTRL_S, delay=0.5)
        assert s.text() == "Z\n"
    finally:
        s.close()
