#!/bin/sh
# check-ci-versions.sh -- the CI workflows say what tests/ci-versions.txt says.
#
# One record (tests/ci-versions.txt) names every version the pipelines run:
# runner labels, BSD releases, actions (tag and commit), pyte, the tools
# tests/ci-install.sh builds at a tag, and the BSD package names that carry a
# version.  A migration edits the record and the workflows in one change; this
# script keeps them equal, offline.
#
#   check-ci-versions.sh [--srcdir DIR] [--selftest] [--max-age DAYS]
#
# No option: the canaries, then the check of the tree.
# --selftest   the canaries only.
# --max-age N  only the freshness of the record: fail when its oldest "date
#              checked" is more than N days before today.  Run by the
#              record-fresh job of codeql.yml, never by make check, so a
#              tarball built long ago does not fail because time passed.
#
# Fails on: a "-latest" runner label; a version in a workflow or in ci-run.sh
# older than, or different from, the record; two versions of one action; an
# action not pinned by a 40-digit commit with a "# vX.Y.Z" comment; the two
# macOS runner lists of macos.yml unequal; a BSD job whose name or release
# input does not come from its matrix.release; a hard-coded Python minor
# version in ci-run.sh; a tool ci-install.sh builds at another tag than the
# record's, or does not build at the recorded one; a record line without its
# fields.
# Not seen: a release or tag NEWER than the record (that needs the network;
# the record's date and --max-age make it visible instead).
#
# Exit: 0 clean, 1 finding or a wrong canary, 77 no .github (a source tarball
# does not carry it).

SRCDIR=${srcdir:-.}
MODE=all
MAXAGE=
while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) MODE=selftest ;;
    --max-age) shift; MODE=age; MAXAGE=$1 ;;
    *) echo "check-ci-versions: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done

. "$(dirname "$0")/workdir.sh"
work_dir civer 1
ERR=$WORK/findings
: > "$ERR"

# Names of the record items every tree must have.
REQUIRED="runner-ubuntu runner-macos-arm runner-macos-intel bsd-freebsd
bsd-netbsd bsd-openbsd pyte pytest pytest-xdist pytest-rerunfailures delve
pkg-freebsd-python pkg-openbsd-python
pkg-openbsd-autoconf pkg-openbsd-automake pkg-netbsd-python"
# The Python packages a workflow or script pins with "==".
PINNED="pyte pytest pytest-xdist pytest-rerunfailures"
# The programs tests/ci-install.sh builds at a tag, as "record-item:program".
BUILT="delve:dlv"

# fail FILE MESSAGE: record one finding.
fail() {
  echo "$1: $2" >> "$ERR"
}

# rec NAME FIELD: print one field of the record's line for NAME.
rec() {
  awk -F'|' -v n="$1" -v f="$2" '$0 !~ /^#/ && $1 == n { print $f; exit }' "$RECORD"
}

# vnum TEXT: the dotted number inside a label ("ubuntu-26.04" -> "26.04",
# "v7.0.1" -> "7.0.1", "py312" -> "312").
vnum() {
  printf '%s' "$1" | sed 's/[^0-9.]//g'
}

# vcmp A B: print -1, 0 or 1 comparing two dotted numbers part by part.
vcmp() {
  awk -v a="$1" -v b="$2" 'BEGIN {
    na = split(a, x, "."); nb = split(b, y, ".")
    n = (na > nb) ? na : nb
    for (i = 1; i <= n; i++) {
      p = x[i] + 0; q = y[i] + 0
      if (p < q) { print -1; exit }
      if (p > q) { print 1; exit }
    }
    print 0
  }'
}

# judge FILE WHAT FOUND RECORDED: a workflow version must equal the record; say
# whether it is older or just different.
judge() {
  j_cmp=$(vcmp "$(vnum "$3")" "$(vnum "$4")")
  if [ "$j_cmp" = -1 ]; then
    fail "$1" "$2 $3 is older than the record ($4)"
  elif [ "$j_cmp" = 1 ] || [ "$3" != "$4" ]; then
    fail "$1" "$2 $3 differs from the record ($4); update tests/ci-versions.txt"
  fi
}

# check_record_lines: every record line has its six fields, a date and a URL.
check_record_lines() {
  awk -F'|' '$0 !~ /^#/ && NF > 0 {
    if (NF != 6) print "line " NR ": " NF " fields, six are needed"
    else if ($5 !~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$/) print "line " NR ": bad date " $5
    else if ($4 !~ /^https?:/) print "line " NR ": the source is not a URL"
  }' "$RECORD" | while IFS= read -r l; do fail "$RECORD" "$l"; done
  for r_name in $REQUIRED; do
    [ -n "$(rec "$r_name" 2)" ] || fail "$RECORD" "no line for $r_name"
  done
}

# check_runner_labels FILE: each runs-on and matrix runner label is the
# recorded one, never a "-latest" label.
check_runner_labels() {
  grep -nE '^[[:space:]]*(runs-on:|- runner:)' "$1" | while IFS= read -r c_line; do
    c_val=$(printf '%s' "$c_line" \
      | sed -E 's/^[0-9]+:[[:space:]]*(runs-on:|- runner:)[[:space:]]*//; s/[[:space:]]*#.*$//; s/[[:space:]]+$//')
    case $c_val in
      *'${{'*) ;;
      *-latest) fail "$1" "runner label $c_val is a -latest label; name the version" ;;
      ubuntu-*) judge "$1" "runner" "$c_val" "$(rec runner-ubuntu 2)" ;;
      macos-*-intel) judge "$1" "runner" "$c_val" "$(rec runner-macos-intel 2)" ;;
      macos-*) judge "$1" "runner" "$c_val" "$(rec runner-macos-arm 2)" ;;
      *) fail "$1" "runner label $c_val is not one the record knows" ;;
    esac
  done
}

# check_macos_lists FILE: every matrix runner list in the file is the same.
check_macos_lists() {
  c_n=$(awk '/^[[:space:]]*- runner:/ {
      if (NR != last + 1) { if (blk != "") print blk; blk = "" }
      blk = blk $3 ","; last = NR
    }
    END { if (blk != "") print blk }' "$1" | sort -u | wc -l)
  if [ "$c_n" -gt 1 ]; then
    fail "$1" "the runner lists differ between the jobs"
  fi
}

# collect_uses FILE: append "file|action|ref|tag" for each uses: line, and
# report the lines that are not pinned the one way.
collect_uses() {
  u_file=$1
  grep -nE '^[[:space:]]*(- )?uses:' "$u_file" | while IFS= read -r u_line; do
    u_spec=$(printf '%s' "$u_line" | sed -E 's/^[0-9]+:[[:space:]]*(- )?uses:[[:space:]]*//')
    case $u_spec in ./*) continue ;; esac
    case $u_spec in
      *@*) ;;
      *) fail "$u_file" "uses: $u_spec has no @ref"; continue ;;
    esac
    u_path=${u_spec%%@*}
    u_rest=${u_spec#*@}
    set -- $u_rest
    u_ref=$1
    u_tag=
    [ "$2" = "#" ] && u_tag=$3
    u_id=$(printf '%s' "$u_path" | cut -d/ -f1-2)
    if ! printf '%s' "$u_ref" | grep -qE '^[0-9a-f]{40}$'; then
      fail "$u_file" "$u_id@$u_ref is not pinned by a commit SHA"
      continue
    fi
    if ! printf '%s' "$u_tag" | grep -qE '^v[0-9]+(\.[0-9]+)*$'; then
      fail "$u_file" "$u_id@$u_ref has no '# vX.Y.Z' comment"
      continue
    fi
    echo "$u_file|$u_id|$u_ref|$u_tag" >> "$WORK/uses"
  done
}

# judge_actions: one version per action across all files, and the record's.
judge_actions() {
  [ -f "$WORK/uses" ] || return 0
  for j_id in $(cut -d'|' -f2 "$WORK/uses" | sort -u); do
    j_n=$(awk -F'|' -v id="$j_id" '$2 == id { print $3 " " $4 }' "$WORK/uses" | sort -u | wc -l)
    if [ "$j_n" -gt 1 ]; then
      fail ".github/workflows" "two versions of $j_id are in use"
    fi
    j_tag=$(rec "action:$j_id" 2)
    j_sha=$(rec "action:$j_id" 3)
    if [ -z "$j_tag" ]; then
      fail "$RECORD" "no line for action:$j_id"
      continue
    fi
    awk -F'|' -v id="$j_id" '$2 == id { print $1 " " $3 " " $4 }' "$WORK/uses" | sort -u \
      | while read -r j_file j_ref j_t; do
          judge "$j_file" "$j_id" "$j_t" "$j_tag"
          [ "$j_ref" = "$j_sha" ] || fail "$j_file" "$j_id commit $j_ref is not the recorded $j_sha"
        done
  done
}

# check_bsd_jobs FILE: each BSD job takes its release from its matrix, in the
# job name and in the action's release input, and the matrix equals the record.
check_bsd_jobs() {
  awk '
    /^  [a-z_]+:[ \t]*$/ { job = $1; sub(":", "", job); next }
    job ~ /^(freebsd|openbsd|netbsd)$/ && /^    name:/ { print job "|name|" $0 }
    job ~ /^(freebsd|openbsd|netbsd)$/ && /^        release:/ { print job "|matrix|" $0 }
    job ~ /^(freebsd|openbsd|netbsd)$/ && /^          release:/ { print job "|input|" $0 }
  ' "$1" > "$WORK/bsdjobs"
  for b_os in freebsd openbsd netbsd; do
    b_name=$(awk -F'|' -v o="$b_os" '$1 == o && $2 == "name"' "$WORK/bsdjobs")
    b_matrix=$(awk -F'|' -v o="$b_os" '$1 == o && $2 == "matrix" { print $3 }' "$WORK/bsdjobs")
    b_input=$(awk -F'|' -v o="$b_os" '$1 == o && $2 == "input" { print $3 }' "$WORK/bsdjobs")
    case $b_name in
      *'matrix.release'*) ;;
      *) fail "$1" "$b_os: the job name does not take its version from matrix.release" ;;
    esac
    case $b_input in
      *'matrix.release'*) ;;
      *) fail "$1" "$b_os: the release input is hard-coded; use matrix.release" ;;
    esac
    b_ver=$(printf '%s' "$b_matrix" | sed -E 's/.*\[[[:space:]]*"?([0-9.]+)"?[[:space:]]*\].*/\1/')
    if [ -z "$b_matrix" ]; then
      fail "$1" "$b_os: no one-element matrix.release"
    else
      judge "$1" "$b_os release" "$b_ver" "$(rec "bsd-$b_os" 2)"
    fi
  done
}

# check_bsd_packages FILE: the package names that carry a version are the
# recorded ones (every autoconf%, automake%, python% token and the py3NN set).
check_bsd_packages() {
  p_ac=$(rec pkg-openbsd-autoconf 2); p_am=$(rec pkg-openbsd-automake 2)
  p_op=$(rec pkg-openbsd-python 2); p_np=$(rec pkg-netbsd-python 2)
  p_fp=$(rec pkg-freebsd-python 2)
  for p_pair in "autoconf%:$p_ac" "automake%:$p_am" "python%:$p_op"; do
    p_stem=${p_pair%%:*}; p_want=${p_pair#*:}
    grep -oE "${p_stem}[0-9.]+" "$1" | while read -r p_tok; do
      judge "$1" "package $p_stem" "${p_tok#"$p_stem"}" "$p_want"
    done
    grep -qE "${p_stem}${p_want}([^0-9.]|$)" "$1" || fail "$1" "no ${p_stem}${p_want} (the recorded package)"
  done
  for p_name in "python$p_np" "py$p_np-test" "py$p_np-pyte" "py$p_np-wcwidth" \
                "${p_fp}-pytest" "${p_fp}-pyte" "${p_fp}-wcwidth"; do
    grep -qE "(^|[^a-z0-9])${p_name}([^a-z0-9]|$)" "$1" || fail "$1" "no package $p_name (the recorded one)"
  done
}

# check_pyte FILES: every pin of pyte and of the pytest packages is the
# recorded version.  A name is matched up to its "==", so pytest does not
# match pytest-xdist.
check_pyte() {
  for y_file in "$@"; do
    [ -f "$y_file" ] || continue
    for y_name in $PINNED; do
      grep -oE "(^|[^a-z-])${y_name}==[0-9.]+" "$y_file" | while read -r y_tok; do
        y_tok=${y_tok#[!a-z]}
        judge "$y_file" "$y_name" "${y_tok#"$y_name"==}" "$(rec "$y_name" 2)"
      done
    done
  done
}

# check_built_pins FILE: every tool the record says the job builds is built at
# the recorded tag, and is built at all.  A tag is read after its program's
# name, so `dlv@v1.27.1` is the record's delve.
check_built_pins() {
  [ -f "$1" ] || return 0
  for b_pair in $BUILT; do
    b_name=${b_pair%%:*}; b_prog=${b_pair#*:}
    b_want=$(rec "$b_name" 2)
    grep -oE "${b_prog}@v[0-9.]+" "$1" | while read -r b_tok; do
      judge "$1" "$b_name" "${b_tok#"$b_prog"@v}" "$b_want"
    done
    grep -qE "${b_prog}@v${b_want}([^0-9.]|$)" "$1" \
      || fail "$1" "no ${b_prog}@v${b_want} (the recorded $b_name)"
  done
}

# check_ci_run FILE: no Python minor version is written into the BSD script.
check_ci_run() {
  if grep -nE '^[^#]*python3\.[0-9]' "$1" > "$WORK/pyhard"; then
    fail "$1" "hard-coded python minor version: $(head -1 "$WORK/pyhard")"
  fi
}

# check_tree ROOT: run every rule over the tree at ROOT.  Returns 1 when
# anything was found.  The record is ROOT/tests/ci-versions.txt.
check_tree() {
  T=$1
  RECORD=$T/tests/ci-versions.txt
  : > "$ERR"; rm -f "$WORK/uses"
  if [ ! -f "$RECORD" ]; then
    fail "$RECORD" "the record is missing"
    return 1
  fi
  check_record_lines
  for t_wf in "$T"/.github/workflows/*.yml; do
    check_runner_labels "$t_wf"
    collect_uses "$t_wf"
  done
  check_macos_lists "$T/.github/workflows/macos.yml"
  judge_actions
  check_bsd_jobs "$T/.github/workflows/bsd.yml"
  check_bsd_packages "$T/.github/workflows/bsd.yml"
  check_pyte "$T"/.github/workflows/*.yml "$T/tests/run-tests.sh" "$T/tests/ci-venv.sh"
  check_built_pins "$T/tests/ci-install.sh"
  check_ci_run "$T/contrib/bsd-test/ci-run.sh"
  [ ! -s "$ERR" ]
}

# days_of DATE: a day number for YYYY-MM-DD, good for differences (1900-2100).
days_of() {
  awk -v d="$1" 'BEGIN {
    split(d, a, "-"); y = a[1] + 0; m = a[2] + 0; dd = a[3] + 0
    if (m <= 2) { y--; m += 12 }
    print int(365.25 * y) + int(30.6001 * (m + 1)) + dd
  }'
}

# check_age ROOT DAYS: the oldest date in ROOT's record is at most DAYS days
# before today (CI_VERSIONS_TODAY, YYYY-MM-DD, replaces the clock in tests).
check_age() {
  RECORD=$1/tests/ci-versions.txt
  : > "$ERR"
  if [ ! -f "$RECORD" ]; then
    fail "$RECORD" "the record is missing"
    return 1
  fi
  a_old=$(awk -F'|' '$0 !~ /^#/ && NF == 6 { print $5 }' "$RECORD" | sort | head -1)
  a_today=${CI_VERSIONS_TODAY:-$(date +%Y-%m-%d)}
  a_age=$(( $(days_of "$a_today") - $(days_of "$a_old") ))
  if [ "$a_age" -gt "$2" ]; then
    fail "$RECORD" "the oldest date checked is $a_old, $a_age days before $a_today; limit $2. Re-check every item against its source and update the record"
    return 1
  fi
  return 0
}

# show_findings: print what the last run found.
show_findings() {
  sed 's/^/check-ci-versions: /' "$ERR" >&2
}

# canary NAME FILE SED-EXPR WANT: copy the tree, apply one mutation to FILE,
# and require the check to fail naming WANT.
canary() {
  rm -rf "$WORK/m"
  cp -R "$WORK/base" "$WORK/m"
  sed "$3" "$WORK/base/$2" > "$WORK/m/$2.new"
  mv "$WORK/m/$2.new" "$WORK/m/$2"
  if cmp -s "$WORK/base/$2" "$WORK/m/$2"; then
    echo "check-ci-versions: canary $1 changed nothing" >&2
    SELF_BAD=1
    return
  fi
  if check_tree "$WORK/m"; then
    echo "check-ci-versions: canary $1 was not refused" >&2
    SELF_BAD=1
  elif ! grep -qF -- "$4" "$ERR"; then
    echo "check-ci-versions: canary $1 was refused, but not for '$4':" >&2
    show_findings
    SELF_BAD=1
  fi
}

# selftest: the unmodified copy of the tree passes, and each planted
# offender is refused for its own reason.
selftest() {
  SELF_BAD=0
  rm -rf "$WORK/base"
  mkdir -p "$WORK/base/.github" "$WORK/base/tests" "$WORK/base/contrib"
  cp -R "$SRCDIR/.github/workflows" "$WORK/base/.github/workflows"
  cp "$SRCDIR/tests/ci-versions.txt" "$SRCDIR/tests/run-tests.sh" \
     "$SRCDIR/tests/ci-venv.sh" "$SRCDIR/tests/ci-install.sh" "$WORK/base/tests/"
  cp -R "$SRCDIR/contrib/bsd-test" "$WORK/base/contrib/bsd-test"
  if ! check_tree "$WORK/base"; then
    echo "check-ci-versions: the tree itself is refused:" >&2
    show_findings
    SELF_BAD=1
  fi
  W=.github/workflows
  canary latest-label $W/linux.yml 's/runs-on: ubuntu-26.04/runs-on: ubuntu-latest/' "is a -latest label"
  canary older-runner $W/codeql.yml 's/runs-on: ubuntu-26.04/runs-on: ubuntu-24.04/' "is older than the record"
  canary two-versions $W/bsd.yml 's|actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2|actions/upload-artifact@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # v7.0.1|' "two versions of actions/upload-artifact"
  canary floating-tag $W/linux.yml 's|actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1|actions/checkout@v7|' "not pinned by a commit SHA"
  canary no-comment $W/codeql.yml 's|@33d7b85da1a04171e211befe2c739303b9a4c066 # v4.39.3|@33d7b85da1a04171e211befe2c739303b9a4c066|' "no '# vX.Y.Z' comment"
  canary macos-lists $W/macos.yml '1,/runs-on: \${{ matrix.runner }}/ s/macos-26-intel/macos-15-intel/' "runner lists differ"
  canary job-name $W/bsd.yml 's/name: FreeBSD \${{ matrix.release }}/name: FreeBSD 15/' "does not take its version from matrix.release"
  canary release-input $W/bsd.yml 's/^          release: \${{ matrix.release }}/          release: "8.0"/' "release input is hard-coded"
  canary old-release $W/bsd.yml 's/release: \["15.1"\]/release: ["14.3"]/' "is older than the record"
  canary old-package $W/bsd.yml 's/autoconf%2.73/autoconf%2.72/' "package autoconf%"
  canary old-pyte tests/run-tests.sh 's/pyte==0.8.2/pyte==0.8.1/' "pyte 0.8.1 is older"
  canary old-pytest tests/ci-venv.sh 's/pytest==9.1.1/pytest==9.0.0/' "pytest 9.0.0 is older"
  canary old-xdist tests/ci-venv.sh 's/pytest-xdist==3.8.0/pytest-xdist==3.7.0/' "pytest-xdist 3.7.0 is older"
  canary old-rerun tests/ci-venv.sh 's/pytest-rerunfailures==16.7/pytest-rerunfailures==16.6/' "pytest-rerunfailures 16.6 is older"
  canary old-delve tests/ci-install.sh 's|dlv@v1.27.1|dlv@v1.24.0|' "delve 1.24.0 is older"
  canary no-delve tests/ci-install.sh 's|go install github.com/go-delve/delve/cmd/dlv@v1.27.1|true|' "no dlv@v1.27.1"
  canary hard-python contrib/bsd-test/ci-run.sh 's/PY=\$(find_python)/PY=python3.12/' "hard-coded python minor"
  canary record-item tests/ci-versions.txt '/^pyte|/d' "no line for pyte"
  canary record-fields tests/ci-versions.txt 's/^bsd-netbsd|11.1|-|/bsd-netbsd|11.1|/' "fields, six are needed"
  selftest_age
  selftest_no_workflows
  return $SELF_BAD
}

# selftest_age: a fresh record passes --max-age, a stale one is refused.
selftest_age() {
  if ! CI_VERSIONS_TODAY=2026-11-20 check_age "$WORK/base" 45; then
    echo "check-ci-versions: a one-day-old record was refused" >&2
    SELF_BAD=1
  fi
  if CI_VERSIONS_TODAY=2027-02-01 check_age "$WORK/base" 45; then
    echo "check-ci-versions: a record 74 days old was accepted" >&2
    SELF_BAD=1
  fi
}

# selftest_no_workflows: a tree without .github is skipped with exit 77.
selftest_no_workflows() {
  mkdir -p "$WORK/empty"
  sh "$0" --srcdir "$WORK/empty" > /dev/null 2>&1
  if [ $? -ne 77 ]; then
    echo "check-ci-versions: a tree without .github did not exit 77" >&2
    SELF_BAD=1
  fi
}

if [ "$MODE" = age ]; then
  if check_age "$SRCDIR" "$MAXAGE"; then
    echo "check-ci-versions: the record is at most $MAXAGE days old"
    exit 0
  fi
  show_findings
  exit 1
fi

if [ ! -d "$SRCDIR/.github/workflows" ]; then
  echo "check-ci-versions: no .github/workflows in $SRCDIR (a source tarball); skipped"
  exit 77
fi

rc=0
selftest || rc=1
if [ "$MODE" = all ]; then
  if check_tree "$SRCDIR"; then
    echo "check-ci-versions: workflows, ci-run.sh, ci-install.sh and run-tests.sh match tests/ci-versions.txt"
  else
    show_findings
    rc=1
  fi
fi
exit $rc
