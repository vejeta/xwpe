#!/bin/sh
# ci-install.sh -- the packages every job of .github/workflows/linux.yml runs
# the suite with (the build job, each pyte shard and the GUI job).
#
# The rows compile programs while they run and start debuggers, so every job
# needs the compilers, the debuggers and the libraries of the built program,
# not only the job that built it.  One list, one place: tests/check-ci-tools.sh
# keeps it equal to tests/ci-tools.txt and to the tools each job expects
# (XWPE_TEST_TOOLS_EXPECTED).
#
#   sh tests/ci-install.sh
#
# gdb, delve, scala, ga68, algol68g and locales are the debuggers, compilers
# and the German locale the rows of the harness declare; x11-apps (xwd,
# xlogo) and netpbm (xwdtopnm, which ImageMagick 7 needs to read a screen
# dump) are what the X11 layer gate needs to run at all.

set -eu

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential autoconf automake autopoint pkg-config \
  libncurses-dev ncurses-term libx11-dev libxext-dev libxft-dev \
  libfontconfig1-dev libcairo2-dev libpango1.0-dev libfreetype-dev \
  libvterm-dev libjson-c-dev zlib1g-dev libgpm-dev libssl-dev \
  libwayland-dev wayland-protocols libwayland-bin libxkbcommon-dev \
  xvfb x11-utils x11-xserver-utils xauth xdotool x11-apps netpbm \
  matchbox-window-manager imagemagick weston wl-clipboard \
  gdb delve scala ga68 algol68g locales \
  python3 python3-pip python3-venv python3-pil
sudo locale-gen de_DE.UTF-8
