"""The libx11-compat hooks cost nothing against a real libX11.

xwpe carries one symbol that exists only in the libx11-compat shim,
libx11CompatRegisterLiveResizeReflow.  It must stay a WEAK undefined symbol:
against a real libX11 the symbol is absent, the pointer is null and the
registration is skipped.  If it became a strong reference, the Linux and
XQuartz builds would stop linking, and a stub added to make them link would
silently turn the hook on.  So the binary is asked what the linker recorded.

The resize rows that prove the hook is inert at run time against a real X
server are tests/x11/test_resize.py and tests/x11/test_resize_mouse.py: they
run the same resize path on Xvfb with no shim present.

Run: XWPE_BIN=$PWD/xwpe tests/.venv/bin/python -m pytest -q tests/x11/test_shim_hooks_inert.py
"""
import os
import shutil
import subprocess
import sys

import pytest

from conftest import XWPE_BIN

SHIM_HOOK = "libx11CompatRegisterLiveResizeReflow"
# nm's letter for a weak undefined symbol; 'U' is a strong undefined one and a
# defined symbol is 'T', 'W' or 'V'.
WEAK_UNDEFINED = "w"


def _nm_lines(binary, *options):
    """The lines nm prints for `binary` with `options`."""
    out = subprocess.run(["nm", *options, os.path.realpath(binary)],
                         capture_output=True, text=True, check=True).stdout
    return out.splitlines()


def _elf_nm_letter(binary, symbol):
    """The letter nm prints for `symbol` in an ELF `binary`, or None when
    absent.  An installed binary is stripped and has only its dynamic symbol
    table, which still lists the weak reference, so that table is read when
    the full one is empty."""
    lines = _nm_lines(binary) or _nm_lines(binary, "-D")
    for line in lines:
        words = line.split()
        if words and words[-1] == symbol:
            return words[-2]
    return None


def _macho_nm_letter(binary, symbol):
    """The same answer for a Mach-O `binary`: nm -m spells a weak undefined
    reference "(undefined) weak external _<symbol>", and prints 'U' for it
    without -m."""
    for line in _nm_lines(binary, "-m"):
        words = line.split()
        if "_" + symbol not in words:
            continue
        if "(undefined)" in words and "weak" in words:
            return WEAK_UNDEFINED
        return line.strip()
    return None


def _nm_letter(binary, symbol):
    """The letter nm prints for `symbol` in `binary`, or None when absent."""
    if sys.platform == "darwin":
        return _macho_nm_letter(binary, symbol)
    return _elf_nm_letter(binary, symbol)


# @scenario SC-macos-18
def test_the_shim_hook_is_a_weak_undefined_symbol():
    """Against a real libX11 the shim's registration hook is weak and
    undefined, so it is never called."""
    if shutil.which("nm") is None:
        pytest.skip("nm is not installed")
    assert os.path.exists(XWPE_BIN), "no xwpe binary at %s" % XWPE_BIN
    letter = _nm_letter(XWPE_BIN, SHIM_HOOK)
    assert letter is not None, \
        "%s is not in the binary: the hook is gone or renamed" % SHIM_HOOK
    assert letter == WEAK_UNDEFINED, \
        "%s is '%s' in nm, not a weak undefined '%s': the hook is no longer " \
        "optional" % (SHIM_HOOK, letter, WEAK_UNDEFINED)
