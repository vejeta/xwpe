"""The first-run question: which keys does a new profile want?

A profile is new when the user has no xwperc in the configuration folder or
in the legacy ~/.xwpe and neither folder exists.  The editor then asks, once,
in a box whose focused button is Modern (so Enter picks it), writes the answer
to the user's option file at once, and says which keys are in force.  It never
asks an existing installation, a run given a style (--keys, XWPE_KEYS), or a
run whose input is not a terminal.

Every other test starts in an empty HOME, which is a new profile, so the
drivers set XWPE_KEYS=borland by default; these rows remove it.
"""
import os

from wpe_driver import WpeSession

BOX = "Which keys do you want?"
NO_STYLE = {"XWPE_KEYS": None}
SAVED = os.path.join(".config", "xwpe", "xwperc")
ENTER = "\r"
MODERN_BITS = 1 + 0x400 + 0x800 + 0x1000


def _screen(s):
    return "\n".join(s.display())


def _run(tmp_path, extra=NO_STYLE, **kw):
    return WpeSession(str(tmp_path), "abc\n", env_extra=extra, new_profile=True, **kw)


def _options2(tmp_path):
    with open(os.path.join(str(tmp_path), SAVED)) as fh:
        for line in fh:
            if line.startswith("Options2"):
                return int(line.split(":")[1])
    return None


# @scenario SC-modern-keymap-04
def test_a_new_profile_is_asked_and_enter_picks_modern(tmp_path):
    s = _run(tmp_path)
    try:
        assert BOX in _screen(s), _screen(s)
        s.key(ENTER, delay=0.5)
        assert BOX not in _screen(s), _screen(s)
        assert "Keys: Modern" in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(tmp_path) & MODERN_BITS == MODERN_BITS


# @scenario SC-menus-30
def test_the_question_says_the_menus_are_the_same_in_both(tmp_path):
    """A user who picks Modern must not take the Borland bar for the wrong
    choice: the box says the menus are the same and only the keys change."""
    s = _run(tmp_path)
    try:
        assert "The menus are the same in both; the keys beside the items change" in \
            _screen(s), _screen(s)
    finally:
        s.close()


def test_the_answer_is_kept_and_not_asked_again(tmp_path):
    s = _run(tmp_path)
    try:
        s.key(ENTER, delay=0.5)
    finally:
        s.close()
    s = _run(tmp_path)
    try:
        assert BOX not in _screen(s), _screen(s)
        assert " F2 Save" not in _screen(s), "the Modern answer was not kept:\n" + _screen(s)
    finally:
        s.close()


def test_borland_can_be_chosen(tmp_path):
    s = _run(tmp_path)
    try:
        assert BOX in _screen(s), _screen(s)
        s.key("\033b", delay=0.5)           # Alt-B, the Borland button
        assert "Keys: Borland" in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(tmp_path) & 1 == 0
    s = _run(tmp_path)          # the next start: no box, the Borland bar
    try:
        assert BOX not in _screen(s), _screen(s)
        assert " F2 Save" in _screen(s), _screen(s)
    finally:
        s.close()


def test_esc_keeps_borland_and_is_not_asked_again(tmp_path):
    """The editor makes its configuration folder at every start, so a profile
    is new at one start only: Esc is an answer (Borland), written like the
    others."""
    s = _run(tmp_path)
    try:
        s.key("\033", delay=0.8)
        assert BOX not in _screen(s), _screen(s)
        assert "Keys: Borland" in _screen(s), _screen(s)
    finally:
        s.close()
    assert _options2(tmp_path) & 1 == 0
    s = _run(tmp_path)
    try:
        assert BOX not in _screen(s), _screen(s)
    finally:
        s.close()


def test_the_harness_default_hides_the_box(tmp_path):
    s = WpeSession(str(tmp_path), "abc\n", new_profile=True)
    try:
        assert BOX not in _screen(s), _screen(s)
        assert " F2 Save" in _screen(s), _screen(s)
    finally:
        s.close()


def test_a_run_given_a_style_is_not_asked(tmp_path):
    s = _run(tmp_path, extra={"XWPE_KEYS": "modern"})
    try:
        assert BOX not in _screen(s), _screen(s)
    finally:
        s.close()
    assert not os.path.exists(os.path.join(str(tmp_path), SAVED))


def test_an_installation_with_a_configuration_folder_is_not_asked(tmp_path):
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"))
    s = _run(tmp_path)
    try:
        assert BOX not in _screen(s), _screen(s)
        assert " F2 Save" in _screen(s), _screen(s)
    finally:
        s.close()


def test_an_older_installation_with_a_legacy_folder_is_not_asked(tmp_path):
    os.makedirs(os.path.join(str(tmp_path), ".xwpe"))
    s = _run(tmp_path)
    try:
        assert BOX not in _screen(s), _screen(s)
    finally:
        s.close()


def test_a_legacy_option_file_is_not_asked(tmp_path):
    folder = os.path.join(str(tmp_path), ".xwpe")
    os.makedirs(folder)
    with open(os.path.join(folder, "xwperc"), "w") as fh:
        fh.write("[General]\nOptions2 : 24\n")
    s = _run(tmp_path)
    try:
        assert BOX not in _screen(s), _screen(s)
    finally:
        s.close()


def test_input_that_is_not_a_terminal_is_not_asked(tmp_path):
    s = _run(tmp_path, stdin_is_terminal=False)
    try:
        assert BOX not in _screen(s), _screen(s)
    finally:
        s.close()
    assert not os.path.exists(os.path.join(str(tmp_path), SAVED))
