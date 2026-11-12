"""Lane xwpe-ollama: the editor's agent against the machine's own Ollama.

The server is the local one (default http://localhost:11434); no hosted API is
involved.  Which model is loaded, and who may unload it, is the runner's
business (see ``run.py``); this lane only starts the editor on the model it is
given.

Nothing here is tuned to make a model pass: the run uses the editor's own
defaults except the backend, the model and the policy.
"""
from lanes import xwpe_common

LANE = "xwpe-ollama"
DEFAULT_ENDPOINT = "http://localhost:11434"
POLICY = "auto"


def run(driver, ctx, model, endpoint=DEFAULT_ENDPOINT):
    """Run the agent on ``ctx`` with ``model``."""
    backend_env = {
        "XWPE_AI_BACKEND": "ollama",
        "XWPE_AI_MODEL": model,
        "XWPE_AI_ENDPOINT": endpoint,
    }
    env = xwpe_common.build_env(ctx.home, ctx.trace_path, POLICY, backend_env)
    return xwpe_common.run_agent(driver, ctx.repo, ctx.home, ctx.trace_path,
                                 env, ctx.deadline_s, ctx.task_text)
