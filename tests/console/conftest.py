"""Fixtures for the console (real Linux VT) test layer.

pyte can only emulate a terminal emulator, and the x11 layer can only drive a
window system: neither can put the editor on a bare virtual console, which is
where the GPM mouse lives.  This layer boots a micro-VM with KVM -- the host's
own kernel and an initramfs holding one static probe -- and reads its serial
port.  A boot takes well under a second, so it is cheap enough to run per
commit rather than by hand.

The probe runs the editor's OWN console predicate, extracted verbatim from
we_gpm.c at build time, so the layer tests the shipped decision and cannot
drift from it.  Extending the layer to run the editor itself inside the VM (a
rootfs with the binary, gpm, and mouse events injected through qemu's monitor)
would reuse this boot machinery; what is here today covers the classification,
which is the part an ssh session can never observe.

Skipped with a precise reason when qemu, gcc, cpio, a readable kernel image or
/dev/kvm is missing, so a machine without virtualisation reports "skipped"
rather than "passed".
"""

import glob
import os
import re
import shutil
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.normpath(os.path.join(HERE, "..", ".."))
# The owner of every process a test starts lives one folder up.
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

import gui_owner  # noqa: E402
import host_tools  # noqa: E402
SRC = os.path.join(TOP, "we_gpm.c")
# Both halves of the decision, in dependency order: the pure per-descriptor
# test, then the chooser that decides which descriptor to ask about.
PREDICATES = ["WpeGpmTtyIsConsole", "WpeGpmIsConsoleTty"]
BOOT_TIMEOUT = 180
COMPILE_TIMEOUT = 300
FIND_TIMEOUT = 60
CPIO_TIMEOUT = 120

REQUIRED = ["qemu-system-x86_64", "gcc", "cpio"]
# The device the VM is given to run on.  A row of tests/test_console_layer.py
# names another file to run the layer without the machine's own.
KVM_DEVICE = os.environ.get("XWPE_TEST_KVM_DEVICE", "/dev/kvm")
# The tools the layer cannot use, each with its reason (the machine has none,
# the tool farm does not give it, it does not run).
_MISSING = host_tools.layer_unmet("console layer", REQUIRED)


def _kernel_image():
    """The kernel to boot: XWPE_TEST_KERNEL, else the newest readable one."""
    env = os.environ.get("XWPE_TEST_KERNEL")
    if env:
        return env if os.access(env, os.R_OK) else None
    for path in sorted(glob.glob("/boot/vmlinuz-*"), reverse=True):
        if os.access(path, os.R_OK):
            return path
    return None


def _kvm_usable():
    return os.access(KVM_DEVICE, os.R_OK | os.W_OK)


_KERNEL = _kernel_image()
if _KERNEL is None:
    _MISSING.append("readable kernel image (/boot/vmlinuz-*, or XWPE_TEST_KERNEL)")
if not _kvm_usable():
    _MISSING.append("%s (usable by this user)" % KVM_DEVICE)
if not os.access(SRC, os.R_OK):
    _MISSING.append("we_gpm.c (is this a build with GPM?)")


def layer_skip_reason():
    """Why the layer cannot run on this host, or None.

    A tool the job installed that is not usable stops the session (a skip
    would hide that the whole layer never ran)."""
    if not _MISSING:
        return None
    host_tools.fail_the_session_on_expected("console layer", _MISSING)
    return "console layer unavailable, missing: " + ", ".join(_MISSING)


def skip_unless_the_layer_can_run():
    """Skip the running row when the layer cannot run.

    The collection hook below marks every row of the layer; this is the same
    answer asked again by the fixture that boots the VM, so that no way of
    reaching the fixture (a marker taken off, a row run by node id) starts
    qemu where the tool farm denies it: that errors the rows at setup and
    ends the session with a stray engine hit."""
    reason = layer_skip_reason()
    if reason:
        pytest.skip(reason)


def pytest_collection_modifyitems(config, items):
    """A conftest-level pytestmark does not reach sibling modules, so skip the
    whole layer from the collection hook instead (same reason as tests/x11)."""
    reason = layer_skip_reason()
    if reason is None:
        return
    for item in items:
        if os.path.dirname(str(item.fspath)) == HERE:
            item.add_marker(pytest.mark.skip(reason=reason))


def _slice_function(text, name):
    """One static function's text, from its signature to its closing brace."""
    for sig in ("static int %s(int fd)" % name, "static int %s(void)" % name):
        start = text.find(sig)
        if start >= 0:
            return text[start:text.index("\n}\n", start) + 3]
    raise ValueError(
        "no static function %s in %s: the console layer would otherwise compile "
        "an empty predicate and quietly test nothing" % (name, SRC))


def extract_predicate():
    """The shipped console test, verbatim, both functions, dependency order.

    Fails loudly when either is not there: a renamed or removed function must
    break this layer, not quietly leave it testing nothing.
    """
    with open(SRC) as fh:
        text = fh.read()
    joined = "\n\n".join(_slice_function(text, n) for n in PREDICATES)
    assert "KDGETMODE" in joined and "/dev/pts/" in joined, (
        "the extracted console test does not look like the real one: %r"
        % joined[:400])
    return joined


def _build(workdir):
    """Compile the probe with the extracted predicate and pack an initramfs."""
    os.makedirs(workdir, exist_ok=True)
    with open(os.path.join(workdir, "pred.inc"), "w") as fh:
        fh.write(extract_predicate())
    probe = os.path.join(workdir, "vtprobe")
    cc = gui_owner.run_tool(
        ["gcc", "-static", "-O2", "-Wall", "-Wno-unused-result",
         "-I", workdir, "-o", probe, os.path.join(HERE, "vtprobe.c")],
        COMPILE_TIMEOUT)
    assert cc.returncode == 0, "the console probe did not build:\n%s" % (
        (cc.stdout + cc.stderr).decode())

    root = os.path.join(workdir, "root")
    os.makedirs(os.path.join(root, "dev", "pts"), exist_ok=True)
    os.makedirs(os.path.join(root, "proc"), exist_ok=True)
    shutil.copyfile(probe, os.path.join(root, "init"))
    os.chmod(os.path.join(root, "init"), 0o755)
    initrd = os.path.join(workdir, "initrd.cpio")
    # Two bounded runs, the names then the archive, not a pipe between two
    # processes: each is on the owner's ledger and has its own deadline.
    names = gui_owner.run_tool(["find", "."], FIND_TIMEOUT, cwd=root).stdout
    packed = gui_owner.run_tool(["cpio", "-o", "-H", "newc", "--quiet"],
                                CPIO_TIMEOUT, cwd=root, input=names)
    with open(initrd, "wb") as out:
        out.write(packed.stdout)
    return initrd


def _boot(workdir, initrd):
    """Boot the micro-VM once and return its serial output."""
    serial = os.path.join(workdir, "serial.txt")
    cmd = ["qemu-system-x86_64", "-m", "512",
           "-kernel", _KERNEL, "-initrd", initrd,
           "-append", "console=ttyS0 init=/init quiet panic=1",
           "-display", "none", "-monitor", "none",
           "-serial", "file:%s" % serial, "-no-reboot"]
    if _kvm_usable():
        cmd[1:1] = ["-enable-kvm", "-cpu", "host"]
    run = gui_owner.run_tool(cmd, BOOT_TIMEOUT)
    said = (run.stdout + run.stderr).decode(errors="replace")
    text = ""
    if os.path.exists(serial):
        with open(serial, errors="replace") as fh:
            text = fh.read()
    assert "VTPROBE DONE" in text, (
        "the console probe did not finish (qemu exit %d). Serial output:\n%s\n"
        "qemu said:\n%s" % (run.returncode, text[-3000:], said[-2000:]))
    return text


def parse_cases(text):
    """Serial output -> {label: {key: value}}, one entry per CASE line."""
    cases = {}
    for line in text.splitlines():
        if not line.startswith("CASE "):
            continue
        fields = dict(re.findall(r"(\w+)=(\S+)", line[len("CASE "):]))
        label = fields.pop("label", None)
        if label:
            cases[label] = fields
    return cases


@pytest.fixture(scope="session")
def vt_cases(tmp_path_factory):
    """The predicate's answers on a real VT, a pty, a serial line and a non-tty."""
    skip_unless_the_layer_can_run()
    workdir = str(tmp_path_factory.mktemp("console-vm"))
    initrd = _build(workdir)
    return parse_cases(_boot(workdir, initrd))


@pytest.fixture(scope="session")
def predicate_source():
    """The text of the shipped console test that the VM ran."""
    return extract_predicate()


@pytest.fixture(scope="session")
def gpm_source():
    """The whole shipped GPM backend, for claims about the file rather than
    about one function inside it."""
    with open(SRC) as fh:
        return fh.read()
