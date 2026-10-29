#!/bin/sh
# Benchmarks.  Run with "make bench" (in tests/, or at the top level),
# or directly with OAKLISP and OAKWORLD set.  Each bench-NAME.oak
# defines (bench); it is compiled once, then run REPEAT times, and
# the best CPU time in milliseconds is printed, as measured by
# (get-time) around the call, so that emulator start up and loading
# are not counted, with the longest and the 99th-percentile GC pause
# of the whole run (--gc-stats, GC.md).  Use BENCHFLAGS='-r N' for a
# different number of repetitions, and BENCHFLAGS='-o FILE' to also
# save the results as "name ms pause-max pause-p99" lines; two such
# files can be compared with
#   sh tests/bench.sh -c old.txt new.txt
# BENCHFLAGS=-n compiles the benchmarks with --compile-native instead
# of --compile, for comparing natively compiled code with bytecode.
# BENCHFLAGS=-x builds each benchmark as a stand-alone executable with
# oakc (always best of 5; needs "make -C src/world native-world"),
# BENCHFLAGS=-w does that in whole-program mode and BENCHFLAGS=-i with
# the C-locals lowering, so the modes compare: bytecode, mixed native,
# separate, whole-program and lowered compilation.
#
# BENCHFLAGS='-p N' runs parallel-tak.oak (N copies of tak, each on an
# OS thread of its own) with 1 thread and with N, timed by the wall
# clock around the whole run, and reports the speed-up and each run's
# GC pauses; it needs an emulator with threads.  -n applies to it too.
#
# The result also depends on the C compiler and its options; compare
# builds made the same way, e.g. with and without --enable-threads.

. "$(dirname "$0")/testlib.sh"

repeat=5
outfile=
compile=--compile
parallel=
while test $# -gt 0; do
  case $1 in
    -r) repeat=$2; shift 2;;
    -p) parallel=$2; shift 2;;
    -n) compile=--compile-native; shift;;
    -x) compile=exe; shift;;
    -w) compile=exe; oakcflags=-w; shift;;
    -i) compile=exe; oakcflags=--ir; shift;;
    -o) outfile=$2; shift 2;;
    -c) shift; old=$1; new=$2
	printf '%-12s %10s %10s %8s\n' benchmark "$old" "$new" ratio
	while read name ms rest; do
	  nms=$(awk -v n="$name" '$1==n {print $2}' "$new")
	  test -n "$nms" || continue
	  printf '%-12s %10s %10s %8s\n' "$name" "$ms" "$nms" \
	    "$(awk -v a="$ms" -v b="$nms" 'BEGIN {printf "%.2f", (a>0)? b/a : 0}')"
	done < "$old"
	exit 0;;
    *) echo "usage: bench.sh [-n] [-r repeat] [-o outfile] | -p threads | -c old new" >&2; exit 2;;
  esac
done

workdir=${TMPDIR:-/tmp}/oaklisp-bench.$$
mkdir -p "$workdir" || exit 1
trap 'rm -rf "$workdir"' 0

if test -n "$parallel"; then
  # Wall-clock milliseconds of a whole run of parallel-tak.oak.
  wall() {
    t0=$(date +%s%N)
    (cd "$workdir" && oak --gc-stats -- --load parallel-tak \
       --eval "(set! bench-copies $parallel)" --eval "(set! bench-threads $1)" \
       --eval '(bench)' --exit > "parallel-$1.log" 2>&1) \
      || { echo "parallel-tak: run with $1 threads failed"; exit 1; }
    t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000000 ))
  }
  enabled=$(oak -- --eval '(print (%threads-enabled?) standard-output)' --exit 2>&1 | tail -c 3)
  case $enabled in *T*) ;; *) echo "parallel-tak: the emulator has no threads"; exit 1;; esac
  cp "$testdir/parallel-tak.oak" "$workdir/"
  (cd "$workdir" && oak -- $compile parallel-tak --exit > parallel-tak.compile.log 2>&1) \
    || { echo "parallel-tak: compile failed (see $workdir/parallel-tak.compile.log)"; exit 1; }
  one=$(wall 1) || exit 1
  many=$(wall "$parallel") || exit 1
  echo "parallel-tak ($parallel copies of tak, wall-clock ms of the whole run):"
  printf '  %d thread%s %6s ms\n' 1 ' ' "$one"
  printf '  %d threads %6s ms\n' "$parallel" "$many"
  echo "  speed-up $(awk -v a="$one" -v b="$many" 'BEGIN {printf "%.2f", (b>0)? a/b : 0}')"
  for n in 1 "$parallel"; do
    echo "  $n thread(s): $(grep '^; GC: ' "$workdir/parallel-$n.log" | sed 's/^; //')"
  done
  exit 0
fi

echo "Oaklisp benchmarks: best of $repeat runs, CPU milliseconds; GC pauses in ms"
"$OAKLISP" --version
printf '%-12s %8s %10s %10s\n' benchmark ms pause-max pause-p99
test -z "$outfile" || : > "$outfile"

if test "$compile" = exe; then
  OAKC=$top_builddir/oakc
  test -x "$OAKC" || { echo "no oakc at $OAKC"; exit 1; }
  test -f "$top_builddir/src/world/native/liboaknat.a" \
    || { echo "no native runtime (make -C src/world native-world)"; exit 1; }
fi

for f in "$testdir"/bench-*.oak; do
  name=$(basename "$f" .oak); name=${name#bench-}
  cp "$f" "$workdir/$name.oak"
  if test "$compile" = exe; then
    cp "$testdir/benchmain.oak" "$workdir/"
    (cd "$workdir" && "$OAKC" $oakcflags "$name.oak" benchmain.oak -o "$name-x" \
       > "$name.compile.log" 2>&1) \
      || { echo "$name: oakc failed (see $workdir/$name.compile.log)"; continue; }
    (cd "$workdir" && OAKLISP_OPTIONS=--gc-stats OAKWORLD=/nonexistent "./$name-x" \
       > "$name.run.log" 2>&1)
  else
  (cd "$workdir" && oak -- $compile "$name" --exit > "$name.compile.log" 2>&1) \
    || { echo "$name: compile failed (see $workdir/$name.compile.log)"; continue; }
  (cd "$workdir" && oak --gc-stats -- --load "$name" \
	--eval "(let loop ((i 0) (best #f)) (if (= i $repeat) (print best standard-output) (let* ((t0 (get-time)) (v (bench)) (t (- (get-time) t0))) (loop (+ i 1) (if (or (not best) (< t best)) t best)))))" \
	--exit > "$name.run.log" 2>&1)
  fi
  # The time is the last line that is not the GC report's.
  ms=$(grep -v '^; ' "$workdir/$name.run.log" | grep -v '^$' | tail -n 1 \
       | sed 's/.*[^0-9]\([0-9][0-9]*\)$/\1/')
  pmax=$(sed -n 's/^; GC: .* max \([0-9.]*\) ms.*/\1/p' "$workdir/$name.run.log" | tail -n 1)
  p99=$(sed -n 's/^; GC: .* p99 \([0-9.]*\) ms.*/\1/p' "$workdir/$name.run.log" | tail -n 1)
  case $ms in
    ''|*[!0-9]*) echo "$name: failed"; continue;;
  esac
  printf '%-12s %8s %10s %10s\n' "$name" "$ms" "${pmax:-?}" "${p99:-?}"
  test -z "$outfile" || echo "$name $ms ${pmax:-?} ${p99:-?}" >> "$outfile"
done
