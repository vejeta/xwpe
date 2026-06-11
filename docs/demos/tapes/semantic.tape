# xwpe LSP demo -- semantic colours (server-driven highlighting, Alt-Q M).
# The regex lexer paints types, calls and variables the same; Metals'
# semantic tokens tell them apart.  Toggle on/off to show the difference.
#
# Needs a running, indexed Metals.  The cold-start/indexing wait and the
# one-time "JDK too new -> pinned" notice are dealt with while Hidden, so
# the visible GIF is just the clean before/after.
#
# Record with:  vhs docs/demos/tapes/semantic.tape   (from the repo root;
# WPE -> built ./wpe, DEMO -> an indexed Scala workspace -- see record.sh).

Output "docs/demos/gifs/semantic.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 980
Set TypingSpeed 90ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} main.scala"
Enter
Sleep 4s
# first Alt-Q M starts Metals (posts the one-time JDK-pin notice) and reports
# "no tokens yet" -- the overlay stays OFF; dismiss the notice and let it index.
Alt+q
Sleep 400ms
Type "m"
Sleep 2s
Enter
Sleep 95s
Show

# BEFORE: the classic regex colours
Sleep 3s
# AFTER: server-driven semantic colours
Alt+q
Sleep 400ms
Type "m"
Sleep 6s
# back to the regex colours
Alt+q
Sleep 400ms
Type "m"
Sleep 3s
Ctrl+q
Sleep 1500ms
