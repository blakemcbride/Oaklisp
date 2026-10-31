Oaklisp
=======

Oaklisp is an object-oriented dialect of Lisp sharing the standard
Lisp syntax, including Common Lisp style macros, first class types,
multiple inheritance, and multiple namespaces (locales).  Oaklisp is
also a Lisp-1 dialect, meaning functions and variables share the same
namespace (like Scheme).

This is a portable implementation of a Lisp interpreter and compiler for
the Oaklisp dialect of Lisp.  The compiler compiles Oaklisp source
code into byte-code for the included Oaklisp emulator / virtual
machine, and, in this repository, into C as well.

This repository is a fork of Oaklisp, located at

*  https://github.com/blakemcbride/Oaklisp

Oaklisp was created by Kevin J. Lang and Barak A. Pearlmutter, who
have developed and maintained it since 1986.  The original project
lives at

*  https://github.com/barak/oaklisp

The README this fork started from is kept unchanged as
[README.orig.md](README.orig.md).  Everything of value here rests on
their design and their code; see the section on this fork, below, for
how the two have diverged.


About Oaklisp
-------------

The implementation is described in the included documentation
(`doc/lang`, the language manual; `doc/lim`, the implementation
manual; `doc/summary`, a concise programmer's reference), and also in

* Kevin J. Lang and Barak A. Pearlmutter.  Oaklisp: an object-oriented
  Scheme with first class types. In OOPSLA-86, pages 30–7. doi:
  10.1145/960112.28701.  Special issue of ACM SIGPLAN Notices 21(11).
  URL http://barak.pearlmutter.net/papers/oaklisp-oopsla-1986.pdf

* Kevin J. Lang and Barak A. Pearlmutter. Oaklisp: an object-oriented
  dialect of Scheme. Lisp and Symbolic Computation, 1(1):39–51, May
  1988.
  URL http://barak.pearlmutter.net/papers/lasc-oaklisp-1988.pdf

* Barak A. Pearlmutter. Garbage collection with pointers to individual
  cells.  Communications of the ACM, 39(12):202–6, December 1996.
  doi: 10.1145/272682.272712.
  URL http://barak.pearlmutter.net/papers/cacm-oaklisp-gc-1996.pdf

* Barak A. Pearlmutter and Kevin J. Lang. The implementation of
  Oaklisp.  In Peter Lee, editor, Topics in Advanced Language
  Implementation, pages 189–215. MIT Press, 1991.
  URL http://barak.pearlmutter.net/papers/Oaklisp-TALI-Chapter-1991.djvu
  http://barak.pearlmutter.net/papers/Oaklisp-TALI-Chapter-1991.pdf

Copies of the first three are in `doc/`.

Oaklisp as it came from upstream already has:

* Native 64-bit as well as 32-bit builds, either byte order
* Dumping a world without exiting, and loading a world at runtime
  (see Dump.md)
* Green threads, and native OS threads with a concurrent mark-sweep
  garbage collector (see Threads.md and GC.md)
* A bootstrap from source alone with Guile Scheme: no preexisting
  Oaklisp, prebuilt world or precompiled `.oa` files needed
* GNU readline line editing, shell completion, and Debian packaging

Several of those (the 64-bit port, the Guile bootstrap, native
threads, the concurrent collector, dump-world/load-world, the Windows
portability work) began as contributions from this fork's author and
were merged upstream, where Barak reworked and extended them.


This fork
---------

The fork branches off upstream Oaklisp 2.1.1 (October 2026) and
goes its own way on a few points of language design, so it is not a
drop-in replacement.  What it adds and changes:

### A native compiler: Oaklisp to C

* **Mixed mode.**  `--compile-native FILE` compiles a file to
  bytecode plus a shared object built from generated C; loading the
  `.oa` picks the `.so` up, and the code runs natively inside the
  ordinary world.
* **Stand-alone executables.**  `oakc prog.oak... -o prog` builds a
  program that is C through and through: no interpreter linked in,
  no bytecode run, nothing loaded at startup.  Options include
  `--whole-program` (calls resolved statically across files, small
  functions inlined, type-proven generic calls bound directly),
  `--ir` (a C-locals lowering of the stack top), `--strip` (dead code
  removal) and `--interpreter` (the emulator linked in for programs
  that `eval`).
* `make native-world` rebuilds the whole runtime natively; the
  emulator can run that world too.

See Compiler.md for how to drive it, CompilerPlan.md for the design,
and Speed.md for measurements against a C-compiled Dylan.

### Threads

* A program-level API: `make-thread`, `thread-join`, condition
  variables, `compare-and-swap`, `fetch-and-add` (doc/lang/threads.tex).
* The interpreter, the bytecode compiler and natively compiled code
  all run with `--pthreads N`; concurrent compiles in futures work.
* Reentrant process-owned mutexes, guarded sections at the runtime's
  shared state, mutexes that park instead of spinning, and an audit of
  the shared state (Threads.md).
* The threads build runs at the default build's speed when threads
  are unused: per-thread allocation buffers, a coalescing sweep, and
  cheap guards.

### Garbage collection

* The mark-sweep collector works on a heap of 1 MB regions mapped on
  demand and unmapped when empty, with no copying fallback; the
  copying collector only compacts the heap for a world dump.
* Allocation no longer slows as small objects stay alive.
* `--gc-stats` reports pauses and stalls; `tests/gc-stress.test` runs
  the collectors under load in every mode.

### Language and runtime changes

* **Rest arguments become lists when used as lists.**  A rest
  argument used only the native way (passed on at the end of a tail
  call, or counted) is handled as before; any other use gets it as a
  list automatically, so no explicit `listify-args` is needed.  Rest
  names starting with `%` opt out.  Upstream keeps the original
  semantics.
* **The world starts in `user-locale`**, which sees only the exported
  user-level language; the whole system is still reachable with
  `--locale system-locale`.  `dump-world` and `load-world` are
  exported.
* **`scheme-locale` and the Scheme compatibility files are gone.**
  The n-ary comparisons (`=`, `<`, ... of two or more arguments) and
  `string->number` that lived there are part of the system proper.
* `load` takes `NAME.oak` over `NAME.oa` when the source is newer;
  Oaklisp switches accept unique prefixes, as the man page says.

### Speed

Library routines measured as slow were reworked without changing
what they do: `eq?` hash tables hash immediates directly and do their
bucket work on the vector, string `append` copies packed words, sort
merges vectors and no longer recurses per element on lists, `throw`
skips the unwinder when there is nothing to unwind, and a global
(operation, type) method cache sits behind the operations' one-entry
caches.  Speed.md records the before and after.

### Documentation and tests

* Locales.md explains locales and how to use them; macOS.md and
  Windows.md cover those platforms; the three manuals are corrected
  where their examples no longer ran, set in Latin Modern, and their
  PDFs are tracked beside the sources.
* One unit testing framework, `doc/examples/unit-testing.oak`, with
  examples in `doc/examples/` and `src/misc/`.
* `make check` runs `tests/*.test`: the language, the compiler
  fixpoint, native/bytecode parity, stand-alone executables, threads
  and the collectors.  `make bench` compares bytecode, mixed native,
  separately compiled and whole-program executables.

Much of this was developed with extensive use of Claude Code.


Building
--------

	autoreconf --install
	./configure
	make
	sudo make install

A plain `make` on a 64-bit host builds a native 64-bit Oaklisp,
bootstrapping from a prebuilt world if one is present, else from
source with Guile 3.  `./configure --enable-threads` adds OS threads
and the concurrent collector.  See BUILD.md for the details, macOS.md
and Windows.md for those platforms, and `make check` to test the
result.  The native compiler runs out of the build tree
(`make -C src/world native-world`, then `./oakc`).

Running the result:

	oaklisp --world src/world/oakworld.bin -- --locale system-locale


License
-------

Oaklisp is free software under the GNU General Public License,
version 2 or later; see COPYING.  The copyright on the original code
belongs to its authors, Kevin J. Lang and Barak A. Pearlmutter; see
AUTHORS for everyone who has contributed.

Fork maintained by [Blake McBride](https://blakemcbride.com)
<blake@mcbridemail.com>.
