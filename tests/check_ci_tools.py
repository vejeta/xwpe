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
      command (a job that installs nothing, as the gate, only reads the list);
  F1  no foreign cut: `timeout` and `gtimeout` are not the command of any
      line of a workflow or of tests/*.sh, nor the first word of an argument
      list in tests/**/*.py (tracked files only).  The cut program of an image
      differs from image to image, and one of them ended its grace at once;
      a job is cut by tests/harness/job_cut.py;
  F2  tests/ci-shard.sh runs its pytest through tests/harness/job_cut.py;
  F3  every workflow step that runs `-m pytest` is cut by job_cut.py or
      ci-shard.sh, or is named, with its reason, in tests/ci-uncut-steps.txt.

Not seen: whether a package carries the program the record names (that needs
the package index); the first run of the job on the image shows it.

  check_ci_tools.py [--srcdir DIR] [--selftest]

With no option the self-test runs (a planted offence per rule must be named,
the unmodified copy must pass), then the tree.

Exit: 0 clean, 1 findings or a wrong canary, 77 no .github (a source tarball).
"""
import argparse
import ast
import glob
import json
import os
import re
import shutil
import subprocess
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
UNCUT_RECORD = os.path.join("tests", "ci-uncut-steps.txt")
# The one program that cuts a job (rules F1 to F3).
JOB_CUT = "tests/harness/job_cut.py"
JOB_CUT_NAME = "job_cut.py"
# The cut programs an image ships, which no file of the tree may run.  Built
# from parts, so that this list is not itself an argument list that starts with
# one (rule F1 reads this file too).
IMAGE_CUT = "time" + "out"
FOREIGN_CUTS = (IMAGE_CUT, "g" + IMAGE_CUT)
# Words a command line may start with before the command itself: the shell's
# keywords that are followed by a command, and the programs that run the
# command they are given (a wrapper), whose own options and arguments come
# between it and that command.
SHELL_KEYWORDS = ("if", "then", "do", "else", "elif", "while", "until", "!", "{",
                  "time")
WRAPPERS = ("sudo", "exec", "env", "nohup", "nice", "xargs", "command", "stdbuf",
            "builtin", "setsid", "ionice", "chronic")
COMMAND_PREFIXES = SHELL_KEYWORDS + WRAPPERS
# The options of `command` that look a program up instead of running it.
LOOKUP_OPTIONS = ("-v", "-V")
# The interpreters a command may start with before the cut tool's own name.
INTERPRETER = re.compile(r"^python[0-9.]*$")
# What a Python argument list that wraps a command may start with.
SHELL_COMMAND_OPTION = "-c"
ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
COMMAND_SEPARATORS = re.compile(r"\|\||&&|\$\(|[;|&(`]")
YAML_LEADER = re.compile(r"^\s*(-\s+)?(run:\s*)?")
STEP_START = re.compile(r"^      - ")
STEP_NAME = re.compile(r"^(?:      - |        )name:\s*(.+?)\s*$")
JOB_START = re.compile(r"^  [A-Za-z0-9_-]+:\s*$")
PYTEST_RUN = re.compile(r"-m pytest\b")
# Folders of tests/ that are not this tree's own tests.
NOT_OURS = (os.path.join("tests", ".venv"), os.path.join("tests", "eval"),
            "__pycache__")
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
    problems.extend(foreign_cut_findings(root))
    problems.extend(uncut_step_findings(root))
    return problems


def logical_lines(text):
    """The lines of a shell text with each backslash continuation joined to
    its line, as (the number of its first line, its text)."""
    joined = []
    pending = None
    for number, line in enumerate(text.splitlines(), 1):
        if pending is None:
            pending = [number, ""]
        if line.rstrip().endswith("\\"):
            pending[1] += line.rstrip()[:-1] + " "
            continue
        pending[1] += line
        joined.append((pending[0], pending[1]))
        pending = None
    if pending is not None:
        joined.append((pending[0], pending[1]))
    return joined


def runs_through_job_cut(command):
    """True when the simple command @command is the cut tool running pytest:
    job_cut.py is the program it starts (behind an interpreter, a variable
    that names one, assignments and options only) and `-m pytest` comes after
    it.  A line that merely names job_cut.py (an `echo`, a comment, an
    argument of another program) does not cut the pytest it also runs."""
    found = PYTEST_RUN.search(command)
    if not found:
        return False
    before = command[:found.start()].split()
    names = [os.path.basename(word.strip("\"'")) for word in before]
    if JOB_CUT_NAME not in names:
        return False
    for word in before[:names.index(JOB_CUT_NAME)]:
        bare = word.strip("\"'")
        name = os.path.basename(bare)
        if not (name in COMMAND_PREFIXES or ASSIGNMENT.match(bare)
                or bare.startswith(("-", "$")) or INTERPRETER.match(name)):
            return False
    return True


def pytest_commands_are_cut(text):
    """True when @text (a script, or the lines of a workflow step) runs
    pytest and every command of it that does is run by the cut tool."""
    runs = pytest_command_cuts(text)
    return bool(runs) and all(runs)


def pytest_command_cuts(text):
    """One bool per command of @text that runs pytest: whether the cut tool
    runs it."""
    runs = []
    for _number, line in logical_lines(text):
        code = YAML_LEADER.sub("", line, count=1)
        if code.startswith("#"):
            continue
        for command in COMMAND_SEPARATORS.split(code.split(" #", 1)[0]):
            if PYTEST_RUN.search(command):
                runs.append(runs_through_job_cut(command))
    return runs


def shard_cut_findings(script):
    """F2: the pytest line of the shard script runs through job_cut.py, the
    cut of the tree, and through no other."""
    if pytest_commands_are_cut(script):
        return []
    return ["%s: a shard is cut by %s, which interrupts pytest and keeps its junit "
            "(a kill writes none), and by nothing else" % (SHARD_SCRIPT, JOB_CUT)]


def command_words(line):
    """The first word of every simple command of one shell or workflow line:
    after the separators (`;` `|` `&&` `$(` ...), a leading `run:` and the
    words that only prefix a command (assignments, sudo, env, options).
    A comment gives none."""
    text = YAML_LEADER.sub("", line, count=1)
    if text.startswith("#"):
        return []
    text = text.split(" #", 1)[0]
    found = []
    for part in COMMAND_SEPARATORS.split(text):
        found.extend(simple_command_words(part))
    return found


def simple_command_words(part):
    """The command word of one simple command, after the keywords of the
    shell (`if`, `then`, `do`, `!`, `time` ...), assignments and options.
    When the command is a wrapper (`env`, `nice -n 5`, `xargs`, `stdbuf -oL`,
    `command` ...), every word after it too: the command it runs is among
    them, behind the wrapper's own options and their values."""
    found = []
    wrapped = False
    asking = False
    for word in part.split():
        bare = word.strip("\"'")
        name = os.path.basename(bare)
        if name in WRAPPERS:
            wrapped = True
            asking = name == "command"
            continue
        if asking and bare in LOOKUP_OPTIONS:
            return found    # `command -v NAME` looks a program up, it runs none
        if bare in SHELL_KEYWORDS or ASSIGNMENT.match(bare) or bare.startswith("-"):
            continue
        if found and not wrapped:
            break
        found.append(name)
    return found


def tracked_files(root):
    """The files of the tree the foreign-cut rule reads, as paths relative to
    @root: what git tracks under .github/workflows and tests, else (a copy
    without git) what is there.  Never tests/.venv or tests/eval, and never a
    file git does not track: an old checkout kept in the working tree is not
    the tree."""
    listed = None
    try:
        top = subprocess.run(["git", "-C", root, "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
        if os.path.realpath(top) == os.path.realpath(root):
            listed = subprocess.run(
                ["git", "-C", root, "ls-files", "-z", "--", ".github/workflows", "tests"],
                capture_output=True, text=True, check=True).stdout.split("\0")
    except (OSError, subprocess.CalledProcessError):
        listed = None
    if listed is None:
        listed = []
        for folder, _dirs, names in os.walk(root):
            listed.extend(os.path.relpath(os.path.join(folder, n), root) for n in names)
    return sorted(path for path in listed if path and not any(
        part in path.split(os.sep) or path.startswith(part) for part in NOT_OURS))


def is_shell_text(path):
    """A workflow or a shell script of tests/ (not in a folder below it)."""
    folder, name = os.path.split(path)
    if folder == os.path.join(".github", "workflows"):
        return name.endswith((".yml", ".yaml"))
    return folder == "tests" and name.endswith(".sh")


SHELLS = ("sh", "bash", "dash", "zsh", "ksh")
SHELL_CALLS = ("system", "popen")


def string_of(node):
    """The text of a string literal or of the literal parts of an f-string,
    else None."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        return "".join(part.value for part in node.values
                       if isinstance(part, ast.Constant) and isinstance(part.value, str))
    return None


def shell_text_runs_a_cut(text):
    """True when a shell command line, as a string, runs a cut program."""
    return any(word in FOREIGN_CUTS for word in command_words(text))


def argv_runs_a_cut(node):
    """True for a list or tuple literal that is an argument vector of a cut
    program: it starts with one, or starts with a wrapper (`env`, `nice` ...)
    that has one among its words, or is a shell with `-c` and a command line
    that runs one."""
    if not isinstance(node, (ast.List, ast.Tuple)) or not node.elts:
        return False
    words = [part.value for part in node.elts
             if isinstance(part, ast.Constant) and isinstance(part.value, str)]
    first = node.elts[0]
    if not words or not isinstance(first, ast.Constant) or not isinstance(first.value, str):
        return False
    name = os.path.basename(first.value)
    if name in FOREIGN_CUTS:
        return True
    if name in WRAPPERS:
        return any(os.path.basename(word) in FOREIGN_CUTS for word in words[1:])
    if name in SHELLS and SHELL_COMMAND_OPTION in words:
        after = words.index(SHELL_COMMAND_OPTION) + 1
        return after < len(words) and shell_text_runs_a_cut(words[after])
    return False


def call_runs_a_cut(node):
    """True for a call that hands a cut program to the shell as text: a
    string literal split into words (`"timeout 5 x".split()`), the command
    of a call with `shell=True`, or of os.system and os.popen."""
    if not isinstance(node, ast.Call):
        return False
    if isinstance(node.func, ast.Attribute):
        text = string_of(node.func.value)
        if node.func.attr == "split" and text is not None:
            return shell_text_runs_a_cut(text)
        if node.func.attr in SHELL_CALLS and node.args:
            text = string_of(node.args[0])
            return text is not None and shell_text_runs_a_cut(text)
    through_shell = any(keyword.arg == "shell" and isinstance(keyword.value, ast.Constant)
                        and keyword.value.value is True for keyword in node.keywords)
    if through_shell and node.args:
        text = string_of(node.args[0])
        return text is not None and shell_text_runs_a_cut(text)
    return False


def foreign_cuts_in_shell(root, path):
    """F1 over one workflow or shell script: the lines whose command is a
    cut program of the image."""
    findings = []
    with open(os.path.join(root, path), encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for number, line in logical_lines(text):
        if any(word in FOREIGN_CUTS for word in command_words(line)):
            findings.append("%s:%d: a foreign cut: `timeout` is the image's program and "
                            "its grace differs from image to image; cut with %s"
                            % (path, number, JOB_CUT))
    return findings


def foreign_cuts_in_python(root, path):
    """F1 over one Python file: an argument vector of a cut program of the
    image (it starts with one, or with a wrapper that runs one, or is a shell
    `-c` of one), and a string that is run as a shell command or split into
    words and runs one.  A string, a comment and a name that is only held, as
    a table of canaries does, are not one."""
    with open(os.path.join(root, path), encoding="utf-8", errors="replace") as fh:
        try:
            tree = ast.parse(fh.read())
        except SyntaxError:
            return []
    return ["%s:%d: a foreign cut: a command that runs `timeout`; cut with %s"
            % (path, node.lineno, JOB_CUT)
            for node in ast.walk(tree)
            if argv_runs_a_cut(node) or call_runs_a_cut(node)]


def foreign_cut_findings(root):
    """F1 over the tracked files of the tree."""
    findings = []
    for path in tracked_files(root):
        if is_shell_text(path):
            findings.extend(foreign_cuts_in_shell(root, path))
        elif path.endswith(".py") and path.startswith("tests" + os.sep):
            findings.extend(foreign_cuts_in_python(root, path))
    return findings


def workflow_steps(text):
    """[(name, the step's lines)] of a workflow: a step starts at a `- ` of the
    step list and ends at the next one or at the next job."""
    steps = []
    for line in text.splitlines():
        if JOB_START.match(line):
            steps.append(None)
        elif STEP_START.match(line):
            steps.append(["", [line]])
        elif steps and steps[-1] is not None:
            steps[-1][1].append(line)
    found = []
    for step in steps:
        if step is None:
            continue
        for line in step[1]:
            named = STEP_NAME.match(line)
            if named:
                step[0] = named.group(1)
                break
        found.append((step[0], step[1]))
    return found


def read_uncut(root):
    """({(workflow, step): reason}, [problem]) from tests/ci-uncut-steps.txt."""
    listed, problems = {}, []
    path = os.path.join(root, UNCUT_RECORD)
    if not os.path.exists(path):
        return listed, ["%s: missing: it names the steps that run pytest with no cut" % UNCUT_RECORD]
    with open(path, encoding="utf-8") as fh:
        for number, raw in enumerate(fh, 1):
            raw = raw.rstrip("\n")
            if not raw.strip() or raw.startswith("#"):
                continue
            fields = raw.split("|")
            if len(fields) != 3 or not fields[2].strip():
                problems.append("%s:%d: a line is `workflow|step name|reason`, the reason "
                                "is not optional" % (UNCUT_RECORD, number))
                continue
            listed[(fields[0], fields[1])] = fields[2]
    return listed, problems


def uncut_step_findings(root):
    """F3: every step of every workflow that runs pytest is cut or listed
    with its reason; a listed step that is not in its workflow is stale."""
    listed, findings = read_uncut(root)
    seen = set()
    for path in sorted(glob.glob(os.path.join(root, ".github", "workflows", "*.yml"))):
        workflow = os.path.basename(path)
        with open(path, encoding="utf-8") as fh:
            steps = workflow_steps(fh.read())
        for name, lines in steps:
            seen.add((workflow, name))
            code = "\n".join(line for line in lines if not line.lstrip().startswith("#"))
            runs_pytest = PYTEST_RUN.search(code) is not None
            runs = pytest_command_cuts(code)
            if runs:
                cut = all(runs)
            else:
                cut = os.path.basename(SHARD_SCRIPT) in code
            if runs_pytest and not cut and (workflow, name) not in listed:
                findings.append("%s: step '%s' runs pytest with no cut: run it through %s "
                                "or tests/ci-shard.sh, or name it in %s with its reason"
                                % (workflow, name, JOB_CUT, UNCUT_RECORD))
    for workflow, name in sorted(set(listed) - seen):
        findings.append("%s: lists step '%s' of %s, which has no such step"
                        % (UNCUT_RECORD, name, workflow))
    return findings


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
    findings.extend(shard_cut_findings(script))
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
    ("cut-tool-removed", SHARD_SCRIPT,
     '"$PY" tests/harness/job_cut.py --after "$CUT" --grace 120 \\\n'
     '  --report "$OUT/shard-$SHARD-cut.json" -- \\\n  "$PY" -m pytest -q',
     '"$PY" -m pytest -q', "is cut by tests/harness/job_cut.py"),
    ("foreign-cut-in-workflow", WORKFLOW,
     "python3 tests/harness/job_cut.py --after 25m --grace 120 -- \\\n"
     "            /tmp/asanenv/bin/python -m pytest -q",
     "timeout -s INT -k 120 25m \\\n            /tmp/asanenv/bin/python -m pytest -q",
     "a foreign cut"),
    ("foreign-cut-long-options", WORKFLOW,
     "python3 tests/harness/job_cut.py --after 25m --grace 120 -- \\\n"
     "            /tmp/asanenv/bin/python -m pytest -q",
     "timeout --signal=INT --kill-after=120 25m \\\n            /tmp/asanenv/bin/python -m pytest -q",
     "a foreign cut"),
    ("foreign-cut-no-option", SHARD_SCRIPT,
     "START=$(date -u +%Y-%m-%dT%H:%M:%SZ)\n",
     "START=$(date -u +%Y-%m-%dT%H:%M:%SZ)\ntimeout 5 true\n", "a foreign cut"),
    ("foreign-cut-in-python", "tests/test_row_clock.py",
     "wrapper=[sys.executable, JOB_CUT,", 'wrapper=["timeout", JOB_CUT,', "a foreign cut"),
    ("foreign-cut-env-in-python", "tests/test_row_clock.py",
     "wrapper=[sys.executable, JOB_CUT,", 'wrapper=["env", "timeout", JOB_CUT,',
     "a foreign cut"),
    ("foreign-cut-split-in-python", "tests/test_row_clock.py",
     "wrapper=[sys.executable, JOB_CUT,",
     'wrapper="timeout -s INT 5".split() + [JOB_CUT,', "a foreign cut"),
    ("foreign-cut-shell-true-in-python", "tests/test_row_clock.py",
     'HANG_MARK = "hang"',
     'HANG_MARK = "hang"\nsubprocess.run("timeout 5 true", shell=True)',
     "a foreign cut"),
    ("foreign-cut-sh-c-in-python", "tests/test_row_clock.py",
     "wrapper=[sys.executable, JOB_CUT,", 'wrapper=["sh", "-c", "timeout 5 true", JOB_CUT,',
     "a foreign cut"),
    ("job-cut-only-named", WORKFLOW,
     "python3 tests/harness/job_cut.py --after 25m --grace 120 -- \\\n"
     "            /tmp/asanenv/bin/python -m pytest -q",
     "echo tests/harness/job_cut.py; /tmp/asanenv/bin/python -m pytest -q",
     "runs pytest with no cut"),
    ("shard-job-cut-only-named", SHARD_SCRIPT,
     '"$PY" tests/harness/job_cut.py --after "$CUT" --grace 120 \\\n'
     '  --report "$OUT/shard-$SHARD-cut.json" -- \\\n  "$PY" -m pytest -q',
     'echo tests/harness/job_cut.py; "$PY" -m pytest -q',
     "is cut by tests/harness/job_cut.py"),
    ("uncut-step", WORKFLOW, "run: sh tests/ci-shard.sh ${{ matrix.shard }}",
     "run: /tmp/ptenv/bin/python -m pytest -q tests", "runs pytest with no cut"),
    ("uncut-list-stale", UNCUT_RECORD, "linux.yml|X11 resize and uncover",
     "linux.yml|X11 resise and uncover", "has no such step"),
    ("uncut-reason-missing", UNCUT_RECORD,
     "|three files of the core-font build, well inside their step limit", "|",
     "the reason is not optional"),
    ("cut-row-rerun", SHARD_SCRIPT, "--rerun-except RowOverBudget", "--reruns-all",
     "--rerun-except RowOverBudget"),
    ("cut-status-masked", SHARD_SCRIPT, "|| rc=$?", "|| true", "hide a cut"),
    ("gate-skipped", WORKFLOW, "    needs: [build, pyte, gui]\n", "    needs: [build, pyte]\n",
     "job gate:"),
    ("unknown-kind", RECORD, "|apt|17.1", "|installed|17.1", "is none of"),
    ("no-package", RECORD, "gdb|gdb|apt|", "gdb|-|apt|", "needs a package"),
)


# The words a shell command may have in front of its program: a canary for
# each, planted in a shell script as the line the form makes (rule F1).
SHELL_HOLES = (
    "if timeout 5 true; then true; fi",
    "if true; then timeout 5 true; fi",
    "while true; do timeout 5 true; done",
    "! timeout 5 true",
    "time timeout 5 true",
    "echo x | xargs timeout 5",
    "echo x | xargs -n1 timeout 5",
    "command timeout 5 true",
    "stdbuf -oL timeout 5 true",
    "env -i FOO=1 timeout 5 true",
    "nice -n 5 timeout 5 true",
    "nohup timeout 5 true",
)
CANARIES = CANARIES + tuple(
    ("foreign-cut-form-%d" % number, SHARD_SCRIPT,
     "START=$(date -u +%Y-%m-%dT%H:%M:%SZ)\n",
     "START=$(date -u +%Y-%m-%dT%H:%M:%SZ)\n" + line + "\n", "a foreign cut")
    for number, line in enumerate(SHELL_HOLES, 1))


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
        shutil.copy(os.path.join(root, UNCUT_RECORD), os.path.join(base, UNCUT_RECORD))
        # One Python file of tests/, so that rule F1 has an argument list to read.
        shutil.copy(os.path.join(root, "tests", "test_row_clock.py"),
                    os.path.join(base, "tests", "test_row_clock.py"))
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
