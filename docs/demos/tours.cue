# Key captions for the per-language LSP tours (docs/demos/gifs/<lang>/tour.gif).
# All five tours share the same tape body, so the post-Show action times are
# (near) identical -- one cue captions all five.  Apply with:
#   docs/demos/captions.sh docs/demos/gifs/c/tour.gif docs/demos/tours.cue
# Format: START_SEC  DURATION_SEC  LABEL...   (seconds from the start of the GIF)
#
# Times are anchored to the C recording (the slowest cursor-nav, so its action
# times are the latest); the other tours' nav is a touch quicker, so each caption
# starts a little BEFORE its action and runs through the action's on-screen hold,
# which absorbs the small per-language drift.  Re-derive after any tape retiming
# by sampling the GIF (see docs/demos/README.md).
#
# START  DUR   LABEL
  5.0    4.2   Alt-Q H   Hover (type / signature)
  9.6    6.0   Alt-Q Y   Inlay hints -- inferred-type pills
  15.1   4.0   Alt-Q U   Highlight all uses
  19.1   4.0   Alt-Q R   References
  23.0   4.0   Alt-Q O   Outline
  31.2   7.0   Alt-Q N   Rename refactor (total -> tally)
  40.0   3.8   Ctrl-U   Undo
