#!/bin/sh
# Random programs in every mode.  progen.scm (Guile) writes a program
# for each seed; it is run loaded from source (each form evaluated as
# it is read), compiled to bytecode, compiled natively (mixed mode),
# and built by oakc as stand-alone executables (separate, -w, --ir).
# Every mode must print the "=> " lines the run from source prints.
# Every run has a small heap (--size-heap 128), so that collections
# come in the middle of things.  A failing program's directory is kept,
# with every mode's output and, cut down by fuzz-shrink.sh for the
# first mode that disagreed, p.oak.min (FUZZNOSHRINK=yes skips that).
#
#   sh tests/fuzz.sh [-n COUNT] [-s FIRST-SEED] [-j JOBS] [-d DIR] [-k]
#
# COUNT programs (default 10) from seed FIRST-SEED (default 1), JOBS at
# a time (default 1), in DIR (default ./fuzz); -k keeps every program's
# directory, not just a failing one's.  OAKLISP, OAKWORLD,
# GUILE and the rest come from testlib.sh, as for the tests.  Exits 1
# if any program failed.  "fuzz.sh --one SEED" runs one program.

. "$(dirname "$0")/testlib.sh"
: ${GUILE:=guile}
test "$GUILE" = no && GUILE=guile
OAKC=$top_builddir/oakc
self=$testdir/fuzz.sh

# The "=> " lines a mode printed.
results() { grep -a -o '=> .*' "$1"; }

run_one() {
  seed=$1
  d=$FUZZDIR/s$seed
  rm -rf "$d" && mkdir -p "$d/src" "$d/bc" "$d/nat" "$d/exe" || exit 1
  "$GUILE" --no-auto-compile "$testdir/progen.scm" "$seed" "$d/p.oak" > "$d/progen.log" 2>&1 \
    || { echo "FAIL $seed: progen.scm failed"; return 1; }
  for m in src bc nat exe; do cp "$d/p.oak" "$d/$m/"; done

  (cd "$d/src" && limit 300 "$OAKLISP" --batch --size-heap 128 --world "$OAKWORLD" -- \
     --locale system-locale --load p --exit > ../src.out 2>&1)
  results "$d/src.out" > "$d/src.res"
  test -s "$d/src.res" || { echo "FAIL $seed: no results from source"; return 1; }

  fail=
  for m in bc nat; do
    case $m in bc) how=compile-file;; nat) how=native-compile-file;; esac
    # A program's generated C can run to a megabyte, which the C
    # compiler takes minutes over on a loaded machine.
    (cd "$d/$m" && limit 1200 "$OAKLISP" --batch --size-heap 128 --world "$OAKWORLD" -- --locale system-locale \
       --eval "(bind ((#*compiler-noisiness 0)) ($how system-locale \"p\"))" --exit \
       > compile.log 2>&1)
    if test ! -f "$d/$m/p.oa"; then fail="$fail $m(compile)"; continue; fi
    if test $m = nat && test ! -f "$d/$m/p.so"; then continue; fi
    (cd "$d/$m" && limit 300 "$OAKLISP" --batch --size-heap 128 --world "$OAKWORLD" -- \
       --locale system-locale --load p --exit > ../$m.out 2>&1)
    results "$d/$m.out" > "$d/$m.res"
    cmp -s "$d/src.res" "$d/$m.res" || fail="$fail $m"
  done

  if test -x "$OAKC" && test -f "$top_builddir/src/world/native/liboaknat.a"; then
    for m in exe exe-w exe-ir; do
      case $m in exe) flags=;; exe-w) flags=-w;; exe-ir) flags=--ir;; esac
      if (cd "$d/exe" && limit 1200 "$OAKC" $flags p.oak -o $m > $m.oakc.log 2>&1); then
	(cd "$d/exe" && OAKLISP_OPTIONS="--size-heap 128" OAKWORLD=/nonexistent limit 300 ./$m > ../$m.out 2>&1)
	results "$d/$m.out" > "$d/$m.res"
	cmp -s "$d/src.res" "$d/$m.res" || fail="$fail $m"
      else
	fail="$fail $m(oakc)"
      fi
    done
  fi

  if test -n "$fail"; then
    # Cut the program down for the first mode that ran and disagreed.
    for m in $fail; do
      case $m in *\(*) continue;; esac
      test -n "$FUZZNOSHRINK" \
	|| sh "$testdir/fuzz-shrink.sh" "$d/p.oak" $m > "$d/shrink.log" 2>&1
      break
    done
    echo "FAIL $seed:$fail"
    return 1
  fi
  echo "ok $seed ($(wc -l < "$d/src.res") results in $(ls "$d"/*.res | wc -l) modes)"
  test -n "$FUZZKEEP" || rm -rf "$d"
}

if test "x$1" = x--one; then
  run_one "$2"
  exit $?
fi

count=10 first=1 jobs=1 dir=fuzz FUZZKEEP=
while test $# -gt 0; do
  case $1 in
    -n) count=$2; shift 2;;
    -s) first=$2; shift 2;;
    -j) jobs=$2; shift 2;;
    -d) dir=$2; shift 2;;
    -k) FUZZKEEP=yes; shift;;
    *) echo "usage: fuzz.sh [-n COUNT] [-s FIRST-SEED] [-j JOBS] [-d DIR] [-k]" >&2; exit 2;;
  esac
done
mkdir -p "$dir" || exit 1
FUZZDIR=$(cd "$dir" && pwd)
export FUZZDIR FUZZKEEP GUILE OAKLISP OAKWORLD top_srcdir top_builddir
seq "$first" $((first + count - 1)) | xargs -P "$jobs" -n 1 sh "$self" --one > "$FUZZDIR/summary" 2>&1
cat "$FUZZDIR/summary"
bad=$(grep -c '^FAIL' "$FUZZDIR/summary")
echo "$count programs, $bad failed"
test "$bad" = 0
