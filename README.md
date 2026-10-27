# Avant

Avant is a programming language for people who want the productivity of a modern, type-inferred language without giving up native speed. You write short, C-flavoured code with curly braces; Avant compiles it ahead of time to a native binary, and a garbage collector (GenImmix) manages memory for you. No borrow checker, no manual `free`, no interpreter.

If you have written JavaScript, Go, or Crystal, the code below will already look familiar. That is the point: Crystal-like density, C-like shape, performance in the C/Rust neighbourhood on the LangArena suite (see [Benchmarks](#benchmarks) for the honest, same-machine version of that claim).

## Hello World

```avant
fn main {
  puts("Hello Avant")
}
```

Save it as `hello.av`. That is a complete program: `fn main` is the entry point, the last expression of a function is its value, and there are no semicolons.

## Install

Today, on Linux x86_64:

1. Download the latest `avant-linux-x86_64.tar.gz` from [Releases](https://github.com/avant-lang/avant/releases) and extract it.
2. Download `myc-llvm-linux-x86_64.tar.gz` from the [safepoints v0.4.0 release](https://github.com/avant-lang/safepoints/releases/tag/v0.4.0), extract it, and point Avant at it: `export AVANT_MYC_LLVM=/path/to/myc-llvm`. (The Avant archive does not bundle this backend binary.)
3. Make sure a C compiler (`cc`), LLVM 20, and libpcre2 are installed (`re2c` is optional; without it, regex literals use PCRE2).
4. Optional but recommended: put both binaries on your `PATH` — `~/.local/bin` needs no sudo (make sure it is on your `PATH`), or use `/usr/local/bin` for a system-wide install. Keep `myc-llvm` next to `avant` there; the `AVANT_MYC_LLVM` export above is still how the compiler finds it today.
5. Run your first program:

```
avant run hello.av          # compile and run
avant compile hello.av hi   # or build a binary named hi
```

That is enough to write and run Avant today. Building the Avant compiler itself is covered at the end, under [Advanced: building the compiler](#advanced-building-the-compiler).

## Index

1. [Programs](#programs)
2. [Structs and classes](#structs-and-classes)
3. [Types](#types)
4. [Control flow](#control-flow)
5. [Modules](#modules)
6. [Errors](#errors)
7. [Strings and collections](#strings-and-collections)
8. [Memory](#memory)
9. [C interop](#c-interop)
10. [Concurrency](#concurrency)
11. [Macros](#macros)
12. [Formatter](#formatter)
13. [Regex](#regex)
14. [Builtins](#builtins)
15. [Not built yet](#not-built-yet)
16. [Benchmarks](#benchmarks)
17. [Goals](#goals)
18. [Advanced: building the compiler](#advanced-building-the-compiler)
19. [License](#license)

## Programs

Braces are required. There are no parentheses on `if`, `while`, or `switch`, and no semicolons. `=` binds a new name or assigns an existing one. The last expression in a function is its value. A type is written `name: Type`.

The entry point is `fn main`, or `fn run` (the compiler synthesizes a myc `main`). Neither needs `pub`.

```avant
fn main {
  puts("Hello Avant")
}

fn fact(n: Int): Int {
  if n <= 1 { return 1 }
  n * fact(n - 1)
}
```

### Comments

`//` runs to the end of the line. There is no `/* */`.

`// file: path` is a marker the test runner uses when it concatenates sources. It resets the lexer's path and line. It is not `import`.

### Names

A name introduced in a block stays in that block. Assigning a name that already exists in an enclosing block writes that outer name. Locals are mutable.

```avant
fn main {
  n = 1
  if true {
    n = 2
    y = 3
  }
  puts(n)
}
```

After the `if`, `n` is `2`. `y` is not in scope.

## Structs and classes

A `struct` is a value. Copying it copies the fields. A `class` is a reference. Copying it copies the pointer. `Type { fields }` builds a struct. It does not build a class.

### Structs

```avant
struct Point {
  x: Int
  y: Int

  fn sum: Int {
    x + y
  }
}

fn main {
  p = Point { x: 1, y: 2 }
  q = p
  puts(q.sum)
}
```

`q` is a separate `Point`. Changing `q.x` does not change `p.x`.

Inside a method, a field name is in scope. `self` is the receiver. An external method is a function with a receiver parameter:

```avant
fn (p: Point) scaled(s: Int): Point {
  Point { x: p.x * s, y: p.y * s }
}
```

### Classes

`Type.new(...)` allocates and calls `initialize`. A field may have a literal or `nil` default.

```avant
class Counter {
  value: Int = 0

  fn initialize(value: Int) {
    self.value = value
  }

  fn inc: Int {
    value += 1
    value
  }
}

fn main {
  c = Counter.new(10)
  puts(c.inc)
}
```

### Overloads, defaults, operators

Two functions may share a name when the arity or the argument types differ. The return type alone does not overload. Defaults are trailing, and the call site fills them in.

```avant
fn show(n: Int): Int { n }
fn show(s: String): Int { s.size }

fn greet(name: String, times: Int = 1): Int {
  times
}
```

An operator is a method whose name is the operator. It is declared inside the type, the same way `inc` is:

```avant
class Counter {
  value: Int

  fn initialize(value: Int) {
    self.value = value
  }

  fn +(other: Counter): Counter {
    Counter.new(value + other.value)
  }
}
```

### Generic functions

An unbound name in a signature is a type parameter. The call site omits it when the arguments determine it.

```avant
fn id(x: T): T {
  x
}
```

`id(1)` is `Int`. `id("a")` is `String`. There is no `[]Int` and no `Array<Int>`.

## Types

### Primitives

`Int` is a 32-bit signed integer. `Int64` and `UInt64` are 64-bit. `Float64`, `Bool`, and `String` are the other everyday primitives. `Void` is the result of a function that does not return a value. An omitted return type means `Void`.

### Nil and unions

`T?` means `T | Nil`. `nil` is the only value of `Nil`. Any other union is written with `|`.

```avant
fn parse_id(raw: Int): Int? {
  if raw < 0 { return nil }
  raw
}

fn label(v: Int | String): Int {
  1
}
```

Failure is a union member. There is no unwind. See [Errors](#errors).

### Arrays

The type is `Array(Int)`, not `[]Int` and not `Array<Int>`. A literal is an expression. An empty literal needs a known element type, because `[]` alone does not say what it holds.

```avant
fn main {
  xs = [1, 2, 3]
  xs.push(4)
  puts(xs[0])
  puts(xs.size)

  ys: Array(Int) = []
  zs = Array(Int).new(8)
}
```

`Array(T).new` takes a size. `push`, `pop`, `clear`, `reserve`, and `fill` are methods. `each` is not a method call: the compiler inlines it as a loop. See [Iteration](#iteration).

### Hashes

`Hash(String, Int)` and `Hash(Int, Int)` are both real. `get` returns `T?`.

```avant
fn main {
  h = Hash(String, Int).new
  h["a"] = 10
  if v = h.get("a") {
    puts(v)
  }
}
```

The `if v = ...` form is [if-assign](#if-and-while). `v` exists only in the then-block.

### Function values

A function value has type `fn(T): U`. The type is invariant: `fn(Int): Int` is not `fn(Int): Int?`. Spellings:

```avant
fn takes_callback(f: fn(Int): Int): Int {
  f(1)
}

fn returns_fn: fn(Int): Int {
  { |x| x + 1 }
}
```

`fn(): Int` takes nothing. `fn(Int, String): Bool` takes two arguments. A bare function name in a value position is that function:

```avant
fn inc(n: Int): Int {
  n + 1
}

fn main {
  f: fn(Int): Int = inc
  puts(f(2))
}
```

There is no `==` on a `fn` value. Methods are not values. A call written `box.f()` is a method call. To call a field that holds a function, parenthesize the field: `(box.f)()`.

### Lambdas and trailing blocks

`{ |x| x + 1 }` is a lambda. A type inside the pipes is optional when the expected `fn` type already gives it. A lambda whose parameter type is still free is an error.

`||` is one token. A lambda with no parameters is `{ || 1 }`. `{ | | 1 }` with a space is two pipes, and it is not that form.

A trailing block on a call fills the last parameter when that parameter is a `fn` and the written arguments are one short:

```avant
fn map(xs: Array(T), f: fn(T): U): Array(U) {
  ys: Array(U) = []
  xs.each { |x| ys.push(f(x)) }
  ys
}

fn main {
  ys = map([1, 2, 3]) { |n| n + 1 }
  puts(ys.size)
}
```

`each`, `spawn`, and `.parallel` keep their own blocks. They do not become `fn` parameters, and `each` stays inlined.

### Capture

A name a lambda reads from an outer function is stored in a shared cell. An assignment on either side is visible to the other.

```avant
fn add(n: Int): fn(Int): Int {
  { |x| x + n }
}

fn main {
  f = add(10)
  puts(f(1))
}
```

`f(1)` is `11`. `n` was captured from `add`.

### Generic structs and classes

A struct or a class may take type parameters. Each use is its own layout. `Box(Int)` and `Box(String)` are different types. An unapplied `Box` is an error. A type parameter must be a fresh name, not an existing type.

```avant
struct Pair(A, B) {
  a: A
  b: B
}

class Box(T) {
  value: T

  fn initialize(value: T) {
    self.value = value
  }
}

fn main {
  p = Pair { a: 1, b: "x" }
  b = Box(Int).new(1)
  c = Box.new(1)
}
```

`Pair { a: 1, b: "x" }` is `Pair(Int, String)`. `Box.new(1)` is `Box(Int)` when `initialize` takes `T`. `Box(Int).new(1)` is the same value, written out.

A struct is copied inline, so a struct that contains itself does not fit:

```avant
struct Node(T) {
  next: Node(T)?
}
```

That is an error. A class is a pointer, so a class may name itself:

```avant
class Link(T) {
  next: Link(T)?
}
```

`Array`, `Hash`, `Ptr`, and `JoinHandle` stay built in. They are not generic structs you declare.

### Interfaces

An `interface` is a set of method signatures. It has no method bodies, and it is not a value: you cannot store a `Show`, and you cannot write `Array(Show)`.

A struct, a class, or a `Ptr` to a C layout opts in by name. The clause can sit on the type or come later. Several names join with `+`. `pub` on a later opt-in is who else can see that fact.

```avant
interface Show {
  fn to_s: String
}

interface Eq(T) {
  fn ==(other: T): Bool
}

struct Vec2: Show + Eq(Vec2) {
  x: Int
  y: Int

  fn to_s: String {
    "vec"
  }

  fn ==(other: Vec2): Bool {
    if x == other.x {
      return y == other.y
    }
    false
  }
}

fn show(v: T: Show): String {
  v.to_s
}
```

`T: Show` is a bound. The call site must have opted in. The compiler copies the function for that concrete type and emits an ordinary method call. There is no vtable.

The witness must match the signature exactly. `Int` does not witness `Int?`. A default on the method does not make a shorter signature count. Builtins do not opt in: `Array` has no `Show` clause, and `each` stays a special form. `Ptr(Int)` is not a layout pointer and cannot opt in. `Ptr(Point)` can, when `Point` is a C layout. See [Layouts](#layouts-and-ptr).

An interface may take type parameters (`interface Eq(T)`). Two bounds are written `T: Show + Eq(T)`.

## Control flow

### If and while

`if` and `while` are statements. `if` may have an `else`. A new name bound in the condition of `if` exists only in the then-block. That is how optional results are opened:

```avant
fn main {
  h = Hash(String, Int).new
  h["a"] = 10
  if v = h.get("a") {
    puts(v)
  }

  n = 0
  while n < 3 {
    n = n + 1
  }
}
```

### Switch

`switch` / `case` discriminates `Int` or `Bool`. A switch used as a value needs `else`, so every input produces a result.

```avant
fn name_of(kind: Int): String {
  switch kind {
    case 0 { "eof" }
    case 1, 2 { "ident-or-int" }
    else { "other" }
  }
}
```

### Iteration

`xs.each { |n| ... }` walks an array. The block is not a `fn` value. The compiler inlines the loop.

There is no general `for`. The only `for` is the comptime field walk in [Macros](#macros).

## Modules

A file is a module. `import name` loads `name.av` beside the importer. Names are module-private unless marked `pub`. Cycles are an error. The compiler takes one root file and follows imports. It does not take a list of files to concatenate.

Public names enter the importer's scope. If two imports publish the same name, qualify to resolve the clash.

```avant
import vec

fn main {
  v = origin()
  puts(v.x)
}
```

`origin` is `pub` in `vec.av`. `fn main` in the root is the entry point. It is not `pub`.

## Errors

A function that can fail returns a union. Postfix `?` unwraps the success side, or returns the failure from the current function.

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

If `parse_id` returns `nil`, `bump` returns `nil` and does not evaluate `n + 1`.

`if user = find("ada") { ... }` is the same idea when you want the success value in a block instead of an early return. `user` is not visible in an `else`, and it is not visible after the `if`.

### Compiled errors

Compile errors print to stderr and exit non-zero; runtime traps do the same in the compiled program.

Compile errors carry a stable code and show the offending line with a caret. The compiler recovers after an error and reports the rest of the program in the same run — up to 20 errors, with a count of any beyond that — so one compile shows every independent mistake it can find:

```
error[E4001]: unknown name nosuch1
  --> src/main.av:3:10
3 |   puts(nosuch1)
  |          ^
```

With several errors, the run ends with a summary line: `error: aborting due to N previous errors`.

The first digit of the code names the phase that found the error: `E1xxx` lexer, `E2xxx` parser, `E3xxx` type resolution, `E4xxx` checker, `E5xxx` module loader, `E6xxx` macro expansion. Recovery is per statement: a bad statement does not hide errors in later statements, functions, or imported files.

## Strings and collections

### Strings

Adjacent pieces and `${expr}` build a new string. A `$` that is not followed by `{` is a dollar sign.

```avant
fn main {
  name = "Ada"
  puts("hello ${name}")
  puts("it costs $5")
}
```

`size` is the byte length. Strings are not mutated in place. Concatenation allocates a new string.

### Arrays, again

Indexing is `xs[i]`. `push` may grow the buffer. `each` is the iteration form. A running total is an outer local, assigned from inside the block:

Indexing an `Array`, `String`, or `Buf` outside `0 <= i < size` traps at runtime: one diagnostic on stderr and a non-zero exit. `.get(i)` returns `T?` instead — the element or nil. Integer `+`, `-`, `*` (and `/`, `%`) are checked the same way: overflow and division by zero trap. The wrapping spelling is explicit literal text: `&+`, `&-`, `&*` wrap on overflow. Checks the compiler can prove safe — a literal index into a literal, an induction variable under its loop bound, a loop whose whole index range is validated once on entry — are not emitted.

```avant
fn main {
  xs: Array(Int) = [1, 2]
  xs.push(3)
  sum = 0
  xs.each { |n| sum += n }
  puts(sum)
}
```

## Memory

Heap allocation goes through `runtime/avant_rt.c`. The collector is precise GenImmix: a copying nursery, then an Immix mature space. The nursery is the default.

`AVANT_NURSERY=0` restores a full, non-moving collection. `AVANT_NURSERY_STACKMAP=0` skips the stack-map walk. `AVANT_NURSERY_DISCARD=0` keeps dead nursery space for diagnosis instead of discarding it.

A heap object stored into a C field is pinned for the process. Pinned objects are not copied. See [C interop](#c-interop).

## C interop

### lib and fun

C is a `lib` block of `fun` declarations. Hand-written bindings and generated ones are the same grammar. There is no `extern fn`. Call through the lib name.

```avant
lib math {
  fun sqrt(x: Float64): Float64
}

fn main {
  puts(math.sqrt(4.0))
}
```

C++ is reached only as `extern "C"`. Link a system library with `--lib NAME` (`--lib z` for zlib) and an extra `.c` or `.o` with `--cc`.

`avant bind HEADER.h` reads a header with libclang and prints an editable `lib` block. The command lives on the identity compiler. The translator is still `tools/bind.c`.

### Layouts and Ptr

A `struct` inside `lib` is a C layout: size, alignment, and field offsets. It is not an Avant value. You do not construct it with `{ }`. You read and write fields through `Ptr(T)`.

```avant
lib point {
  struct Point {
    x: Int
    y: Int
  }
}

fn (p: Ptr(Point)) sum: Int {
  p.x + p.y
}
```

`Ptr(Int)` is rejected. `Ptr` of a layout, and `Ptr(Void)`, are the pointer types this is for. A `String` stored into a C field is pinned for the process, so the collector will not move it while C holds the pointer.

JSON in the examples is yyjson. The loopback HTTP example is uSockets (C, not C++). Both live under `runtime/third_party/`.

## Concurrency

### spawn

`spawn { }` runs the block on a pool of OS threads. The pool size is the number of processors online, read once when the pool starts. `AVANT_THREADS` replaces that size and must be a positive integer. Extra tasks wait in a queue.

The block's result is `Int`. `.join` waits for it. Calling `.join` or `.parallel` from inside a pool worker is an error. A nested `spawn` from a worker is allowed.

An `Array` captured from the enclosing scope is read-shared: it is read-only inside the block, and the sender may not mutate it until the handle is joined. Both are compile errors. Mutation crosses a task boundary through a `Mutex(Array)`, a `Channel`, or `.parallel` instead.

```avant
fn six_times_seven: Int {
  6 * 7
}

fn main {
  h = spawn { six_times_seven() }
  puts(h.join)
}
```

### What may cross a task

A value may be sent into `spawn` or `.parallel` when it is `Int`, `Int64`, `UInt64`, `Bool`, `Float64`, or a struct whose every field may cross. A `String` crosses as a shared pointer: both tasks keep it alive. A `Channel` or `Mutex` value crosses.

A class, `Array`, `Hash`, `fn`, or `JoinHandle` does not cross. A struct that contains one of those does not either.

### parallel

`items.parallel { |item| ... }` on an `Array(T)` takes one block parameter and returns `Array(U)` in input order. The parent waits outside the pool.

```avant
fn main {
  xs = [1, 2, 3, 4]
  ys = xs.parallel { |n| n * n }
  puts(ys[0])
}
```

`parallel { }` as a statement, with no receiver, is not built.

### Channel and Mutex

`Channel(T)` and `Mutex(T)` are ordinary generic classes. Nothing imports them for you. The proof is `tests/cases/s35.av`.

`Channel(T).new` is a rendezvous channel. `Channel(T).new(n)` is buffered. `recv` returns `T?`. `Mutex(T).with { |x| ... }` holds the lock for the block. `x` is a place that dies with the block. `return` inside `with` is an error. Waiting and waking go through the runtime's `avant_gate_*` functions.

## Macros

`quote { }` pastes functions at the top level or in a type body, and statements inside a function. `#()` splices an expression, and only inside `quote`. Names the quote introduces are hygienic: they do not capture names from the paste site. Nested `quote` is an error. There is no general comptime interpreter.

The one comptime loop walks fields:

```avant
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

That pastes `fn x` and `fn y`. `for f in Box.fields` on a generic type is an error. The walk wants a concrete, non-generic name.

A quote can also introduce a whole function:

```avant
quote {
  fn doubled(n: Int): Int {
    n + n
  }
}
```

## Formatter

`format` (short name `f`) reprints a file from its tokens. It does not parse, typecheck, or follow imports. Comments stay. Your line breaks stay. Blank lines collapse to one. Indent is two spaces. The canonical colon is `name: Type`.

`name : Type` is accepted. The formatter prints `name: Type`: no space before the colon, one space after.

Stdout is the default. `--write` replaces the file. `--check` prints `canonical` or `would change PATH` and does not write. A lex error prints the diagnostic and leaves the file alone.

```
./bin/avant-av-b format FILE.av
./bin/avant-av-b format FILE.av --check
./bin/avant-av-b format FILE.av --write
```

`compile`, `run`, and `dump` still drop comments. Formatting is a separate command so a format bug cannot change what a program means.

## Regex

Three calls: `re_compile(pattern, flags)` returns a handle (`flags` 1 means case-insensitive), `re_find(handle, text, from)` returns 1 on a match, and `re_count(pattern, text, flags)` counts non-overlapping matches. After a find, `re_m0()`/`re_m1()` are the match bounds and `re_c0()`/`re_c1()` the first capture group.

```avant
fn main {
  re = re_compile("x(y)", 0)
  if re_find(re, "zxy", 0) == 1 {
    puts(re_m0())  // 1
    puts(re_c0())  // 2
  }
  puts(re_count("a+", "aa ba", 0))  // 2
}
```

A string-literal pattern is compiled to a native scanner with re2c at compile time; a computed pattern (or one re2c cannot express, such as a backreference) stays on PCRE2 with JIT, and the compiler says so. `re_count` always uses PCRE2. Without `re2c` installed, literals use PCRE2 too; `AVANT_RE2C` overrides the binary.

## Builtins

Runnable examples live in `examples/` (`hello`, `sieve`, `errors`, `collect`, `json`, `spawn`, `matmul`, and more). Beyond the core language, programs can use `sha256`, `zlib_compress`, the `json_*` family, base64, the `re_*` family (see [Regex](#regex)), `sqrt`, `dir_list`, `now_ms` / `now_us`, `file_read` / `file_write` / `file_exists`, `argv`, `env_get`, and `process_run` / `process_run_out`.

## Not built yet

These are decided and currently do not compile.

| What | Status |
| --- | --- |
| `parallel { }` with no receiver | decided, not shipping |
| Packages, an HTTP server | decided, not shipping |
| A general `for` loop | not part of the language yet; `each` and `while` are the loops |

## Benchmarks

Seconds, lower is better. **Bold** is fastest. Median of 3 runs on one machine (Ryzen 7 5700G, 2026-10-26), every language built here, checksums 50/50 in every run. Full provenance and raw data: `results/2026-10-26-waveA/SUMMARY.md`.

Three honest caveats. `Etc::TextRaytracer` (‡) is invalid for Avant and is excluded from the totals: `--final` hoists its pure render out of the loop (0.013s; honest `--debug` run 10.86s vs C 0.936s). JSON, Base64, hashing and regex rows lean on C libraries (yyjson, aklomp, PCRE2) in every language, so they measure bindings as much as languages. Memory is not ranked here: Avant averages 29.67 MB per task, while the other ports run the whole suite in one process and only expose a suite peak. This is not the adopted official number; that remains the 2026-09-27 run.

| Language | 49 tasks (s) | 50 as recorded (s) |
| --- | ---: | ---: |
| Avant | **40.245** | 40.258 |
| C | 45.443 | 46.379 |
| Rust | 46.587 | 47.402 |
| Crystal | 75.241 | 76.352 |
| Go | 96.233 | 97.758 |

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Etc::Sieve` | 1.286 | **1.092** | 1.146 | 1.939 | 1.568 |
| `Binarytrees::Obj` | 1.167 | 2.224 | 2.331 | **1.069** | 1.564 |
| `Binarytrees::Arena` | 0.508 | **0.233** | 0.262 | 0.604 | 0.265 |
| `Matmul::Single` | **1.401** | 4.656 | 4.826 | 4.980 | 4.879 |
| `Matmul::T4` | **0.473** | 1.190 | 1.225 | 2.995 | 1.299 |
| `Matmul::T8` | **0.345** | 0.610 | 0.629 | 4.791 | 0.718 |
| `Matmul::T16` | **0.332** | 0.430 | 0.432 | 4.875 | 0.634 |
| `Sort::Quick` | 1.363 | **0.981** | 0.982 | 1.285 | 1.111 |
| `Sort::Merge` | **1.136** | 1.188 | 1.288 | 1.927 | 1.362 |
| `Sort::Self` | **0.131** | 1.776 | 0.290 | 0.888 | 1.131 |
| `CLBG::Mandelbrot` | 0.980 | **0.814** | 0.845 | 0.821 | 0.838 |
| `CLBG::Fannkuchredux` | 1.161 | 1.004 | **0.947** | 1.005 | 0.976 |
| `CLBG::Spectralnorm` | 1.041 | **0.853** | 0.854 | 0.864 | 0.892 |
| `CLBG::Nbody` | 1.295 | 1.044 | **0.994** | 1.077 | 1.205 |
| `Brainfuck::Array` | 1.671 | **1.287** | 2.726 | 1.957 | 1.425 |
| `Brainfuck::Recursion` | 1.830 | **1.427** | 1.593 | 1.696 | 1.702 |
| `Etc::GameOfLife` | 1.882 | 1.307 | 1.237 | 2.369 | **0.812** |
| `Etc::TextRaytracer` | 0.013 | 0.936 | **0.815** | 1.111 | 1.525 |
| `Etc::NeuralNet` | 1.359 | **0.957** | 0.963 | 1.049 | 1.841 |
| `Etc::CacheSimulation` | **0.541** | 0.631 | 0.697 | 1.090 | 1.389 |
| `Etc::Words` | 1.639 | 1.660 | **0.718** | 0.805 | 1.017 |
| `Graph::BFS` | **1.018** | 1.024 | 1.411 | 2.465 | 2.061 |
| `Graph::DFS` | 0.954 | **0.931** | 1.116 | 1.638 | 1.283 |
| `Graph::AStar` | **0.530** | 0.572 | 0.564 | 1.585 | 3.107 |
| `Hash::CRC32` | 0.323 | 1.119 | **0.260** | 0.811 | 3.492 |
| `Hash::SHA256` | 1.325 | **0.887** | 1.021 | 1.001 | 1.014 |
| `Distance::Jaro` | 0.934 | **0.756** | 0.760 | 1.069 | 0.773 |
| `Distance::NGram` | 0.782 | 1.415 | **0.411** | 0.656 | 0.776 |
| `Base64::Encode` | 0.107 | **0.106** | 0.251 | 1.224 | 2.010 |
| `Base64::Decode` | 0.131 | **0.121** | 0.285 | 1.246 | 1.444 |
| `Json::Generate` | 0.752 | **0.239** | 0.271 | 0.959 | 1.026 |
| `Json::ParseDom` | 0.081 | **0.073** | 0.749 | 0.525 | 1.168 |
| `Json::ParseMapping` | 0.194 | **0.183** | 0.334 | 1.314 | 2.316 |
| `CSV::Parse` | 2.319 | 1.853 | **0.672** | 3.092 | 2.119 |
| `Template::Regex` | **0.889** | 1.144 | 3.465 | 1.445 | 6.662 |
| `Template::Parse` | **0.758** | 0.946 | 1.027 | 2.349 | 0.784 |
| `Maze::Generator` | 0.438 | **0.409** | 0.713 | 1.443 | 0.921 |
| `Maze::BFS` | 0.513 | **0.442** | 0.912 | 1.889 | 1.523 |
| `Maze::AStar` | 0.953 | **0.729** | 1.081 | 2.482 | 3.165 |
| `Etc::LogParser` | 0.474 | 0.398 | **0.270** | 1.002 | 23.826 |
| `Calculator::Ast` | **0.264** | 0.695 | 0.524 | 0.440 | 0.472 |
| `Calculator::Interpreter` | **0.439** | 0.840 | 0.730 | 0.921 | 0.767 |
| `Compress::BWTEncode` | 0.436 | 0.399 | **0.299** | 1.196 | 1.962 |
| `Compress::BWTDecode` | 0.693 | **0.360** | 0.391 | 0.594 | 0.607 |
| `Compress::HuffEncode` | **0.537** | 0.600 | 0.559 | 0.971 | 0.758 |
| `Compress::HuffDecode` | 0.506 | 0.676 | **0.471** | 0.834 | 1.121 |
| `Compress::ArithEncode` | 1.111 | 0.863 | **0.805** | 0.948 | 0.840 |
| `Compress::ArithDecode` | 0.903 | **0.720** | 1.119 | 0.786 | 0.753 |
| `Compress::LZWEncode` | **0.153** | 1.106 | 0.590 | 1.207 | 1.224 |
| `Compress::LZWDecode` | **0.187** | 0.473 | 0.541 | 1.063 | 1.631 |

## Goals

Beginners welcome, advanced programmers unblocked, memory managed, errors as values. The target: Crystal-class expressiveness, runtime nearer C/Rust than Crystal is today, clearly faster than Go, memory better than Crystal without giving up the collector. LangArena is the public scoreboard.

## Advanced: building the compiler

Everything below is for compiler developers, not for writing Avant programs.

The compiler is self-hosted: it is written in Avant (`compiler/*.av`) and compiles itself. It emits [myc](https://github.com/kostya/myc) IR and runs `myc-llvm` from the safepoints shard (which still uses Crystal; that Crystal is not in this repository).

```
.av  →  parser and checker (compiler/*.av)  →  myc IR  →  myc-llvm  →  binary
```

Build the next compiler with a published one (the `v0.11.0` seed or newer; `avant bind` additionally needs libclang):

```
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant   # rebuild with itself
```

`AVANT_ROOT` overrides where the compiler looks for `runtime/build`. Daily compiles use myc-llvm `--debug` (fast); `AVANT_MYC_DEBUG=0` restores myc's default codegen and `AVANT_MYC_FINAL=1` selects `--final`. The same choice is available as flags on `avant compile` (and `run`): `--debug` selects the fast daily codegen and `--release` selects `--final`, the scored mode used in [Benchmarks](#benchmarks). An explicit flag wins over the env vars; the env vars keep working for one transition and must not carry scored state. Safepoints stay on in every mode. `dump FILE.av` prints myc IR without linking.

Tests are the native suite, run on the compiler you just built (B) and, for a release, on the next one (C):

```
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
AVANT_COMPILER=./bin/avant-av-c ./bin/avant-av-c run tests/run.av
```

Adding `tests/cases/foo.av` needs no edit to `tests/run.av`. `--coverage` (or `AVANT_COVERAGE=1`) writes `lcov.info`; goldens live in `tests/goldens/`.

## License

[MIT](LICENSE).
