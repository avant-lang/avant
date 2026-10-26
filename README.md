# Avant

Avant is a systems language: required braces, type inference, automatic memory management with GenImmix, and ahead-of-time compilation to native code.

The compiler you run is written in Avant (`compiler/*.av`). It is self-hosted and compiles itself. The backend is still [myc](https://github.com/kostya/myc): the compiler emits myc IR and runs `myc-llvm`. Building `myc-llvm` from the safepoints shard still uses Crystal. That Crystal is not in this repository.

## Index

1. [Getting started](#getting-started)
2. [Programs](#programs)
3. [Structs and classes](#structs-and-classes)
4. [Types](#types)
5. [Control flow](#control-flow)
6. [Modules](#modules)
7. [Errors](#errors)
8. [Strings and collections](#strings-and-collections)
9. [Memory](#memory)
10. [C interop](#c-interop)
11. [Concurrency](#concurrency)
12. [Macros](#macros)
13. [Formatter](#formatter)
14. [Regex](#regex)
15. [Builtins](#builtins)
16. [Toolchain](#toolchain)
17. [Tests](#tests)
18. [Not built yet](#not-built-yet)
19. [Benchmarks](#benchmarks)
20. [Goals](#goals)
21. [License](#license)

## Getting started

### Requirements

A published `avant` compiler (the `v0.9.0` seed or newer), a C compiler (`cc`), LLVM 20, libpcre2, re2c, and `myc-llvm` from the safepoints `v0.4.0` release (`myc-llvm-linux-x86_64.tar.gz`, [repo](https://github.com/avant-lang/safepoints)). The Avant release archive does not include that binary. Set `AVANT_MYC_LLVM` to the extracted `myc-llvm`. `avant bind` needs libclang. Crystal is required only to build `myc-llvm` from that shard. A string literal at `re_compile` is compiled with re2c. A computed pattern uses PCRE2. Without `re2c`, that literal stays on PCRE2.

### Build a compiler

From this directory:

```
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant
./bin/avant run examples/hello.av
```

The published compiler builds the next one. After `bin/avant` exists, rebuild with it:

```
./bin/avant compile compiler/main.av bin/avant
```

`AVANT_ROOT` overrides where the compiler looks for `runtime/build`.

### How a build is code-generated

Daily compiles pass myc-llvm `--debug`. That is fast instruction selection. The compiler's own frontend takes about the same time either way. Safepoints stay on, so the copying nursery still sees stack maps.

`AVANT_MYC_DEBUG=0` restores myc's default codegen. `AVANT_MYC_FINAL=1` asks for `--final` (slower compile, more LLVM optimization). A daily compiler passes `--debug`.

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

You can pass regex patterns as strings to `re_compile`, `re_find`, and `re_count`.

### `re_compile`

```
re_compile(pattern: String, flags: Int): Ptr(Void)
```

`pattern` is the regular expression. `flags` is `0` for a case-sensitive match and `1` for a case-insensitive one. The result is a handle for `re_find`.

A string literal is compiled with re2c. A value computed at runtime stays on PCRE2, and so does a literal re2c cannot express.

```avant
fn main {
  re = re_compile("x(y)", 0)
  bot = re_compile("bot", 1)
  built = "a"
  built = built + "+"
  dyn = re_compile(built, 0)
}
```

`"x(y)"` and `"bot"` are literals. `built` is computed, so `dyn` is a PCRE2 handle. A literal backreference is also PCRE2, and the compiler prints why:

```avant
fn main {
  re = re_compile("(a)\\1", 0)
}
```

### `re_find`

```
re_find(re: Ptr(Void), text: String, from: Int): Int
```

`re` is a handle from `re_compile`. `text` is the string to search. `from` is the byte index to start at. The result is `1` when a match is found and `0` when it is not. The next successful find overwrites the previous span.

`re_m0` and `re_m1` are the byte bounds of the whole match. `re_c0` and `re_c1` are the first capturing group. With no group they repeat the whole match.

```avant
fn main {
  re = re_compile("x(y)", 0)
  if re_find(re, "zxy", 0) == 1 {
    puts(re_m0())
    puts(re_m1())
    puts(re_c0())
    puts(re_c1())
  }
}
```

That prints `1`, `3`, `2`, and `3`: the whole match is `xy`, and the group is `y`.

A second find continues after the previous match:

```avant
fn main {
  re = re_compile("a+", 0)
  i = 0
  text = "xxaa"
  if re_find(re, text, i) == 1 {
    puts(re_m0())
    i = re_m1()
  }
}
```

`re_m0` is `2` and `re_m1` is `4`.

### `re_count`

```
re_count(pattern: String, text: String, flags: Int): Int
```

`pattern` and `flags` are the same as for `re_compile`. `text` is the string to scan. The result is how many non-overlapping matches are in `text`. This call always uses PCRE2, including when `pattern` is a string literal.

```avant
fn main {
  puts(re_count("a+", "aa ba", 0))
  built = "a"
  built = built + "+"
  puts(re_count(built, "aa ba", 0))
}
```

Both lines print `2`.

### Under the hood

A string literal at `re_compile` is compiled with [re2c](https://re2c.org) (`-b`) while the program is compiled. The generated C is linked into the binary, and `re_find` runs that scanner. A pattern built at runtime stays on PCRE2 with JIT.

re2c does not take a backreference, a lookahead, or a lookbehind. The non-greedy form it does take is `.*?` before a one- or two-character literal, as in `\{\{(.*?)\}\}`. Anything else in a literal stays on PCRE2, and the compiler prints `re2c: pattern stays on PCRE2`. If `re2c` is not installed, a literal stays on PCRE2 the same way. `AVANT_RE2C` overrides the `re2c` binary.

## Builtins

These examples run on the identity compiler: `examples/hello.av`, `fact.av`, `vec2.av`, `sieve.av`, `methods.av`, `trees.av`, `binarytrees.av`, `switch.av`, `defaults.av`, `sqrt.av`, `zlib.av` (`--lib z`), `add.av` (`--cc examples/c/add.c`), `errors.av`, `collect.av`, `json.av`, `http.av`, `spawn.av`. `matmul.av` checks that the sequential and spawned checksums match.

Helpers the native suite uses, beyond the core language: `sha256`, `zlib_compress`, `json_parse` / `json_int` / `json_str` / `json_root` / `json_get` / `json_free` / `json_gen_body` / `json_gen_into`, base64, `re_compile` / `re_find` / `re_count` (see [Regex](#regex)), `sqrt`, `dir_list`, `now_ms` / `now_us`, `file_read` / `file_write` / `file_exists`, `argv`, `env_get`, `process_run` / `process_run_out`. A host-only name is ported when a failing native test needs it, not before.

## Toolchain

```
.av  →  parser and checker (compiler/*.av)  →  myc IR  →  myc-llvm  →  binary
```

New language goes in `compiler/*.av`. The compiler is that tree. A published seed builds it. Git history still has the old Crystal host.

```
./bin/avant-av-b run FILE.av
./bin/avant-av-b compile FILE.av OUT
./bin/avant-av-b dump FILE.av
```

`dump` prints myc IR. It does not link a binary.

## Tests

The daily proof is the native suite on B or on the next compiler, C.

```
AVANT_COMPILER=./bin/avant-av-b ./bin/avant-av-b run tests/run.av
./bin/avant-av-b compile compiler/main.av bin/avant-av-c
AVANT_COMPILER=./bin/avant-av-c ./bin/avant-av-c run tests/run.av
```

Adding `tests/cases/foo.av` does not require an edit to `tests/run.av`. The runner stitches `tests/harness.av` and `tests/cases/*.av` into one program.

`--coverage` or `AVANT_COVERAGE=1` instruments the identity compiler and writes `lcov.info`. `AVANT_COVERAGE_LCOV=0` disables that file. A path overrides the name. The compiler stays quiet unless `AVANT_COVERAGE_REPORT=1`. A program compiled with `--coverage` still prints its own table. The runner writes `junit.xml` from the same pass and fail events (`AVANT_JUNIT=0` disables it). Dump goldens live in `tests/goldens/`.

## Not built yet

These are decided and currently do not compile.

| What | Status |
| --- | --- |
| `parallel { }` with no receiver | decided, not shipping |
| Packages, an HTTP server | decided, not shipping |
| A general `for` loop | not part of the language yet; `each` and `while` are the loops |

## Benchmarks

Latest 50-task prod, 2026-10-26, Ryzen 7 5700G, `--final`. C, Rust, and Go are the 2026-10-21 LangArena snapshot (`C/Clang`, `Rust`, `Go`) on a Ryzen 7 3800X. Seconds and megabytes, lower is better. **Bold** is the lowest in that row. This run is not the adopted official number.

| | Avant | C | Rust | Go |
| --- | ---: | ---: | ---: | ---: |
| Wall sum | **48.040** | 54.179 | 54.823 | 114.472 |
| RSS average | 24.70 | **23.41** | 29.48 | 67.37 |

| Task | Avant | C | Rust | Go |
| --- | ---: | ---: | ---: | ---: |
| `Etc::Sieve` | 1.337 | 1.221 | 1.119 | **1.090** |
| `Binarytrees::Obj` | **1.192** | 2.463 | 2.607 | 1.258 |
| `Binarytrees::Arena` | 0.511 | 0.314 | **0.301** | 0.329 |
| `Matmul::Single` | **1.397** | 4.947 | 5.142 | 5.019 |
| `Matmul::T4` | **0.480** | 1.293 | 1.328 | 1.305 |
| `Matmul::T8` | **0.358** | 0.682 | 0.699 | 0.682 |
| `Matmul::T16` | **0.311** | 0.351 | 0.520 | 0.490 |
| `Sort::Quick` | 1.375 | 1.143 | **1.140** | 1.324 |
| `Sort::Merge` | 1.141 | **1.098** | 1.616 | 1.550 |
| `Sort::Self` | **0.154** | 1.991 | 0.417 | 1.282 |
| `CLBG::Mandelbrot` | 0.976 | **0.905** | 0.949 | 0.919 |
| `CLBG::Fannkuchredux` | 1.169 | 1.160 | **1.022** | 1.084 |
| `CLBG::Spectralnorm` | 1.038 | 0.924 | 0.924 | **0.875** |
| `CLBG::Nbody` | **1.283** | 1.502 | 1.384 | 1.662 |
| `Brainfuck::Array` | 1.892 | 1.519 | **1.514** | 1.901 |
| `Brainfuck::Recursion` | 1.923 | 1.632 | **1.630** | 1.874 |
| `Etc::GameOfLife` | 1.893 | 1.946 | 1.693 | **0.881** |
| `Etc::TextRaytracer` | **0.011** | 1.088 | 1.061 | 1.688 |
| `Etc::NeuralNet` | 1.334 | **1.051** | 1.183 | 2.028 |
| `Etc::CacheSimulation` | **0.551** | 0.829 | 0.839 | 1.737 |
| `Etc::Words` | 1.669 | 2.001 | **1.040** | 1.098 |
| `Graph::BFS` | **1.016** | 1.193 | 1.387 | 1.955 |
| `Graph::DFS` | 0.967 | **0.880** | 1.092 | 1.253 |
| `Graph::AStar` | **0.550** | 1.078 | 1.986 | 2.652 |
| `Hash::CRC32` | 0.325 | 0.285 | **0.284** | 4.056 |
| `Hash::SHA256` | 1.314 | 0.923 | **0.904** | 1.082 |
| `Distance::Jaro` | 0.926 | **0.847** | 0.971 | 0.981 |
| `Distance::NGram` | 0.783 | 1.811 | **0.533** | 0.626 |
| `Base64::Encode` | 0.112 | **0.101** | 0.298 | 2.074 |
| `Base64::Decode` | 0.132 | **0.130** | 0.324 | 1.609 |
| `Json::Generate` | 0.753 | 0.499 | **0.290** | 2.398 |
| `Json::ParseDom` | **0.082** | 0.083 | 1.682 | 3.762 |
| `Json::ParseMapping` | **0.194** | 0.203 | 0.424 | 2.614 |
| `CSV::Parse` | 2.312 | 2.334 | **0.815** | 3.022 |
| `Template::Regex` | **0.880** | 1.236 | 3.650 | 8.977 |
| `Template::Parse` | 0.768 | 0.918 | 0.951 | **0.743** |
| `Maze::Generator` | 0.439 | **0.435** | 0.859 | 0.972 |
| `Maze::BFS` | 0.520 | **0.436** | 0.811 | 1.244 |
| `Maze::AStar` | 0.989 | **0.957** | 1.049 | 2.419 |
| `Etc::LogParser` | 0.469 | 0.489 | **0.302** | 30.764 |
| `Calculator::Ast` | **0.266** | 1.052 | 0.608 | 0.540 |
| `Calculator::Interpreter` | **0.459** | 1.225 | 0.784 | 0.638 |
| `Compress::BWTEncode` | 1.339 | 0.739 | **0.418** | 1.669 |
| `Compress::BWTDecode` | 1.293 | 0.888 | **0.879** | 1.133 |
| `Compress::HuffEncode` | 0.704 | 0.812 | **0.673** | 0.905 |
| `Compress::HuffDecode` | 0.702 | **0.445** | 0.820 | 1.567 |
| `Compress::ArithEncode` | **1.119** | 1.169 | 1.151 | 1.254 |
| `Compress::ArithDecode` | **0.916** | 0.957 | 1.483 | 1.088 |
| `Compress::LZWEncode` | **0.184** | 1.440 | 0.666 | 1.072 |
| `Compress::LZWDecode` | 5.532 | **0.554** | 0.601 | 1.327 |

## Goals

Beginners are welcome. Advanced programmers are not blocked. Memory is managed for you. Errors are values. The public scoreboard is LangArena: Crystal's expressiveness, runtime nearer C and Rust than Crystal is today and clearly faster than Go, and memory better than Crystal without giving up the collector.

Official LangArena numbers are still the 2026-09-27 run. A daily `--debug` build is the development compiler. A scored run uses `AVANT_MYC_FINAL=1` when that run is asked for.

## License

[MIT](LICENSE).
