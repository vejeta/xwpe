"""A program under a debugger that waits for the row, not for the clock.

A row that debugs a looping program used to give the program a sleep and race
its own keys against it: the program passed its line every two seconds while
the row needed longer than that to take a breakpoint off, and the wait scale
only moved the race.  Here the program waits at one source line for the file
`open-<n>` and then writes `passed-<n>`, so the row decides when each pass
happens and learns when it has: a row that must be quick does not depend on a
duration at all.

Three forms, each one source line per language so a program keeps the line
numbers its row asserts against:

- hold: wait for `open-<n>`, then write `passed-<n>`; the program writes
  `arrived-<n>` before it waits, so the row can tell the program is there
  (not a guess that "by now" it is).
- ticker: run a body every GATE_TICK_MS until `open-<n>` exists, then run a
  last body once and write `passed-<n>`; for a program that prints while the
  row looks at the editor.
- stay: wait for a pass the row never opens; a program that only has to be
  alive.  The session's end ladder ends it, as it ends any program.

All three also leave when the file `closed` exists.  ProgramGate.close()
writes it from the row's teardown (tests/conftest.py), so a program the end
ladder missed does not outlive its row, with no clock involved.

The pass number is n + 1, with n the program's own counter before it is
incremented.  The poll inside a program is a short sleep of its own
(GATE_POLL_MS; a68g sleeps whole seconds: A68_GATE_POLL_S).
"""
import os

import gui_owner

# The line of a program text that stands for the wait.
MARKER = "@@GATE@@"
# The line of a program text that stands for a stay.
STAY_MARKER = "@@STAY@@"
# How long the program sleeps between two looks at the gate file.
GATE_POLL_MS = 20
# a68g's sleep takes whole seconds: the grain of its look at the gate.
A68_GATE_POLL_S = 1
# How long a ticker waits between two runs of its body.
GATE_TICK_MS = 100
GATE_SEEN_S = 60
# The pass number a stay waits for; rows open passes from 1, never this one.
STAY_PASS = 0

# Where a hold's pass number comes from, as each language writes it.
PASS_EXPRESSION = {"c": "(int)(n + 1)", "py": "(n + 1)", "java": "(n + 1)",
                   "go": "n+1", "rust": "n + 1", "a68": "1", "sh": "1"}
# The text of a program's directory as each language writes a string.
QUOTED = {"c": '"%s"', "py": '"%s"', "java": '"%s"', "go": '"%s"',
          "rust": '"%s"', "a68": '"%s"', "sh": "'%s'"}


def _c_hold(forever):
    wait = "access(c, F_OK)" if forever else "access(o, F_OK) && access(c, F_OK)"
    passed = "" if forever else (
        ' snprintf(p, sizeof p, "%s/passed-%d", @@DIR@@, @@N@@);'
        ' fclose(fopen(p, "w"));')
    return ('    { char o[512], c[512], p[512];'
            ' snprintf(o, sizeof o, "%s/open-%d", @@DIR@@, @@N@@);'
            ' snprintf(c, sizeof c, "%s/closed", @@DIR@@);'
            ' snprintf(p, sizeof p, "%s/arrived-%d", @@DIR@@, @@N@@);'
            ' fclose(fopen(p, "w"));'
            ' while (' + wait + ') usleep(' + str(GATE_POLL_MS * 1000) + ');'
            + passed + ' }')


def _py_hold(forever):
    wait = ('not os.path.exists(@@DIR@@ + "/closed")' if forever else
            'not (os.path.exists(@@DIR@@ + "/open-%d" % @@N@@)'
            ' or os.path.exists(@@DIR@@ + "/closed"))')
    passed = "" if forever else (
        '; open(@@DIR@@ + "/passed-%d" % @@N@@, "w").close()')
    return ('    open(@@DIR@@ + "/arrived-%d" % @@N@@, "w").close();'
            ' [time.sleep(' + str(GATE_POLL_MS / 1000.0) + ') for _ in iter('
            'lambda: ' + wait + ', False)]' + passed)


def _java_hold(forever):
    wait = ('!new java.io.File(@@DIR@@ + "/closed").exists()' if forever else
            '!new java.io.File(@@DIR@@ + "/open-" + @@N@@).exists()'
            ' && !new java.io.File(@@DIR@@ + "/closed").exists()')
    passed = "" if forever else (
        ' new java.io.File(@@DIR@@ + "/passed-" + @@N@@).createNewFile();')
    return ('            new java.io.File(@@DIR@@ + "/arrived-" + @@N@@)'
            '.createNewFile(); while (' + wait + ') Thread.sleep('
            + str(GATE_POLL_MS) + ');' + passed)


def _go_hold(forever):
    stat_open = "" if forever else (
        'if _, e := os.Stat(@@DIR@@ + fmt.Sprint("/open-", @@N@@)); e == nil'
        ' { break }; ')
    passed = "" if forever else (
        ' os.WriteFile(@@DIR@@ + fmt.Sprint("/passed-", @@N@@), nil, 0644)')
    return ('\t\t{ os.WriteFile(@@DIR@@ + fmt.Sprint("/arrived-", @@N@@), nil,'
            ' 0644); for { ' + stat_open +
            'if _, e := os.Stat(@@DIR@@ + "/closed"); e == nil { break };'
            ' time.Sleep(' + str(GATE_POLL_MS) + ' * time.Millisecond) };'
            + passed + ' }')


def _rust_hold(forever):
    wait = ("!std::path::Path::new(&c).exists()" if forever else
            "!std::path::Path::new(&o).exists()"
            " && !std::path::Path::new(&c).exists()")
    passed = "" if forever else (
        ' std::fs::write(format!("{}/passed-{}", @@DIR@@, @@N@@), "").unwrap();')
    return ('        { let o = format!("{}/open-{}", @@DIR@@, @@N@@);'
            ' let c = format!("{}/closed", @@DIR@@);'
            ' std::fs::write(format!("{}/arrived-{}", @@DIR@@, @@N@@), "")'
            '.unwrap(); while ' + wait + ' {'
            ' std::thread::sleep(std::time::Duration::from_millis('
            + str(GATE_POLL_MS) + ')); }' + passed + ' }')


def _a68_hold(forever):
    """One closed clause (BEGIN ... END), so that two gates in one program do
    not declare the same names in one range."""
    # The tags have names with a space so that no program's own tag, in the
    # range the clause is in, is hidden by one of them.
    wait = ('open (gate c, @@DIR@@ + "/closed", stand in channel) /= 0'
            if forever else
            'open (gate f, @@DIR@@ + "/open-@@N@@", stand in channel) /= 0'
            ' AND open (gate c, @@DIR@@ + "/closed", stand in channel) /= 0')
    passed = "" if forever else (
        ' FILE gate p; VOID (establish (gate p, @@DIR@@ + "/passed-@@N@@",'
        ' stand out channel)); put (gate p, "x"); close (gate p);')
    names = "FILE gate c;" if forever else "FILE gate f; FILE gate c;"
    return ('BEGIN FILE gate a; VOID (establish (gate a,'
            ' @@DIR@@ + "/arrived-@@N@@", stand out channel));'
            ' put (gate a, "x"); close (gate a);'
            ' ' + names + ' WHILE ' + wait + ' DO VOID (sleep ('
            + str(A68_GATE_POLL_S) + ')) OD;' + passed + ' SKIP END;')


def _sh_hold(forever):
    wait = ("[ ! -e @@DIR@@/closed ]" if forever else
            "[ ! -e @@DIR@@/open-@@N@@ ] && [ ! -e @@DIR@@/closed ]")
    passed = "" if forever else " : > @@DIR@@/passed-@@N@@;"
    return (": > @@DIR@@/arrived-@@N@@; while " + wait + "; do sleep "
            + str(GATE_POLL_MS / 1000.0) + "; done;" + passed)


HOLDS = {"c": _c_hold, "py": _py_hold, "java": _java_hold, "go": _go_hold,
         "rust": _rust_hold, "a68": _a68_hold, "sh": _sh_hold}


def _c_ticker(body, last):
    return ('    { char o[512], c[512], p[512]; int i;'
            ' snprintf(o, sizeof o, "%s/open-%d", @@DIR@@, @@N@@);'
            ' snprintf(c, sizeof c, "%s/closed", @@DIR@@);'
            ' for (i = 0; access(o, F_OK) && access(c, F_OK); i++) { '
            + body + '; fflush(stdout); usleep(' + str(GATE_TICK_MS * 1000)
            + '); } ' + last + '; fflush(stdout);'
            ' snprintf(p, sizeof p, "%s/passed-%d", @@DIR@@, @@N@@);'
            ' fclose(fopen(p, "w")); }')


def _go_ticker(body, last):
    return ('\tfor i := 0; ; i++ {'
            ' if _, e := os.Stat(@@DIR@@ + fmt.Sprint("/open-", @@N@@));'
            ' e == nil { break };'
            ' if _, e := os.Stat(@@DIR@@ + "/closed"); e == nil { break };'
            ' ' + body + '; time.Sleep(' + str(GATE_TICK_MS)
            + ' * time.Millisecond) }; ' + last + ';'
            ' os.WriteFile(@@DIR@@ + fmt.Sprint("/passed-", @@N@@), nil, 0644)')


def _rust_ticker(body, last):
    return ('    { let o = format!("{}/open-{}", @@DIR@@, @@N@@);'
            ' let c = format!("{}/closed", @@DIR@@); let mut i = 0;'
            ' while !std::path::Path::new(&o).exists()'
            ' && !std::path::Path::new(&c).exists() { ' + body +
            '; i += 1; std::thread::sleep(std::time::Duration::from_millis('
            + str(GATE_TICK_MS) + ')); } ' + last + ';'
            ' std::fs::write(format!("{}/passed-{}", @@DIR@@, @@N@@), "")'
            '.unwrap(); }')


def _sh_ticker(body, last):
    return ("i=0; while [ ! -e @@DIR@@/open-@@N@@ ] && [ ! -e @@DIR@@/closed ];"
            " do " + body + "; i=$((i + 1)); sleep "
            + str(GATE_TICK_MS / 1000.0) + "; done; " + last
            + "; : > @@DIR@@/passed-@@N@@")


TICKERS = {"c": _c_ticker, "go": _go_ticker, "rust": _rust_ticker,
           "sh": _sh_ticker}


class ProgramGate:
    """The gate files of one row, in a folder of its own.

    @folder is a folder the program can see: when the program runs inside a
    contained area (an assistant's run_command), the caller passes a folder
    inside it, never one beside it."""

    live = []        # every gate not yet closed; tests/conftest.py closes them

    def __init__(self, folder):
        self.dir = os.path.join(str(folder), "gate")
        os.makedirs(self.dir, exist_ok=True)
        ProgramGate.live.append(self)

    def _fill_text(self, lang, text, pass_value, make):
        text = text.replace("@@DIR@@", QUOTED[lang] % self.dir)
        text = text.replace("@@N@@", str(pass_value))
        return text.replace("$", "$$") if make else text

    def line(self, lang, fixed_pass=None, make=False):
        """The one source line that makes a program of @lang wait for the
        gate: for pass n + 1 of its own counter, or, with @fixed_pass, for
        that pass (a program with no counter, or one that waits once).  With
        @make, a dollar sign is doubled, for a make recipe."""
        pass_value = PASS_EXPRESSION[lang] if fixed_pass is None else fixed_pass
        return self._fill_text(lang, HOLDS[lang](False), pass_value, make)

    def ticker_line(self, lang, body, last, n=1, make=False):
        """One source line that runs @body every GATE_TICK_MS (the tick
        number is `i`) until the row opens pass @n, then runs @last once and
        writes `passed-<n>`.  A row that looks at the editor while a program
        prints gets output with no clock of the program's own, and the last
        line it prints is known."""
        text = TICKERS[lang](body, last)
        return self._fill_text(lang, text, n, make)

    def stay_line(self, lang, make=False):
        """One source line that holds the program until the row ends.  It
        replaces every long sleep a program used to stand in for "running
        for as long as the row needs"."""
        return self._fill_text(lang, HOLDS[lang](True), STAY_PASS, make)

    def fill(self, program, lang, fixed_pass=None):
        """@program with its marker line replaced by the gate's line."""
        return self.fill_with(program, self.line(lang, fixed_pass))

    def fill_with(self, program, line):
        """@program with its marker line replaced by @line."""
        lines = program.split("\n")
        at = [i for i, text in enumerate(lines) if text.strip() == MARKER]
        assert len(at) == 1, "a program has one %s line" % MARKER
        lines[at[0]] = line
        return "\n".join(lines)

    def fill_markers(self, program, lang):
        """@program with each MARKER line replaced by the hold of the next
        pass (1, then 2, ...) and each STAY_MARKER line by a stay: for a
        program a fixture writes before the row's body runs, when only the
        gate's folder is known."""
        lines = program.split("\n")
        n = 0
        for i, text in enumerate(lines):
            if text.strip() == MARKER:
                n += 1
                lines[i] = self.line(lang, fixed_pass=n)
            elif text.strip() == STAY_MARKER:
                lines[i] = self.stay_line(lang)
        return "\n".join(lines)

    def open(self, n):
        """Let pass @n of the program through."""
        with open(os.path.join(self.dir, "open-%d" % n), "w"):
            pass

    def close(self):
        """End every wait of the gate: a program that is still held, or still
        printing, leaves.  Called from the row's teardown; no clock."""
        with open(os.path.join(self.dir, "closed"), "w"):
            pass
        if self in ProgramGate.live:
            ProgramGate.live.remove(self)

    def has_passed(self, n):
        """True once the program has reached the gate of pass @n and gone
        through it."""
        return os.path.exists(os.path.join(self.dir, "passed-%d" % n))

    def has_arrived(self, n):
        """True once the program has reached the gate of pass @n."""
        return os.path.exists(os.path.join(self.dir, "arrived-%d" % n))

    def passed(self, session, n, tmo=GATE_SEEN_S):
        """Wait, reading the editor as the row's waits do, until the program
        has gone through pass @n; False when @tmo seconds (scaled) run out."""
        import wpe_driver       # not at import: the graphical rows need none
        return wpe_driver.wait_until(session, lambda: self.has_passed(n), tmo,
                                     what="the program to pass gate %d" % n)

    def arrived(self, session, n, tmo=GATE_SEEN_S):
        """Wait, reading the editor, until the program is held at pass @n."""
        import wpe_driver       # not at import: the graphical rows need none
        return wpe_driver.wait_until(session, lambda: self.has_arrived(n), tmo,
                                     what="the program to arrive at gate %d" % n)

    def arrived_seen(self, n, tmo=GATE_SEEN_S):
        """The wait for `arrived-<n>` for a row with no terminal session to
        read (the X11 and Wayland rows): through the owner's wait."""
        return gui_owner.wait_for(lambda: self.has_arrived(n),
                                  "the program to arrive at gate %d" % n, tmo)

    def seen(self, n, tmo=GATE_SEEN_S):
        """The wait for `passed-<n>` for a row with no terminal session to
        read (the X11 and Wayland rows): through the owner's wait."""
        return gui_owner.wait_for(lambda: self.has_passed(n),
                                  "the program to pass gate %d" % n, tmo)


def close_all():
    """Close every gate of the row that ended.  Run by the autouse fixture of
    tests/conftest.py, so it also runs when the row failed."""
    for gate in list(ProgramGate.live):
        gate.close()
