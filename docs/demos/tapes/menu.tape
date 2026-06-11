# xwpe LSP demo -- the Metals action menu (Borland-style, discoverable).
# No language server needed: the dropdown is a static render.
#
# Record with:  vhs docs/demos/tapes/menu.tape   (run from the repo root;
# set WPE to the built ./wpe and DEMO to a Scala workspace -- see record.sh).

Output "docs/demos/gifs/menu.gif"
Set Shell bash
Set FontSize 18
Set Width 1100
Set Height 700
Set TypingSpeed 90ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.scala"
Enter
Sleep 4s
Show

Sleep 1500ms
Alt+q
Sleep 400ms
Type "?"
Sleep 4s
Escape
Sleep 1s
Ctrl+q
Sleep 1500ms
