#!/usr/bin/env python3
"""Checks of the evaluation suite itself (no editor is started).

    tests/.venv/bin/python tests/eval/selfcheck.py

* the screen end check reads only the pane's last line;
* the final reply is read with history and refused when its start is lost;
* the scorer builds in a clean environment;
* E02's held-out program fails the fixes that only look right;
* a lane directory is loaded by path, and a run that raises still gives a row.
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run as runner  # noqa: E402
import score  # noqa: E402
from lanes import external, xwpe_common  # noqa: E402

OLD_ERROR_ROW = "[AI] connection refused by the model server"
RUNNING_ROW = "[agent] run_command: make -B test"


def write(path, text):
    with open(path, "w") as fh:
        fh.write(text)


class ScriptedPane:
    """Stands in for the driver and the session: the pane is a list of pages,
    PgUp moves one page back, and a page is its list of logical lines."""

    KEY_PGUP = "PGUP"

    def __init__(self, pages):
        self.pages = pages
        self.at = len(pages) - 1

    def pane_rows(self, disp):
        return list(disp)

    def pane_logical_lines(self, rows):
        return [r for r in rows]

    def click_into_pane(self, session):
        pass

    def display(self, drain=0.6):
        return list(self.pages[self.at])

    def key(self, key, delay=0.0):
        if key == self.KEY_PGUP and self.at > 0:
            self.at -= 1


class ScreenEndCheck(unittest.TestCase):
    def test_an_old_error_row_above_a_running_command_is_not_the_end(self):
        display = ["[agent] start", OLD_ERROR_ROW, RUNNING_ROW]
        # The whole-display read this replaces would have seen the old row.
        self.assertIsNotNone(xwpe_common.error_row(display))
        idle = 0
        for _ in range(xwpe_common.IDLE_LOOKS + 3):
            reason, idle = xwpe_common.judge_screen(display, display, idle)
            self.assertIsNone(reason)

    def test_an_error_row_that_is_the_last_line_ends_the_run(self):
        display = ["[agent] start", RUNNING_ROW, OLD_ERROR_ROW, ""]
        idle = 0
        reason = None
        for _ in range(xwpe_common.IDLE_LOOKS):
            reason, idle = xwpe_common.judge_screen(display, display, idle)
        self.assertEqual(reason, xwpe_common.END_ERROR)

    def test_the_working_mark_resets_the_count(self):
        display = [OLD_ERROR_ROW, "thinking (Esc cancels)"]
        reason, idle = xwpe_common.judge_screen(display, display, 2)
        self.assertEqual((reason, idle), (None, 0))


class FinalReplyReader(unittest.TestCase):
    def read(self, pages):
        pane = ScriptedPane(pages)
        result = xwpe_common.LaneResult()
        text = xwpe_common.read_final_reply(pane, pane, result)
        return text, result

    def test_a_reply_with_its_start_in_view_is_read_as_it_is(self):
        text, result = self.read([["[agent] go", "DONE first", "second"]])
        self.assertEqual(text, "DONE first\nsecond")
        self.assertEqual(result.notes, [])

    def test_a_reply_whose_start_scrolled_out_is_read_with_history(self):
        pages = [["[agent] go", "the chain is a > b > c", "more", "x"],
                 ["y", "DONE chain above", "tail"]]
        text, result = self.read(pages)
        self.assertIn("a > b > c", text)
        self.assertIn("tail", text)
        self.assertEqual(result.notes, [])

    def test_a_reply_whose_start_cannot_be_found_is_not_scored(self):
        pages = [["prose", "DONE x", "tail"]]
        text, result = self.read(pages)
        self.assertEqual(text, "")
        self.assertEqual(len(result.notes), 1)
        self.assertIn("answer not scored", result.notes[0])


class ScorerEnvironment(unittest.TestCase):
    def test_make_does_not_see_the_callers_cc_or_makeflags(self):
        probe = {"CC": "false", "CFLAGS": "-DBROKEN", "MAKEFLAGS": "-n"}
        saved = {k: os.environ.get(k) for k in probe}
        os.environ.update(probe)
        try:
            with tempfile.TemporaryDirectory() as work:
                write(os.path.join(work, "Makefile"),
                      "all:\n\ttest \"$(CC)\" = cc\n\ttest -z \"$$CFLAGS\"\n")
                ok, note = score.run_command(work, "make -B -f Makefile")
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        self.assertTrue(ok, note)


class E02HeldOut(unittest.TestCase):
    BUGGY = ("    for (i = from; i <= from + count && i < n; i++)\n"
             "        s += v[i];\n    return s;\n")

    def score_with(self, replacement=None, prefix=None):
        source = score.fixture_dir("E02")
        with tempfile.TemporaryDirectory() as work:
            for name in os.listdir(source):
                path = os.path.join(source, name)
                if os.path.isfile(path):
                    shutil.copy2(path, os.path.join(work, name))
            with open(os.path.join(work, "stats.c")) as fh:
                text = fh.read()
            if replacement is not None:
                self.assertIn(self.BUGGY, text)
                text = text.replace(self.BUGGY, replacement)
            if prefix is not None:
                marker = "    int s = 0;\n    int i;\n\n    if (from < 0)"
                self.assertIn(marker, text)
                text = text.replace(marker, prefix + marker)
            write(os.path.join(work, "stats.c"), text)
            return score.score_repo(work, "E02")

    def test_a_special_case_for_the_visible_test_fails(self):
        out = self.score_with(prefix="    if (from == 2 && count == 3)\n"
                                     "        return 12;\n")
        self.assertFalse(out["test_ok"], out["notes"])

    def test_a_read_past_the_end_fails(self):
        out = self.score_with(replacement="    for (i = from; i <= from + count"
                              " && i < n; i++)\n        s += v[i];\n"
                              "    return s - v[from + count];\n")
        self.assertFalse(out["test_ok"], out["notes"])

    def test_the_plain_fix_passes(self):
        out = self.score_with(replacement=self.BUGGY.replace("<= from", "< from"))
        self.assertTrue(out["files_ok"] and out["build_ok"] and out["test_ok"],
                        out["notes"])

    def test_a_rewritten_loop_passes(self):
        out = self.score_with(replacement=(
            "    {\n        int end = from + count;\n\n"
            "        if (end > n)\n            end = n;\n"
            "        for (i = from; i < end; i++)\n            s += v[i];\n"
            "    }\n    return s;\n"))
        self.assertTrue(out["files_ok"] and out["build_ok"] and out["test_ok"],
                        out["notes"])


SCRATCH_LANE = '''\
from lanes import xwpe_common

LANE = "scratch-lane"


def run(driver, ctx, model, endpoint):
    result = xwpe_common.LaneResult()
    result.end_reason = "done"
    result.answer_text = "model=%s" % model
    return result
'''

RAISING_LANE = '''\
LANE = "raising-lane"


def run(driver, ctx, model, endpoint):
    raise RuntimeError("the lane broke")
'''


class LaneDirectory(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def lane(self, name, text):
        write(os.path.join(self.dir, name), text)

    def test_a_lane_is_loaded_by_path(self):
        self.lane("one.py", SCRATCH_LANE)
        lanes = external.load_lane_dir(self.dir, taken=("mock", "local"))
        self.assertEqual(list(lanes), ["scratch-lane"])

    def test_a_missing_folder_is_a_clear_error(self):
        with self.assertRaises(external.LaneLoadError) as ctx:
            external.load_lane_dir(os.path.join(self.dir, "absent"))
        self.assertIn("not a directory", str(ctx.exception))

    def test_a_file_that_does_not_import_is_a_clear_error(self):
        self.lane("bad.py", "def run(:\n")
        with self.assertRaises(external.LaneLoadError) as ctx:
            external.load_lane_dir(self.dir)
        self.assertIn("failed to load", str(ctx.exception))

    def test_a_file_without_the_interface_is_a_clear_error(self):
        self.lane("nolane.py", "X = 1\n")
        with self.assertRaises(external.LaneLoadError) as ctx:
            external.load_lane_dir(self.dir)
        self.assertIn("LANE", str(ctx.exception))
        self.lane("nolane.py", 'LANE = "x"\n')
        with self.assertRaises(external.LaneLoadError) as ctx:
            external.load_lane_dir(self.dir)
        self.assertIn("run(", str(ctx.exception))

    def test_a_built_in_name_cannot_be_reused(self):
        self.lane("one.py", SCRATCH_LANE.replace("scratch-lane", "mock"))
        with self.assertRaises(external.LaneLoadError):
            external.load_lane_dir(self.dir, taken=("mock", "local"))

    def test_the_runner_runs_a_loaded_lane_and_survives_one_that_raises(self):
        self.lane("one.py", SCRATCH_LANE)
        self.lane("two.py", RAISING_LANE)
        lanes = external.load_lane_dir(self.dir)
        out = os.path.join(self.dir, "out")
        results = runner.Results(out, "selfcheck")
        for name in ("raising-lane", "scratch-lane"):
            runner.run_external_lane(None, lanes[name], ["E03"], ["m1"], 1,
                                     30, "http://unused", results)
        raised, ran = results.rows
        self.assertEqual(raised["end_reason"], xwpe_common.END_EXCEPTION)
        self.assertIn("the lane broke", raised["notes"])
        self.assertEqual(ran["end_reason"], "done")
        self.assertEqual(ran["lane"], "scratch-lane")
        self.assertFalse(runner.passed_everything(ran))

    def test_a_server_that_cannot_be_reached_gives_rows_not_a_traceback(self):
        out = os.path.join(self.dir, "out")
        results = runner.Results(out, "unreachable")
        # Port 9 on the loopback address: nothing listens there.
        runner.run_ollama_lane(None, ["E01", "E03"], ["m1"], 1, 30,
                               "http://127.0.0.1:9", results)
        self.assertEqual([r["end_reason"] for r in results.rows],
                         [xwpe_common.END_SERVER_UNREACHABLE] * 2)
        self.assertIn("unreachable", results.rows[0]["notes"])


if __name__ == "__main__":
    unittest.main()
