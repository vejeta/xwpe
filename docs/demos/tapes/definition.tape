# xwpe LSP demo -- go to definition (Alt-Q D).
# Put the cursor on the describe(...) call near the bottom, then Alt-Q D
# leaps it up to the `def describe` that defines it -- the iconic IDE jump,
# over LSP.
#
# Needs a running, indexed Metals.  The cold-start/indexing wait and the
# one-time "JDK too new -> pinned" notice are handled while Hidden.
#
# Record with:  vhs docs/demos/tapes/definition.tape   (from the repo root;
# WPE -> built ./wpe, DEMO -> an indexed Scala workspace -- see record.sh).

Output "docs/demos/gifs/definition.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 90ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.scala"
Enter
Sleep 4s
# start Metals (Alt-Q E = diagnostics), dismiss the one-time JDK-pin notice,
# and let it index -- all hidden.
Alt+q
Sleep 400ms
Type "e"
Sleep 2s
Enter
Sleep 95s
Show

Sleep 2s
# move the cursor onto the describe(...) CALL on line 29 (no menus: pure
# arrows, so the recording is deterministic).  Down to the line, right into
# the identifier.
Down@60ms 28
Right@45ms 15
Sleep 2s
# jump to where describe is DEFINED -- the cursor leaps up to line 14.
Alt+q
Sleep 400ms
Type "d"
Sleep 5s
Ctrl+q
Sleep 1500ms
