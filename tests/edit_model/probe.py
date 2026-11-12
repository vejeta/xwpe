"""Does the state channel answer every kind of operation wherever the keys
land?  For each place keys are read (the text, a prefix, the menu bar, a
dialog, a message box, the File Manager) and each operation kind, the probe
enters the place, sends the operation's keys, and records whether a dump
came back.  A "no" is a key path the channel misses."""
import tempfile

from .inventory import CONTEXTS, LEAVE_ESC_MAX, LEAVE_KEY, OP_KINDS
from .oracle import ChannelMiscount, ChannelTimeout, EditSession

# A probe answer is expected at once; a missing one costs only this long.
PROBE_TIMEOUT_S = 5.0
SEED = ("int main(void)\n{\n\treturn 0;\n}\nalpha beta gamma\n"
        "café au lait\nlast line\n")
FILENAME = "t.c"


class Prober:
    """One session at a time, started again whenever an operation left the
    editor somewhere the next probe cannot start from."""

    def __init__(self, base, binary=None):
        self.base = base
        self.binary = binary
        self.session = None
        self.starts = 0
        self.start_ms = []

    def fresh(self):
        self.close()
        wd = tempfile.mkdtemp(dir=self.base)
        self.session = EditSession(wd, SEED, filename=FILENAME, binary=self.binary)
        self.starts += 1
        self.start_ms.append(self.session.start_ms)
        return self.session

    def close(self):
        if self.session is not None:
            self.session.close()
            self.session = None

    def at_text(self):
        """Is the editor back at the seed file's text, ready for a probe?"""
        s = self.session
        if s is None or not s.alive():
            return False
        d = s.last
        return d.ctx == "edit" and d.win is not None and d.win.name == FILENAME

    def back_to_text(self):
        """Press Esc until the text has the keys again, or start afresh."""
        try:
            for _ in range(LEAVE_ESC_MAX):
                if self.at_text():
                    return
                self.session.send([LEAVE_KEY], timeout=PROBE_TIMEOUT_S)
        except (ChannelTimeout, ChannelMiscount, OSError):
            pass
        if not self.at_text():
            self.fresh()

    def probe_one(self, enter, want_ctx, keys):
        """Enter a place, send @keys there; the verdict and the ctx after."""
        if not self.at_text():
            self.fresh()
        s = self.session
        try:
            d = s.send(enter, timeout=PROBE_TIMEOUT_S) if enter else s.last
        except (ChannelTimeout, ChannelMiscount) as e:
            self.fresh()
            return ("enter-failed", str(e))
        if not d.ctx.startswith(want_ctx):
            got = d.ctx
            self.back_to_text()
            return ("enter-failed", "ctx=%s" % got)
        try:
            d = s.send(keys, timeout=PROBE_TIMEOUT_S)
        except ChannelTimeout as e:
            self.fresh()
            return ("no", str(e))
        except ChannelMiscount as e:
            self.fresh()
            return ("no", str(e))
        ctx = d.ctx
        self.back_to_text()
        return ("yes", ctx)


def probe_all(base, contexts=CONTEXTS, ops=OP_KINDS, binary=None):
    """{(context, op): (verdict, detail)} over every pair, and the prober
    (for its start count and times)."""
    p = Prober(base, binary)
    table = {}
    answer_ms = []
    try:
        for cname, enter, want in contexts:
            p.fresh()
            for oname, keys in ops:
                table[(cname, oname)] = p.probe_one(enter, want, keys)
                if p.session is not None:
                    answer_ms.extend(p.session.answers_ms[-1:])
    finally:
        p.close()
    return table, p, answer_ms


def format_table(table, contexts=CONTEXTS, ops=OP_KINDS):
    """The op x context table as text: yes / no / enter-failed per cell."""
    names = [c for c, _, _ in contexts]
    lines = ["%-24s %s" % ("op", " ".join("%-12s" % n[:12] for n in names))]
    for oname, _ in ops:
        cells = []
        for c in names:
            cells.append("%-12s" % table[(c, oname)][0])
        lines.append("%-24s %s" % (oname, " ".join(cells)))
    return "\n".join(lines)
