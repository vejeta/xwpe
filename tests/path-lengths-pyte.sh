#!/bin/sh
# path-lengths-pyte.sh -- the ncurses pyte suite at a long temporary folder.
#
# The pyte half of `make check-path-lengths`.  A row must mean the same
# whatever the folder pytest gives it, so the whole ncurses suite runs once
# with --basetemp and TMPDIR at LENGTH bytes (300 by default: the C side of
# the same lane has a cliff at 246, and every pyte row red at 120 was red at
# 240).  The default lane already covers the short case: pytest's own folder
# is 30 to 40 bytes.
#
#   path-lengths-pyte.sh [--srcdir DIR] [--builddir DIR] [--length N]
#                        [--jobs N] [--list-expected] [PYTEST-ARGS...]
#
# The python is DIR/tests/.venv/bin/python when it is there, else $PYTHON_BIN,
# else python3 (it needs pyte and pytest).
#
# The folder is made under /var/tmp (or $PATH_LENGTHS_BASE), which is on disk,
# and never inside the build tree: a build that walks up from the test's
# folder finds the tree's own Makefile and sources, and a row then fails for
# where the folder is and not for how long it is (measured: the gdb rows that
# run a program under megabytes of output).  It is padded to exactly LENGTH
# bytes with names of at most 200 bytes (NAME_MAX is 255).  Everything the run
# makes, its output too, is in that one folder and is removed when the script
# ends; a failing run prints the first lines of each failure's assertion.
# HOME and XDG_STATE_HOME stay short: the lane is about the test folder, not
# about where the editor keeps its state.  The suites of tests/x11 and
# tests/wayland run at the same length from their own CI step once a display
# is there (tests/run-tests.sh --x11 with PYTEST_ADDOPTS="--basetemp=...").
#
# Live files are left out twice: --ignore-glob='*_live*' drops them from the
# collection, and XWPE_TEST_LIVE is removed from the environment, so the live
# gate of tests/conftest.py (tests/live_gate.py) is in effect for any that
# remain.  The selection is by file name, never -k: a -k "not live" also drops
# every row whose name has "alive" or "outlive" in it.
#
# Rows that are red at a long folder for a reason that is not a test's own
# are named below, by id, with the reason.  They fail here and the lane
# calls them expected: it fails when one of them turns green (remove its
# line), and when any other row fails.  They are never hidden by a shorter
# path.
#
# CI: this is slow (about one run of the ncurses suite, an hour at -n 10), so
# a scheduled job runs it (weekly, and on a change to tests/wpe_driver.py,
# tests/testdir.h, tests/gui_owner.py or tests/deep_folder.py), never on every
# push:
#
#   - name: The ncurses suite at a 300 byte folder
#     run: make -s check-path-lengths-pyte
#
# Exit: 0 the suite is green but for the expected rows, 1 a failure, an
# expected row gone green, or no run, 2 a bad call.

SRCDIR=${srcdir:-.}
BUILDDIR=.
LENGTH=300
JOBS=${JOBS:-4}
LIST=0
NAME_PART=200
FAILURE_LINES=200
BAD=2

# The expected-red rows: "id|reason" per line.  The jdb row is a product
# question for the screen owner of a box sentence (editor bug or documented
# behaviour); a shorter folder would bury it.  The two debugger-listing rows
# are not explained yet: they wait for an evidence pass, and no test is
# changed until it names the class.  (The title the editor cuts at its head,
# dropping the file name and cutting by bytes, is the other open product
# question, we_wind.c e_std_rahmen: its row is marked as an incoherence in
# tests/test_compile_and_messages.py.)  The contain rows are a product defect
# of the path-buffers class (we_contain.c CONTAIN_TOKEN_MAX cuts a folder named
# in AIContainPathAllow at 255 bytes), and the hosted read_scope rows end on
# the farm's deny stub and are unexplained: both are in FINDINGS-open.md.
EXPECTED_RED='tests/test_debugger_integration.py::test_jdb_vm_that_cannot_enter_its_directory_says_why|the box that says jdb cannot enter a folder keeps the path and loses the reason when the path is long
tests/test_buffer_owner.py::test_a_stack_shrinking_by_two_leaves_no_stale_row|the Stack window lists a frame by its whole path, which at a long folder fills the window so the third frame is out of view; not yet classified
tests/test_debugger_integration.py::test_gdb_stop_deep_in_a_recursion_is_marked[gdb-mi]|no current-line mark appears at a long folder; not yet classified
tests/test_ai_agent_engine_contracts.py::test_claudecli_spawn_is_its_own_session_with_no_terminal|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_claudecli_text_only_under_ask_no_tip|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_claudecli_text_only_under_auto_with_tip|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_engine_argv_opens_the_web_once_the_switch_is_on|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_engine_argv_under_ask|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_engine_argv_under_auto|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_engine_argv_under_edits|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_engine_spawn_is_its_own_session_with_no_terminal|CONTAIN_TOKEN_MAX
tests/test_ai_agent_engine_contracts.py::test_hosted_session_end_at_the_prompt_reviews_then_says_so|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_agent_carries_project_instructions_under_the_default|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_ask_plain_turn_carries_no_project_instructions|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_bound_key_cuts_the_file_and_names_itself[env]|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_bound_key_cuts_the_file_and_names_itself[xwperc]|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_chat_carries_project_instructions_from_its_first_tool_turn|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_prompt_states_the_tool_list_stands_after_the_project_block|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_scope_all_folds_project_instructions_into_a_plain_ask[env]|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_scope_all_folds_project_instructions_into_a_plain_ask[xwperc]|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_scope_off_keeps_project_instructions_out_of_the_agent|CONTAIN_TOKEN_MAX
tests/test_ai_agent_project_instructions.py::test_specific_file_wins_over_agents_md|CONTAIN_TOKEN_MAX
tests/test_ai_claudecli_stale_model.py::test_stale_ollama_model_not_forwarded|CONTAIN_TOKEN_MAX
tests/test_ai_claudecli_web.py::test_claudecli_allows_web_tools_when_the_switch_is_on|CONTAIN_TOKEN_MAX
tests/test_ai_claudecli_web.py::test_claudecli_denies_web_tools_when_the_switch_is_off|CONTAIN_TOKEN_MAX
tests/test_ai_contain_credentials.py::test_an_engine_in_a_dropped_workspace_folder_is_named_in_the_pane|CONTAIN_TOKEN_MAX
tests/test_ai_contain_credentials.py::test_the_claude_backend_keeps_its_login_variables_and_no_others|CONTAIN_TOKEN_MAX
tests/test_ai_contain_credentials.py::test_the_users_own_config_can_allow_the_folder_an_engine_sits_in|CONTAIN_TOKEN_MAX
tests/test_ai_edit_entry_contracts.py::test_complete_backend_error|CONTAIN_TOKEN_MAX
tests/test_ai_edit_error_guard.py::test_edit_refuses_backend_error|CONTAIN_TOKEN_MAX
tests/test_ai_edit_request_body.py::test_whole_file_edit_sends_instruction_windows_header_and_file|CONTAIN_TOKEN_MAX
tests/test_ai_pane_roles.py::test_chat_context_is_the_file_not_the_transcript|CONTAIN_TOKEN_MAX
tests/test_ai_session.py::test_ai_session_claudecli_resume|CONTAIN_TOKEN_MAX
tests/test_ai_tool_children.py::test_a_long_prompt_reaches_the_cli_whole|CONTAIN_TOKEN_MAX
tests/test_ai_read_scope.py::test_the_hosted_engine_is_told_the_folders_the_user_already_allowed|HOSTED_UNEXPLAINED
tests/test_ai_read_scope.py::test_the_hosted_engine_routes_its_prompts_to_the_editor_at_every_dial[ask-manual]|HOSTED_UNEXPLAINED
tests/test_ai_read_scope.py::test_the_hosted_engine_routes_its_prompts_to_the_editor_at_every_dial[auto-acceptEdits]|HOSTED_UNEXPLAINED
tests/test_ai_read_scope.py::test_the_hosted_engine_routes_its_prompts_to_the_editor_at_every_dial[edits-acceptEdits]|HOSTED_UNEXPLAINED'

# The two reasons the lines above name by a word (a line is "id|WORD").
REASON_CONTAIN="a folder in PATH or in AIContainPathAllow is cut at 255 bytes (we_contain.c CONTAIN_TOKEN_MAX), so the stand-in engine of the row is never found; product, path-buffers class, in FINDINGS-open.md"
REASON_HOSTED="the hosted engine's prompts end on the farm's deny stub claude at a long folder; unexplained, evidence pending, in FINDINGS-open.md"

# expected_with_reasons - EXPECTED_RED, a word standing for its full reason.
expected_with_reasons() {
  printf '%s\n' "$EXPECTED_RED" | sed -e "s#|CONTAIN_TOKEN_MAX\$#|$REASON_CONTAIN#" \
    -e "s#|HOSTED_UNEXPLAINED\$#|$REASON_HOSTED#"
}

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --builddir) shift; BUILDDIR=$1 ;;
    --length) shift; LENGTH=$1 ;;
    --jobs) shift; JOBS=$1 ;;
    --list-expected) LIST=1 ;;
    --) shift; break ;;
    *) break ;;
  esac
  shift
done

if [ $LIST = 1 ]; then
  expected_with_reasons | sed 's/|/  expected red: /'
  exit 0
fi

BUILDDIR=$(cd "$BUILDDIR" && pwd) || exit $BAD
SRCDIR=$(cd "$SRCDIR" && pwd) || exit $BAD
WORK_BASE=${PATH_LENGTHS_BASE:-/var/tmp}
. "$SRCDIR/tests/workdir.sh"
work_dir path-lengths-pyte 2

# pad_base BASE LENGTH - BASE grown to exactly LENGTH bytes by names of at
# most NAME_PART bytes, made and printed.
pad_base() {
  path=$1
  want=$2
  while [ $((${#path} + 1 + NAME_PART)) -lt "$want" ]; do
    path=$path/$(printf '%*s' "$NAME_PART" '' | tr ' ' a)
  done
  rest=$((want - ${#path} - 1))
  if [ "$rest" -gt 0 ]; then
    path=$path/$(printf '%*s' "$rest" '' | tr ' ' b)
  fi
  mkdir -p "$path" || exit 2
  printf '%s\n' "$path"
}

# finish STATUS - leave the folder removable and end.  Tests of the Go tools
# leave a module cache of read-only folders, which rm -rf cannot remove.
finish() {
  chmod -R u+w "$WORK" 2> /dev/null
  exit "$1"
}

if [ $((${#WORK} + 2)) -gt "$LENGTH" ]; then
  echo "path-lengths-pyte: the work folder $WORK is longer than $LENGTH bytes" >&2
  finish $BAD
fi
BASE=$(pad_base "$WORK/long" "$LENGTH") || finish 2
mkdir -p "$WORK/home" "$WORK/state"

PYTHON=$SRCDIR/tests/.venv/bin/python
[ -x "$PYTHON" ] || PYTHON=${PYTHON_BIN:-python3}
PARALLEL=
if "$PYTHON" -c "import xdist" >/dev/null 2>&1; then
  PARALLEL="-n $JOBS"
fi
WPE=$BUILDDIR/wpe
if [ ! -x "$WPE" ]; then
  echo "path-lengths-pyte: no $WPE; build first" >&2
  finish $BAD
fi

echo "path-lengths-pyte: the ncurses suite at a $(printf '%s' "$BASE" | wc -c | tr -d ' ') byte folder"
expected_with_reasons | sed 's/^/path-lengths-pyte: expected red: /; s/|/ -- /'

OUT=$WORK/pytest.out
(
  cd "$SRCDIR" || exit 2
  env -u OPENAI_API_KEY -u ANTHROPIC_API_KEY -u DISPLAY -u WAYLAND_DISPLAY \
    -u XWPE_TEST_LIVE \
    HOME="$WORK/home" XDG_STATE_HOME="$WORK/state" TMPDIR="$BASE" \
    WPE_BIN="$WPE" \
    "$PYTHON" -m pytest -q -rf -p no:cacheprovider $PARALLEL \
      --basetemp="$BASE/pt" \
      --ignore=tests/x11 --ignore=tests/wayland --ignore=tests/eval \
      --ignore=tests/harness --ignore-glob='*_live*' "$@" tests
) > "$OUT" 2>&1
PYTEST_STATUS=$?
tail -n 5 "$OUT"

if ! grep -qE '^[0-9]+ (passed|failed)|(passed|failed).* in ' "$OUT"; then
  echo "path-lengths-pyte: FAIL: pytest did not finish a run (status $PYTEST_STATUS)"
  finish 1
fi

STATUS=0
# Every FAILED id, with the " - reason" pytest appends cut off.
FAILED=$(sed -n 's/^FAILED \([^ ]*\).*/\1/p' "$OUT")
for id in $FAILED; do
  if printf '%s\n' "$EXPECTED_RED" | cut -d'|' -f1 | grep -qxF -- "$id"; then
    echo "path-lengths-pyte: expected red: $id"
  else
    echo "path-lengths-pyte: FAIL: $id"
    STATUS=1
  fi
done
for id in $(printf '%s\n' "$EXPECTED_RED" | cut -d'|' -f1); do
  if ! printf '%s\n' "$FAILED" | grep -qxF -- "$id"; then
    echo "path-lengths-pyte: FAIL: $id is expected red and did not fail; remove it from EXPECTED_RED"
    STATUS=1
  fi
done
if [ $STATUS != 0 ]; then
  echo "path-lengths-pyte: the first lines of the assertions that failed:"
  grep -E '^(E  |_____)' "$OUT" | cut -c1-220 | head -n "$FAILURE_LINES"
else
  echo "path-lengths-pyte: ok"
fi
finish $STATUS
