# Agent evaluation suite

Measures what the editor's agent does on small, synthetic repositories, so a
change to the agent can be judged by outcomes and not by impressions.

## Run

Always with the test virtual environment's python, from the repository root,
with `./wpe` built (`--enable-ai`):

    tests/.venv/bin/python tests/eval/run.py --lanes mock --tasks E03
    tests/.venv/bin/python tests/eval/run.py --lanes local --tasks E01,E02 \
        --models <ollama-model> --runs 1

- `--lanes mock`: the scripted backend only. This is the suite's own
  self-test (see below).
- `--lanes local`: the self-test, then the editor against the local Ollama
  server (`--endpoint`, default `http://localhost:11434`). No hosted service.
  There is no default model: pass `--models` or set `XWPE_AI_MODEL`.
- `--lanes <name>`: a lane loaded from `--lane-dir` (see Lanes). A comma list
  runs several.
- `--tasks` fixture ids (default all), `--models` (comma list; default
  `$XWPE_AI_MODEL`), `--runs N` per task and model, `--deadline-s` per run
  (default 1500 s local, 120 s mock).

The binary must be named `wpe`: under the name `we` the editor's programming
layer, including the assistant prefix, is off and every run would measure
nothing.

## What a run does

Each run gets one scratch folder under `/var/tmp`, printed when made and
removed by its exact path afterwards. It holds `repo/` (a copy of the fixture
without `checks.json`; the editor's working folder) and `home/` (HOME and the
XDG roots of that run). Every run keeps its trace, the reply the answer was
read from (`.answer`) and the whole pane (`.pane`) under `out/<stamp>/traces/`;
a run that did not fully pass also keeps its copy under `out/<stamp>/keep/`.

The editor is driven through `tests/wpe_driver.WpeSession`: Alt-G, `g`, the
prompt, Enter. The prompt is the same for every task: "Read TASK.md in this
folder and do what it says. Reply DONE with a two-line summary when finished."
The run ends on the trace's end line (DONE, an answer with no protocol line,
a backend or transport error), the step-limit row, an `[AI]` error row once
the spinner is gone, the editor exiting, or the deadline.

Results: `out/<stamp>.jsonl` (one row per task, lane, model, run; a rerun is a
new file) and `out/<stamp>.md` (the table). `out/` is not tracked.

## Columns

`task lane model run done files_ok build_ok test_ok turns tool_calls tok_in
tok_out ctx_peak wall_s end_reason folds loop_stops cut_retries malformed
denials cost_usd notes`

- `done`: the run ended with the model's DONE marker.
- `files_ok`, `build_ok`, `test_ok`: from `fixtures/<id>/checks.json` run on
  the copy after the run. `files_ok` also includes the answer check of an
  explain task. Results only: no leftover old name, `make -B -f Makefile`
  builds, `make -B -f Makefile test` passes, the answer names the right
  functions. `-f Makefile` keeps a `GNUmakefile` or `makefile` the agent
  added from replacing the fixture's own checks. The answer is read from the
  model's last reply (the lines around its DONE line), not from guesses it
  wrote earlier in the pane. The reply is read with the pane's history (PgUp,
  up to 8 pages); a reply whose first line still cannot be seen is not scored
  and `notes` says so. Build and test commands run in a clean environment (no
  inherited `CC`, `CFLAGS` or `MAKEFLAGS`). A different correct solution scores
  the same as the reference one.
- `tool_calls`, `denials`: counted from the editor's trace today.
- `turns`, `tok_in`, `tok_out`, `ctx_peak`, `folds`, `loop_stops`,
  `cut_retries`: read from `agent turn=` / `agent fold` / `agent cut` /
  `agent end` trace lines once the editor writes them. Until then the cell is
  empty. Empty means not measured, never zero. `malformed` has no source yet.
- `cost_usd`: 0 for the local and scripted lanes.
- `notes`: why a check failed, plus `ollama ctx=N`, the window the server
  reported for the model after the run.

## Lanes

- `xwpe-mock` (`lanes/xwpe_mock.py`): the editor's scripted backend.
- `xwpe-ollama` (`lanes/xwpe_ollama.py`): the editor against local Ollama,
  policy `auto`, the editor's own defaults for everything else.

Both share `lanes/xwpe_common.py`. The runner unloads only a model it loaded
itself, through `/api/generate` with `keep_alive: 0`. If a model it did not
load is resident, it waits for the server to be free of it (at most 30 minutes,
looking every minute) and skips the lane otherwise. A server that cannot be
reached gives one row per task with `end_reason=server-unreachable`. An
exception inside one run gives a row with `end_reason=exception` and the suite
goes on.

### Lanes outside the repository

`--lane-dir DIR` (or `XWPE_EVAL_LANE_DIR`) names a folder of lane modules. The
runner imports every `*.py` file there by path (files starting with `_` are
skipped). Each exposes the interface of `lanes/xwpe_ollama.py`: a `LANE` name
(a non-empty string that no other lane uses) and
`run(driver, ctx, model, endpoint)` returning a `xwpe_common.LaneResult`. A
missing folder, a file that fails to import, or a file without `LANE` or `run`
stops the runner with a message before any run starts. Reference lanes can
live outside the repository this way. Select one with `--lanes <its LANE
name>`; the model list is passed through as given.

## Self-test

`mock/<task>.reply` solves the task and must score done, files_ok, build_ok and
test_ok. `mock/<task>.wrong` does not and must score not done.
`mock/<task>.wrong_done` leaves the files wrong and claims DONE anyway: its row
has `done=1` and must not pass files_ok, build_ok and test_ok together. The
runner exits 1 if any of these does not hold. A scorer that passed both would be measuring
the script and not the outcome. Scripts separate turns with `@@TURN@@`; the
last turn repeats.

## Fixtures

Synthetic, small, no network. Each has `TASK.md`, `checks.json` and a
`Makefile` whose `make` and `make test` are the build and functional checks.
`checks.json` is documented in `score.py`.

- `E01` explain: the calls between `main` and `parse_stmt`; answer only, no edit.
- `E02` off-by-one: `make test` fails on one assertion in a 529-line file;
  `check.c` and `stats.h` must not change. A held-out program
  (`fixtures/E02/hidden/edges.c`, never copied into the editor's folder) is
  built after the run with `-fsanitize=address,undefined` against the copy's
  `stats.c` and `stats.h`, and a failure there makes `test_ok` false. It guards
  against a fix that only satisfies the visible assertion or reads past the
  array.
- `E03` rename a function across three files; `make` and `make test` pass.

Fixtures are frozen once a baseline exists: a change needs a reason and a new
baseline on every lane. No string from a fixture's files may appear in the
editor's sources.

Frozen E02 edge values (`window_sum` over `v = 1..10`, length 10, heap
allocated at exactly that length), as `(from, count) -> sum`:
`(0,0)->0`, `(5,0)->0`, `(0,1)->1`, `(3,1)->4`, `(5,2)->13`, `(2,3)->12`,
`(7,3)->27`, `(0,10)->55`, `(8,5)->19`, `(9,1)->10`, `(0,11)->55`,
`(10,3)->0`. A negative `from` is left out on purpose: the fixture does not say
whether a window starting before the array is clamped or cut.

## Not here yet

E04 to E15, the reference lanes (kept outside the repository, loaded with
`--lane-dir`) and the cost cap.

## Checks of the suite itself

    tests/.venv/bin/python tests/eval/selfcheck.py

Covers the screen end check on a scripted pane, the final-reply reader, the
scorer's clean environment and the lane loader.
