#!/usr/bin/env python3
"""check_ci_tools.py -- the Linux job installs what the rows need, and says so.

One record, tests/ci-tools.txt, maps each host tool a row needs to the Ubuntu
package that provides it and to what kind of tool it is (apt, layer, built,
base, image, absent:R, condition).  Three things must agree with it, offline:

  C1  every tool a manifest row of tests/harness declares in "needs" has a
      line in the record;
  C2  every apt and layer line names a package, and that package is in the
      apt-get list of each job of .github/workflows/linux.yml that expects
      the tool; a built line names no package but a command, and each such
      job runs it.  A note that starts with "run: " names a command the job
      also runs (a package alone does not make a locale);
  C3  the XWPE_TEST_TOOLS_EXPECTED of each job (its own, else the workflow's)
      is exactly the apt, layer and built tools that job installs.
      The harness reads a declared tool of this list that skips as a failure
      of the image; a display layer gate stops the session on a missing one;
  C4  the build job, every pyte shard and the GUI job install through the one
      script tests/ci-install.sh, which holds the apt-get list and the locale
      command (a job that installs nothing, as the gate, only reads the list).

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

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "harness"))
try:
    import suite_plan
finally:
    sys.path.pop(0)

RECORD = os.path.join("tests", "ci-tools.txt")
WORKFLOW = os.path.join(".github", "workflows", "linux.yml")
INSTALL_SCRIPT = os.path.join("tests", "ci-install.sh")
SHARD_SCRIPT = os.path.join("tests", "ci-shard.sh")
# The jobs that run the suite and so install through the script.
SCRIPT_JOBS = ("build", "pyte", "gui")
MANIFESTS = os.path.join("tests", "harness", "*", "manifest.json")
EXPECTED_VARIABLE = "XWPE_TEST_TOOLS_EXPECTED"
KINDS = ("apt", "layer", "built", "base", "image", "absent", "condition")
# The kinds the job installs itself: a row that skips for one of them is a
# failure of the image, and the job's XWPE_TEST_TOOLS_EXPECTED names it.
INSTALLED_KINDS = ("apt", "layer", "built")
# The installed kinds that come from a package of the job's apt-get list; the
# other one is built by the command its note names.
PACKAGE_KINDS = ("apt", "layer")
BUILT_KIND = "built"
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


def run_command(note):
    """The command a record note names after RUN_NOTE, or None when it names
    none."""
    if not (note.startswith(RUN_NOTE) or (" " + RUN_NOTE) in note):
        return None
    return note.split(RUN_NOTE, 1)[1].strip()


def installs_here(entry, packages, text):
    """Does one job install this record entry: the package of an apt or layer
    tool in the job's apt-get list, the command a built tool's note names in
    the job's own text (its script included)?"""
    package, kind, note = entry
    if kind in PACKAGE_KINDS:
        return package in packages
    command = run_command(note)
    return command is not None and command in text


def what_installs(entry):
    """What installs this record entry, for a finding: its package, or the
    command that builds it."""
    package, kind, note = entry
    if kind == BUILT_KIND:
        return run_command(note) or package
    return package


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
        workflow = fh.read()
    with open(os.path.join(root, INSTALL_SCRIPT), encoding="utf-8") as fh:
        script = fh.read()
    jobs = split_jobs(workflow)
    top_expected = expected_tools(workflow.split("\njobs:", 1)[0])
    installed = {need for need, (_p, kind, _n) in record.items() if kind in INSTALLED_KINDS}
    for need in sorted(installed):
        package, kind, note = record[need]
        if kind in PACKAGE_KINDS and package == NO_PACKAGE:
            problems.append("%s: an installed tool needs a package in %s" % (need, RECORD))
        if kind == BUILT_KIND and RUN_NOTE not in note:
            problems.append("%s: a tool the job builds needs the command that "
                            "builds it ('%s') in %s" % (need, RUN_NOTE, RECORD))
    seen_in_a_job = set()
    for job in SCRIPT_JOBS:
        if INSTALL_SCRIPT not in jobs.get(job, ""):
            problems.append("job %s: does not run %s, the one list of packages"
                            % (job, INSTALL_SCRIPT))
    for job, text in sorted(jobs.items()):
        expected = expected_tools(text)
        if expected is None:
            expected = top_expected
        if expected is None:
            continue
        uses_script = INSTALL_SCRIPT in text
        packages = apt_packages(text) | (apt_packages(script) if uses_script else set())
        if not packages:
            continue    # a job that installs nothing (the gate) only reads the list
        if uses_script:
            text = text + "\n" + script
        wanted = {need for need in installed
                  if installs_here(record[need], packages, text)}
        seen_in_a_job |= wanted
        for need in sorted(expected - wanted):
            entry = record.get(need)
            problems.append("job %s: %s is expected but %s is not installed there"
                            % (job, need,
                               what_installs(entry) if entry else NO_PACKAGE))
        for need in sorted(wanted - expected):
            problems.append("job %s: %s installs %s and %s does not expect %s"
                            % (job, need, what_installs(record[need]),
                               EXPECTED_VARIABLE, need))
        for need in sorted(wanted):
            command = run_command(record[need][2])
            if command and command not in text:
                problems.append("job %s: %s needs the command '%s'" % (job, need, command))
    for need in sorted(installed - seen_in_a_job):
        if record[need][1] == BUILT_KIND:
            problems.append("%s: no job that sets %s runs the command that "
                            "builds it" % (need, EXPECTED_VARIABLE))
        else:
            problems.append("%s: package %s is in no job's apt-get list that "
                            "sets %s" % (need, record[need][0], EXPECTED_VARIABLE))
    problems.extend(shard_findings(root, jobs))
    return problems


def first_number(pattern, text):
    """The integer of the first line of @text that matches @pattern, or None."""
    found = re.search(pattern, text, re.M)
    return int(found.group(1)) if found else None


def shard_findings(root, jobs):
    """The findings of the shard rules (C5) over the workflow's jobs and the
    shard script: the cut inside the step's limit inside the job's inside
    GitHub's, no masked exit status, no rerun of a cut row, a gate that
    always runs after every job it judges."""
    findings = []
    pyte = jobs.get("pyte", "")
    job_limit = first_number(r"^    timeout-minutes:\s*(\d+)", pyte)
    step_limit = first_number(r"^        timeout-minutes:\s*(\d+)", pyte)
    cut = suite_plan.SHARD_CUT_MIN
    if job_limit is None or step_limit is None:
        findings.append("job pyte: the job and its shard step each need timeout-minutes")
    elif not cut < step_limit < job_limit < suite_plan.GITHUB_JOB_LIMIT_MIN:
        findings.append("job pyte: the cut (%d), the step (%d), the job (%d) and GitHub's "
                        "limit (%d) minutes must rise in that order"
                        % (cut, step_limit, job_limit, suite_plan.GITHUB_JOB_LIMIT_MIN))
    elif (step_limit, job_limit) != (suite_plan.STEP_LIMIT_MIN, suite_plan.JOB_LIMIT_MIN):
        findings.append("job pyte: the limits %d and %d are not the %d and %d of "
                        "tests/harness/suite_plan.py" % (
                            step_limit, job_limit, suite_plan.STEP_LIMIT_MIN,
                            suite_plan.JOB_LIMIT_MIN))
    with open(os.path.join(root, SHARD_SCRIPT), encoding="utf-8") as fh:
        script = fh.read()
    if "timeout -s INT" not in script:
        findings.append("%s: a shard is cut with `timeout -s INT`, never a kill (a kill "
                        "writes no junit)" % SHARD_SCRIPT)
    if "--rerun-except RowOverBudget" not in script:
        findings.append("%s: a row the clock cut must not be rerun: pass "
                        "--rerun-except RowOverBudget" % SHARD_SCRIPT)
    if "|| true" in script:
        findings.append("%s: `|| true` would hide a cut (exit 124) from the status file"
                        % SHARD_SCRIPT)
    gate = jobs.get("gate", "")
    needs = re.search(r"^    needs:\s*\[([^\]]*)\]", gate, re.M)
    listed = {n.strip() for n in needs.group(1).split(",")} if needs else set()
    if not {"build", "pyte", "gui"} <= listed or "if: always()" not in gate:
        findings.append("job gate: it needs build, pyte and gui and runs `if: always()`, so "
                        "a cut or failed job is judged and not skipped")
    return findings


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
    ("package-missing", INSTALL_SCRIPT, "  gdb scala", "  scala",
     "is expected but"),
    ("expected-drift", WORKFLOW, "XWPE_TEST_TOOLS_EXPECTED: >-\n    gdb dlv",
     "XWPE_TEST_TOOLS_EXPECTED: >-\n    dlv", "installs gdb and"),
    ("no-record-line", RECORD, "\ngdb|gdb|apt|", "\nxgdb|gdb|apt|", "has no line for it"),
    ("run-command-gone", INSTALL_SCRIPT, "sudo locale-gen de_DE.UTF-8", "true",
     "needs the command"),
    ("built-command-gone", INSTALL_SCRIPT,
     "go install github.com/go-delve/delve/cmd/dlv@v1.27.1",
     "true", "is expected but go install"),
    ("built-without-its-command", RECORD,
     "run: go install github.com/go-delve/delve/cmd/dlv@v1.27.1", "built by hand",
     "builds needs the command"),
    ("script-not-run", WORKFLOW,
     "        run: sh tests/ci-install.sh\n\n      - name: Set up the pytest venv\n"
     "        run: sh tests/ci-venv.sh\n\n      - name: Run the shard",
     "        run: true\n\n      - name: Set up the pytest venv\n"
     "        run: sh tests/ci-venv.sh\n\n      - name: Run the shard",
     "job pyte: does not run"),
    ("step-limit-under-the-cut", WORKFLOW, "        timeout-minutes: 110\n",
     "        timeout-minutes: 90\n", "must rise in that order"),
    ("job-limit-over-github", WORKFLOW, "    timeout-minutes: 130\n",
     "    timeout-minutes: 400\n", "must rise in that order"),
    ("shard-killed-not-interrupted", SHARD_SCRIPT, "timeout -s INT", "timeout -s KILL",
     "with `timeout -s INT`"),
    ("cut-row-rerun", SHARD_SCRIPT, "--rerun-except RowOverBudget", "--reruns-all",
     "--rerun-except RowOverBudget"),
    ("cut-status-masked", SHARD_SCRIPT, "|| rc=$?", "|| true", "hide a cut"),
    ("gate-skipped", WORKFLOW, "    needs: [build, pyte, gui]\n", "    needs: [build, pyte]\n",
     "job gate:"),
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
        shutil.copy(os.path.join(root, INSTALL_SCRIPT), os.path.join(base, INSTALL_SCRIPT))
        shutil.copy(os.path.join(root, SHARD_SCRIPT), os.path.join(base, SHARD_SCRIPT))
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
