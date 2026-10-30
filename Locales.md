# Oaklisp Locales

A *locale* is a namespace: a table mapping symbols to global variable
cells, plus a list of macros, plus the set of variables frozen there
(`src/world/locales.oak`).  Each locale has a list of *superiors*.
Looking up a name searches the locale itself, then its superiors, depth
first and left to right (`find-locale`).  Defining a name that isn't
already found in the chain installs it in the locale itself; assigning
to a name that is found updates the existing cell wherever it lives.

Code is always read, compiled and evaluated relative to a locale: the
fluid `#*current-locale` at the top level, or the locale passed to
`load`, `eval` or `compile-file`.  A file must be compiled and loaded
relative to the same locale.

## The four standard locales

```
   system-locale  . . . . (exports) . . . .  oaklisp-locale
        |                                          |
compiler-locale                               user-locale
```

Solid lines are inheritance (child sees everything in its superior).
The dotted link is not inheritance: `oaklisp-locale` has no superiors
and receives an explicit list of names copied from `system-locale`.

| Locale | Superiors | Contents / purpose |
|--------|-----------|--------------------|
| `system-locale` | none | The root: the base language, its implementation and internals (`%`-prefixed primitives etc.), plus the compiler's entry points. Not the compiler's internals. |
| `compiler-locale` | `system-locale` | The compiler's internals, kept out of `system-locale`. |
| `oaklisp-locale` | none | The clean, user-level subset of `system-locale`. |
| `user-locale` | `oaklisp-locale` | Where user programs run: sees only the user-level language. The world starts here. |

### system-locale

The root.  The cold world's global variables are entered into it at
warm boot (`patch-locales.oak`), and the runtime, macros, bignums,
rationals and development tools are all loaded into it.  It holds the
base system, including implementation debris, which is why the cleaner
`oaklisp-locale` exists.  It does *not* hold everything: the
compiler's internals live in `compiler-locale` (only its entry points
are copied up).  It is the root of that locale, not a superset of it.
Reach it with `--locale system-locale`.

### compiler-locale

Holds the compiler (the `COMPFILES`: assembler, code generator,
peephole optimizer, native compiler, ...), loaded there while building
`oakworld-3.bin`.  Because it inherits `system-locale`, compiler code
sees the whole system, but its many internal names don't pollute
`system-locale`.  Only the entry points are pushed back up, sharing
their cells (`compiler-exports.oak`): `cc`, `compile-file`,
`compile-code-fragment`, `native-compile-file`,
`native-read-summaries`.  Compiler source files must be compiled with
`--locale compiler-locale`.

### oaklisp-locale

Has no superiors, so it sees nothing by default.  At the end of world
building `oaklisp-exports.oak` copies a curated list of names from
`system-locale` into it with `export-symbol`.  Exported variables share
the same cell (so a change through either locale is seen by both) and
the same frozen status (so the compiler still open-codes them);
exported macros share the same expander.  Since macros are not
hygienic, the helpers that exported macros expand into (`%fluid`,
`_add-method`, `%install-method`, ...) must be exported as well; the
file has a section for them.  The locale names themselves are
exported, so that `--locale system-locale` and code naming a locale
work from here.  It is the definition of "the Oaklisp language" as
opposed to its implementation.

### user-locale

Inherits only `oaklisp-locale`.  Code here sees the user-level
language and none of the internals (e.g. `car`, `map`, `format`,
`define-instance` are visible; `%tag` is not; see `tests/locales.oak`).
New definitions land in `user-locale` itself, so user programs can't
accidentally clobber system internals by reusing an internal name.

## The locale at startup

When `oaklisp` boots, `#*current-locale` is `user-locale`:

```
$ oaklisp -- --eval '(print #*current-locale standard-output)' --exit
#<Locale USER-LOCALE ...>
```

Nothing chooses it at boot time.  `#*current-locale` is an ordinary
fluid whose value is saved in the world image, so a world starts in
whatever locale was current when it was dumped.  `freeze.oak` sets it
to `system-locale` early in the build, and the last build step
(`oakworld.bin` in `src/world/Makefile.am`) switches to `user-locale`
with `--locale user-locale` just before the world is dumped.  Every
build and test command that runs a world therefore names the locale
it needs (`tests/testlib.sh`'s `oak` passes `--locale
system-locale`).  The REPL, `--eval`, `--load` and `--compile` all use
the current locale until a `--locale` switch or a `set!` changes it.

## How a regular user uses locales

Most of the time you don't think about locales at all.  `oaklisp`
starts in `user-locale`, and you type, `load` and `compile-file`
there.  You see the documented language and nothing else.  The typical
choices are:

- **Programs:** stay in `user-locale`.  What happens when you `define`
  a name depends on whether `user-locale` can already see it:

  - **A new name, or an internal one** (not exported, so invisible
    here): the lookup finds nothing, so `define` creates a new cell in
    `user-locale`.  It is yours alone; the system's internal binding of
    the same name, if any, is untouched.
  - **An exported name** (`reverse`, `map`, `format`, ...): the lookup
    finds it, and `define` assigns to the cell it found.  That cell is
    the very cell `system-locale` uses, so you have replaced the
    system's `reverse`, and every system function that calls `reverse`
    now calls yours.  This can break the system.

  To shadow an exported name safely, give it its own cell in
  `user-locale` before defining it:

  ```lisp
  (set! (variable-here? user-locale 'reverse) #t)  ; new local cell
  (define (reverse x) ...)                          ; now only yours
  ```

  Your code in `user-locale` then sees your `reverse`; the system keeps
  its own.
- **Exploring or extending the implementation:** work in
  `system-locale` (`oaklisp -- --locale system-locale`), where the
  whole base system, internals included, is visible.  The catch is
  that a top-level `define` of a name the system already uses (say a
  helper called `filter` or `position`) replaces the system's own
  binding, and anything in the system that calls it now calls yours.
  Pick distinctive names.
- **A library with private helpers:** make your own locale that
  inherits from `user-locale` or `system-locale`, load the library into
  it, and export only its public names to the locale your program runs
  in.  `export-symbol` is in `system-locale`, so do this from there:

  ```lisp
  (define mylib-locale (make locale (list system-locale)))
  (load "mylib" mylib-locale)
  (export-symbol mylib-locale user-locale 'mylib-entry)
  ```

Whichever locale you use, compile and load a file in the same one:
`oaklisp -- --locale system-locale --compile prog --load prog`.  To
switch locales during a REPL session, `(set! #*current-locale
system-locale)`.

## Using locales

```lisp
(define my-locale (make locale (list system-locale)))
(eval 'expr my-locale)              ; evaluate in a locale
(load "myfile" my-locale)           ; load a file into a locale
(compile-file my-locale "myfile")   ; compile relative to a locale
(set! #*current-locale my-locale)   ; switch the top-level locale
```

From the command line: `oaklisp -- --locale system-locale`.

Inspection: `variable?`, `macro?`, `frozen?` search the chain;
`variable-here?`, `macro-here?`, `frozen-here?` look at one locale
only; `(find-locale loc 'sym variable-here?)` returns the locale that
defines a name.  `export-symbol` / `export-sharing-cell` share a
binding between locales.  `find-locale` and `export-symbol` are in
`system-locale` only.

## Build and tool notes

- Each source file compiles in the locale it will be loaded into:
  `COMPFILES` in `compiler-locale`, everything else in `system-locale`
  (see `src/world/Makefile.am`).
- `oakc --strip` empties all four locales before dumping, keeping only
  what the program needs (`strip-locales` in `load-file.oak`).
