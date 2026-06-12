# xwpe LSP tour -- RUST (rust-analyzer).  A RICH single-window walk on the
# rust-lsp testbed showing the breadth of LSP in rust: diagnostics, hover,
# inlay hints, document highlight, references, outline, and a RENAME refactor
# with Undo.  The cursor starts on the total(...) call.  Before the rename it is
# re-established on `total` with an Up/Down/Right reset, because some actions
# (inlay) move it -- so the refactor always targets the right symbol.
# Record from the repo root AS wpe (programming mode -- Alt-Q is off in `we`);
# see docs/demos/record-tours.sh.  rust-analyzer indexes std on first start, so
# the Hidden warm-up is the longest of the wired servers.  Timings are
# deliberately unhurried so each action's effect (especially the inlay-hint
# pills) is clearly readable; the shared caption cue is docs/demos/tours.cue.

Output "docs/demos/gifs/rust/tour.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 80ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} src/main.rs"
Enter
Sleep 3s
Alt+q
Sleep 400ms
Type "e"
Sleep 90s
Show

Sleep 1500ms
Down@40ms 39
Right@40ms 15
Sleep 1500ms
# Hover -- the signature / type of the symbol under the cursor
Alt+q
Sleep 400ms
Type "h"
Sleep 3200ms
Escape
Sleep 700ms
# Inlay hints -- the inferred type pops in as a grey pill at each line's end
# (Alt-Q Y toggles them).  Held long so the reveal is unmistakable.
Alt+q
Sleep 400ms
Type "y"
Sleep 5000ms
# Document highlight -- every use of the symbol lights up at once
Alt+q
Sleep 400ms
Type "u"
Sleep 3400ms
# References -- every use listed in the Messages window
Alt+q
Sleep 400ms
Type "r"
Sleep 3400ms
# Outline -- the file's symbols in a popup
Alt+q
Sleep 400ms
Type "o"
Sleep 3400ms
Escape
Sleep 700ms
# Re-establish the cursor on `total` (the actions above may have moved it),
# then RENAME it across the file -- the refactor finale.
Up@20ms 60
Down@30ms 39
Right@30ms 15
Sleep 1400ms
Alt+q
Sleep 400ms
Type "n"
Sleep 1500ms
Right 8
Backspace 8
Type "tally"
Sleep 1200ms
Enter
Sleep 3500ms
# Undo -- the server's rewrite is a single, reversible edit (Ctrl-U)
Ctrl+u
Sleep 3200ms
Ctrl+q
Sleep 1500ms
