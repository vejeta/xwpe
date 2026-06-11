# xwpe LSP tour -- Go (gopls).  Cursor on the total(shapes) call (line 36):
# hover -> references -> outline -> go-to-definition.  gopls loads the module
# while Hidden.  Record from the repo root (see docs/demos/record-tours.sh).

Output "docs/demos/gifs/go/tour.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 80ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.go"
Enter
Sleep 3s
Alt+q
Sleep 400ms
Type "e"
Sleep 12s
Show

Sleep 1500ms
Down@40ms 35
Right@40ms 9
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
