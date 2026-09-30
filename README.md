# Avant

**Avant** is a systems programming language: Crystal-class density, a C-family silhouette, automatic memory management, AOT to native code.

It is for people who want as little syntax as Crystal (or less) to say the same thing, required braces without TypeScript paperwork, and enough speed that simplicity is not an apology. Home: [github.com/avant-lang/avant](https://github.com/avant-lang/avant) (private until minimally working, then public). License: [MIT](LICENSE).

This folder is the language. In the local workbench it sits next to `myc/` (IR backend) and `LangArena/` (scoreboard). Those trees are not part of this repository's identity.

This README is the **living language document** until a website exists. It describes what the compilers actually accept today. The target sketch (including features that are decided but not built) is [syntax.md](syntax.md) **S3**. Stages 6–17 are closed; 18–25 wait ([roadmap.md](roadmap.md), [d43.md](d43.md)). Identity is the oracle. Copying nursery is the default.

## What compiles today

Braces are required. No parentheses on `if` / `while` / `switch`. No semicolons. `=` binds or assigns. `fn`. Last expression is the value. Types are `name: Type`. `T?` is `T | Nil`. Failure is a union member; postfix `?` returns the failure. `struct` is a value; `class` is a reference. Entry is `fn main`, or `fn run` (a myc `main` is synthesized).

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

Proof programs (identity compiler **B** matches the host on this list): `examples/hello.av`, `fact.av`, `vec2.av`, `sieve.av`, `methods.av`, `trees.av`, `binarytrees.av`, `switch.av`, `defaults.av`, `sqrt.av`, `zlib.av` (`--lib z`), `add.av` (`--cc examples/c/add.c`), `errors.av`, `collect.av`, `json.av`, `http.av`, `spawn.av`; `matmul.av` checksums only. Call C as `libname.funname(...)`. Heap is GenImmix; the copying nursery is the default (`AVANT_NURSERY=0` restores Stage 7 full collect).

Identity also runs helpers the native suite pulled: `sha256` / `zlib_compress` (`examples/digest.av`), `json_root` / `json_get` / `json_free`, base64, PCRE2 `re_count`, `Array.reserve` / `.fill`, `dir_list`, `now_us`, and similar. Remaining host-only names wait for a failing native test (**D40**). Programs compile from a root; `import` follows neighbouring `.av` files (**D42**). `quote { }` injects code; `#()` splices inside quote; `comptime { for f in Type.fields { quote { ... } } }` is the one derived walk (**D29**).

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

## Decided, not built

These are in [decisions.md](decisions.md) / [syntax.md](syntax.md). They are **not** shipping. Do not read S3 as a tutorial for today's binary.

| What | Decision | When |
| --- | --- | --- |
| First-class `fn(T): U` values (S3 `map`) | D46 | D43 Stage 19 |
| Generic structs and classes | D47 | D43 Stage 20 Part 0 |
| Named interfaces | D45 | D43 Stage 21 |
| Real `.parallel` / `Channel(T)` / `Mutex(T)` | D35 | D43 Stage 20 |
| C struct field layouts, methods on `Ptr(T)` | D36 leftover | D43 Stage 18 |
| Macros beyond quote, packages, HTTP *server*, formatter | Wave 4 / D19 | D43 Stages 22 and 25; see [d43.md](d43.md) |

## How it is compiled

```
.av source → parser/checker (`compiler/*.av`; Crystal `src/` is frozen recovery) → myc IR → myc-llvm → binary
```

Still emit myc IR (**D39**). Identity is the oracle. Crystal `src/` is scaffolding and recovery.

## Compiler and tests

Needs Crystal 1.21, a C compiler (`cc`), libclang (for `avant bind`), and `myc-llvm` (workbench sibling `../myc/myc-llvm`, or `AVANT_MYC_LLVM`). Heap allocation goes through `runtime/avant_rt.c` (GenImmix, D34). JSON is yyjson; HTTP is uSockets (both under `runtime/third_party/`). C libraries link with `--lib` / `--cc`. Spawn links `-pthread`.

Daily proof is the native suite on identity **B** or **C**. Recovery is `crystal spec`. Host-vs-port dump specs skip unless `AVANT_BLESS_HOST=1`.

```
crystal src/cli.cr compile compiler/main.av bin/avant-av
./bin/avant-av compile compiler/main.av bin/avant-av-b
./bin/avant-av-b run examples/hello.av
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
AVANT_COMPILER=./bin/avant-av-c ./bin/avant-av-c run tests/run.av
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/bless_goldens.av
crystal spec
```

Adding `tests/cases/foo.av` does not edit `tests/run.av`. Coverage: `--coverage` / `AVANT_COVERAGE=1` on the **identity** compiler (not Crystal host codegen). Dump goldens: `tests/goldens/` (**D48**). Compiler fixed-point: [fixed-point.md](fixed-point.md). Recovery `crystal spec`; re-bless host-vs-port with `AVANT_BLESS_HOST=1 crystal spec`. Stage records: [roadmap.md](roadmap.md). **Do not** start Stages 18–25 unless asked.

## Live ledgers

These files are **current truth**. They are rewritten in place. They are not logs.

| File | Role |
| --- | --- |
| [philosophy.md](philosophy.md) | Why Avant exists, non-negotiables, measurable objectives |
| [decisions.md](decisions.md) | Technical choices that are in force |
| [questions.md](questions.md) | Open questions (Wave 5 closed; Stages 18–25 numbered, not opened) |
| [syntax.md](syntax.md) | Canonical sketch **S3** (target; not all of it compiles) |
| [baseline.md](baseline.md) | LangArena numbers Avant will be judged against |
| [roadmap.md](roadmap.md) | Stages. 6–17 closed; 18–25 planned (D43) |
| [d43.md](d43.md) | Sequence after Stage 13. Stages 14–17 closed; 18–25 planned |
| [bootstrap.md](bootstrap.md) | Stage 8 — Bootstrap. Identity roots, I/O, pitfalls |
| [native-tests.md](native-tests.md) | Stage 9 — Native tests and coverage |
| [trust.md](trust.md) | Stage 10 — Native suite / trust |
| [overloading.md](overloading.md) | Stage 11 — Defaults, overloading, operators, user generics |
| [modules.md](modules.md) | Stage 12 — Modules |
| [macros.md](macros.md) | Stage 13 — Macros (`quote` / `#()`) |
| [goldens.md](goldens.md) | Stage 14 — Dump goldens |
| [fixed-point.md](fixed-point.md) | Stage 15 — Compiler fixed-point |
| [oracle.md](oracle.md) | Stage 16 — Identity is the oracle |
| [safepoints.md](safepoints.md) | Stage 17 — myc safepoints / copying nursery |
| [experiments/immix.md](experiments/immix.md) | Sticky vs GenImmix measurement (closed) |

Read `philosophy.md`, then `decisions.md`. Patience, measurement, and organized code are part of the identity.
