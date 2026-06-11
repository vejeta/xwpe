# Investigation: arrow keys don't navigate dialogs / the LSP picker (#215)

**Status:** root cause found and fixed in `we_term.c` (`e_t_csi_key`).
**Method:** data-driven -- instrumented `e_opt_kst` and the key path, drove the
picker headlessly, read the actual key codes. No guessing.

## Symptom

Selecting a non-default entry in the LSP code-action picker (`Alt-Q A`) -- and
navigating any `e_opt_kst` radio dialog or menu -- could not be driven by the
arrow keys from the pyte harness: the selection stayed on the first option no
matter how many Downs were sent. (Reported as "it should be reliable with keys
too".)

## What the data showed

Instrumented `e_opt_kst` to log every key it receives (`XWPE_DLG_TRACE=<file>`).
Pressing **Down** (`\033[B` = bytes `27 91 66`) in the picker logged:

```
radio-getch  c=0       <- ESC handled, returned 0
radio-getch  c=66      <- 'B' arrived as a LITERAL character, not CDO
radio-nav    c=66      <- e_get_opt_sw('B') = no movement
```

The arrow was **not** decoded to `CDO` (335). Key codes for reference:
`CUP=327 CDO=335 CLE=330 CRI=332  CR=13 ESC=27 TAB=9 space=32`.

## Root cause

`fk_getch()` is ncurses `getch()` and `keypad(stdscr, TRUE)` is set, so ncurses
*usually* folds `ESC [ B` into one `KEY_DOWN`, which `e_t_getch()` maps to `CDO`.
But the raw escape can slip through un-assembled -- inside a modal dialog's poll
loop the three bytes arrive across separate `getch()` calls. When that happens
`e_t_getch()`'s ESC handler assumed the byte after ESC is an **Alt-combo** and
called `e_tast_sim()`:

- `e_tast_sim('[')` is not in its table -> `default: return 0`,
- and the final `'B'` then surfaces on the next `getch()` as a literal letter.

There was **no CSI (`ESC [ ...`) / SS3 (`ESC O ...`) decoder** in the fallback,
so whenever keypad assembly did not fire, every arrow/Home/End/PageUp key in a
dialog or menu was lost. This is why the Options dialogs and the picker ignored
the arrows; the plain editor mostly escaped it because its tight read loop let
ncurses assemble the sequence.

## Fix

`we_term.c`: added `e_t_csi_key(intro)` and called it from the ESC handler when
the byte after ESC is `'['` or `'O'`. It reads the remaining byte(s) and returns
the proper xwpe key (`CUP/CDO/CLE/CRI`, `POS1/ENDE`, `EINFG/ENTF`, `BUP/BDO`,
consuming the trailing `~`/modifier digits of `ESC [ n ~` forms). It only runs
when ncurses did *not* already assemble the key, so it is purely additive and
cannot break the working keypad path.

## Second bug: Enter did not confirm a radio/checkbox dialog

Once arrows decoded, the trace showed Down moving focus and **Space** selecting
the focused radio (`*` moves) -- but **Enter** was a no-op: `if(c == WPE_CR) {
sw = 0; c = cold; break; }` just dropped focus, so the dialog never closed and
nothing applied. The text/numeric widgets already routed Enter to the default
button (`if(c == WPE_CR) c = o->crsw;`); the radio and checkbox blocks did not.

Fix (`we_opt.c`, Borland Enter=OK):
- **radio**: Enter SELECTS the focused option (`pstr->num = j`) and routes to
  `crsw` -- so Down-to-it then Enter is one move; Space first is optional.
- **checkbox**: Enter routes to `crsw` (confirm); Space still toggles.
- `e_lsp_pick` (we_debug.c) now sets `o->crsw = AltO` so Enter activates its Ok.

## Verified end to end (by keyboard)

With both fixes, a live-Metals probe drove the code-action picker entirely by
keyboard -- `Alt-Q A`, arrow-down to "Convert to named arguments", **Enter** --
and the call on disk became the named-argument form. The dialog trace showed
`c=335` (CDO) for Down and `c=13` (CR) applying. Covered by
`tests/test_lsp_menu.py::test_code_action_resolve_named_arguments`.

The reliable keyboard sequences are now: radios -- Down/Up to the option, Enter
(or Space then Enter); checkboxes -- Space to toggle, Enter to confirm.

## Verifying headlessly

See `tests/README.md` ("pyte harness gotchas"). Drive arrows with `\033[B` etc.
The `XWPE_DLG_TRACE` key-logging used above was temporary instrumentation in
`e_opt_kst` (removed once the fix landed); re-add a couple of `fprintf`s there if
you need to confirm the dialog receives `CDO=335` rather than the raw `66`.
