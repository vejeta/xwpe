#!/usr/bin/env python3
"""check_ci_tools.py -- the Linux job installs what the rows need, and says so.

One record, tests/ci-tools.txt, maps each host tool a row needs to the Ubuntu
package that provides it and to what kind of tool it is (apt, layer, base,
image, absent:R, condition).  Three things must agree with it, offline:

  C1  every tool a manifest row of tests/harness declares in "needs" has a
      line in the record;
  C2  every apt and layer line names a package, and that package is in the
      apt-get list of each job of .github/workflows/linux.yml that expects
      the tool; a note that starts with "run: " names a command the job also
      runs (a package alone does not make a locale);
  C3  the XWPE_TEST_TOOLS_EXPECTED of each job is exactly the apt and layer
      tools whose package that job installs.  The harness reads a declared
      tool of this list that skips as a failure of the image; a display layer
      gate stops the session on a missing one.

Not seen: whether a package carries the program the record names (that needs
the package index); the first run of the job on the image shows it.

  check_ci_tools.py [--srcdir DIR] [--selftest]

With no option the self-test runs (a planted offence per rule must be named,
the unmodified copy must pass), then the tree.

Exit: 0 clean, 1 findings or a wrong canary, 77 no .github (a source tarball).
"""
import argparse
import glob
import json
import os
import re
import shutil
import sys
import tempfile

RECORD = os.path.join("tests", "ci-tools.txt")
WORKFLOW = os.path.join(".github", "workflows", "linux.yml")
MANIFESTS = os.path.join("tests", "harness", "*", "manifest.json")
EXPECTED_VARIABLE = "XWPE_TEST_TOOLS_EXPECTED"
KINDS = ("apt", "layer", "base", "image", "absent", "condition")
INSTALLED_KINDS = ("apt", "layer")
RUN_NOTE = "run: "
NO_PACKAGE = "-"
SKIPPED = 77


def read_record(path):
    """({need: (package, kind, note)}, [problem]) from the record."""
    lines = {}
    problems = []
    with open(path, encoding="utf-8") as fh:
        for number, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            fields = raw.split("|")
            if len(fields) != 4:
                problems.append("%s:%d: %d fields, four are needed"
                                % (path, number, len(fields)))
                continue
            need, package, kind, note = fields
            if kind.split(":", 1)[0] not in KINDS:
                problems.append("%s:%d: kind %s is none of %s"
                                % (path, number, kind, ", ".join(KINDS)))
                continue
            if need in lines:
                problems.append("%s:%d: %s is listed twice" % (path, number, need))
            lines[need] = (package, kind.split(":", 1)[0], note)
    return lines, problems


def manifest_needs(root):
    """{need: [capability, ...]} over every manifest row's "needs"."""
    found = {}
    for path in sorted(glob.glob(os.path.join(root, MANIFESTS))):
        capability = os.path.basename(os.path.dirname(path))
        with open(path, encoding="utf-8") as fh:
            manifest = json.load(fh)
        for scenario in manifest["scenarios"]:
            for need in scenario.get("needs") or []:
                found.setdefault(need, set()).add(capability)
    return found


def split_jobs(text):
    """{job name: its text} for the jobs of a workflow."""
    jobs = {}
    name = None
    in_jobs = False
    for line in text.splitlines():
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        match = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if in_jobs and match:
            name = match.group(1)
            jobs[name] = []
        elif name is not None:
            jobs[name].append(line)
    return {job: "\n".join(lines) for job, lines in jobs.items()}


def apt_packages(job_text):
    """The packages of every `apt-get install` of a job's text."""
    packages = set()
    lines = job_text.splitlines()
    for index, line in enumerate(lines):
        if "apt-get install" not in line:
            continue
        chunk = line.split("apt-get install", 1)[1]
        cursor = index
        while lines[cursor].rstrip().endswith("\\") and cursor + 1 < len(lines):
            cursor += 1
            chunk += " " + lines[cursor]
        for word in chunk.replace("\\", " ").split():
            if not word.startswith("-"):
                packages.add(word)
    return packages


def expected_tools(job_text):
    """The blank-separated names of the job's XWPE_TEST_TOOLS_EXPECTED, or
    None when the job does not set it (a plain scalar or a folded block)."""
    lines = job_text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^(\s*)%s:\s*(.*)$" % EXPECTED_VARIABLE, line)
        if not match:
            continue
        indent = len(match.group(1))
        value = match.group(2).strip()
        if value[:1] in (">", "|"):
            parts = []
            for follow in lines[index + 1:]:
                if follow.strip() and len(follow) - len(follow.lstrip()) <= indent:
                    break
                parts.append(follow.strip())
            value = " ".join(parts)
        return set(value.strip("\"'").split())
    return None


def check_tree(root):
    """The findings of every rule over the tree at @root."""
    problems = []
    record, found = read_record(os.path.join(root, RECORD))
    problems.extend(found)
    for need, capabilities in sorted(manifest_needs(root).items()):
        if need not in record:
            problems.append("%s: a row of %s needs it and %s has no line for it"
                            % (need, ", ".join(sorted(capabilities)), RECORD))
    with open(os.path.join(root, WORKFLOW), encoding="utf-8") as fh:
        jobs = split_jobs(fh.read())
    installed = {need for need, (_p, kind, _n) in record.items() if kind in INSTALLED_KINDS}
    for need in sorted(installed):
        package, _kind, note = record[need]
        if package == NO_PACKAGE:
            problems.append("%s: an installed tool needs a package in %s" % (need, RECORD))
    seen_in_a_job = set()
    for job, text in sorted(jobs.items()):
        expected = expected_tools(text)
        if expected is None:
            continue
        packages = apt_packages(text)
        wanted = {need for need in installed if record[need][0] in packages}
        seen_in_a_job |= wanted
        for need in sorted(expected - wanted):
            problems.append("job %s: %s is expected but %s is not installed there"
                            % (job, need, record.get(need, (NO_PACKAGE,))[0]))
        for need in sorted(wanted - expected):
            problems.append("job %s: %s installs %s and %s does not expect %s"
                            % (job, need, record[need][0], EXPECTED_VARIABLE, need))
        for need in sorted(wanted):
            note = record[need][2]
            if note.startswith(RUN_NOTE) or (" " + RUN_NOTE) in note:
                command = note.split(RUN_NOTE, 1)[1].strip()
                if command not in text:
                    problems.append("job %s: %s needs the command '%s'" % (job, need, command))
    for need in sorted(installed - seen_in_a_job):
        problems.append("%s: package %s is in no job's apt-get list that sets %s"
                        % (need, record[need][0], EXPECTED_VARIABLE))
    return problems


def mutate(root, relative, old, new):
    """Replace @old by @new in one file of the copy at @root; False when
    @old is not there (a canary that changes nothing is blind)."""
    path = os.path.join(root, relative)
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    if old not in text:
        return False
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text.replace(old, new, 1))
    return True


# One planted offence per rule: (name, file, text to change, its replacement,
# a phrase the findings must contain).
CANARIES = (
    ("package-missing", WORKFLOW, "            gdb delve scala", "            delve scala",
     "is expected but"),
    ("expected-drift", WORKFLOW, "XWPE_TEST_TOOLS_EXPECTED: >-\n        gdb dlv",
     "XWPE_TEST_TOOLS_EXPECTED: >-\n        dlv", "installs gdb and"),
    ("no-record-line", RECORD, "\ngdb|gdb|apt|", "\nxgdb|gdb|apt|", "has no line for it"),
    ("run-command-gone", WORKFLOW, "sudo locale-gen de_DE.UTF-8", "true",
     "needs the command"),
    ("unknown-kind", RECORD, "|apt|17.1", "|installed|17.1", "is none of"),
    ("no-package", RECORD, "gdb|gdb|apt|", "gdb|-|apt|", "needs a package"),
)


def selftest(root):
    """True when the copy of the tree passes and each canary is named."""
    ok = True
    with tempfile.TemporaryDirectory(prefix="citools.") as scratch:
        base = os.path.join(scratch, "base")
        os.makedirs(os.path.join(base, ".github"))
        os.makedirs(os.path.join(base, "tests"))
        shutil.copytree(os.path.join(root, ".github", "workflows"),
                        os.path.join(base, ".github", "workflows"))
        shutil.copy(os.path.join(root, RECORD), os.path.join(base, RECORD))
        shutil.copytree(os.path.join(root, "tests", "harness"),
                        os.path.join(base, "tests", "harness"),
                        ignore=shutil.ignore_patterns("out", "__pycache__", "*.py"))
        clean = check_tree(base)
        if clean:
            sys.stderr.write("check_ci_tools: the tree itself is refused:\n  %s\n"
                             % "\n  ".join(clean))
            ok = False
        for name, relative, old, new, phrase in CANARIES:
            copy = os.path.join(scratch, name)
            shutil.copytree(base, copy)
            if not mutate(copy, relative, old, new):
                sys.stderr.write("check_ci_tools: canary %s changed nothing\n" % name)
                ok = False
                continue
            found = check_tree(copy)
            if not any(phrase in line for line in found):
                sys.stderr.write("check_ci_tools: canary %s was not refused for '%s' (%s)\n"
                                 % (name, phrase, found[:3]))
                ok = False
    return ok


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--srcdir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.srcdir)
    if not os.path.isdir(os.path.join(root, ".github", "workflows")):
        print("check_ci_tools: no .github/workflows in %s (a source tarball); skipped" % root)
        return SKIPPED
    if not selftest(root):
        return 1
    if args.selftest:
        print("check_ci_tools: every canary is refused, the tree is not")
        return 0
    problems = check_tree(root)
    for problem in problems:
        sys.stderr.write("check_ci_tools: %s\n" % problem)
    if problems:
        return 1
    print("check_ci_tools: the record, the manifests and linux.yml agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
