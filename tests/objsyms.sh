# objsyms.sh -- the one reader of an object's symbols, sourced by the check
# scripts.
#
# No check script calls nm with an option beyond -g or -u, and none parses
# nm's lines itself: each system prints them its own way, and a script that
# guessed one way saw everything on its own system and nothing on the next.
#
# The undefined side, as each system's nm -u prints it:
#   GNU, FreeBSD, NetBSD:   "                 U poll"   or  "0000 U poll"
#   llvm:                   "---------------- U poll"
#   a bare "U" column:      "U poll"
#   OpenBSD, macOS:         "poll"                       (the name alone)
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
#
# The defined side, as nm -g prints it ("0000000000000000 T main"; the
# address is blank or dashes on an undefined line, and a weak undefined
# symbol, "w name" or "v name", has no address either):
#
#   nm_defined_names < output of nm -g
#
# Prints the symbols the object defines, of any type letter but U, with a
# "@version" or "$suffix" taken off, and the same name without a leading "_".
# A line with no hex address is not a definition.
#
#   symbol_source_name NAME
#
# The name a libc-private spelling stands for, so that an allow file names
# the call the C source makes and not each system's spelling of it:
#   __errno_location, __error, __errno       -> errno_location
#   NetBSD __NAME13/14/30/50/60 (a versioned
#   system call, __fstat50, __sigprocmask14)  -> NAME
#   __sigaction_siginfo                      -> sigaction
# A fortified twin (__memcpy_chk, __open_2) is a different entry point with
# its own allow line and is NOT folded.
#
#   section_kind SECTION
#
# How a reader that follows relocations treats the section a relocation
# points into: "skip" (holds no code and no state), "visit" (read-only or
# code: follow what it refers to), "data" (a variable) or "unknown" (a kind
# this table does not know, which is a problem and never a pass).  Only the
# OpenBSD retguard cookie sections are skipped: .openbsd.randomdata proper
# holds writable random-initialised data and is judged as data.
#
# The two judgements above are written once, as awk functions in
# OBJSYMS_AWK_LIB, so a script that reads relocations in awk calls the same
# table as the shell wrappers do:
#   awk "$OBJSYMS_AWK_LIB"' ... objsyms_source_name(n) ... objsyms_section_kind(s) ... '

OBJSYMS_AWK_LIB='
function objsyms_source_name(n,   m) {
  if (n == "__errno_location" || n == "__error" || n == "__errno")
    return "errno_location"
  if (n == "__sigaction_siginfo") return "sigaction"
  if (n ~ /^__[a-z_]+(13|14|30|50|60)$/) {
    m = n
    sub(/^__/, "", m)
    sub(/(13|14|30|50|60)$/, "", m)
    return m
  }
  return n
}
function objsyms_section_kind(s) {
  if (s ~ /^\.text/) return "visit"
  if (s ~ /^\.rodata\.str/) return "skip"
  if (s ~ /^\.openbsd\.randomdata\.retguard\./) return "skip"
  if (s ~ /^\.data\.rel\.ro/ || s ~ /^\.rodata/) return "visit"
  if (s ~ /^\.(data|bss|tdata|tbss|openbsd\.randomdata)/) return "data"
  return "unknown"
}'

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

nm_defined_names() {
  awk '
    /:$/ { next }
    NF >= 3 && $(NF-1) ~ /^[A-TV-Zb-tv-z]$/ && $(NF-2) ~ /^[0-9a-fA-F]+$/ {
      n = $NF
      sub(/[@$].*/, "", n)
      print n
      if (n ~ /^_/) { m = n; sub(/^_/, "", m); print m }
    }'
}

symbol_source_name() {
  awk -v n="$1" "$OBJSYMS_AWK_LIB"' BEGIN { print objsyms_source_name(n) }'
}

section_kind() {
  awk -v s="$1" "$OBJSYMS_AWK_LIB"' BEGIN { print objsyms_section_kind(s) }'
}
