# Avant

**Avant** is a systems programming language: Crystal-class density, a C-family silhouette, automatic memory management, AOT to native code.

It is for people who want as little syntax as Crystal (or less) to say the same thing, required braces without TypeScript paperwork, and enough speed that simplicity is not an apology. Home: [github.com/avant-lang/avant](https://github.com/avant-lang/avant) (private until minimally working, then public). License: [MIT](LICENSE).

This folder is the language. The compiler you run is written in Avant (`compiler/*.av`). Crystal `src/` is recovery scaffolding.

## Index

1. [Getting started](#getting-started)
2. [Language](#language)
3. [Types](#types)
4. [Modules](#modules)
5. [Errors](#errors)
6. [Memory](#memory)
7. [C interop](#c-interop)
8. [Concurrency](#concurrency)
9. [Macros](#macros)
10. [Builtins](#builtins)
11. [Toolchain](#toolchain)
12. [Tests](#tests)
13. [Not built yet](#not-built-yet)
14. [Goals](#goals)
15. [License](#license)

## Getting started

Needs Crystal 1.21, a C compiler (`cc`), libclang (for `avant bind`), LLVM 20, and a safepoints-enabled `myc-llvm` (workbench sibling `../safepoints/myc-llvm`, or `AVANT_MYC_LLVM`).

From this directory:

```
crystal src/cli.cr compile compiler/main.av bin/avant-av
./bin/avant-av compile compiler/main.av bin/avant-av-b
./bin/avant-av-b run examples/hello.av
```

`AVANT_ROOT` and `AVANT_MYC_LLVM` override discovery. Daily builds pass myc-llvm `--debug` (fast codegen; the compiler frontend stays the same speed). `AVANT_MYC_DEBUG=0` restores myc's default codegen. `AVANT_MYC_FINAL=1` asks for `--final` (slow compile, more LLVM opts). Crystal `src/` is frozen and still invokes myc at its default when it builds A.

## Language

Braces are required. No parentheses on `if` / `while` / `switch`. No semicolons. `=` binds or assigns. `fn`. Last expression is the value. Types are `name: Type`. Entry is `fn main`, or `fn run` (a myc `main` is synthesized).

Comments are `//` to end of line. There is no `/* */`. `// file: path` is a test-runner concat marker (it resets lexer path and line); it is not a module import.

```avant
fn main {
  // greeting
  puts("Hello Avant")
}

fn fact(n: Int): Int {
  if n <= 1 { return 1 }
  n * fact(n - 1)
}
```

Structs, classes, inherent methods, external receivers, defaults, overloading, operators, and generic functions:

```avant
struct Point {
  x: Int
  y: Int

  fn sum: Int {
    x + y
  }
}

fn (p: Point) scaled(s: Int): Point {
  Point { x: p.x * s, y: p.y * s }
}

fn greet(name: String, times: Int = 1): Int {
  times
}

fn show(n: Int): Int { n }
fn show(s: String): Int { s.size }

fn id(x: T): T { x }

class Counter {
  value: Int = 0

  fn initialize(value: Int) {
    self.value = value
  }

  fn inc: Int {
    value += 1
    value
  }

  fn +(other: Counter): Counter {
    Counter.new(value + other.value)
  }
}
```

A `struct` is a value (copy). A `class` is a reference (identity). `Type { fields }` constructs a struct, not a class.

## Types

`T?` is `T | Nil`. Unions are first-class (`Int | String`, `User | DbError`). Type application is `Array(Int)`, `Hash(String, Int)` — not `[]Int`, not `Array<Int>`. Array literals `[1, 2, 3]` and empty `[]` are expressions.

New names are block-scoped. `=` to an existing enclosing name assigns outward. Mutable by default.

`switch` / `case` is Int (or Bool) only; a value switch needs `else`.

A function value has type `fn(T): U`. An omitted return is `Void`. Spellings: `fn(): Int`, `fn(Int): Int`, `fn(Int, String): Bool`. The type is invariant. A bare function name in that context is the value. `{ |x| x + 1 }` is a lambda. A zero-argument lambda is `{ || ... }`, because `||` is one token. A trailing block on a call whose last parameter is a `fn` is that argument. A name the lambda uses from an outer function is a shared cell: an assignment on either side is visible to the other. `each`, `spawn`, and `.parallel` keep their own blocks, and `each` stays inlined. Methods are not values. Call a function field as `(box.f)()`. There is no `==` on a `fn` value. `Array(T).new` takes a size; an empty array with a known element type is `ys: Array(Int) = []`.

A struct or class may take type parameters: `struct Pair(A, B)` and `class Box(T)`. Each use is its own layout. `Box(Int)` and `Box(String)` are different types. `Box(Int).new(1)` and `Box.new(1)` are the same value when `initialize` takes `T`. `Pair { a: 1, b: "x" }` is `Pair(Int, String)`. An unapplied `Box` is an error, and a type parameter must be a fresh name. A struct whose inline layout is infinite (`struct Node(T) { next: Node(T)? }`) is an error. A class may name itself (`class Link(T) { next: Link(T)? }`). `Array`, `Hash`, `Ptr`, and `JoinHandle` stay builtins.

A named `interface` is a set of method signatures. A struct, class, or `Ptr` to a C layout opts in by name: `struct Vec2: Show` on the declaration, or later `Vec2: Show`. Several names use `+`. `pub` on a later opt-in is who else can see that fact. An interface may take type parameters (`interface Eq(T)`). A generic function states a bound on a type parameter (`fn show(v: T: Show): String`). The call site must have opted in. The body is specialized, and the call is an ordinary method call. `Show` is not a value, so it cannot be stored or placed in an `Array`. `Array` itself does not opt in, and `each` stays a special form.

```avant
fn name_of(kind: Int): String {
  switch kind {
    case 0 { "eof" }
    case 1, 2 { "ident-or-int" }
    else { "other" }
  }
}

fn inc(n: Int): Int {
  n + 1
}

fn map(xs: Array(T), f: fn(T): U): Array(U) {
  ys: Array(U) = []
  xs.each { |x| ys.push(f(x)) }
  ys
}

fn add(n: Int): fn(Int): Int {
  { |x| x + n }
}
```

## Modules

A file is a module. `import name` loads `name.av` beside the importer. `pub` is the public surface; names default module-private. Cycles are errors. The CLI takes one root (not a concat list). `fn main` / `fn run` are entry, not `pub`. Public names enter the importer’s scope. Duplicate public names are a compile error; qualify only to resolve a clash.

```avant
import vec
fn main {
  v = origin()
  puts(v.x)
}
```

## Errors

Failure is a union member, not unwind. Postfix `?` unwraps the success variant(s) or returns the failure from the current function. `if user = find("ada") { ... }` binds only in the then-block.

```avant
fn parse_id(raw: Int): Int? {
  if raw < 0 { return nil }
  raw
}

fn bump(id: Int): Int? {
  n = parse_id(id)?
  n + 1
}
```

## Memory

Heap allocation goes through `runtime/avant_rt.c` (precise GenImmix: copying nursery, Immix mature). The copying nursery is the default. `AVANT_NURSERY=0` restores a full, non-moving collect. Objects that escape to C are pinned and are not copied.

Interpolation in double-quoted strings is `${expr}`. A `$` that is not followed by `{` is a literal dollar.

```avant
fn main {
  xs: Array(Int) = [1, 2]
  xs.push(3)
  sum = 0
  xs.each { |n| sum += n }

  h = Hash(String, Int).new
  h["a"] = 10
  if v = h.get("a") { puts(v) }

  puts("n=${41}")
  puts("it costs $5")
}
```

## C interop

C is `lib` / `fun`. Hand-written and generated bindings are the same grammar. There is no `extern fn`. Call as `libname.funname(...)`. Link extra `.c` / `.o` with `--cc` and system libraries with `--lib NAME`. C++ is `extern "C"` only. `avant bind HEADER.h` translates a header with libclang into an editable `lib` block. It also emits `struct` layouts when clang's size, alignment, and offsets match the natural layout of the Avant field types. A layout is not a value. Read and write its fields through `Ptr(T)` (`p.x`). Methods are `fn (p: Ptr(T)) name`. A heap `String` stored into a C field is pinned for the process.

```avant
lib math {
  fun sqrt(x: Float64): Float64
}

fn main {
  puts(math.sqrt(4.0))
}
```

JSON is yyjson. HTTP loopback is uSockets C (both under `runtime/third_party/`). Spawn links `-pthread`.

## Concurrency

`spawn { }` submits the block to a bounded OS-thread pool. The size is the number of processors online, read once when the pool starts. `AVANT_THREADS` replaces that size and must be a positive integer. Tasks beyond the pool wait in the queue. The block's result is `Int`, joined with `.join`. `join` or `.parallel` from inside a pool worker is an error. A nested `spawn` from a worker is allowed.

A value crosses `spawn` or `.parallel` when it is `Int`, `Int64`, `UInt64`, `Bool`, `Float64`, a `String` (the pointer is copied and both tasks root it), or a struct whose every field crosses. A `Channel` or `Mutex` value crosses. A class, `Array`, `Hash`, `fn`, or `JoinHandle` does not, and a struct that contains one of those does not.

`items.parallel { |item| ... }` on `Array(T)` takes one block parameter and returns `Array(U)` in input order. The parent waits outside the pool.

`Channel(T)` and `Mutex(T)` are ordinary generic classes. The proof is `tests/cases/s35.av`: rendezvous `Channel(T).new`, buffered `Channel(T).new(n)`, `recv` as `T?`, and `Mutex(T).with { |x| ... }`, where `x` is a place that dies with the block. Wait and wake go through `avant_gate_*`. Those classes are not imported for you. Statement-form `parallel { }` is later.

```avant
fn six_times_seven: Int {
  6 * 7
}

fn main {
  h = spawn { six_times_seven() }
  puts(h.join)
}
```

## Macros

`quote { }` injects functions (top-level / type body) or statements (inside a function). `#()` splices only inside quote. Hygiene gensyms names the quote introduces. `comptime` is one field walk: `for f in Type.fields { quote { ... } }`. Nested quote is an error. There is no arbitrary comptime interpreter.

```avant
quote {
  fn doubled(n: Int): Int {
    n + n
  }
}

struct Point {
  x: Int
  y: Int
}

comptime {
  for f in Point.fields {
    quote {
      fn #(f.name)(p: Point): #(f.type) {
        p.#(f.name)
      }
    }
  }
}
```

## Builtins

Proof programs (identity compiler **B** matches the host on this list): `examples/hello.av`, `fact.av`, `vec2.av`, `sieve.av`, `methods.av`, `trees.av`, `binarytrees.av`, `switch.av`, `defaults.av`, `sqrt.av`, `zlib.av` (`--lib z`), `add.av` (`--cc examples/c/add.c`), `errors.av`, `collect.av`, `json.av`, `http.av`, `spawn.av`; `matmul.av` checksums only.

Helpers the native suite pulled: `sha256` / `zlib_compress` (`examples/digest.av`), `json_parse` / `json_int` / `json_str` / `json_root` / `json_get` / `json_free`, base64, PCRE2 `re_count`, `Array.reserve` / `.fill` / `.pop` / `.clear`, `dir_list`, `now_ms` / `now_us`, `file_read` / `file_write` / `file_exists` / `argv` / `env_get` / `process_run` / `process_run_out`. Remaining host-only names wait for a failing native test.

## Toolchain

```
.av source → parser/checker (compiler/*.av; Crystal src/ is frozen recovery) → myc IR → myc-llvm (safepoints shard) → binary
```

The product emit is still myc IR. Identity is the compiler you trust. Crystal `src/` still builds A if you need recovery.

`format` reprints a file. It does not compile it. Stdout is the default. `--check` prints `canonical` or `would change PATH`. `--write` replaces the file.

```
./bin/avant-av-b format FILE.av
./bin/avant-av-b format FILE.av --check
./bin/avant-av-b format FILE.av --write
```

## Tests

Daily proof is the native suite on identity **B** or **C**. Recovery is `crystal spec`.

```
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
AVANT_COMPILER=./bin/avant-av-c ./bin/avant-av-c run tests/run.av
```

Adding `tests/cases/foo.av` does not edit `tests/run.av`. Coverage: `--coverage` / `AVANT_COVERAGE=1` on the identity compiler (not Crystal host codegen). That process writes `lcov.info` (`AVANT_COVERAGE_LCOV=0` disables; a path overrides). The compiler CLI does not print the Jest table (`AVANT_COVERAGE_REPORT=1` forces it); a `--coverage` program still prints it. The native runner writes `junit.xml` from the same `PASS` / `FAIL` events (`AVANT_JUNIT=0` disables). Jest-shaped stdout stays. Dump goldens live under `tests/goldens/`.

## Not built yet

These are decided. They are **not** shipping. Do not read a target sketch as a tutorial for today’s binary.

| What | Status |
| --- | --- |
| Statement-form `parallel { }` | decided, not shipping |
| Packages, HTTP *server* | decided, not shipping |

## Goals

Beginners are welcome; advanced programmers are not blocked. Automatic memory management is a given. Errors are values. The public scoreboard is LangArena: expressiveness at Crystal’s class, runtime nearer C/Rust than Crystal already is and clearly faster than Go, memory better than Crystal without giving up GC.

## License

[MIT](LICENSE).
