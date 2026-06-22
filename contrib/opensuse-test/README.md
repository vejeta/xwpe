# openSUSE Tumbleweed runtime test

A Vagrant VM that installs the **packaged** xwpe from the maintainer's OBS
project (`home:vejeta`) and smoke-tests the console binary. Where `contrib/bsd-test`
builds from source, this verifies the actual deliverable: that the Build Service
produced a `wpe` that launches and drives its menu over a pty.

## What it checks

1. **Package installs.** Adds the OBS repo, installs `xwpe`, and confirms all
   four entry points (`we`, `wpe`, `xwpe`, `xwe`) are present.
2. **Console runtime.** Builds `../bsd-test/sgr_mouse_probe.c` (fetched from
   Codeberg) and runs it against the installed `/usr/bin/wpe`: it launches wpe
   under a pty, injects an SGR (1006) mouse click on the File menu, and checks
   the menu opens -- under several `TERM` values.

## Running it

Needs `vagrant` + `libvirt` (or VirtualBox) on the host.

```sh
cd contrib/opensuse-test
VAGRANT_DEFAULT_PROVIDER=libvirt vagrant up      # installs the RPM, prints PASS/FAIL
VAGRANT_DEFAULT_PROVIDER=libvirt vagrant ssh      # poke around: run `wpe`
vagrant destroy -f                                # tear down
```

The build status itself (compile/link/`make check`) is covered by OBS; this
harness adds the runtime check that OBS does not perform.
