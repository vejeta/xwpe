#!/bin/sh
# Install the packaged xwpe from OBS and smoke-test the console binary.
set -eu

OBS_REPO="https://download.opensuse.org/repositories/home:/vejeta/openSUSE_Tumbleweed/"
PROBE_URL="https://codeberg.org/mendezr/xwpe/raw/branch/main/contrib/bsd-test/sgr_mouse_probe.c"

echo "=== make the (rolling) Tumbleweed box consistent first ==="
# A months-old Tumbleweed box is mid-snapshot: installing anything pulls newer
# libs that mismatch the box's older ones (e.g. libgobject vs libglib symbol
# skew that crashes glib-compile-schemas).  A full dup realigns the snapshot so
# the package install -- and wpe's pango/glib link at runtime -- are coherent.
zypper --non-interactive --gpg-auto-import-keys refresh
zypper --non-interactive dup --auto-agree-with-licenses --allow-vendor-change --no-recommends

echo "=== add the maintainer OBS repo and install the xwpe package ==="
zypper --non-interactive removerepo home_vejeta 2>/dev/null || true
zypper --non-interactive addrepo -fc "${OBS_REPO}" home_vejeta
zypper --non-interactive --gpg-auto-import-keys refresh
zypper --non-interactive install --allow-vendor-change xwpe gcc

echo "=== installed package + entry points ==="
rpm -q xwpe
for b in we wpe xwpe xwe; do command -v "$b" || { echo "MISSING entry point: $b"; exit 1; }; done

echo "=== build the forkpty mouse probe and run it against the INSTALLED /usr/bin/wpe ==="
# The same probe the BSD harness uses: launches wpe under a pty, injects an SGR
# (1006) mouse click on the File menu, and checks the menu opens.  openpty() is
# in libutil on glibc, so link -lutil as on the BSDs.
curl -fsSL -o /tmp/sgr_mouse_probe.c "${PROBE_URL}"
cc -O2 -o /tmp/probe /tmp/sgr_mouse_probe.c -lutil

rc=0
for term in xterm-256color xterm-direct vt220; do
  echo "--- probe TERM=${term} ---"
  if /tmp/probe /usr/bin/wpe "${term}"; then
    echo "OPENSUSE PROBE OK (${term})"
  else
    echo "OPENSUSE PROBE FAILED (${term})"
    rc=1
  fi
done

echo "=== result ==="
if [ "${rc}" -eq 0 ]; then
  echo "OPENSUSE RUNTIME OK: packaged wpe launches and the console menu responds"
else
  echo "OPENSUSE RUNTIME FAILED"
fi
exit "${rc}"
