"""The owner's APIs for durations (tests/gui_owner.py): what a row hands the
product, how a measured time is bounded, how a file time is made."""
import os

import pytest

import gui_owner


def test_a_seconds_duration_is_stretched_and_rounded_up(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "2.5")
    assert gui_owner.product_duration("XWPE_DEBUG_ANSWER_TIMEOUT", 2) == "5"
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "1.2")
    assert gui_owner.product_duration("XWPE_DEBUG_ANSWER_TIMEOUT", 2) == "3"


def test_a_milliseconds_duration_is_stretched_and_rounded(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "3")
    assert gui_owner.product_duration("XWPE_AI_MOCK_DELAY_MS", 5000) == "15000"


def test_a_name_that_is_no_product_duration_is_refused():
    with pytest.raises(ValueError, match="not a duration"):
        gui_owner.product_duration("XWPE_KEYS", 1)


def test_a_value_above_the_products_maximum_is_refused(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "1")
    with pytest.raises(ValueError, match="maximum"):
        gui_owner.product_duration("XWPE_DEBUG_ANSWER_TIMEOUT", 10 ** 6)


def test_the_names_are_read_from_the_sources_including_headers_and_stubs():
    names = gui_owner.product_duration_names()
    for name in ("XWPE_DEBUG_ANSWER_TIMEOUT", "XWPE_AI_MOCK_DELAY_MS",
                 "XWPE_AI_TIMEOUT", "XWPE_AI_TOOL_TIMEOUT",
                 "XWPE_STUB_CONTINUE_DELAY_S", "XWPE_STUB_QUIT_NAP_S"):
        assert name in names, name
    assert "XWPE_WL_UIDUMP_AFTER" not in names


def test_an_option_line_for_a_duration_option_is_scaled(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "3")
    assert gui_owner.product_option_line("AIToolTimeout", 2) == "AIToolTimeout : 6\n"
    with pytest.raises(ValueError):
        gui_owner.product_option_line("AIModel", 2)


def test_an_elapsed_time_over_its_scaled_bound_names_the_measure(monkeypatch):
    monkeypatch.setenv(gui_owner.WAIT_SCALE_VAR, "2")
    import time
    start = time.monotonic() - 3.0
    assert gui_owner.elapsed_within(start, 2, "the call") >= 3.0
    with pytest.raises(gui_owner.OwnerTimeout, match="the call took"):
        gui_owner.elapsed_within(start, 1, "the call")


def test_a_file_time_is_taken_from_the_files_own_clock(tmp_path):
    path = tmp_path / "f"
    path.write_text("x")
    os.utime(path, (1000, 1000))
    assert gui_owner.file_time_from(str(path), 5) == 1005
