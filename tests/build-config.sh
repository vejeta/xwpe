# build-config.sh -- what this build contains, asked of the binary once.
#
# Sourced by the check scripts that scan the editor's objects.  The one
# answer to "does this build have X" is the line `we --version` prints:
#
#   built with: enable-ai=yes with-x=no libvterm=no ...
#
# configure states each fact once; the binary says it; this file reads it.
# A script never reads config.h, an object's presence or a string in the
# binary to learn a configure answer.  The words are configure's options
# without their dashes (enable-ai, with-wayland) or a library's name
# (libvterm, cairo); wpe_option_of turns one back into what the user typed.
#
# Needs WPE_BUILD_BINARY (`make check` sets it; a hand run may leave it, and
# ./we is used).  WPE_FEATURE_SOURCES ("word:src.c src.c;word:...", set by
# `make check`) says which sources belong to which word.
#
# Sourcing prints the line (not when WPE_BUILD_QUIET is set, for a script
# whose output is a list), so the log of a script that fails carries the
# build it failed on, and exits 99 when the binary cannot tell: a binary
# that does not start is one cause, shown here, not a dozen failures.

wpe_bc_binary=${WPE_BUILD_BINARY:-./we}

# The binary is run with leak checking off: a sanitizer build may exit
# non-zero at exit with the right text on stdout, and only the text is read.
_wpe_version_out=$(ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" \
  "$wpe_bc_binary" --version 2>/dev/null </dev/null)
_wpe_features=$(printf '%s\n' "$_wpe_version_out" | sed -n 's/^built with: *//p' | head -n 1)
if [ -z "$_wpe_features" ]; then
  echo "build-config: $wpe_bc_binary printed no 'built with:' line from --version (not built yet, or not this tree's editor)" >&2
  exit 99
fi
[ -n "$WPE_BUILD_QUIET" ] || echo "built with: $_wpe_features"

# wpe_built WORD -- 0 when the build has WORD, 1 when it does not; exit 99
# for a word the binary does not print.
wpe_built() {
  case " $_wpe_features " in
    *" $1=yes "*) return 0 ;;
    *" $1=no "*) return 1 ;;
  esac
  echo "build-config: $wpe_bc_binary does not say whether it has '$1'" >&2
  exit 99
}

# wpe_built_with -- the whole "word=yes word=no ..." line, for a check
# written in another language that judges the words itself.
wpe_built_with() {
  printf '%s\n' "$_wpe_features"
}

# wpe_option_of WORD -- what the user typed: --enable-ai, --with-x; a
# library's name stays as it is.
wpe_option_of() {
  case $1 in
    enable-*|with-*) echo "--$1" ;;
    *) echo "$1" ;;
  esac
}

# wpe_not_built_reason WORD -- "not built here: --enable-ai", the wording of
# every skip and every note that comes from a feature the build lacks.
wpe_not_built_reason() {
  echo "not built here: $(wpe_option_of "$1")"
}

# wpe_feature_of_object OBJECT -- the word whose source list holds the
# object's source, or nothing for an object every build has.
wpe_feature_of_object() {
  _wpe_src=${1%.o}.c
  printf '%s\n' "$WPE_FEATURE_SOURCES" | tr ';' '\n' |
    while IFS=: read -r _wpe_word _wpe_list; do
      case " $_wpe_list " in
        *" $_wpe_src "*) echo "$_wpe_word"; break ;;
      esac
    done
}

# wpe_object_of NAME LIST -- the one judgement of an object that is named
# but may be absent.  Prints the path of NAME among the objects of LIST, or
# "not-built WORD" when NAME belongs to a feature the build does not have,
# or "unknown": an object every build has, or a feature the build has, is
# not in the list, which means the list is wrong and the scan is blind.
wpe_object_of() {
  for _wpe_o in $2; do
    case $(basename "$_wpe_o") in
      "$1"|we-"$1") echo "$_wpe_o"; return 0 ;;
    esac
  done
  _wpe_word=$(wpe_feature_of_object "$1")
  if [ -n "$_wpe_word" ] && ! wpe_built "$_wpe_word"; then
    echo "not-built $_wpe_word"
  else
    echo unknown
  fi
}

# wpe_not_built_note FILE... -- "; N line(s) not built here: --a, --b" for
# the words listed one per line in the files, nothing for none: the end of
# a script's ok line.
wpe_not_built_note() {
  _wpe_words=$(cat "$@" 2>/dev/null | sort -u)
  [ -n "$_wpe_words" ] || return 0
  _wpe_n=$(cat "$@" 2>/dev/null | wc -l | tr -d ' ')
  _wpe_opts=
  for _wpe_word in $_wpe_words; do
    _wpe_opts="${_wpe_opts:+$_wpe_opts, }$(wpe_option_of "$_wpe_word")"
  done
  printf '; %s line(s) not built here: %s' "$_wpe_n" "$_wpe_opts"
}
