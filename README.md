# Avant

**Avant** is a systems programming language: Crystal-class density, a C-family silhouette, automatic memory management, AOT to native code.

It is for people who want as little syntax as Crystal (or less) to say the same thing, required braces without TypeScript paperwork, and enough speed that simplicity is not an apology. Home: [github.com/avant-lang/avant](https://github.com/avant-lang/avant) (private until minimally working, then public). License: [MIT](LICENSE).

This folder is the language. In the local workbench it sits next to `myc/` (IR backend) and `LangArena/` (scoreboard). Those trees are not part of this repository's identity.

This README is the **living language document** until a website exists. It describes what the compilers actually accept today. The target sketch (including features that are decided but not built) is [syntax.md](syntax.md) **S3**.

Waves 1–3 are closed. Stages 6–12 of the compiler exist. **D42** (file-modules, `import` / `pub`) is implemented (`stage12.md`). Stage 13 is D29. Immix closed ([experiments/immix.md](experiments/immix.md)). Stage 7 official prod closed (`results/2026-09-27-opt3/`, 50/50).

## What compiles today

Braces are required. No parentheses on `if` / `while` / `switch`. No semicolons. `=` binds or assigns. `fn`. Last expression is the value. Types are `name: Type`. `T?` is `T | Nil`. Failure is a union member; postfix `?` returns the failure. `struct` is a value; `class` is a reference. Entry is `fn main`, or `fn run` (a myc `main` is synthesized).

```avant
fn main {
  puts("Hello Avant")
}

fn fact(n: Int): Int {
  if n <= 1 { return 1 }
  n * fact(n - 1)
}
```

Structs, classes, inherent methods, and external receivers:

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

A file is a module. `import name` loads `name.av` beside the importer. `pub` is the public surface; names default module-private. Cycles are errors. The CLI takes one root (not a concat list). `fn main` / `fn run` are entry, not `pub`.

```avant
import vec
fn main {
  v = origin()
  puts(v.x)
}
```

Nilable types, postfix `?`, if-assign, interpolation, arrays, hashes, blocks:

```avant
fn parse_id(raw: Int): Int? {
  if raw < 0 { return nil }
  raw
}

fn bump(id: Int): Int? {
  n = parse_id(id)?
  n + 1
}

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

`switch` / `case` (Int discriminant), C `lib` / `fun`, `spawn` / `.join`, JSON via yyjson, HTTP via uSockets:

```avant
fn name_of(kind: Int): String {
  switch kind {
    case 0 { "eof" }
    case 1, 2 { "ident-or-int" }
    else { "other" }
  }
}

lib math {
  fun sqrt(x: Float64): Float64
}

fn six_times_seven: Int {
  6 * 7
}

fn main {
  puts(math.sqrt(4.0))
  doc = json_parse("{\"n\": 7}")
  if n = json_int(doc, "n") { puts(n) }
  h = spawn { six_times_seven() }
  puts(h.join)
}
```

Proof programs (identity compiler **B** matches the host on this list): `examples/hello.av`, `fact.av`, `vec2.av`, `sieve.av`, `methods.av`, `trees.av`, `binarytrees.av`, `switch.av`, `defaults.av`, `sqrt.av`, `zlib.av` (`--lib z`), `add.av` (`--cc examples/c/add.c`), `errors.av`, `collect.av`, `json.av`, `http.av`, `spawn.av`; `matmul.av` checksums only. Call C as `libname.funname(...)`. Heap is GenImmix; the copying nursery is off until myc safepoints.

Identity also runs the Stage 7 helpers the native suite pulled: `sha256` / `zlib_compress` (`examples/digest.av`), `json_root` / `json_get` / `json_free`, base64, PCRE2 `re_count`, `Array.reserve` / `.fill`, `dir_list`, `now_us`, and similar. Remaining host-only names wait for a failing native test (**D40**). Programs compile from a root; `import` follows neighbouring `.av` files (**D42**).

## Decided, not built

These are in [decisions.md](decisions.md) / [syntax.md](syntax.md). They are **not** shipping. Do not read S3 as a tutorial for today's binary.

| What | Decision | When (D41) |
| --- | --- | --- |
| First-class `fn(T): U` values (S3 `map`) | D30 leftover | After Stage 11 / later |
| `comptime` / `quote { }` / splice `#()` | D29 | Stage 13 (quote first) |
| Real `.parallel` / `Channel(T)` / `Mutex(T)` | D35 | After Stages 10–13 |
| C struct field layouts, methods on `Ptr(T)` | D36 leftover | After Stages 10–13 |
| Macros beyond quote, packages, HTTP *server*, formatter | Wave 4 / D19 | After Stages 10–13; see [questions.md](questions.md) |

## How it is compiled

```
.av source → parser/checker (Crystal host *and* `compiler/*.av`) → myc IR → myc-llvm → binary
```

Still emit myc IR (**D39**). Crystal `src/` is scaffolding and the identity oracle until a later decision. Identity is `compiler/*.av`.

## Compiler and tests

Needs Crystal 1.21, a C compiler (`cc`), libclang (for `avant bind`), and `myc-llvm` (workbench sibling `../myc/myc-llvm`, or `AVANT_MYC_LLVM`). Heap allocation goes through `runtime/avant_rt.c` (GenImmix, D34). JSON is yyjson; HTTP is uSockets (both under `runtime/third_party/`). C libraries link with `--lib` / `--cc`. Spawn links `-pthread`.

```
crystal spec
AVANT_COMPILER=./bin/avant-av-b crystal src/cli.cr run tests/run.av
crystal src/cli.cr run examples/hello.av
crystal src/cli.cr run examples/fact.av
crystal src/cli.cr run examples/vec2.av
crystal src/cli.cr run examples/sieve.av
crystal src/cli.cr run examples/methods.av
crystal src/cli.cr run examples/trees.av
crystal src/cli.cr run examples/binarytrees.av
crystal src/cli.cr run examples/switch.av
crystal src/cli.cr run examples/defaults.av
crystal src/cli.cr run examples/sqrt.av
crystal src/cli.cr run examples/zlib.av --lib z
crystal src/cli.cr run examples/add.av --cc examples/c/add.c
crystal src/cli.cr run examples/errors.av
crystal src/cli.cr run examples/collect.av
crystal src/cli.cr run examples/json.av
crystal src/cli.cr run examples/http.av
crystal src/cli.cr run examples/spawn.av
crystal src/cli.cr run examples/matmul.av
crystal src/cli.cr bind --lib add examples/c/add.h
crystal src/cli.cr dump examples/trees.av
crystal build src/cli.cr -o bin/avant
```

Self-host and native suite (after A and B exist; `AVANT_ROOT` / `AVANT_MYC_LLVM` as in [bootstrap.md](bootstrap.md)):

```
crystal src/cli.cr compile compiler/main.av bin/avant-av
./bin/avant-av compile compiler/main.av bin/avant-av-b
./bin/avant-av-b run examples/hello.av
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
./bin/avant-av-c run examples/hello.av
```

Adding `tests/cases/foo.av` does not edit `tests/run.av`. Coverage: `--coverage` / `AVANT_COVERAGE=1` on the **identity** compiler (not Crystal host codegen).

**Stage 8** (closed): identity compiler; `bin/avant-av`; A builds B; B runs `hello.av` ([bootstrap.md](bootstrap.md)). **Stage 9** (closed): native runner + coverage on B ([stage9.md](stage9.md)). **Stage 10** (closed): native behavioral suite on B is the daily language oracle ([stage10.md](stage10.md)). **Stage 11** (closed): D26 and user generics ([stage11.md](stage11.md)). **Stage 12** (closed): D42 file-modules, `import` / `pub` ([stage12.md](stage12.md)). Crystal `spec/` stays the host-vs-port identity oracle (**D40**). Next, when asked: **Stage 13** D29 quote-first ([roadmap.md](roadmap.md)).

## Live ledgers

These files are **current truth**. They are rewritten in place. They are not logs.

| File | Role |
| --- | --- |
| [philosophy.md](philosophy.md) | Why Avant exists, non-negotiables, measurable objectives |
| [decisions.md](decisions.md) | Technical choices that are in force (D41 = sequence after Stage 9; D42 = modules) |
| [questions.md](questions.md) | Open questions (later bucket after 10–13; Q24–Q25 closed) |
| [syntax.md](syntax.md) | Canonical sketch **S3** (target; not all of it compiles) |
| [baseline.md](baseline.md) | LangArena numbers Avant will be judged against |
| [roadmap.md](roadmap.md) | Stages. 6–12 closed; 13 planned |
| [bootstrap.md](bootstrap.md) | Stage 8 record (closed). Identity roots, I/O, pitfalls |
| [stage9.md](stage9.md) | Stage 9 record (closed). Native tests + coverage on B (D40) |
| [stage10.md](stage10.md) | Stage 10 record (closed). Native suite on B; B→C; goldens |
| [stage11.md](stage11.md) | Stage 11 record (closed). D26 and user generics |
| [stage12.md](stage12.md) | Stage 12 record (closed). D42 `import` / `pub` |
| [experiments/immix.md](experiments/immix.md) | Sticky vs GenImmix measurement (closed) |

Read `philosophy.md`, then `decisions.md`. Patience, measurement, and organized code are part of the identity.
