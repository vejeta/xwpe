#!/bin/sh
# ci-venv.sh -- the Python environment every job of .github/workflows/linux.yml
# runs the suite with, at /tmp/ptenv.
#
# The versions are the ones tests/ci-versions.txt records, and
# tests/check-ci-versions.sh fails when this file says another.  pytest 8.2 or
# newer is needed for the `@file` argument the pyte jobs read their rows from;
# pytest-rerunfailures gives `--rerun-except`, which keeps a row the row clock
# cut from running again.
#
#   sh tests/ci-venv.sh

set -eu

python3 -m venv /tmp/ptenv
/tmp/ptenv/bin/pip install --quiet pyte==0.8.2 pytest==9.1.1 \
  pytest-xdist==3.8.0 pytest-rerunfailures==16.7 pillow
