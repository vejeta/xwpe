# xwpe LSP tour -- C (clangd).  A RICH single-window walk on the
# c-lsp testbed showing the breadth of LSP in c: diagnostics, hover,
# inlay hints, document highlight, references, outline, and a RENAME refactor
# with Undo.  The cursor starts on the total(...) call.  Before the rename it is
# re-established on `total` with an Up/Down/Right reset, because some actions
# (inlay) move it -- so the refactor always targets the right symbol.
# Record from the repo root AS wpe (programming mode -- Alt-Q is off in `we`);
# see docs/demos/record-tours.sh.

Output "docs/demos/gifs/c/tour.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 80ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.cpp"
Enter
Sleep 3s
Alt+q
Sleep 400ms
Type "e"
Sleep 10s
Show

Sleep 1000ms
Down@40ms 42
Right@40ms 17
Sleep 1200ms
# Hover -- the signature / type of the symbol under the cursor
Alt+q
Sleep 300ms
Type "h"
Sleep 2600ms
Escape
Sleep 400ms
# Inlay hints -- inferred types appear inline (Alt-Q Y toggles them)
Alt+q
Sleep 300ms
Type "y"
Sleep 2600ms
# Document highlight -- every use of the symbol lights up at once
Alt+q
Sleep 300ms
Type "u"
Sleep 2400ms
# References -- every use listed in the Messages window
Alt+q
Sleep 300ms
Type "r"
Sleep 2600ms
# Outline -- the file's symbols in a popup
Alt+q
Sleep 300ms
Type "o"
Sleep 2400ms
Escape
Sleep 400ms
# Re-establish the cursor on `total` (the actions above may have moved it),
# then RENAME it across the file -- the refactor finale.
Up@20ms 60
Down@30ms 42
Right@30ms 17
Sleep 1000ms
Alt+q
Sleep 300ms
Type "n"
Sleep 1200ms
Right 8
Backspace 8
Type "tally"
Sleep 1000ms
Enter
Sleep 3000ms
# Undo -- the server's rewrite is a single, reversible edit (Ctrl-U)
Ctrl+u
Sleep 2800ms
Ctrl+q
Sleep 1500ms
