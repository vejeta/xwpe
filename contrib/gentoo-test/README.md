# Gentoo build/runtime test

Builds xwpe on a current Gentoo and runs the console mouse probe against it.
Gentoo is Linux, so this uses the official **gentoo/stage3 Docker image** rather
than a VM (the BSD harness needs Vagrant only because a BSD kernel cannot run in
a container). The stage3 image is up to date (23.0 profile, working binhost),
which sidesteps the ancient-Vagrant-box profile/binpkg breakage.

It builds the way the `app-editors/xwpe` ebuild does -- from the `make dist`
tarball, which ships `configure` (no autoreconf) -- console-only, then drives
the built `wpe` under a pty and checks the File menu opens.

## Running it

Needs Docker.

```sh
cd contrib/gentoo-test
./run.sh                       # uses the tarball in the build tree
./run.sh /path/to/xwpe-1.6.6.tar.gz
```

The packaging submission (the ebuild + Manifest, PR'd to gentoo/gentoo via the
maintainer's fork) is separate and lives outside the repo.
