#!/bin/sh
# Shrink a random program that fuzz.sh found failing: delete its forms
# one at a time (progen.scm writes one top-level form per line), keeping
# each deletion after which MODE still disagrees with the run from
# source on a result that disagreed at first, while the run from source
# still reports no error.  Repeats until a whole pass deletes nothing.
# The lines that lift the print limits stay: without them loading
# source abbreviates what it prints and loading an object does not.
#
#   sh tests/fuzz-shrink.sh FILE MODE
#
# MODE is bc, nat, exe, exe-w or exe-ir.  FUZZOPTS adds emulator
# options to every run, as for fuzz.sh.  The result is FILE.min, with
# both runs' results beside it (FILE.min.src, FILE.min.MODE).

. "$(dirname "$0")/testlib.sh"
OAKC=$top_builddir/oakc
file=$1 mode=$2
test -f "$file" && test -n "$mode" || { echo "usage: fuzz-shrink.sh FILE MODE" >&2; exit 2; }
work=$(mktemp -d "${TMPDIR:-/tmp}/fuzz-shrink.XXXXXX") || exit 1
trap 'rm -rf "$work"' 0

results() { grep -a -o '=> .*' "$1"; }

# run MODE DIR: run DIR/p.oak in MODE, its results in DIR/MODE.res.
run() {
  m=$1 d=$2
  case $m in
    src)
      (cd "$d" && limit 300 "$OAKLISP" --batch --size-heap 128 $FUZZOPTS --world "$OAKWORLD" -- \
	 --locale system-locale --load p --exit > src.out 2>&1);;
    bc|nat)
      case $m in bc) how=compile-file;; nat) how=native-compile-file;; esac
      rm -rf "$d/$m" && mkdir "$d/$m" && cp "$d/p.oak" "$d/$m/"
      (cd "$d/$m" && limit 1200 "$OAKLISP" --batch --size-heap 128 $FUZZOPTS --world "$OAKWORLD" -- \
	 --locale system-locale \
	 --eval "(bind ((#*compiler-noisiness 0)) ($how system-locale \"p\"))" \
	 --exit > compile.log 2>&1 \
       && limit 300 "$OAKLISP" --batch --size-heap 128 $FUZZOPTS --world "$OAKWORLD" -- \
	 --locale system-locale --load p --exit > ../$m.out 2>&1);;
    exe|exe-w|exe-ir)
      case $m in exe) flags=;; exe-w) flags=-w;; exe-ir) flags=--ir;; esac
      rm -f "$d/x" "$d/$m.out"
      (cd "$d" && limit 1200 "$OAKC" $flags p.oak -o x > oakc.log 2>&1 \
       && OAKLISP_OPTIONS="--size-heap 128 $FUZZOPTS" OAKWORLD=/nonexistent limit 300 ./x > $m.out 2>&1);;
  esac
  touch "$d/$m.out"
  results "$d/$m.out" > "$d/$m.res"
}

# The tags of the results that differ between the two runs in DIR.
differing() {
  { diff "$1/src.res" "$1/$mode.res"; } | sed -n 's/^[<>] => \([^ ]*\) .*/\1/p' | sort -u
}

# fails FILE: whether FILE still shows the failure: a result whose tag
# is in $work/tags differs.
fails() {
  d=$work/try
  rm -rf "$d" && mkdir "$d" && cp "$1" "$d/p.oak"
  run src "$d"
  test -s "$d/src.res" || return 1
  grep -a -q 'An error occurred' "$d/src.out" && return 1
  # An object printed as #<...> carries a hash number, which differs
  # from run to run: no comparison.
  grep -a -q '#<' "$d/src.res" && return 1
  run "$mode" "$d"
  cmp -s "$d/src.res" "$d/$mode.res" && return 1
  test -n "$(differing "$d" | grep -x -F -f "$work/tags")"
}

cp "$file" "$work/cur.oak"
rm -rf "$work/try" && mkdir "$work/try" && cp "$file" "$work/try/p.oak"
run src "$work/try"; run "$mode" "$work/try"
differing "$work/try" > "$work/tags"
test -s "$work/tags" || { echo "$file does not fail in $mode"; exit 1; }
echo "results that differ: $(tr '\n' ' ' < "$work/tags")"
fails "$work/cur.oak" || { echo "$file does not fail in $mode"; exit 1; }
pass=0
while :; do
  pass=$((pass + 1))
  changed=no
  n=$(wc -l < "$work/cur.oak")
  i=2				# line 1 is the generator's comment
  while test $i -le "$n"; do
    case $(sed -n "${i}p" "$work/cur.oak") in
      "(set! (fluid print-"*) i=$((i + 1)); continue;;
    esac
    sed "${i}d" "$work/cur.oak" > "$work/cand.oak"
    if fails "$work/cand.oak"; then
      mv "$work/cand.oak" "$work/cur.oak"
      n=$((n - 1)); changed=yes
      echo "pass $pass: deleted a form, $n lines left"
    else
      i=$((i + 1))
    fi
  done
  test $changed = yes || break
done
cp "$work/cur.oak" "$file.min"
fails "$file.min" >/dev/null
cp "$work/try/src.res" "$file.min.src"
cp "$work/try/$mode.res" "$file.min.$mode"
echo "$file.min: $(wc -l < "$file.min") lines; the results differ:"
diff "$file.min.src" "$file.min.$mode" | head -20
