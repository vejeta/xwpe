"""The one row test_basetemp_parent.py runs under a test folder that does not
exist yet.  It starts nothing: it only needs a session, so the conftest's
session start (the tool farm under the basetemp) runs.  Its name does not
begin with test_, so a plain run of tests/ never collects it; it lives in
tests/ itself so that tests/conftest.py is the conftest it runs under."""


def test_a_row_runs_under_a_basetemp_made_for_it(tmp_path):
    assert tmp_path.is_dir()
