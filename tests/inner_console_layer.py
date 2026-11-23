"""A plugin of the child pytest that tests/test_console_layer.py runs over the
console layer (tests/console).

It makes the tool farm of the child run without qemu, as on a job whose farm
was never given it, and, when XWPE_INNER_UNSKIP is set, takes the layer's own
skip marks off its rows so that they reach the fixture that boots the VM.  Its
name does not begin with test_, so a plain run of tests/ never collects it.
"""
import os

import pytest

import engine_guard

QEMU = "qemu-system-x86_64"
UNSKIP_VAR = "XWPE_INNER_UNSKIP"

engine_guard.BASELINE_TOOLS = tuple(
    name for name in engine_guard.BASELINE_TOOLS if name != QEMU)


@pytest.hookimpl(trylast=True)
def pytest_collection_modifyitems(config, items):
    """With XWPE_INNER_UNSKIP set, run the rows the layer marked as skipped."""
    if not os.environ.get(UNSKIP_VAR):
        return
    for item in items:
        item.own_markers = [m for m in item.own_markers if m.name != "skip"]
