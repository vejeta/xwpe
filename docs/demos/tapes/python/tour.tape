# xwpe LSP tour -- Python (pyright / pylsp).  Cursor on the total(shapes) call
# (line 31): hover -> references -> outline -> go-to-definition.  The Python
# server starts while Hidden.  Record from the repo root (see record-tours.sh).

Output "docs/demos/gifs/python/tour.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 80ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.py"
Enter
Sleep 3s
Alt+q
Sleep 400ms
Type "e"
Sleep 10s
Show

Sleep 1500ms
Down@40ms 30
Right@40ms 11
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
