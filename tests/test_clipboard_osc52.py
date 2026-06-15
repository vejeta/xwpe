"""Console copy reaches the OS clipboard via OSC 52.

A plain Copy in wpe (^C / ^Ins) -- and Cut, which copies first -- now also pushes
the selection to the system clipboard by emitting the OSC 52 "set clipboard"
sequence (ESC ] 52 ; c ; base64 BEL).  The terminal emulator performs the real
OS-clipboard write, so the copy is pasteable in any other application, locally
and over SSH.

pyte swallows OSC 52, so we scan the raw byte stream the driver keeps
(WpeSession.raw, decoded by WpeSession.osc52_payload()).
"""
from wpe_driver import WpeSession

CK = "\x0b"   # Ctrl-K, the WordStar block prefix
CC = "\x03"   # Ctrl-C = Copy (Edit menu: "Copy  ^Ins / ^C")
CX = "\x18"   # Ctrl-X = Cut  (Edit menu: "Cut   Shift Del / ^X")


def test_copy_pushes_selection_to_os_clipboard(tmp_path):
    """^K X (Mark Whole) then ^C emits OSC 52 carrying the selected text."""
    with WpeSession(str(tmp_path), "clipme\n", filename="t.c") as w:
        w.key(CK, "x")          # Mark Whole
        w.key(CC)               # Copy -> internal clipboard + OS clipboard
        payload = w.osc52_payload()
    assert payload == "clipme", \
        "Copy should emit OSC 52 'clipme', got %r" % payload


def test_copy_utf8_roundtrips_through_osc52(tmp_path):
    """UTF-8 survives: the OSC 52 payload base64-decodes back to the accented
    text.  (The legacy X11 path served only Latin-1 STRING and mangled this.)"""
    with WpeSession(str(tmp_path), "café\n", filename="t.c") as w:
        w.key(CK, "x")
        w.key(CC)
        payload = w.osc52_payload()
    assert payload == "café", \
        "Copy should round-trip UTF-8 via OSC 52, got %r" % payload


def test_cut_also_pushes_to_os_clipboard(tmp_path):
    """Cut (^X) routes through the same Copy path, so it exports too."""
    with WpeSession(str(tmp_path), "cutme\n", filename="t.c") as w:
        w.key(CK, "x")          # Mark Whole
        w.key(CX)               # Cut -> copies first, then deletes
        payload = w.osc52_payload()
    assert payload == "cutme", \
        "Cut should emit OSC 52 'cutme', got %r" % payload
