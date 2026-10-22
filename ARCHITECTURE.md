# 7ABA Runtime Architecture

The current implementation-grounded language behavior and known semantic
gaps are summarized in [LANGUAGE_SPEC.md](./LANGUAGE_SPEC.md). The document
distinguishes implemented behavior from roadmap items and is a draft pending
resolution of the integer-width inconsistencies noted there.

## Implemented Foundation

The program is compiled into a stack virtual machine. Supported control
structures, actions, assignments, collection operations, file commands, and
object operations execute as bytecode. There is no source-line interpreter
fallback: unsupported statements are rejected during compilation.

The expression instruction set is:

| Instruction                            | Operand                 | Stack effect                                           |
| -------------------------------------- | ----------------------- | ------------------------------------------------------ |
| `BC_PUSH_INT`                          | Integer literal         | Push value                                             |
| `BC_PUSH_BOOL`, `BC_PUSH_NULL`         | Literal                 | Push tagged scalar value                               |
| `BC_LOAD_VAR`                          | Symbol-table index      | Push tagged runtime value or convert the legacy integer value (zero if undefined) |
| `BC_DECLARE_VAR`                       | Symbol/type             | Check initializer tag, declare the variable, and store its value |
| `BC_LIST_GET`                          | Collection symbol       | Replace index/key with a tagged collection value       |
| `BC_BUILD_MAP`, `BC_BUILD_SET`          | Literal item count      | Consume keys/values and push a managed collection      |
| `BC_MAP_SET`                           | Map symbol-table index  | Consume key/value and update a map                     |
| `BC_STORE_VAR`                         | Symbol-table index      | Store a tagged value in the current scope              |
| `BC_INPUT`                             | Names symbol/count      | Read one input line and store parsed tagged values     |
| `BC_MATRIX_CREATE`                    | Matrix symbol           | Create a managed, zero-initialized matrix               |
| `BC_MATRIX_SET`                       | Matrix symbol           | Store a tagged value at a checked row/column            |
| `BC_MATRIX_GET`                       | Matrix/target symbols   | Read a checked cell into a tagged variable              |
| `BC_QUEUE_CREATE`                     | Queue symbol            | Create a managed FIFO queue                             |
| `BC_QUEUE_ENQUEUE`                    | Queue symbol            | Append a tagged value                                   |
| `BC_QUEUE_DEQUEUE`, `BC_QUEUE_PEEK`   | Queue/target symbols    | Remove or read the front tagged value                   |
| `BC_COLLECTION_LEN`                  | Collection/target       | Store managed collection length                          |
| `BC_LIST_CREATE`, `BC_PILE_CREATE`   | Variable symbol         | Create a managed array with list/pile display flavor     |
| `BC_COLLECTION_PUSH`, `BC_COLLECTION_POP` | Array symbol        | Append or discard a tagged array value                   |
| `BC_COLLECTION_SAVE`, `BC_COLLECTION_LOAD` | Array/path symbols | Save or restore integer collection elements             |
| `BC_FILE_DISPLAY`                    | Path symbol             | Display a bounded text file                              |
| `BC_FILE_READ`                       | Path/target symbols     | Read bounded text into a managed string                  |
| `BC_FILE_WRITE`                      | Path/source/mode        | Write a scalar value using overwrite or append            |
| `BC_CONTAINS`                          | None                    | Consume item/key and collection; push membership bool |
| `BC_DISPLAY_VALUE`                     | Optional symbol         | Print a tagged value or managed collection              |
| `BC_DISPLAY_SPECIAL`                   | Symbol-table index      | Display a managed object or property                   |
| `BC_DISPLAY_NEWLINE`                   | None                    | End a display statement                                |
| `BC_LOAD_FIELD`, `BC_STORE_FIELD`     | Object/field symbols    | Read or update a managed object field                  |
| `BC_NEW`                               | Target/class/arity      | Allocate an instance and invoke its constructor        |
| `BC_METHOD_CALL`                       | Receiver/method/arity   | Dynamically dispatch an instance method                |
| `BC_NEGATE`                            | None                    | Negate top value                                       |
| `BC_COMPARE`                           | `ValueBinaryOp`          | Compare two values and push a boolean                  |
| `BC_STDLIB_CALL`                       | Built-in ID/arity       | Call a source-level standard-library function          |
| `BC_JUMP`                              | Instruction offset      | Set program counter                                    |
| `BC_JUMP_IF_FALSE`                     | Instruction offset      | Pop condition and branch when false                    |
| `BC_REPEAT_INIT`, `BC_REPEAT_NEXT`     | Exit/body offsets       | Maintain a nested repeat counter                       |
| `BC_CALL`                              | Function index/arity    | Bind arguments in a local scope and call action        |
| `BC_RETURN`                            | None                    | Free the local scope and return to caller              |
| `BC_ADD`, `BC_SUB`, `BC_MUL`, `BC_DIV` | None                    | Consume two values and push result                     |
| `BC_HALT`                              | None                    | Verify the program's expected stack depth and stop     |

The compiler applies conventional arithmetic precedence, emits comparison
expressions for control-flow conditions, and records source token columns and
lengths in bytecode. Diagnostics underline the reported span; older call sites
that provide only a column underline one character. The VM checks stack shape,
list bounds, integer overflow, and
division by zero. `BytecodeProgram` owns its instruction and symbol allocations;
callers release both through `bytecode_free`.

`ast.h`/`ast.c` provide an owned expression AST with 1-based source spans and
exclusive end positions. Production bytecode expression compilation parses
and lowers expressions through this AST path, including expressions embedded
in statements. Its grammar preserves the existing single-name index/member
forms and builtin behavior; AST groups preserve the inner token spans used by
diagnostics. `bytecode_compile_ast_expression` remains as a compatibility entry
point to the same lowering implementation. `tests/ast_bytecode_test.c`
(`make test-ast-bytecode`) checks parser acceptance parity, representative
runtime value parity, and source spans.

`program_ast.h`/`program_ast.c` define an owned, spanned program AST for the
implemented statement subset: assignments, scalar annotated declarations,
list/pile creation, collection push/pop/length, input destinations, collection
save/load, text file display/read/write, display, action calls and definitions,
`empty`, `throw`, conditionals, and loops. File paths and destination names
retain their statement spans; input can declare multiple destinations, and
file writes retain append versus overwrite mode. The parser reuses the
expression parser and rejects syntax outside that subset; it does not yet
represent all production syntax such as matrices, queues, classes, `try/catch`,
or object operations.
`resolver.h`/`resolver.c` provide an independent
name-resolution pass with owned symbol and reference tables. It predeclares
program-scope actions and variables to support forward references, creates one
child scope per action for parameters and locals, and treats nested control
blocks as sharing their enclosing scope. It reports unresolved names, duplicate
actions, repeated typed declarations, invalid action arity, and action
definitions inside blocks. Collection creation, length destinations, input
destinations, and file or collection load destinations enter the resolver's
variable table. Save and file-write sources are resolved normally. Built-in
expression calls are accepted by the
expression parser; action calls are statement-level.
Known scalar argument types are checked against annotated action parameters;
unknown types are allowed and remain dynamic. `empty` is treated as a Null
return and checked against an annotated action return type. This does not yet
check inferred fall-through returns or value-return statements, which the AST
does not currently represent.

These components are tested independently but are not connected to production
bytecode compilation and do not yet provide grammar parity. The production
compiler still parses statements through its existing line-oriented path, so
this resolver does not change current VM compatibility behavior.
`make test-program-ast` and `make test-resolver` exercise the standalone
components, including the existing `tests/lists_files.7ba` fixture.

The `typecheck.h`/`typecheck.c` analyzer infers types of supported expression trees
and reports unknown names explicitly. Production compilation uses it for
`name: Int|Float|Bool|String|Null = expression` declarations: statically known
initializer mismatches are compile errors, unknown values receive an exact-tag
runtime check, and variable replacement APIs enforce a declared tag whenever
that metadata is present. Unannotated assignment behavior remains dynamic.
The runtime stores this scalar metadata per variable; it is not GC-managed or
serialized. `Float` declarations are supported, but source float literals are
not. Production typed action parameters and returns, fields, collections, and
generic types remain unsupported. `make test-typecheck` checks expression analysis,
and `make test-type-annotations` checks the production declaration slice.

Source expressions currently provide `abs(number)`, `min(left, right)`,
`max(left, right)`, `len(collection_or_string)`, `lower(string)`,
`upper(string)`, `substr(string, byte_start, byte_length)`, `trim(string)`,
`starts_with(string, prefix)`, `ends_with(string, suffix)`,
`contains(string, needle)`, `index_of(string, needle)`,
`count(string, needle)`,
`replace(string, search, replacement)`, `repeat(string, count)`,
`sqrt(number)`, `sin(number)`, `cos(number)`, `log(number)`, `exp(number)`,
`floor(number)`, `ceil(number)`, `round(number)`, `pi()`,
`random_int(min, max)`, and `random_seed(integer)`. Trigonometric inputs are radians. Math functions return
finite floating-point values; invalid domains and overflow are runtime errors.
`random_int` uses inclusive integer bounds and rejection sampling to avoid
modulo bias. Each `RuntimeContext` owns its PRNG state; `random_seed` makes
sequences repeatable in that context. This generator is not cryptographically
secure. Case conversion and prefix/suffix comparisons are ASCII/byte based;
substring indices count bytes, not Unicode code points. `trim` removes the six
ASCII whitespace bytes. String search and replacement operate on byte
sequences; `index_of` returns `-1` when absent, and `replace` matches
non-overlapping occurrences. `count` counts non-overlapping occurrences, with
an empty needle matching each byte boundary. An empty replacement search is a no-op.
`repeat` rejects negative counts and size overflows. Each builtin validates
arity and runtime types, and
failures produce runtime diagnostics. The string and math helper
implementations are also available through `stdlib.h`/`stdlib.c`; they do not
require loading a module.

The VM operand stack stores tagged `Value` entries and registers its active range
as a root in the executing `RuntimeContext`. Scalar assignments store `Value`
slots and scalar display expressions execute with display opcodes. Lists, piles,
matrices, queues, maps, and sets use managed values; list-versus-pile remains
display metadata on the variable. Integer bytecode operations retain checked
32-bit semantics. Object instances and their fields are managed `Value` objects;
the collector traces values stored in fields, including object references.
`Variable` still contains legacy scalar compatibility fields; those fields do
not provide an alternate source-statement execution path. Object identity is
carried by `VALUE_OBJECT`, not an external object heap or integer handle.

Managed allocations request collection after 64 KiB or 1,024 allocations since
the previous successful collection, whichever threshold is reached first. The
VM checks for a pending request at each instruction boundary after updating the
operand-stack root depth. Successful collections adapt their next thresholds to
the live heap, with the initial thresholds as lower bounds. Explicit `gc_collect`
remains available to C callers. If mark-work allocation fails, collection
conservatively reclaims nothing and the pending request remains for a later safe
point.

`main` compiles the source ranges into bytecode before execution. `while`,
`repeat`, and `when/otherwise` blocks become VM jumps and repeat frames; the
compiler matches braces while ignoring quoted text and comments. Top-level
`action name(params)` definitions compile behind skip jumps. Direct statement
calls such as `name(integer_expressions)` bind arguments in local scopes.
`empty` is a standalone early-return statement; actions do not produce values.
Recursive calls are bounded by the call-stack limit. Keywords are matched
exactly, without typo autocorrection. The shared diagnostic reporter prints
the script path, line, column, source line, and caret. Bytecode instructions
consume a shared execution-step budget.

This is a pure-bytecode statement and expression execution path. `BC_INPUT`
reads and validates one line for all declared input targets before assigning
tagged values. Matrix, queue, list, pile, length, persistence, and file
operations execute through bytecode instructions over managed `Value`
instances. Comparisons used by `when` and `while` compile into expression
instructions and execute on the VM stack. Unrecognized statements produce a
compile-time diagnostic instead of entering an evaluator fallback. This does
not mean the frontend is a whole-program AST or that all runtime state is
context-owned. No performance advantage is claimed without caching and
benchmarks. The step budget cannot guarantee termination of blocking native
I/O or other external calls.

## Compatibility Contract

- Existing 7ABA expression and statement syntax is retained.
- Variable lookup happens at execution time, so loop-updated values remain dynamic.
- Undefined variables and invalid list indexes evaluate to zero, matching current behavior.
- Division by zero reports a guard diagnostic and preserves the left operand, matching current behavior.
- A trailing block opener is accepted for existing forms such as `repeat 3 {`.
- Persistence formats and file operations remain behind runtime entry points,
  outside the bytecode module.

`pro.7ba` is the primary compatibility sample. `tests/vm_expression.7ba` exercises expression and control-flow bytecode.
`tests/vm_value_stack.7ba` covers strings loaded from legacy variables and string
literals passed through the tagged VM stack into action parameters.
`tests/actions.7ba` covers forward calls, parameters, nested/recursive actions, local scopes, and empty returns. `tests/comprehensive_showcase.7ba` combines the syntax in one program. `tests/invalid_call_prefix.7ba` and `tests/invalid_empty_modifier.7ba` are expected-failure fixtures for the removed syntax forms.
`tests/run_input_multiple.ps1` checks mixed-type input, quoted strings, value
count validation, malformed/oversized input, and end-of-file handling.

## Building the Interpreter

Current Windows downloads and the Unix source package are listed on the
website download page. Historical versioned executables are not kept in the
repository; tagged release builds are handled by `.github/workflows/release.yml`.

Build from the repository root with GCC. On Windows, link with `-lm`; on Linux,
also link with `-ldl`. The Unix source package includes a platform-aware
`build.sh`.

```sh
gcc -Wall -Wextra -Werror -O3 -std=c99 -o 7aba main.c lexer.c parser.c eval.c bytecode.c diagnostic.c value.c gc.c runtime.c stdlib.c ffi.c ast.c typecheck.c -lm -ldl
```

## Lightweight Objects

Classes are top-level declarations. Fields have integer or quoted-string literal defaults; methods use the existing `action` syntax. `init` is an optional constructor, called by `new` with integer expression arguments. Method parameters are integer values, and methods do not return values, matching current action semantics.

```7aba
class Animal {
    field name = "unnamed"
    field age = 0

    action init(years) {
        this.age = years
    }

    action birthday() {
        this.age = this.age + 1
    }

    action speak() {
        display "age=" this.age
    }
}

class Dog extends Animal {
    field tricks = 0

    action speak() {
        display "dog age=" this.age
    }
}

pet = new Dog(4)
pet.birthday()
pet.speak()
display pet
```

Grammar:

```text
class        := "class" Identifier ["extends" Identifier] "{" member* "}"
member       := field | method
field        := "field" Identifier "=" (Integer | QuotedString)
method       := "action" Identifier "(" [Identifier ("," Identifier)*] ")" block
creation     := Identifier "=" "new" Identifier "(" [IntegerExpression ("," IntegerExpression)*] ")"
method-call  := (Identifier | "this") "." Identifier "(" [IntegerExpression ("," IntegerExpression)*] ")"
property     := (Identifier | "this") "." Identifier
```

Properties are object-owned and use direct `object.field` / `this.field` reads and writes. Fields are public in this initial model; there are no visibility modifiers or abstract-method declarations. Inheritance is single-parent; inherited fields are initialized from base to derived, and a method lookup starts at the instance's runtime class before walking its parents. Thus an override is selected dynamically for every object method call. Constructors named `init` are inherited and dispatched the same way. `display object` prints the class, address, and current fields in a box.

Constructor and action arguments use the supported VM expression syntax,
including scalar strings and object references. The VM preserves tagged values
across action and constructor frames; object parameters can be used in property
reads and writes such as `target.hp = target.hp - 45`. Assigning a parameter into
a field, as in `this.name = hero_name`, copies its tagged value into that field.

The compiler stores class definitions, flattened inherited field layouts, and
method source ranges in bounded compile-time registries. Method bodies are
emitted as bytecode; class declarations themselves are compile-time metadata.
`BC_NEW`, `BC_LOAD_FIELD`, `BC_STORE_FIELD`, and `BC_METHOD_CALL` provide
managed allocation, field access, and runtime method dispatch. Method calls
reuse action call frames and bind `this` as a tagged object value in the callee
scope. Fields hold tagged values and are traced by the context-bound collector.
Objects are process-local and are not serialized by the existing
scalar/collection persistence format; method return values and abstract
declarations are not supported.

Run `powershell -ExecutionPolicy Bypass -File tests/run_oop.ps1` on Windows to
build strictly and verify method calls, managed fields, inheritance, overrides,
object aliases, and object display. `tests/run_oop_constructor_args.ps1` covers
constructor arguments and external object-field mutation.

## Collections and File I/O

Lists, piles, matrices, and queues are managed `Value` objects, with their
storage traced by the context-bound collector. Lists and piles can contain
tagged values, although global-state and explicit `save`/`load` persistence
currently support integer elements only. Matrix indexing is zero-based. A
matrix may contain at most 1,000,000 cells; queues and lists are limited to
1,000,000 elements at the VM boundary. Dynamic arrays start with a capacity of
four elements and double when full; pile display reports the backing capacity.
Matrices and queues retain their
`VAR_MATRIX`/`VAR_QUEUE` global-state record formats.

| Statement                                        | Behavior                                                                          |
| ------------------------------------------------ | --------------------------------------------------------------------------------- |
| `matrix name rows columns`                       | Create a zero-initialized two-dimensional matrix.                                 |
| `matrix_set name row column value`               | Store a tagged value at a zero-based cell.                                        |
| `matrix_get name row column target`              | Read a cell into a scalar variable.                                               |
| `queue name`                                     | Create or reset a FIFO queue.                                                     |
| `enqueue name value`                             | Add a tagged value to the back of a queue.                                        |
| `dequeue name target`                            | Remove the front item into a scalar; an empty queue reports an error.             |
| `peek name target`                               | Read the front item without removing it.                                          |
| `list name` / `pile name`                        | Create or reset a managed array with list or stack display.                       |
| `push name expression` / `pop name`              | Append a tagged value or discard the final value.                                 |
| `len name target`                                | Return collection item count; for matrices this is rows multiplied by columns.    |
| `save name "path"` / `load name "path"`          | Save or restore integer collection values, one per line.                          |
| `input name [name ...]`                          | Read one value per named variable; integer and quoted string values may be mixed. |
| `file_display "path"`                            | Display a text file as numbered lines without changing it.                        |
| `file_read "path" target`                        | Read a text file of at most 255 bytes into a string variable.                     |
| `file_write "path" variable [overwrite\|append]` | Write a scalar value. The default mode overwrites.                                |

For multiple input, write the target names on one instruction and enter the same number of values on the prompted line, separated by whitespace. Each value is inferred independently as an integer or string; quote strings that contain spaces, for example `input name age` followed by `"Ada Lovelace" 36`. A value count mismatch is reported as an input error.

Quoted paths may contain spaces. Relative file paths are resolved from the process working directory. `file_display`, `file_read`, `file_write`, and collection save/load each execute as bytecode operations; file operations open and close their stream during the instruction. `file_write` accepts scalar strings, integers, floats, booleans, or null. On Windows, startup configures the console output code page to UTF-8 so collection and file display borders render correctly; if the configuration fails, 7ABA reports a warning and continues. `display list_name` prints an indexed table, `display pile_name` shows indexed values with the stack pointer marked on the top item, `display matrix_name` prints row and column indices, and `display queue_name` shows FIFO order, front/back markers, and the full state inside the box. Empty pile and queue states are also shown inside their boxes. Collection and file displays use UTF-8 box-drawing characters for their borders. `file_display` escapes non-printable bytes and, like `file_read`, is limited to 255-byte text files. Files are not runtime objects; `file_read` loads file content into a managed string. File I/O does not provide binary streams or arbitrary-length reads.

Run `powershell -ExecutionPolicy Bypass -File tests/run_collections_io.ps1` on Windows to perform a strict build and verify matrix access, FIFO ordering, quoted-path text I/O, and collection persistence across two executions. Run `powershell -ExecutionPolicy Bypass -File tests/run_lists_files.ps1` to verify managed list/pile operations and file bytecode.

## Runtime Evolution Boundaries

### Tagged Values (Foundation)

`runtime.h`/`runtime.c` define `RuntimeContext`, zero-initialized with
`RUNTIME_CONTEXT_INIT` before scopes or bytecode execution and destroyed after
scopes are released. Each context owns an
independent collector heap; allocation, roots, collection, statistics, value
construction, and bytecode entry points receive the context explicitly. There is
no process-global GC heap. Custom trace callbacks receive a mark function and
opaque mark context, allowing graph tracing without collector-global state.

`value.h` and `value.c` define the tagged `Value` representation for null, 64-bit
integers, doubles, booleans, strings, arrays, maps, sets, object references, and
native functions. Strings, arrays, maps, and sets are allocated through the
non-moving mark-and-sweep collector in `gc.h`/`gc.c`; all three collection types
trace contained `Value` references and may participate in cycles. Arrays use a
growable `Value` buffer. Maps use open addressing and tombstones; sets use the same
probing policy. Both keep power-of-two capacities and grow before exceeding a
three-quarter live-entry load. Keys are restricted to null, booleans, finite
integers/floats, and strings. Integer and float keys share the runtime's numeric
equality; arrays, maps, sets, objects, native functions, and non-finite floats are
not valid keys.

Runtime owners register addresses of live `Value` roots with `value_root_add` and
unregister them with `value_root_remove`. `gc_collect` traces registered roots and
collection/object fields; user-defined object payloads must be allocated with
`gc_allocate` and provide a trace callback for all contained `Value` references.
`VALUE_OBJECT` allocations use the managed object layout, and their fields are
traced by the runtime.
Every active interpreter scope is a GC root and traces tagged values stored in its
variables through `variable_set_runtime_value`. Freeing a scope unregisters the
root and clears those handles. Threshold-triggered collection runs at VM
instruction safe points; C callers may also request an explicit collection.
`runtime_destroy` releases all remaining allocations. Copies are shallow
handles; `value_release` clears a handle but does not reclaim its allocation.
The legacy integer/string compatibility fields remain per-variable mirrors and
are not collector roots.

The VM operand stack is tagged `Value` storage and its live prefix is registered
as a GC root. The compiler accepts source-level collection literals and operations:

```7aba
users = {"name": "Ada", 7: "seven"}
tags = {"compiler", "runtime"}
empty_map = {}
empty_set = set{}
users["name"] = "Grace"
name = users["name"]
has_tag = "runtime" in tags
has_user = "name" in users
```

`{}` is an empty map; a non-empty brace literal without colons is a set; `set{}`
spells an empty set. Map indexing reads values and indexed assignment inserts or
updates entries. `in` tests map keys or set membership and produces a boolean.
Map keys and set elements follow the C API's hashable-value rules; invalid keys,
missing map keys, and invalid indices report runtime errors. `len` accepts these
managed collections, and `display` prints scalar lookup results and collection
sizes.

### RuntimeContext Isolation

All mutable interpreter state is owned by a `RuntimeContext`: collector heap and
roots, scope/call stack, function and class registries, source-line buffers and
filename, current source line, diagnostic count and most recent diagnostic,
execution budget, and instruction counter. Compiler and diagnostic entry points
receive the context explicitly. A compiled `BytecodeProgram` retains a
non-owning pointer to the context it was compiled against; execution rejects a
different context, and the program must be freed before its context is destroyed.
VM operand, call, loop, and exception frames are local to an execution and do not
use process-global fallback mirrors. Separate contexts can therefore compile and
execute independently or interleaved without sharing variables, registries,
source lookup, diagnostics, or GC roots.

This is still an incremental runtime foundation, not full dynamic typing. Tagged
scalar and collection assignments use `Value` slots, while legacy scalar fields
remain in `Variable` as compatibility mirrors. Arithmetic retains checked
32-bit integer semantics. Source statements, conditions, and call arguments
execute through compiled bytecode; unsupported statement forms fail
compilation.
Source-level set mutation/removal and map deletion are not yet exposed. GC is
threshold-driven and runs automatically at VM instruction safe points.
Execution state is context-isolated as described above; this does not imply that
the VM is thread-safe. Concurrent access to the same `RuntimeContext` is
unsupported, and host applications must synchronize access or use one context
per thread.

`tests/runtime_context_test.c` verifies independent source buffers, variables,
function/class registries, diagnostics, execution budgets, and rejection of a
program executed against a context other than the one used to compile it.

Run `powershell -ExecutionPolicy Bypass -File tests/run_language_collections.ps1`
for a strict build plus language-level map/set literal, indexing, map updates,
membership, aliasing, invalid-key, and missing-key coverage. Run `make test-value` and
`make test-scope-gc` to exercise C values, collection, and interpreter scope
roots with strict C99 builds.

### Integer Exception Blocks

The bytecode compiler accepts `try { ... } catch name { ... }` and `throw
integer_expression`. Handler frames preserve stack depth, loop depth, call depth,
and the current scope. Throws unwind scopes and bind their integer payload to the
catch variable. Division-by-zero in expressions emitted as bytecode is caught with
payload `1`; outside a handler the legacy warning and left-operand-preserved
behavior remains. This initial exception layer does not yet turn every evaluator,
I/O, bounds, and allocation failure into catchable exceptions, and thrown values
are still integers.

### Native Streams, Arrays, and FFI

`stdlib.h`/`stdlib.c` provide binary/text stream handles, chunked arbitrary-length
reads, writes, seeking, flushing, and close; array map/filter callbacks and stable
numeric/string sorting operate on tagged `Value` arrays. These are C APIs and are
not yet callable from 7ABA source syntax.

`ffi.h`/`ffi.c` load and unload native libraries and call explicitly declared
scalar ABI signatures (zero, one, or two `int64_t`/`double` arguments with an
`int64_t`/`double` result). This intentionally excludes pointers, variadic
functions, and implicit ownership. The caller must use a trusted library and
declare the exact ABI; a mismatched declaration is undefined behavior in native
code. POSIX builds link `-ldl`; Windows uses `LoadLibraryA`/`GetProcAddress`.

There is no `BC_EXEC_LINE` opcode, single-line evaluator, recursive
`execute_block`, or compatibility condition evaluator. OOP creation, calls,
property reads, and field assignments use managed-value bytecode operations.
`len` handles managed arrays, maps, sets, matrices, and queues. List/pile and
file operations also use dedicated bytecode instructions. Display uses a
formatting helper for objects and collections; it does not execute source text.
This is not a whole-program AST, nor does it mean all runtime state is
context-owned. The collector roots the tagged operand stack and tagged scope
values, including managed objects, and the VM services allocation-triggered
collections at instruction safe points.

1. **Runtime convergence:** Mutable runtime and compiler state is context-owned and there are no global fallback mirrors. Remaining cleanup is removal of per-variable scalar compatibility fields and any future state added outside `RuntimeContext`; compiled programs are caller-owned and must not outlive their context.
2. **Modules:** Source-level imports, explicit exports, module namespaces, and module caching are not implemented. The agreed design is filename-derived namespaces with explicit exports; path canonicalization, cycle behavior, export validation, and a context-owned module registry remain prerequisites.
3. **Diagnostics:** Bytecode token spans and underlined diagnostics are implemented for expression compiler errors, identifiers, and comparison operators. Several statement-level diagnostics still report a single column; complete token spans require carrying lexer spans across all statement emitters and diagnostic sites.
4. **Standard library and FFI:** Stream/array utilities and scalar FFI are C APIs; most are not callable from 7ABA source. Isolate platform loading behind handle APIs and require declared signatures and explicit ownership rules.
5. **Benchmarks:** `powershell -ExecutionPolicy Bypass -File tests/run_vm_benchmark.ps1` builds and runs the expression-dispatch microbenchmark. It reports dynamic VM instructions/second and is not a representative application-performance claim.
6. **Snapshots, reactive values, and concurrency:** These require explicit semantics before implementation: snapshot consistency and I/O boundaries, dependency invalidation rules, scheduler/memory visibility, and resource ownership. They are not part of the current runtime and should not be advertised as implemented or zero-cost.

The project remains a C99 interpreter. Neither world-first novelty, leak freedom, nor performance parity with established VMs is asserted by this incremental foundation.
