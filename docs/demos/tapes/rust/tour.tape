# xwpe LSP tour -- Rust (rust-analyzer).  Cursor on the total(&shapes) call
# (line 40): hover -> references -> outline -> go-to-definition.  rust-analyzer
# indexes std on first start, so the Hidden warm-up is long.  Record from the
# repo root, opening src/main.rs from inside the crate (see record-tours.sh).

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
Sleep 1800ms
Alt+q
Sleep 300ms
Type "h"
Sleep 3s
Escape
Sleep 600ms
Alt+q
Sleep 300ms
Type "d"
Sleep 3500ms
Alt+q
Sleep 300ms
Type "r"
Sleep 3s
Alt+q
Sleep 300ms
Type "o"
Sleep 2800ms
Escape
Sleep 800ms
Ctrl+q
Sleep 1500ms
