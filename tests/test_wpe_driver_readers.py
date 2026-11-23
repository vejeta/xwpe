"""The readers of tests/wpe_driver.py that make a row independent of how long
its temporary path is: the pane read back page by page, the window title read
by column and the screen width taken from the path a row waits for.

No editor runs here: each row feeds the reader the rows a screen would hold,
or drives pane_rows_with_history through a fake session that serves the pane
the way the editor does (PgUp moves a page and keeps one row of the page
below, the top of the transcript clamps, a repaint can be late).

Run: tests/.venv/bin/python -m pytest -q tests/test_wpe_driver_readers.py
"""
import wpe_driver as drv

KEEP = 1        # the rows of the page below that a PgUp keeps in view
WIDTH = 40      # the interior width of the fake pane
FULL_ROW = "Q" * (WIDTH - 1)      # a row cut at the wrap width inside one word


def _padded(rows, width=40):
    """Pane rows as pane_rows gives them: every one as wide as the pane."""
    return [row.ljust(width) for row in rows]


def _title_row(title, fill=20, number="1", cut=False):
    """A window title row as pyte shows it: corner, the frame's line, the
    title between spaces, the line, the window number, the buttons."""
    body = title if cut else " %s " % title
    return "l" + "q" * fill + body + "q" * fill + number + "q □ ✕qk"


class FakePane:
    """The AI pane as a session shows it: a frame around WIDTH columns and
    ``height`` rows of a transcript, at the bottom of it to begin with.

    PgUp moves the view up by a page less KEEP row, never past the top (the
    editor's own rule, we_ai_ui.c).  ``lag`` makes the screen served right
    after a PgUp the one from before it, once: a repaint that is still to
    come.  ``pgups`` counts the PgUp keys the reader sent."""

    def __init__(self, transcript, height=8, lag=False):
        self.transcript = [row.ljust(WIDTH) for row in transcript]
        self.height = height
        self.top = max(0, len(self.transcript) - height)
        self.lag = lag
        self.stale = None
        self.pgups = 0

    def _screen(self):
        rows = ["l" + "qq" + " AI " + "q" * (WIDTH - 6) + "k"]
        for row in self.transcript[self.top:self.top + self.height]:
            rows.append("x" + row + "x")
        rows.append("m" + "q" * WIDTH + "j")
        return rows

    def display(self, drain=0.6):
        if self.stale is not None:
            shown, self.stale = self.stale, None
            return shown
        return self._screen()

    def key(self, *keys, delay=0.0):
        for key in keys:
            if key != drv.KEY_PGUP:
                continue
            self.pgups += 1
            before = self._screen()
            self.top = max(0, self.top - (self.height - KEEP))
            if self.lag:
                self.stale = before
                self.lag = False


# @scenario SC-test-paths-01
def test_a_title_with_a_letter_q_is_read_whole():
    """The old reader took the text between two "q"s: a name with a q in it
    came back as a piece of it."""
    shown = drv.active_window_title([" menu ", _title_row("qa q.c")])
    assert shown.text == "qa q.c" and not shown.elided


# @scenario SC-test-paths-01
def test_the_mark_of_a_tool_window_is_not_part_of_its_title():
    """Messages carries a wrench just inside its corner (a locked file, a
    padlock); the title is the name."""
    row = "lq⚙" + "q" * 30 + " Messages " + "q" * 30 + "2q □ ✕qk"
    assert drv.active_window_title([row]).text == "Messages"


# @scenario SC-test-paths-01
def test_a_title_with_a_letter_l_and_spaces_is_read_whole():
    shown = drv.active_window_title([_title_row("my lib file.c")])
    assert shown.text == "my lib file.c"


# @scenario SC-test-paths-02
def test_a_title_the_editor_cut_says_so():
    """we_wind.c cuts a long title at its head and ends it with "...": the
    reader gives what is on screen and says it was cut, so a row that wants
    the whole name can say the editor lost it."""
    shown = drv.active_window_title([_title_row("/var/tmp/qq/dd/ee/ff...",
                                                cut=True)])
    assert shown.text == "/var/tmp/qq/dd/ee/ff..." and shown.elided


# @scenario SC-test-paths-01
def test_the_title_is_found_on_the_row_that_has_the_buttons():
    rows = [" menu ", "x a title  with no buttons          x", _title_row("t.c")]
    assert drv.active_window_title(rows).text == "t.c"


# @scenario SC-test-paths-01
def test_no_active_window_is_said_with_the_screen():
    try:
        drv.active_window_title(["no window here"])
    except AssertionError as why:
        assert "no active window on screen" in str(why)
        assert "no window here" in str(why)
    else:
        raise AssertionError("a screen with no window gave a title")


def test_window_buttons_are_on_the_active_title_row_only():
    assert drv.window_buttons_on(_title_row("t.c"))
    assert not drv.window_buttons_on("lqqqq t.c qqqqk")


# --- the pane read back page by page -------------------------------------

def _pages(transcript, height):
    """The views a PgUp walks through over @transcript (rows), newest first:
    each older view keeps KEEP row of the one below."""
    views = [transcript[-height:]]
    top = len(transcript) - height
    while top > 0:
        top = max(0, top - (height - KEEP))
        views.append(transcript[top:top + height])
    return views


# @scenario SC-test-paths-03
def test_pages_are_merged_by_the_rows_they_share_not_stacked():
    """Every page boundary inside a long wrapped line used to repeat one row
    of it, so a path in the joined line was never the real one."""
    transcript = ["row %02d" % i for i in range(30)]
    views = _pages(transcript, 8)
    seen = list(views[0])
    for older in views[1:]:
        seen = drv.rows_above(older, seen)
    assert seen == transcript


# @scenario SC-test-paths-03
def test_a_page_stopped_by_the_top_shares_more_rows_and_adds_only_the_rest():
    transcript = ["row %02d" % i for i in range(11)]
    below = transcript[-8:]                  # rows 3..10
    top = transcript[:8]                     # PgUp clamped at the top: rows 0..7
    assert drv.pane_overlap(top, below) == 5
    assert drv.rows_above(top, below) == transcript


# @scenario SC-test-paths-03
def test_identical_rows_of_a_long_one_letter_path_are_not_an_overlap():
    """A folder name of one repeated letter fills whole rows with the same
    text; only the one row the pane keeps is shared between two pages."""
    run = "b" * 40
    older = ["head", run, run, run]
    newer = [run, run, "tail"]
    assert drv.pane_overlap(older, newer) == 1
    assert drv.rows_above(older, newer) == ["head", run, run, run, run, "tail"]


def test_blank_rows_are_not_an_overlap():
    assert drv.pane_overlap(["a", "", ""], ["", "", "b"]) == 0


# @scenario SC-test-paths-03
def test_a_wrapped_path_crossing_a_page_edge_is_joined_once():
    """The sentence of a changeset refusal quotes a folder that wraps over
    several rows; the page edge falls inside it."""
    width = 40
    path = "/very/long/" + "Q" * 100
    sentence = path + " is busy now."
    rows = []
    rest = sentence
    while rest:
        rows.append(rest[:width].ljust(width + 1))   # the pane's padding
        rest = rest[width:]
    older = rows[:3]
    newer = rows[2:]                         # one row kept, as the pane does
    seen = drv.rows_above(older, list(newer))
    assert len(seen) == len(rows)
    joined = drv.pane_logical_lines(seen)
    assert path in joined[0] and joined[0].count("Q") == 100


# @scenario SC-test-paths-04
def test_a_needle_lying_only_on_the_first_row_is_read_further_back():
    """The needle is whole on the first row's line, but that row may be the
    middle of a longer line: page back once more."""
    rows = _padded(["regular file (a pipe) was named", "next line", "last"])
    assert drv.needs_older_rows(rows, ["regular file"])
    assert not drv.needs_older_rows(
        _padded(["regular file (a pipe) was named", "next line",
                 "a regular file again"]), ["regular file"])


def test_a_missing_needle_is_read_further_back():
    assert drv.needs_older_rows(_padded(["a", "b"]), ["not here"])
    assert not drv.needs_older_rows(_padded(["a", "needle here", "b"]),
                                    ["needle"])


# @scenario SC-test-paths-04
def test_a_phrase_a_box_wrapped_is_found_whole_in_its_text():
    """"Use Save" ends one row and "As to write" starts the next: the box's
    text joins them."""
    title = "x   lqqqqqq Message qqqqqq✕qk   v"
    inner = len(title) - len("x   l") - len("k   v")
    disp = [title]
    for words in ("Cannot write", "long path.  Use Save", "As to write it."):
        disp.append("x   x" + words.center(inner) + "x   a")
    disp.append("x   m" + "q" * inner + "j   a")
    text = drv.box_text(disp, "Message")
    assert "Use Save As to write it." in text
    assert "Cannot write long path." in text


# --- pane_rows_with_history driven through a fake session -----------------

def _long_path_transcript():
    """A sentence that quotes a path wrapped in full rows (cut inside the
    word), the rows of the path all alike, between lines of other text, so
    page edges fall inside the path."""
    first = ["line %02d of the run" % i for i in range(6)]
    path_rows = [FULL_ROW] * 4 + ["Q" * 20]
    last = ["line %02d after it" % i for i in range(14)]
    return first + ["could not restore"] + path_rows + last, \
        "could not restore " + "Q" * (4 * (WIDTH - 1) + 20)


def test_a_line_longer_than_a_page_is_read_from_its_start():
    q = "Q" * WIDTH
    sentence = ["the folder " + "Q" * (WIDTH - 11)] + [q] * 9 + ["QQQQ is not a regular file"]
    transcript = ["line %02d" % i for i in range(10)] + sentence + ["line %02d after" % i for i in range(7)]
    lines = drv.pane_text_with_history(FakePane(transcript), ["regular file"])
    assert "the folder QQQ" in " ".join(lines)


# @scenario SC-test-paths-03
def test_the_reader_reads_a_path_whole_across_page_edges():
    """The reader is driven through the fake pane: the sentence is above the
    view, its rows are alike, and each PgUp keeps one row.  Stacking the pages
    blind (seen = page + seen) repeats a row of the path at every edge."""
    transcript, whole = _long_path_transcript()
    fake = FakePane(transcript)
    lines = drv.pane_text_with_history(fake, [whole])
    assert whole in lines


# @scenario SC-test-paths-04
def test_the_reader_pages_back_for_a_needle_on_the_first_row():
    """The bottom view opens on the second row of a sentence; the needle is
    only the weak word that row holds.  The reader reads one page more, as
    the phrase the row asserts is cut at that row."""
    sentence = ["the engine says that it is not a",
                "regular file (a pipe) was named"]
    transcript = ["line %02d" % i for i in range(10)] + sentence \
        + ["line %02d after" % i for i in range(7)]
    fake = FakePane(transcript)
    lines = drv.pane_text_with_history(fake, ["regular file"])
    assert "it is not a regular file" in " ".join(lines)


# @scenario SC-test-paths-04
def test_a_late_repaint_is_not_taken_for_the_top():
    """The screen after the first PgUp is still the old one; the reader reads
    it again before it believes it is at the top of the transcript."""
    transcript = ["line %02d" % i for i in range(30)]
    transcript[12] = "the needle is here"
    fake = FakePane(transcript, lag=True)
    lines = drv.pane_text_with_history(fake, ["the needle is here"])
    assert any("the needle is here" in line for line in lines)


# @scenario SC-test-paths-04
def test_the_top_of_the_transcript_ends_the_paging():
    """A needle that is nowhere: the reader stops at the top (the same page
    twice) and not after the pages it was allowed."""
    transcript = ["line %02d" % i for i in range(11)]
    fake = FakePane(transcript)
    drv.pane_text_with_history(fake, ["not in the transcript"], pages=10)
    assert fake.pgups <= 3, fake.pgups


# @scenario SC-test-paths-03
@drv.incoherence("the clamped top page: only the pane's scroll position can "
                 "say that a page stopped by the top moved less (pane_overlap)")
def test_a_clamped_top_page_of_alike_rows_is_not_read_twice():
    """A 14-row transcript, a path of 7 rows all alike at its middle, a view
    of 8: the one PgUp is clamped by the top and moves 6 rows, so it shares
    two rows with the bottom view, not the one the pane keeps.  The last row
    of the page equals the first row below, the one row is taken, and a row
    of the path is read twice: 8 rows of it, one too many."""
    transcript = ["line 0", "line 1", "start"] + [FULL_ROW] * 7 \
        + ["line 10", "line 11", "line 12", "line 13"]
    fake = FakePane(transcript)
    rows = drv.pane_rows_with_history(fake, ["start"])
    alike = [row for row in rows if row.strip() == FULL_ROW]
    assert len(alike) == 7, len(alike)


# --- the width a row needs ------------------------------------------------

# @scenario SC-test-paths-05
def test_the_width_follows_the_path_the_row_waits_for():
    assert drv.cols_for("x" * 10) == drv.COLS
    long_path = "/" + "d" * 330
    width = drv.cols_for(long_path, tail=drv.COMPILER_LINE_TAIL)
    assert width >= len(long_path) + drv.COMPILER_LINE_TAIL
    assert drv.cols_for(long_path, at_least=500) == 500


# --- the pane frame, drawn whole ------------------------------------------

def test_a_pane_frame_painted_to_its_corner_is_drawn_and_a_half_painted_one_is_not():
    """A row that reads the pane right after a wait for its text can catch the
    title row with no right corner yet: pane_frame then has nothing to find."""
    whole = _title_row(drv.PANE_NAME)
    half = whole[:whole.index(" %s " % drv.PANE_NAME) + len(drv.PANE_NAME) + 4]
    assert drv.FRAME_TOP_RIGHT not in half
    assert drv.pane_frame_drawn([whole])
    assert not drv.pane_frame_drawn([half])
    assert not drv.pane_frame_drawn(["no pane on this screen"])
