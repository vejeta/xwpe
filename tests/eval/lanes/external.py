"""Lanes that live outside the repository.

A lane is one Python file exposing the interface of ``xwpe_ollama.py``: a
``LANE`` name (a non-empty string) and a function ``run(driver, ctx, model,
endpoint)`` that returns a ``xwpe_common.LaneResult``.  The runner loads every
``*.py`` file of a directory given with ``--lane-dir`` (or the environment
variable ``XWPE_EVAL_LANE_DIR``), so reference lanes can be kept apart from the
sources of this repository.  A file whose name starts with ``_`` is skipped.
"""
import importlib.util
import os

LANE_ATTR = "LANE"
RUN_ATTR = "run"
MODULE_PREFIX = "xwpe_eval_lane_"


class LaneLoadError(Exception):
    """A lane directory or lane file that cannot be used; the text says why."""


def lane_files(directory):
    """The lane files of ``directory``, sorted; an error when it is no folder."""
    if not os.path.isdir(directory):
        raise LaneLoadError("lane directory %s is not a directory" % directory)
    names = [n for n in sorted(os.listdir(directory))
             if n.endswith(".py") and not n.startswith("_")]
    return [os.path.join(directory, n) for n in names]


def load_lane_file(path):
    """Import ``path`` and check it exposes a lane; returns the module."""
    name = MODULE_PREFIX + os.path.splitext(os.path.basename(path))[0]
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise LaneLoadError("lane file %s cannot be imported" % path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as exc:
        raise LaneLoadError("lane file %s failed to load: %s: %s"
                            % (path, type(exc).__name__, exc))
    lane = getattr(module, LANE_ATTR, None)
    if not isinstance(lane, str) or not lane:
        raise LaneLoadError("lane file %s has no %s name (a non-empty string)"
                            % (path, LANE_ATTR))
    if not callable(getattr(module, RUN_ATTR, None)):
        raise LaneLoadError("lane file %s has no %s(driver, ctx, model, "
                            "endpoint) function" % (path, RUN_ATTR))
    return module


def load_lane_dir(directory, taken=()):
    """{lane name: module} for every lane file of ``directory``.

    ``taken`` are names already in use (the built-in lanes); a lane may not
    reuse one, and two files may not share a name."""
    lanes = {}
    for path in lane_files(directory):
        module = load_lane_file(path)
        name = getattr(module, LANE_ATTR)
        if name in taken or name in lanes:
            raise LaneLoadError("lane name %s in %s is already in use"
                                % (name, path))
        lanes[name] = module
    return lanes
