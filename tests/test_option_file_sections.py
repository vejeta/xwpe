"""An option file whose first setting comes before any [Section] line.

The reader of option files matches every "key : value" line against the
section it sits in.  A line before the first [Section] has none, and used to
take the editor down before it drew anything: a folder's ./xwperc is
untrusted (a cloned project carries one), so one line of it could kill the
editor at start.  Such a line is now reported like any other line the editor
cannot place, and the editor lives.
"""
import os

from wpe_driver import WpeSession

SEED = "int main(void){return 0;}\n"
SEED_ON_SCREEN = "int main"
ERROR_BOX = "Can't read Option-File"
GOOD_RC = "[General]\nTab : 3\n"


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


def _start(tmp_path, folder_rc=None, user_rc=None, args=()):
    """Start the editor on t.c with the given option files; return the screen
    text, whether the editor was still running, and whether the box for a
    malformed option file was shown."""
    work = str(tmp_path)
    if folder_rc is not None:
        _write(os.path.join(work, "xwperc"), folder_rc)
    if user_rc is not None:
        _write(os.path.join(work, ".config", "xwpe", "xwperc"), user_rc)
    s = WpeSession(work, SEED, args=args)
    try:
        screen = "\n".join(s.display())
        boxed = ERROR_BOX in screen
        if boxed:
            # The box a malformed option file gets; OK, then the file.
            s.key("\r", delay=0.6)
            screen = "\n".join(s.display())
        return screen, s.alive(), boxed
    finally:
        s.close()


def _lives_and_draws(screen, alive, boxed, box_expected):
    assert alive, "the editor died at start\n" + screen
    assert SEED_ON_SCREEN in screen, "no screen with t.c drawn\n" + screen
    if box_expected:
        assert boxed, "the malformed option file was not reported\n" + screen
    else:
        assert not boxed, "a well-formed option file was reported\n" + screen


# @scenario SC-xwperc-section-01
def test_folder_xwperc_of_one_colon_does_not_crash(tmp_path):
    _lives_and_draws(*_start(tmp_path, folder_rc="a:"), box_expected=True)


# @scenario SC-xwperc-section-02
def test_folder_key_without_a_header_does_not_crash(tmp_path):
    _lives_and_draws(*_start(tmp_path, folder_rc="Tabsize : 4\n"),
                     box_expected=True)


# @scenario SC-xwperc-section-03
def test_comments_then_a_key_do_not_crash(tmp_path):
    rc = "# a project\n\n   # more\nTabsize : 4\n[General]\nTab : 3\n"
    _lives_and_draws(*_start(tmp_path, folder_rc=rc), box_expected=True)


# @scenario SC-xwperc-section-04
def test_users_own_config_with_the_key_first_does_not_crash(tmp_path):
    _lives_and_draws(*_start(tmp_path, user_rc="Tabsize : 4\n"),
                     box_expected=True)


# @scenario SC-xwperc-section-05
def test_sf_file_with_the_key_first_does_not_crash(tmp_path):
    rc = os.path.join(str(tmp_path), "named.rc")
    _write(rc, "Tabsize : 4\n")
    _lives_and_draws(*_start(tmp_path, args=("-sf", rc)), box_expected=True)


# @scenario SC-xwperc-section-06
def test_control_general_section_and_key_starts(tmp_path):
    _lives_and_draws(*_start(tmp_path, folder_rc=GOOD_RC), box_expected=False)


# @scenario SC-xwperc-section-07
def test_control_unclosed_header_starts(tmp_path):
    _lives_and_draws(*_start(tmp_path, folder_rc="[\n"), box_expected=True)


# @scenario SC-xwperc-section-08
def test_control_empty_file_starts(tmp_path):
    _lives_and_draws(*_start(tmp_path, folder_rc=""), box_expected=False)


# @scenario SC-xwperc-section-09
def test_users_own_config_bad_under_a_good_folder_xwperc_does_not_crash(tmp_path):
    # The user's own file is read after the folder's, for the keys only he
    # may set; its key before any [Section] used to crash the editor too.
    # Only the editor living is asserted: whether this read reports the
    # malformed file is not settled.
    screen, alive, _boxed = _start(tmp_path, folder_rc=GOOD_RC,
                                   user_rc="Tabsize : 4\n")
    assert alive, "the editor died at start\n" + screen
    assert SEED_ON_SCREEN in screen, "no screen with t.c drawn\n" + screen
