# xwpe LSP tour -- C/C++ (clangd).
# A single-window walk on the c-lsp testbed, cursor on the total(shapes) call:
# hover (signature) -> go-to-definition (jumps up to `double total(...)`) ->
# references -> outline.  Definition runs right after hover, while the cursor is
# still on the call, so it never depends on the later actions.  clangd starts
# quickly; it is Hidden so the GIF opens on the action.  Cursor is positioned
# with arrows (Find/Alt-S is not scriptable in VHS).
#
# Record from the repo root with WPE -> built ./wpe and DEMO -> a copy of
# docs/examples/c-lsp (see docs/demos/record-tours.sh).

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
Sleep 8s
Show

Sleep 1500ms
# move onto the `total` call on line 43 (col 18): down to the line, right to it
Down@40ms 42
Right@40ms 17
Sleep 1800ms
# Hover: the signature/type of the symbol under the cursor
Alt+q
Sleep 300ms
Type "h"
Sleep 3s
Escape
Sleep 600ms
# Go to definition: the cursor leaps up to `double total(...)`
Alt+q
Sleep 300ms
Type "d"
Sleep 3500ms
# References: every use of `total`, listed in Messages
Alt+q
Sleep 300ms
Type "r"
Sleep 3s
# Outline: the file's symbols in a popup (the finale)
Alt+q
Sleep 300ms
Type "o"
Sleep 2800ms
Escape
Sleep 800ms
Ctrl+q
Sleep 1500ms
