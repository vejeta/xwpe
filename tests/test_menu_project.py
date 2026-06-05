"""Project-menu (Alt-P) regression tests for wpe (terminal mode).

Project items (we_menue.c): New Project -> e_new_project, Open Project,
Close Project, Add Item -> e_p_add_item, Delete Item, Options.

New Project writes the .prj file on disk, so it is checked against that file
(ground truth).  Add Item / persistence and the project windows are covered in
depth by test_project_persist.py and test_project_windows.py (they drive the
project-management UI), so this section focuses on New Project.
"""
import os
import time

from wpe_driver import WpeSession, ALT


def test_new_project_creates_a_prj(tmp_path):
    """New Project (File-Manager): type a name, Enter:Create writes the .prj
    with the project template (EXENAME derived from the name)."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        w.menu(ALT.PROJECT, "n")         # Project -> New Project (File-Manager)
        w.key("myproj.prj")              # the project name into Name
        w.key("\r")                      # Enter : Create
        time.sleep(0.4)
        prj = os.path.join(str(tmp_path), "myproj.prj")
        assert w.alive(), "wpe died during New Project"
        assert os.path.exists(prj), "New Project should create the .prj, dir=%r" % \
            os.listdir(str(tmp_path))
        content = open(prj).read()
        assert "EXENAME=" in content and "myproj" in content, \
            "the new .prj should carry the project template, got:\n%s" % content
