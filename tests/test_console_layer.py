"""The console layer asks the tool farm before it runs (tests/host_tools.py,
layer_unmet).

The layer boots a micro-VM with qemu.  On a machine that has qemu and KVM the
layer used to run although the tool farm did not give the rows qemu: every
row of the layer errored at setup, and the session ended with a stray engine
hit.  One function now decides for the three layer gates (x11, wayland,
console): a tool is usable when the machine has it, the farm gives it and it
runs.  The rows run the real layer under a child pytest on a machine made to
look like the bad one (a stand-in qemu, a kernel and a KVM device that are
plain files), so they do not depend on the host having the real ones.
"""
import os
import re
import stat
import sys

import pytest

import engine_guard
import gui_owner
import host_tools

HERE = os.path.dirname(os.path.abspath(__file__))
LAYER_FILE = os.path.join(HERE, "console", "test_console_tty_predicate.py")
INNER_TIMEOUT_S = 150
QEMU = "qemu-system-x86_64"
QEMU_STAND_IN = "#!/bin/sh\nexit 0\n"
FARM_REASON = host_tools.FARM_REFUSES


def make_program(folder, name):
    """An executable file called @name in @folder; returns the folder."""
    folder.mkdir(exist_ok=True)
    path = folder / name
    path.write_text(QEMU_STAND_IN)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return folder


def layer_run(tmp_path, unskip=False):
    """Run the console layer under a child pytest whose farm has no qemu, on a
    machine that has a stand-in qemu, a kernel file and a KVM device file.
    Returns (exit status, text)."""
    bins = make_program(tmp_path / "bin", QEMU)
    (tmp_path / "kernel").write_text("not a kernel")
    (tmp_path / "kvm").write_text("")
    home = tmp_path / "home"
    home.mkdir()
    env = {"HOME": str(home), "TERM": "xterm",
           "PATH": "%s:%s" % (bins, os.environ["PATH"]),
           "PYTHONPATH": HERE,
           "XWPE_TEST_KERNEL": str(tmp_path / "kernel"),
           "XWPE_TEST_KVM_DEVICE": str(tmp_path / "kvm")}
    if unskip:
        env["XWPE_INNER_UNSKIP"] = "1"
    argv = [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
            "-p", "inner_console_layer", "-rsE", "--basetemp", str(tmp_path / "inner"),
            LAYER_FILE]
    done = gui_owner.run_nested(argv, INNER_TIMEOUT_S, cwd=str(tmp_path), extra_env=env)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def assert_skipped_with_the_farm_reason(status, text):
    """No row errored, the session did not end with a stray engine hit, and
    the layer skipped saying that the farm does not provide qemu."""
    shown = text[-2500:]
    assert "EngineReached" not in text, shown
    assert not re.search(r"\b\d+ errors?\b", text), shown
    assert status == 0, shown
    assert "skipped" in text.splitlines()[-1], shown
    assert "%s: %s" % (QEMU, FARM_REASON) in text, shown


# @scenario SC-platform-console-layer-01
def test_the_layer_skips_when_the_farm_does_not_provide_qemu_and_errors_nothing(tmp_path):
    status, text = layer_run(tmp_path)
    assert_skipped_with_the_farm_reason(status, text)


def test_the_fixture_that_boots_the_vm_asks_the_farm_first(tmp_path):
    """With the layer's skip marks taken off, the rows reach `vt_cases`; it
    must skip itself before it builds or boots anything."""
    status, text = layer_run(tmp_path, unskip=True)
    assert_skipped_with_the_farm_reason(status, text)


def test_qemu_is_in_the_baseline_of_the_farm_with_its_reason():
    assert engine_guard.is_baseline_tool(QEMU)
    with open(os.path.join(HERE, "engine_guard.py"), encoding="utf-8") as fh:
        text = fh.read()
    assert "boots a micro-VM" in text and '"%s"' % QEMU in text


def test_a_tool_on_the_path_that_the_farm_does_not_give_is_not_usable_by_a_layer(
        tmp_path, monkeypatch):
    bins = make_program(tmp_path / "bin", "not-in-the-baseline-tool")
    monkeypatch.setenv("PATH", "%s:%s" % (bins, os.environ["PATH"]))
    unmet = host_tools.layer_unmet("a layer", ["not-in-the-baseline-tool", "sh"])
    assert len(unmet) == 1, unmet
    assert "not-in-the-baseline-tool: %s" % FARM_REASON in unmet[0], unmet


def test_a_tool_a_job_declared_to_the_farm_is_usable_by_a_layer(tmp_path, monkeypatch):
    bins = make_program(tmp_path / "bin", "declared-tool")
    monkeypatch.setenv("PATH", "%s:%s" % (bins, os.environ["PATH"]))
    monkeypatch.setenv(engine_guard.TOOLS_VAR, "declared-tool")
    assert host_tools.layer_unmet("a layer", ["declared-tool"]) == []


def test_a_missing_tool_reads_as_missing_apart_from_a_denied_one():
    unmet = host_tools.layer_unmet("a layer", ["no-such-program-at-all"])
    assert unmet and "no no-such-program-at-all on this host" in unmet[0], unmet


def test_one_usable_alternative_is_enough_for_a_layer(tmp_path, monkeypatch):
    unmet = host_tools.layer_unmet("a layer", ["no-such-one|sh"])
    assert unmet == []
    unmet = host_tools.layer_unmet("a layer", ["no-such-one|no-such-two"])
    assert unmet and "no-such-one" in unmet[0] and "no-such-two" in unmet[0], unmet


def test_the_first_usable_tool_is_the_pick_of_a_program():
    assert host_tools.first_usable(("no-such-one", "sh")) == "sh"
    assert host_tools.first_usable(("no-such-one",)) is None


def test_a_layer_tool_an_installing_job_expects_stops_the_session_for_a_farm_refusal(monkeypatch):
    """A job that installed the tool and has it refused by the farm has a
    broken image: the layer must stop the session, not skip."""
    monkeypatch.setenv(host_tools.EXPECTED_VARIABLE, QEMU)
    entry = "%s: %s" % (QEMU, FARM_REASON)
    assert host_tools.expected_among([entry]) == [entry]
    with pytest.raises(pytest.exit.Exception):
        host_tools.fail_the_session_on_expected("console layer", [entry])
