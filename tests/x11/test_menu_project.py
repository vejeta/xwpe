"""Project-menu (Alt-P) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_project.py for wpe.

New Project drives the File-Manager dialog (type a name, Enter:Create) and is
checked against the .prj written to disk.  Add Item / persistence are covered
by the wpe project tests.
"""
import os
import time


def test_new_project_creates_a_prj(xwpe):
    """New Project: type a name into the File-Manager, Enter writes the .prj."""
    xwpe.menu("p", "n")                  # Project -> New Project (File-Manager)
    xwpe.type("myproj.prj")              # project name into Name
    xwpe.key("Return")                   # Enter : Create
    time.sleep(0.5)
    prj = os.path.join(os.path.dirname(xwpe.srcfile), "myproj.prj")
    assert xwpe.proc.poll() is None, "xwpe died during New Project"
    assert os.path.exists(prj), "New Project should create the .prj, dir=%r" % \
        os.listdir(os.path.dirname(xwpe.srcfile))
    content = open(prj).read()
    assert "EXENAME=" in content and "myproj" in content, \
        "the new .prj should carry the project template, got:\n%s" % content
