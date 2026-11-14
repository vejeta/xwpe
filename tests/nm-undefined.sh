# nm-undefined.sh -- the one reader of `nm -u`, sourced by the check scripts.
#
# Each system's nm prints the undefined symbols of an object its own way:
#   GNU, FreeBSD, NetBSD:   "                 U poll"   or  "0000 U poll"
#   a bare "U" column:      "U poll"
#   OpenBSD, macOS:         "poll"                       (the name alone)
# A script that kept only the lines with "U" in the second-to-last field saw
# every symbol on the first and nothing on the last, so its canaries fired
# and its allow lines read as stale.  nm_undefined_names takes any of them.
#
#   nm_undefined_names < output of nm -u
#
# Prints one symbol per line, as nm spelt it (versions are the caller's to
# strip).  A "file.o:" header line is skipped.
#
#   nm_undefined_source_names < output of nm -u
#
# The same names as the C source spells them: Mach-O puts a "_" before every
# C name ("_poll"), so a name with a leading "_" is printed a second time
# without it.  On ELF that only adds a spelling no object imports.

nm_undefined_names() {
  awk '
    /:$/ { next }
    NF == 1 { print $1; next }
    NF >= 2 && $(NF-1) == "U" { print $NF }'
}

nm_undefined_source_names() {
  nm_undefined_names | awk '
    { print }
    /^_/ { n = $0; sub(/^_/, "", n); print n }'
}
