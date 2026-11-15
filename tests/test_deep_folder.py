"""A deep folder is made as deep as the system holds (tests/deep_folder.py).

Rows that build a folder of 4040 or 2117 bytes died in os.makedirs on macOS
and the BSDs, which hold 1024, before they reached the editor.  These rows run
the helper on Linux with the limit the other systems have: path_max answers
1024 (the stand-in is put on the owner, procinfo, the one place the limit is
asked), and a row about the editor's own old cut skips by name while a row
about the system's refusal still makes a path that is refused.

Run: tests/.venv/bin/python -m pytest -q tests/test_deep_folder.py
"""
import os

import pytest

import deep_folder
import procinfo

SMALL_SYSTEM = 1024          # what macOS and the BSDs hold
OLD_CUT = 1105               # a path over the 1023 bytes a buffer once held


@pytest.fixture
def small_system(monkeypatch):
    """Make the system answer as macOS does for the path limit."""
    monkeypatch.setattr(procinfo, "path_max", lambda path: SMALL_SYSTEM)


def test_folder_of_length_is_exactly_that_long(tmp_path):
    folder = deep_folder.folder_of_length(tmp_path, 700)
    assert len(folder) == 700 and os.path.isdir(folder)


def test_folder_of_length_uses_the_name_length_it_is_given(tmp_path):
    folder = deep_folder.folder_of_length(tmp_path, 900, component=200)
    names = os.path.relpath(folder, str(tmp_path)).split(os.sep)
    assert max(len(name) for name in names) == 200


def test_held_lets_a_row_through_where_the_system_holds_the_path(tmp_path):
    deep_folder.held(tmp_path, procinfo.path_max(tmp_path) - 1)


def test_held_skips_by_name_where_the_system_holds_less(tmp_path, small_system):
    """The row about the editor's cut at 1023 bytes cannot exist on a system
    that holds no path of 1105."""
    with pytest.raises(pytest.skip.Exception) as skipped:
        deep_folder.held(tmp_path, OLD_CUT)
    assert "needs a system path limit over %d bytes (this one: %d)" % (
        OLD_CUT, SMALL_SYSTEM) in str(skipped.value)


def test_folder_of_length_over_the_limit_is_refused_by_the_system(tmp_path):
    """Without held() a row on a 1024 system goes on to make the folder and
    the system refuses it with OSError (63, "File name too long", on macOS):
    the failure held() turns into a skip.  Linux refuses at its own limit."""
    with pytest.raises(OSError):
        deep_folder.folder_of_length(tmp_path, procinfo.path_max(tmp_path) + 1)


def test_over_the_limit_is_a_length_the_system_refuses(tmp_path):
    folder = deep_folder.over_the_limit(tmp_path)
    assert folder == procinfo.path_max(tmp_path) + deep_folder.COMPONENT
    with pytest.raises(OSError):
        deep_folder.folder_of_length(tmp_path, folder)


def test_over_the_limit_follows_the_systems_limit(tmp_path, small_system):
    assert deep_folder.over_the_limit(tmp_path) == SMALL_SYSTEM + deep_folder.COMPONENT


def test_as_deep_as_allowed_keeps_what_the_row_asked_where_it_fits(tmp_path):
    assert deep_folder.as_deep_as_allowed(tmp_path, 3800) == min(
        3800, procinfo.path_max(tmp_path) - deep_folder.NAME_MARGIN)


def test_as_deep_as_allowed_stays_under_the_limit_of_a_small_system(tmp_path, small_system):
    allowed = deep_folder.as_deep_as_allowed(tmp_path, 3800)
    assert allowed == SMALL_SYSTEM - deep_folder.NAME_MARGIN
    assert allowed > 255, "still far over the 255 bytes the old buffers held"


def test_under_the_limit_leaves_only_the_room_asked(tmp_path, small_system):
    assert deep_folder.under_the_limit(tmp_path, 56) == SMALL_SYSTEM - 56
    folder = deep_folder.folder_of_length(tmp_path, SMALL_SYSTEM - 56)
    name = "q" * 74 + ".prj"
    assert len(os.path.join(folder, name)) >= SMALL_SYSTEM
