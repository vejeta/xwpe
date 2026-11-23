"""A program under a debugger that waits for the row, not for the clock.

A row that debugs a looping program used to give the program a sleep and race
its own keys against it: the program passed its line every two seconds while
the row needed longer than that to take a breakpoint off, and the wait scale
only moved the race.  Here the program waits at one source line for the file
`open-<n>` and then writes `passed-<n>`, so the row decides when each pass
happens and learns when it has: a row that must be quick does not depend on a
duration at all.

One source line per language, so a program keeps the line numbers its row
asserts against.  The pass number is n + 1, with n the program's own counter
before it is incremented.  The poll inside the program is a short sleep of
its own (GATE_POLL_S), which is also what a debugger's interrupt lands in.
"""
import os

import wpe_driver

# The line of a program text that stands for the wait.
MARKER = "@@GATE@@"
# How long the program sleeps between two looks at the gate file.
GATE_POLL_MS = 20
# A program that only has to stay alive stays this long, before the scale:
# longer than any row's budget.
PROGRAM_STAYS_S = 100000
GATE_SEEN_S = 60

LINES = {
    "c": ('    { char p[512]; snprintf(p, sizeof p, "%s/open-%d", @@DIR@@, (int)(n + 1));'
          ' while (access(p, F_OK)) usleep(' + str(GATE_POLL_MS * 1000) + ');'
          ' snprintf(p, sizeof p, "%s/passed-%d", @@DIR@@, (int)(n + 1));'
          ' fclose(fopen(p, "w")); }'),
    "py": ('    [time.sleep(' + str(GATE_POLL_MS / 1000.0) + ') for _ in iter(lambda: '
           'not os.path.exists(@@DIR@@ + "/open-%d" % (n + 1)), False)];'
           ' open(@@DIR@@ + "/passed-%d" % (n + 1), "w").close()'),
    "java": ('            while (!new java.io.File(@@DIR@@ + "/open-" + (n + 1)).exists())'
             ' Thread.sleep(' + str(GATE_POLL_MS) + ');'
             ' new java.io.File(@@DIR@@ + "/passed-" + (n + 1)).createNewFile();'),
    "go": ('\t\tp := @@DIR@@ + fmt.Sprint("/open-", n+1); for { if _, e := os.Stat(p);'
           ' e == nil { break }; time.Sleep(' + str(GATE_POLL_MS) + ' * time.Millisecond) };'
           ' os.WriteFile(@@DIR@@ + fmt.Sprint("/passed-", n+1), nil, 0644)'),
    "rust": ('        { let p = format!("{}/open-{}", @@DIR@@, n + 1);'
             ' while !std::path::Path::new(&p).exists() {'
             ' std::thread::sleep(std::time::Duration::from_millis(' + str(GATE_POLL_MS) + ')); }'
             ' std::fs::write(format!("{}/passed-{}", @@DIR@@, n + 1), "").unwrap(); }'),
}
# How each line writes the pass number, for a pass named in the row.
PASS_EXPRESSION = {"c": "(int)(n + 1)", "py": "(n + 1)", "java": "(n + 1)",
                   "go": "n+1", "rust": "n + 1"}
# The text of a program's directory as each language writes a string.
QUOTED = {"c": '"%s"', "py": '"%s"', "java": '"%s"', "go": '"%s"', "rust": '"%s"'}


class ProgramGate:
    """The gate files of one row, in a folder of its own."""

    def __init__(self, folder):
        self.dir = os.path.join(str(folder), "gate")
        os.makedirs(self.dir, exist_ok=True)

    def line(self, lang, fixed_pass=None):
        """The one source line that makes a program of @lang wait for the
        gate: for pass n + 1 of its own counter, or, with @fixed_pass, for
        that pass (a program with no counter, or one that waits once)."""
        text = LINES[lang]
        if fixed_pass is not None:
            text = text.replace(PASS_EXPRESSION[lang], str(fixed_pass))
        return text.replace("@@DIR@@", QUOTED[lang] % self.dir)

    def fill(self, program, lang, fixed_pass=None):
        """@program with its marker line replaced by the gate's line."""
        lines = program.split("\n")
        at = [i for i, text in enumerate(lines) if text.strip() == MARKER]
        assert len(at) == 1, "a program has one %s line" % MARKER
        lines[at[0]] = self.line(lang, fixed_pass)
        return "\n".join(lines)

    def open(self, n):
        """Let pass @n of the program through."""
        with open(os.path.join(self.dir, "open-%d" % n), "w"):
            pass

    def has_passed(self, n):
        """True once the program has reached the gate of pass @n and gone
        through it."""
        return os.path.exists(os.path.join(self.dir, "passed-%d" % n))

    def passed(self, session, n, tmo=GATE_SEEN_S):
        """Wait, reading the editor as the row's waits do, until the program
        has gone through pass @n; False when @tmo seconds (scaled) run out."""
        return wpe_driver.wait_until(session, lambda: self.has_passed(n), tmo,
                                     what="the program to pass gate %d" % n)
