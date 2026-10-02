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

`AVANT_ROOT` and `AVANT_MYC_LLVM` override discovery. `AVANT_MYC_FINAL=1` asks myc-llvm for `--final` (slow compile, more LLVM opts). Daily builds use myc-llvm default.

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

```avant
fn name_of(kind: Int): String {
  switch kind {
    case 0 { "eof" }
    case 1, 2 { "ident-or-int" }
    else { "other" }
  }
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

C is `lib` / `fun`. Hand-written and generated bindings are the same grammar. There is no `extern fn`. Call as `libname.funname(...)`. Link extra `.c` / `.o` with `--cc` and system libraries with `--lib NAME`. C++ is `extern "C"` only. `avant bind HEADER.h` translates a header with libclang into an editable `lib` block.

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

`spawn { }` is a 1:1 OS thread. The result of the block is joined with `.join`. Int (and similar copyable) locals may be copied into the thunk. A bare `class` may not be captured. `.parallel` exists in the grammar but still lowers to a sequential map.

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

## Tests

Daily proof is the native suite on identity **B** or **C**. Recovery is `crystal spec`.

```
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
AVANT_COMPILER=./bin/avant-av-c ./bin/avant-av-c run tests/run.av
```

Adding `tests/cases/foo.av` does not edit `tests/run.av`. Coverage: `--coverage` / `AVANT_COVERAGE=1` on the identity compiler (not Crystal host codegen). That process writes `lcov.info` (`AVANT_COVERAGE_LCOV=0` disables; a path overrides). `AVANT_COVERAGE_REPORT=0` quiets the compiler CLI table; a `--coverage` program still prints it. The native runner writes `junit.xml` from the same `PASS` / `FAIL` events (`AVANT_JUNIT=0` disables). Jest-shaped stdout stays. Dump goldens live under `tests/goldens/`.

## Not built yet

These are decided. They are **not** shipping. Do not read a target sketch as a tutorial for today’s binary.

| What | Status |
| --- | --- |
| First-class `fn(T): U` values | decided, not shipping |
| Generic structs and classes | decided, not shipping |
| Named interfaces | decided, not shipping |
| Real `.parallel` / `Channel(T)` / `Mutex(T)` | decided, not shipping |
| C struct field layouts, methods on `Ptr(T)` | decided, not shipping |
| Formatter, packages, HTTP *server* | decided, not shipping |

## Goals

Beginners are welcome; advanced programmers are not blocked. Automatic memory management is a given. Errors are values. The public scoreboard is LangArena: expressiveness at Crystal’s class, runtime nearer C/Rust than Crystal already is and clearly faster than Go, memory better than Crystal without giving up GC.

## License

[MIT](LICENSE).
