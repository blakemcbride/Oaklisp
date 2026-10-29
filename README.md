Oaklisp
=======

Oaklisp is an object-oriented dialect of lisp sharing the standard
lisp syntax, including common lisp style macros, first class types,
multiple inheritance, and multiple namespaces (packages).  Oaklisp is
also a Lisp-1 dialect meaning functions and variables share the same
namespace (like Scheme).

This is a portable implementation of a lisp interpreter / compiler for
the Oaklisp dialect of lisp.

Project homepage(s)

*  https://github.com/barak/oaklisp       (homepage, development)
*  http://barak.pearlmutter.net/oaklisp/  (ancient history)

The compiler compiles Oaklisp source code into byte-code for the
included Oaklisp emulator / virtual machine.  The implementation
is described in the included documentation, and also in

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

See BUILD.md for instructions on how to build the system.

Features of this implementation
-------------------------------

Much of this was developed with extensive use of Claude Code; the
hard parts were done by
[Blake McBride]( https://blakemcbride.com ) <blake@mcbridemail.com>
and then reworked/extended by BAP.

1. Native 64-bit as well as 32-bit builds, either byte order
2. Dumping a world without exiting, and loading a world at runtime
   (see Dump.md)
3. Native threads alongside green threads (see Threads.md,
   Native-Threads.md), with a concurrent mark-sweep GC (see GC.md)
4. A native compiler from Oaklisp to C: mixed-mode native code in the
   running world, and stand-alone executables with oakc (see
   Compiler.md)
5. (Optional) bootstrap from source with Guile Scheme: no preexisting
   oaklisp, prebuilt world or precompiled .oa files needed.

This repo is located at:  https://github.com/barak/oaklisp

Barak Pearlmutter (& Blake McBride, who is **THE MAN**.)
