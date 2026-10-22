# 7aba Language Specification (Current Baseline)

Status: implementation-grounded draft for the current bytecode language.
This document records behavior present in the repository; it does not claim
that planned features are implemented or that every edge case has a stable
public contract. Changes to these rules should be accompanied by regression
tests and an explicit compatibility decision.

## Execution model

A source file is compiled into a `BytecodeProgram` and then executed by the
bytecode VM. Unsupported statements fail compilation. Each run belongs to one
`RuntimeContext`, which owns its heap, source text, variable scopes, function
and class registries, diagnostic state, and execution budget. A compiled
program is tied to its originating context and must not outlive that context.
Concurrent access to the same context is unsupported.

## Values and variables

The runtime representation is a tagged value. Implemented tags include null,
integer, float, boolean, string, arrays, matrices, queues, maps, sets, objects,
and native functions. Variables remain dynamically typed unless they use one
of the supported optional scalar declarations:

```text
count: Int = 3
ratio: Float = existing_float_value
ready: Bool = true
label: String = "ready"
empty_value: Null = null
```

Declared types require exact runtime tags; there are no implicit conversions.
The expression type analyzer rejects an initializer at compile time when its
type is known and differs from the declaration, and rejects statically
detectable invalid operations. Unknown initializer types are allowed through
compilation and checked when the declaration executes. After a declaration
executes, every variable replacement path that uses the shared storage API
(ordinary assignment, input, collection creation/loading, file reads, and
persistence restoration) enforces the declared tag. A failed replacement
leaves the prior value and declared type unchanged. Repeating a typed
declaration for a variable that is already typed is a runtime error.
Unannotated variables retain dynamic assignment behavior.

The runtime supports `Float` values and declarations, but the current source
expression grammar has no floating-point literal. A float initializer must
therefore obtain its value from a dynamically typed expression whose value is
already a float. Declared-type metadata belongs to each runtime variable, is
scalar (not GC-managed), and is not serialized. Persistence restoration uses
the shared checked storage API when metadata is present; across process
restarts the metadata is re-established only when the typed declaration
executes, so the existing persistence format does not independently preserve a
type declaration.

An uninitialized variable currently loads as integer zero. This behavior is a
compatibility rule in the VM, not a recommendation for future language design.
Variable assignment copies a tagged handle/value; managed collection and object
handles therefore retain reference identity.

## Expressions and operators

The bytecode expression grammar currently supports integer and string literals,
`true`, `false`, `null`, variable reads, parenthesized expressions, unary
negation, `+`, `-`, `*`, `/`, comparisons, collection indexing, map/set
membership with `in`, collection literals, and the built-ins documented in
`ARCHITECTURE.md`.

Arithmetic precedence is unary, multiplication/division, addition/subtraction,
then comparison. Parentheses override precedence. Comparisons produce booleans.
Adding with at least one string concatenates using the runtime's scalar
formatting. Numeric operations accept integer/float values in the value layer;
integer operations are checked for overflow and division by zero is an error.
Modulo exists in the C value API but is not part of the current source
expression grammar.

## Standard-library functions

The expression-level builtins are callable without imports:
`abs(number)`, `min(left, right)`, `max(left, right)`,
`len(string_or_collection)`, `lower(string)`, `upper(string)`,
`substr(string, byte_start, byte_length)`, `trim(string)`,
`starts_with(string, prefix)`, `ends_with(string, suffix)`,
`contains(string, needle)`, `index_of(string, needle)`,
`count(string, needle)`,
`replace(string, search, replacement)`, and `repeat(string, count)`.
`trim` removes ASCII space, tab, line feed, carriage return, vertical tab,
and form feed from both ends. Prefix/suffix checks and substring indices are
byte-based, not Unicode grapheme operations. `contains` and `index_of` perform
byte-sequence searches; `index_of` returns `-1` when absent and zero for an
empty needle. `count` counts non-overlapping matches; an empty needle matches
each byte boundary. `replace` replaces non-overlapping matches from left to right;
an empty search string leaves the input unchanged. `repeat` requires a
nonnegative integer count. Case conversion affects ASCII letters only.

Math builtins are `sqrt`, `sin`, `cos`, `log`, `exp`, `floor`, `ceil`, and
`round`, each taking one number and returning a finite Float; trigonometric
arguments are in radians. `pi()` returns the π constant. Invalid domains such
as a negative square root, nonpositive logarithm, or non-finite result are
runtime errors. `random_int(min, max)` returns an integer in the inclusive
range and rejects reversed bounds. `random_seed(integer)` sets the
`RuntimeContext`-local pseudo-random generator and returns Null; reseeding with
the same value repeats the sequence. The generator is not suitable for
cryptographic use. Each function checks runtime argument types and arity.

Equality is value-based for null, booleans, and strings; integer and float
values compare through numeric conversion. Managed arrays, matrices, queues,
maps, sets, objects, and native functions compare by identity. Relational
comparison accepts either two strings (bytewise lexicographic order) or two
numbers; mixed string/number comparisons are errors.

Conditional truthiness is currently defined by `value_truthy`: null, false,
numeric zero, empty strings, and empty collections are false; non-empty
collections and objects are true. `repeat` counts are currently interpreted
through the VM's integer conversion path and non-positive counts execute zero
iterations.

The implementation has known boundary inconsistencies that must be resolved
before treating this draft as a stable language standard: source integer
literals are parsed through a C `int` range, while tagged integer values and
arithmetic support wider values; unary negation and loop counts also use
narrower conversion helpers. Tests should capture the intended final width
before those paths are unified.

## Collections

Arrays, matrices, queues, maps, and sets are managed by the context's
collector. Arrays and queues contain tagged values. Matrix indices and array
indices are zero-based and bounds checked. Map and set keys must be null,
boolean, finite numbers, or strings; integer and float keys with equal numeric
values identify the same key. Map update and lookup, set construction and
membership, and basic collection lengths are supported. Full source-level
map/set removal and set algebra are not currently specified or implemented.

## Control flow and errors

`when/otherwise`, `while`, `repeat`, actions, returns via `empty`, and
`try/catch`/`throw` are compiled to bytecode. Exceptions currently carry integer
payloads. An uncaught throw is a runtime error. Not all runtime failures are
catchable; I/O, allocation, type, and bounds failures may report diagnostics
directly instead.

Diagnostics carry a code, severity, line, column, and span length in the
runtime context. Some expression diagnostics have useful spans; some statement
diagnostics still underline only one character. Columns are currently based on
source bytes, with tab expansion for display; Unicode grapheme-aware locations
are not guaranteed.

## Explicit non-goals of this baseline

The following are roadmap items, not current language guarantees:

- Integration of the whole-program AST and name resolver into production
  bytecode compilation.
- Production typed action parameters/returns, collection/generic types, or
  compile-time metaprogramming.
- `import`/`export`, module namespaces, dependency resolution, and package
  management.
- Source-level standard-library modules for filesystem, networking, database,
  or cryptographic APIs.
- LSP-backed semantic completion/navigation/diagnostics.
- Structured concurrency, tasks, channels, or language-level threads.
- An optimizing IR, JIT, or native-code backend.

Production expressions now parse into the spanned expression AST and lower to
bytecode. The production statement compiler remains line-oriented. Separately,
`program_ast.h`/`program_ast.c` implement a parser for a supported whole-program
statement subset, not the complete production grammar: list/pile creation,
push/pop/length, input destinations, collection save/load, and text file
display/read/write are represented. Matrices, queues, class definitions,
`try/catch`, and object operations are not yet represented.
`resolver.h`/`resolver.c` implement lexical name resolution for
that AST. This standalone pass supports forward references
within the program/action scope, action parameters and locals, and rejects
unresolved variable/action names, repeated typed declarations, duplicate
actions, and invalid action-call arity. Control-flow blocks share their
containing scope; action bodies have a child scope. Statically known scalar
argument types are checked against parameter annotations; unknown values stay
dynamic. `empty` is considered a Null return and checked against an annotated
return type. Value-return expressions and inferred fall-through returns are
not represented/checked yet. These rules are not yet applied to production
compilation, whose legacy uninitialized-variable behavior remains unchanged.

A separate expression type analyzer is used for production variable
declaration initializers. The declaration syntax is limited to
`name: Int|Float|Bool|String|Null = expression`; it does not extend to action
parameters, returns, fields, or collection element types. The expression
analyzer has no float literal node and reports name-dependent types as unknown,
so those cases rely on the runtime tag check.

## Selected roadmap direction (not implemented)

The following choices were selected for future, staged implementation. They
must not be interpreted as supported syntax or runtime behavior until the
compiler, runtime, tests, and tooling implement them:

- **File-based modules:** `import "path.7ba"` loads a module under a namespace
  derived from its filename stem; modules expose names only through explicit
  `export` declarations. Module variables are isolated from the importing
  file's scope. Canonical paths, duplicate imports, circular imports, and
  initialization-failure behavior must be specified and tested before release.
- **Cooperative concurrency:** start with structured cooperative tasks and
  message-passing channels, not native threads. A task must not outlive its
  structured parent. The scheduler and every suspended task's stack/temporaries
  must be GC roots. Tasks must not concurrently mutate the same runtime state;
  native threads remain deferred.
- **Backend evolution:** retain the bytecode VM as the reference execution
  engine. Define a target-independent IR only after the AST and semantic
  validation passes are stable. Any optimizer or future native backend must
  preserve the language semantics and GC/rooting contract; JIT/AOT is deferred
  until workload benchmarks justify it.
- **Standard-library layering:** expose filesystem and text functions through
  stable source-level modules first. Networking is a later optional module
  behind portable host APIs; database and cryptographic implementations should
  use reviewed external libraries rather than custom algorithms.

## Stabilization requirements

Before declaring this baseline stable, resolve the integer-width discrepancies,
decide whether string operations are byte-oriented or Unicode-aware, define
conversion and error/exception behavior consistently, and add tests for every
operator and boundary case. A future static type checker or AST must preserve
the behavior above unless a versioned language change says otherwise.
