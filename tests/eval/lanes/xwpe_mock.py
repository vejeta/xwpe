"""Lane xwpe-mock: the editor's agent against its scripted in-process backend.

This lane is the suite's own self-test.  A scripted reply that solves a task
must score done, files_ok and build_ok; a scripted reply that does not must
score not done.  A scorer that passed both would be measuring the script, not
the outcome.

Scripts live in ``tests/eval/mock/<task>.<variant>``: the variant ``reply``
solves the task, ``wrong`` does not and never says DONE, ``wrong_done`` leaves
the files wrong and claims DONE anyway (the scorer must not pass it).  Turns are separated by ``@@TURN@@``; the
last turn repeats for as long as the run asks for more.
"""
import os

from lanes import xwpe_common

LANE = "xwpe-mock"
VARIANTS = ("reply", "wrong", "wrong_done")
MOCK_DIR = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "mock")
POLICY = "auto"


def script_path(task, variant):
    return os.path.join(MOCK_DIR, "%s.%s" % (task, variant))


def has_script(task, variant):
    return os.path.isfile(script_path(task, variant))


def load_script(task, variant):
    """The scripted reply with the file's own final newline taken off."""
    with open(script_path(task, variant)) as fh:
        return fh.read().rstrip("\n")


def run(driver, ctx, variant):
    """Run the scripted agent on ``ctx`` (a runner task context)."""
    backend_env = {
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": load_script(ctx.task, variant),
    }
    env = xwpe_common.build_env(ctx.home, ctx.trace_path, POLICY, backend_env)
    return xwpe_common.run_agent(driver, ctx.repo, ctx.home, ctx.trace_path,
                                 env, ctx.deadline_s, ctx.task_text)
