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

1. Make sure a C compiler (`cc`), LLVM 20, and libpcre2 are installed (`re2c` is optional; without it, regex literals use PCRE2).
2. Download the latest `avant-linux-x86_64.tar.gz` from [Releases](https://github.com/avant-lang/avant/releases) and extract it. The archive holds `avant`, its `myc-llvm` backend, and `lib/std/testing.av` beside the compiler.
3. Install both binaries into `~/.local/bin` (recommended: it needs no sudo, and keeping `avant` and `myc-llvm` together means the compiler finds its backend with no configuration). For a system-wide install, use `/usr/local/bin` instead (that one needs sudo).

```sh
mkdir -p ~/.local/bin/lib/std
install -m 755 avant myc-llvm ~/.local/bin/
install -m 644 lib/std/testing.av ~/.local/bin/lib/std/testing.av
```

Make sure `~/.local/bin` is on your `PATH`; if it is not, add it once:

```sh
export PATH="$HOME/.local/bin:$PATH"   # add this line to your ~/.bashrc or ~/.zshrc to keep it
```

No environment variable needs to be set for the compiler itself. The compiler finds its `myc-llvm` backend in this order: `myc-llvm` on `PATH`, then a `myc-llvm` beside the `avant` binary, then `AVANT_MYC_LLVM` (an advanced override for unusual layouts). The first binary found must accept the safepoints GC flags; a stock `myc-llvm` is refused with an error naming the path, never silently used. Because the release archive bundles `myc-llvm` beside `avant`, installing both as above is enough; a separate backend download is only needed with an older archive.

4. Run your first program:

```sh
avant run hello.av          # compile and run
avant compile hello.av hi   # or build a binary named hi
./hi
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
14. [Testing](#testing)
15. [Builtins](#builtins)
16. [Not built yet](#not-built-yet)
17. [Benchmarks](#benchmarks)
18. [Goals](#goals)
19. [Advanced: building the compiler](#advanced-building-the-compiler)
20. [License](#license)

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

`for` walks a range or an array. The range is end-exclusive and its bounds are evaluated once; the loop variable is a new name that exists only inside the loop, and `break`, `continue`, and `return` behave as they do in `while`:

```avant
fn main {
  sum = 0
  for i in 0..10 {
    sum = sum + i
  }
  for x in xs {
    sum = sum + x
  }
}
```

The comptime field walk in [Macros](#macros) is the other `for`, and it keeps its own spelling.

## Modules

A file is a module. Its name is its path from the directory that owns it, with a slash written as a dot: `http/server.av` is `http.server`, and a file beside the compilation root keeps that file's basename. `import http.server` loads `http/server.av`. A keyword after a dot is a name, so `import http.spawn` loads `http/spawn.av` and a clash is qualified as `http.spawn.Server`. A keyword that starts a phrase stays that keyword. There is no `module` line, and a directory is not a module.

The compiler looks beside the importing file, then in the directory of the file being compiled, then under the nearest `avant.toml` above that file, then in each fetched dependency. The same path found in two of those places is one file. Two different files for one import are an error. A dependency does not look in the program that imported it. Names are private unless marked `pub`. Public names enter the importer's scope. If two imports publish the same name, qualify with the full module name (`http.server.Server`). Cycles are an error, including across directories. The compiler takes one root file and follows imports. It does not take a list of files to concatenate. The first segment `std` is reserved for the standard library. `import std.testing` loads the bundled test library, and a project file cannot take that name.

A file under `tests/math/` can import the project next to `avant.toml` and use the public names unqualified:

```avant
import vec
import http.server

fn main {
  v = Vec2 { x: 1, y: 2 }
  s = Server.new
}
```

```avant
import vec

fn main {
  v = origin()
  puts(v.x)
}
```

`origin` is `pub` in `vec.av`. `fn main` in the root is the entry point. It is not `pub`.

### Packages

A package is a git repository of `.av` files. A project that uses packages carries an `avant.toml` beside its root file and runs `avant fetch` to download the dependencies into a `deps/` folder. There is no central registry: a dependency is named by its git URL, and `avant.lock` records the exact commit that was fetched. Compilation still starts at one root file and follows `import`, exactly as described above, and everything still compiles as one program. The manifest is tooling only; it is not a second way to load modules.

A fetched project looks like this:

```text
myapp/
  avant.toml      # the dependencies you asked for
  avant.lock      # the exact commits fetch resolved (generated)
  main.av         # your root file
  deps/
    greeter/      # a git checkout of one dependency
      avant.toml
      greeter.av
```

`avant fetch` needs `git` on the machine (it looks in `/usr/bin`, `/usr/local/bin`, then `/bin`).

#### The manifest

`avant.toml` has a `[package]` section naming your own project, and a `[dependencies]` section naming what to fetch. Each dependency gives a `git` URL and a `rev` to check out. A `rev` may be a full commit SHA, a tag, or a branch; pinning a SHA or a tag is the reproducible choice.

```toml
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
greeter = { git = "https://github.com/example/greeter.git", rev = "v1.0.0" }
```

The same dependency may also be written as its own section:

```toml
[dependencies.greeter]
git = "https://github.com/example/greeter.git"
rev = "v1.0.0"
```

A dependency with no `git`, a duplicate dependency, or an unknown key is an error with the file and line named. A bare version string such as `greeter = "1.0"` is also an error, because there is no registry to resolve it against.

#### Installing a package

Add the dependency to `avant.toml` as above, then run `avant fetch` with the project directory (the current directory when the argument is omitted):

```bash
avant fetch
```

To fetch a project that lives somewhere else, name its directory:

```bash
avant fetch path/to/myapp
```

Fetch clones each dependency into `deps/`, checks out the pinned commit, and writes `avant.lock`. A successful fetch prints one line per dependency with the commit it resolved:

```text
fetched greeter 58a9ab1a2db9803a031edf34adb9e16c74f9d126
```

The lockfile is generated; do not edit it by hand:

```toml
# avant.lock - generated by avant fetch. Do not edit.
version = 1

[[package]]
name = "greeter"
source = "git+https://github.com/example/greeter.git#58a9ab1a2db9803a031edf34adb9e16c74f9d126"
```

If any dependency cannot be cloned or checked out, fetch prints the git output, exits with a non-zero status, and writes no lockfile, so a failed fetch never leaves a half-recorded set of versions behind.

#### Importing an installed package

You import a module by its path from the checkout root, not by the dependency's package name. A file `greeter.av` at the root of the checkout is `greeter`. A nested file `http/server.av` in that checkout is `http.server`, with no package prefix. The checkout does not search your tree:

```avant
import greeter

fn main {
  puts(greeting())
}
```

That program works when `greeter.av` in the checkout publishes the name it uses:

```avant
pub fn greeting: String {
  "hello from greeter"
}
```

Compiling and running the project needs no extra flags:

```bash
avant run main.av
```

The compiler looks beside the importing file, then in the directory of the file being compiled, then under the project's `avant.toml`, then in each fetched checkout under `deps/`. A file inside a dependency uses that checkout as its root and does not look back into your program. Visibility is the same as for your own files: only `pub` names enter your scope, and a module inside a dependency may itself import other modules, including modules from other fetched dependencies. If the module cannot be found and the project has a manifest, the error tells you the remedy:

```text
error[E5000]: cannot find greeter.av next to main.av or in fetched deps (run: avant fetch .)
```

As a reminder of the shape a dependency takes, a project that has never been fetched does not compile, and one command fixes it:

```bash
avant fetch
avant run main.av
```

#### Updating a package

A plain `avant fetch` is reproducible: when `avant.lock` already records a commit for a dependency, fetch checks out that commit again, even if the branch or tag in `avant.toml` has since moved. Fetching after a dependency publishes new commits therefore changes nothing until you ask for the update.

To move a single package to a chosen commit, pin that commit (or a tag) as its `rev` in `avant.toml`:

```toml
[dependencies]
greeter = { git = "https://github.com/example/greeter.git", rev = "a15f74088f34acd03cfbb9e096f417ad857f8f3a" }
```

Then fetch again, which checks out the new pin and rewrites the lockfile:

```bash
avant fetch
```

To update every package at once, re-resolve all branch and tag pins to their current commits:

```bash
avant fetch --update
```

`--update` deletes the existing checkouts, clones them again, and rewrites `avant.lock` with the commits it found. Commit the updated `avant.lock` afterwards so everyone else fetches the same code (see "What to commit" below).

#### Creating and publishing a package

A package is an ordinary git repository whose `.av` module files sit at the repository root, because the compiler looks for `deps/<name>/<module>.av`. Give the repository an `avant.toml` naming the package; its own dependencies, if any, are declared the same way and are fetched transitively for your consumers.

```toml
[package]
name = "greeter"
version = "1.0.0"
```

Write the modules it publishes, marking the public surface with `pub` (everything else stays private to the package):

```avant
pub fn greeting: String {
  "hello from greeter"
}

fn whisper: String {
  "psst"
}
```

Publishing is pushing the repository to any git host (or sharing any URL `git clone` can read, including a local path for private or offline use), ideally with a tag that consumers can pin:

```bash
git init
git add -A
git commit -m "greeter 1.0.0"
git tag v1.0.0
git push origin main v1.0.0
```

Consumers then install it by URL and tag, as in "Installing a package" above. A new published version is a new tag or commit; existing consumers keep the commit their lockfile records until they update.

#### Package conflicts

Three different clashes can occur, and each is reported rather than silently resolved.

If two fetched dependencies both provide a module with the same file name, importing that name is a compile error, because the compiler will not guess which one you meant:

```text
error[E5000]: import shared is ambiguous: more than one fetched dependency provides shared.av
```

Rename the module in one of the packages (or stop fetching one of them); there is no priority order between dependencies. A file beside your own root file is different: your local `shared.av` is found first and shadows any fetched copy, which is the supported way to patch a dependency locally while you work.

If the same dependency name reaches two different commits — for example your manifest pins one commit while a transitive dependency asks for another — fetch stops instead of picking a winner:

```text
avant fetch: dependency greeter resolves to two different commits (...); one version per name in this spike
```

Align the pins in `avant.toml` (your manifest's entry is the one to change, since transitive requests are fixed by their publishers) and fetch again. One name always means one version in a project.

The third clash is about public names, and predates packages: if two imported modules publish the same name, qualify the use with the module name, as in `lexer.Lexer`. Fetching a package does not change that rule.

For example, suppose one fetched dependency provides `alpha.av`, which publishes a function named `value`:

```avant
pub fn value: Int {
  1
}
```

Another provides `beta.av`, which publishes a class with the same name:

```avant
pub class value {
  n: Int

  fn initialize(n: Int) {
    self.n = n
  }
}
```

Using the bare name `value` as a type is then ambiguous, because the compiler cannot tell the function and the class apart:

```avant
import alpha
import beta

fn show(v: value): Int {
  v.n
}
```

```text
error[E4000]: ambiguous name value; qualify it
```

Qualifying each use with its module name says which published name you mean, and the program compiles:

```avant
import alpha
import beta

fn show(v: beta.value): Int {
  v.n
}

fn main {
  puts(alpha.value())
  puts(show(beta.value.new(2)))
}
```

Running that program prints the function's result and the class field, one per line:

```bash
avant run main.av
```

```text
1
2
```

#### Removing a package

Delete the dependency's entry from `avant.toml`, delete its checkout, and fetch again so the lockfile is rewritten without it:

```bash
rm -rf deps/greeter
avant fetch
```

Until the checkout is deleted, the compiler can still find its modules under `deps/`, so removing the manifest entry alone is not enough.

#### What to commit

Commit `avant.toml` and `avant.lock` with your sources. Do not commit `deps/`: it is recreated by `avant fetch`, and the lockfile already records exactly what belongs there. A fresh clone of your project is built in two steps:

```bash
avant fetch
avant run main.av
```

#### Requirements and limits

Packages require `git` at fetch time only; compiling and running afterwards work offline from the checkouts in `deps/`. Module files must sit at a dependency's repository root (subdirectories are not searched), one dependency name resolves to one commit per project, and there is no registry, no version-range solving, and no per-package build step: a dependency is source that compiles together with your program as one unit, under the same `pub` visibility and cycle rules as your own files.

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

`split(sep)` splits on a separator and keeps empty fields, `fields()` splits on runs of whitespace and drops empties, and `strip()` / `strip_left()` / `strip_right()` trim whitespace. `Bytes` is the immutable binary counterpart of `String`: `s.bytes` views a string's bytes, `String.from_utf8(b)` returns `String?` (nil on invalid UTF-8), `Bytes` supports `size`, indexing, `.get(i)`, and `.slice(a, b)`, and `Buf` converts with `.to_bytes` / `push_bytes(b)` and `b.to_buf`.

Indexing stays byte-based; the text-correct path is `.chars`, which returns one `String` per Unicode codepoint on both `String` and `Bytes` (`Bytes` traps on invalid UTF-8). A raw string `r"..."` is single-line with no escapes and no interpolation, for paths and templates: in `r"C:\tmp"` the backslash is a literal character, not an escape. Backtick strings are not a form.

On a nilable value, `v ?? fallback` and `v.or(fallback)` are the fallback value when `v` is nil, with the fallback evaluated only then. Predicate methods return `Bool`, including `Buf.starts_with`, and arrays add the short-circuiting scans `any { |x| ... }` and `all { |x| ... }` with a `Bool`-returning block.

### Arrays, again

Indexing is `xs[i]`. `push` may grow the buffer. `each` is the iteration form. A running total is an outer local, assigned from inside the block:

Indexing an `Array`, `String`, `Buf`, or `Bytes` outside `0 <= i < size` traps at runtime: one diagnostic on stderr and a non-zero exit. `.get(i)` returns `T?` instead — the element or nil. Integer `+`, `-`, `*` (and `/`, `%`) are checked the same way: overflow and division by zero trap. The wrapping spelling is explicit literal text: `&+`, `&-`, `&*` wrap on overflow. Checks the compiler can prove safe — a literal index into a literal, an induction variable under its loop bound, a loop whose whole index range is validated once on entry — are not emitted.

`sort` orders `Int`, `Int64`, `UInt64`, `Float64` (NaN last), `String` and `Bytes` (byte order), and `Bool` elements in place, or any element type with a comparator block `sort { |a, b| ... }` returning `Bool`; `sorted` returns a new array and leaves the original alone. `reverse`, `contains(x)`, `index_of(x): Int?`, `min` / `max: T?`, and `sum` cover the other common scans. Hashes add `keys`, `values`, `has_key(k)`, `get_or(k, default)`, `inc(k, by = 1)`, and `each { |k, v| ... }`; `keys` and `values` are in unspecified order.

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

A heap object stored into a C field is pinned for the process. Pinned objects are not copied. Code that hands a heap value to C for a bounded time can pin it for exactly that span with `pin(value) { ... }`: the value stays put for the block and is unpinned on every way out, and pins are counted, so a scoped pin never clears a process pin. See [C interop](#c-interop).

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

A `fun` parameter may be an Avant `fn` type; that declares a C callback. Pass a function name or a lambda that captures nothing, and C calls it with exactly the declared arguments (libc `qsort` with an Avant comparator is the proven shape). A capturing lambda, a `fn` value held in a variable, and a C function returning a function pointer do not compile.

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

`Channel(T)` and `Mutex(T)` are ordinary generic classes. Nothing imports them for you. The proof is `tests/cases/test_s35.av`.

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
avant format FILE.av
avant format FILE.av --check
avant format FILE.av --write
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

## Testing

`avant test` discovers and runs a project's tests. A test file is an ordinary program. It imports `std.testing`, checks what it cares about, and returns `t.finish` from `fn main: Int`. That integer is the process status: `0` when every check passed, `1` when any failed. A void `fn main` is still a successful program and does not change. The runner trusts the status. It does not read the words `PASS` and `FAIL`.

```avant
import std.testing

fn greet(name: String): String {
  "Hello, ${name}"
}

fn main: Int {
  t = Test.new
  t.check(greet("Ada") == "Hello, Ada", "greet uses the name")
  t.check_eq(greet(""), "Hello, ", "greet handles empty")
  t.finish
}
```

Save it as `tests/test_greet.av` and run `avant test`. With no path, that walks `tests/` recursively for `test_*.av`, relative to the nearest `avant.toml`, or relative to the current directory when there is no manifest. `avant test DIR` is the same walk. `avant test FILE.av` runs that file even when the name does not match. An empty selection exits 1. The file prints `PASS greet uses the name`, `PASS greet handles empty`, and `0 failed, 2 passed`. A failing check prints `FAIL` and the name, then the run continues, so one file reports every failure. `finish` returns 1 when any check failed.

`check_eq` is an overload for `Int`, `Bool`, and `String`. On failure it prints `got` and `want` under the `FAIL` line.

Several cases over the same function read better as a table. Keep the inputs and the expected values in two arrays of the same length, and check them in one loop.

```avant
import std.testing

fn greet(name: String): String {
  "Hello, ${name}"
}

fn main: Int {
  t = Test.new
  names = ["Ada", "Bo", "Cy"]
  wants = ["Hello, Ada", "Hello, Bo", "Hello, Cy"]
  i = 0
  while i < names.size {
    t.check(greet(names[i]) == wants[i], "greet ${names[i]}")
    i = i + 1
  }
  t.finish
}
```

Name each check after the case it covers, so a `FAIL` line alone says what broke. Put the functions under test in their own module and `import` it. A test sees `pub` names only.

Checks written in the file, and bodies passed to `t.test`, run one at a time in source order. `t.parallel` overlaps other parallel tests of that same file, and only after the file's serial work. Each parallel test is a new process of the same binary. The file runs again, serial checks and `t.test` bodies do not, and only that parallel body does. Code in `main` outside those bodies runs in every process. `avant run` on the file runs it once, so parallel bodies do not run until `avant test` starts them.

```avant
import std.testing

fn main: Int {
  t = Test.new
  t.test("adds") {
    t.check(1 + 2 == 3, "1+2")
  }
  t.parallel("other") {
    t.check(true, "other")
  }
  t.finish
}
```

Files run at the same time by default. `--jobs` is how many test processes may run at once. It defaults to the number of online processors and does not read `AVANT_THREADS`. `--jobs 1` runs one file at a time. `--shard I/N` runs one slice of the selection, counted from 1, and may be combined with `--jobs`. It does not launch the other slices. The log is flushed in path order, and within a file in source order. The runner then prints `ok tests/test_greet.av` or `FAIL tests/test_greet.av`, and a total. Its own status is 1 when any file failed, when nothing was selected, or when the command was refused.

A file listed in `[test] exclusive` runs only while no other selected test file is compiling or running. A benchmark that needs a quiet machine, as far as the other tests are concerned, is its own exclusive file. The list does not discover files. On a command with no path, every listed path has to be one of the discovered tests. `--shard` is an error when the selection contains an exclusive file, unless `--skip-exclusive` is also passed. With `--jobs 1`, every file is already alone and the list does not change the order.

```toml
[test]
dir = "tests"
exclusive = ["tests/test_bench.av"]
```

`dir` replaces the default `tests/` walk. `entry` names one file and replaces the walk. Setting both is an error. A path on the command overrides `dir` and `entry`. There is no command string and no `jobs` key. Paths in the section are relative to the manifest.

`golden` compares a string to a saved file. `run_golden` compiles and runs a program and compares its stdout to a saved file. `rejects` compiles a source file and requires a non-zero status whose stderr contains a substring. Nothing rewrites an expected file from the program's output. Update the saved file by hand, after reading the new output.

```avant
import std.testing

fn main: Int {
  t = Test.new
  t.golden("want.txt", "Hello Avant\n")
  t.run_golden("hello.av", "hello.want")
  t.rejects("bad.av", "unknown name")
  t.finish
}
```

`want.txt` and `hello.want` are the exact bytes, including a trailing newline when the program prints one. `run_golden` fails the check when the program's status is non-zero. `rejects` fails the check when the source compiles or the diagnostic does not contain the substring.

`avant test` compiles each file and runs that binary, so the process status is the file's. `avant run FILE.av --` does the same when the program takes arguments. `avant run FILE.av` without `--` stays on the fast path and does not forward the status.

`avant run [RUN-OPTIONS] FILE.av [-- PROGRAM-ARGS...]` is how a program receives arguments. Options may sit before the file or after it, up to `--`, so `avant run FILE.av --debug` still works. Arguments after `--` are the program's, starting at `argv()` index 1. `avant test` does not forward arguments.

```avant
fn main: Int {
  args = argv()
  if args.size > 1 {
    puts(args[1])
  }
  0
}
```

```sh
avant run prog.av -- hello
```

`--junit PATH` writes one JUnit case per file. It is off unless you pass it. Other flags are an error, including `--jobs 0`.

Testing the compiler itself is different, and is covered for contributors under [Advanced: building the compiler](#advanced-building-the-compiler). The programs in this section are exercised by `tests/cases/test_testing_docs.av`.

## Builtins

Runnable examples live in `examples/` (`hello`, `sieve`, `errors`, `collect`, `json`, `spawn`, `matmul`, and more). Beyond the core language, programs can use `sha256`, `zlib_compress`, the `json_*` family, base64, the `re_*` family (see [Regex](#regex)), `sqrt`, `dir_list`, `now_ms` / `now_us`, `file_read` / `file_write` / `file_exists`, `argv`, `env_get`, and `process_run` / `process_run_out`.

## Not built yet

These are decided and currently do not compile.

| What | Status |
| --- | --- |
| `parallel { }` with no receiver | decided, not shipping |
| An HTTP server | decided, not shipping |
| An inclusive range (`a..=b`) | not part of the language yet; write `a..b + 1` |

## Benchmarks

Seconds, lower is better. **Bold** is fastest. Measured on a Ryzen 7 5700G.

| Language | 49 tasks (s) | Memory |
| --- | ---: | --- |
| Avant | 50.876 | 33 MB |
| C | **45.443** | 23 MB |
| Rust | 46.587 | 29 MB |
| Crystal | 75.241 | 67 MB |
| Go | 96.233 | 67 MB |

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Etc::Sieve` | 1.710 | **1.092** | 1.146 | 1.939 | 1.568 |
| `Binarytrees::Obj` | **1.037** | 2.224 | 2.331 | 1.069 | 1.564 |
| `Binarytrees::Arena` | 0.643 | **0.233** | 0.262 | 0.604 | 0.265 |
| `Matmul::Single` | **1.406** | 4.656 | 4.826 | 4.980 | 4.879 |
| `Matmul::T4` | 1.755 | **1.190** | 1.225 | 2.995 | 1.299 |
| `Matmul::T8` | 1.156 | **0.610** | 0.629 | 4.791 | 0.718 |
| `Matmul::T16` | 1.160 | **0.430** | 0.432 | 4.875 | 0.634 |
| `Sort::Quick` | 1.988 | **0.981** | 0.982 | 1.285 | 1.111 |
| `Sort::Merge` | 2.634 | **1.188** | 1.288 | 1.927 | 1.362 |
| `Sort::Self` | **0.139** | 1.776 | 0.290 | 0.888 | 1.131 |
| `CLBG::Mandelbrot` | 0.977 | **0.814** | 0.845 | 0.821 | 0.838 |
| `CLBG::Fannkuchredux` | 1.744 | 1.004 | **0.947** | 1.005 | 0.976 |
| `CLBG::Spectralnorm` | 1.053 | **0.853** | 0.854 | 0.864 | 0.892 |
| `CLBG::Nbody` | 1.290 | 1.044 | **0.994** | 1.077 | 1.205 |
| `Brainfuck::Array` | 2.351 | **1.287** | 2.726 | 1.957 | 1.425 |
| `Brainfuck::Recursion` | 2.629 | **1.427** | 1.593 | 1.696 | 1.702 |
| `Etc::GameOfLife` | 2.260 | 1.307 | 1.237 | 2.369 | **0.812** |
| `Etc::NeuralNet` | 1.372 | **0.957** | 0.963 | 1.049 | 1.841 |
| `Etc::CacheSimulation` | **0.586** | 0.631 | 0.697 | 1.090 | 1.389 |
| `Etc::Words` | 1.381 | 1.660 | **0.718** | 0.805 | 1.017 |
| `Graph::BFS` | 1.222 | **1.024** | 1.411 | 2.465 | 2.061 |
| `Graph::DFS` | 1.257 | **0.931** | 1.116 | 1.638 | 1.283 |
| `Graph::AStar` | 1.018 | 0.572 | **0.564** | 1.585 | 3.107 |
| `Hash::CRC32` | **0.136** | 1.119 | 0.260 | 0.811 | 3.492 |
| `Hash::SHA256` | **0.610** | 0.887 | 1.021 | 1.001 | 1.014 |
| `Distance::Jaro` | 1.165 | **0.756** | 0.760 | 1.069 | 0.773 |
| `Distance::NGram` | 1.155 | 1.415 | **0.411** | 0.656 | 0.776 |
| `Base64::Encode` | **0.106** | **0.106** | 0.251 | 1.224 | 2.010 |
| `Base64::Decode` | 0.127 | **0.121** | 0.285 | 1.246 | 1.444 |
| `Json::Generate` | 0.753 | **0.239** | 0.271 | 0.959 | 1.026 |
| `Json::ParseDom` | 0.080 | **0.073** | 0.749 | 0.525 | 1.168 |
| `Json::ParseMapping` | 0.195 | **0.183** | 0.334 | 1.314 | 2.316 |
| `CSV::Parse` | 1.024 | 1.853 | **0.672** | 3.092 | 2.119 |
| `Template::Regex` | **0.848** | 1.144 | 3.465 | 1.445 | 6.662 |
| `Template::Parse` | 1.027 | 0.946 | 1.027 | 2.349 | **0.784** |
| `Maze::Generator` | 0.703 | **0.409** | 0.713 | 1.443 | 0.921 |
| `Maze::BFS` | 0.717 | **0.442** | 0.912 | 1.889 | 1.523 |
| `Maze::AStar` | 1.358 | **0.729** | 1.081 | 2.482 | 3.165 |
| `Etc::LogParser` | 0.478 | 0.398 | **0.270** | 1.002 | 23.826 |
| `Calculator::Ast` | 0.571 | 0.695 | 0.524 | **0.440** | 0.472 |
| `Calculator::Interpreter` | **0.440** | 0.840 | 0.730 | 0.921 | 0.767 |
| `Compress::BWTEncode` | 0.436 | 0.399 | **0.299** | 1.196 | 1.962 |
| `Compress::BWTDecode` | 0.689 | **0.360** | 0.391 | 0.594 | 0.607 |
| `Compress::HuffEncode` | 0.684 | 0.600 | **0.559** | 0.971 | 0.758 |
| `Compress::HuffDecode` | 0.764 | 0.676 | **0.471** | 0.834 | 1.121 |
| `Compress::ArithEncode` | 1.935 | 0.863 | **0.805** | 0.948 | 0.840 |
| `Compress::ArithDecode` | 1.698 | **0.720** | 1.119 | 0.786 | 0.753 |
| `Compress::LZWEncode` | **0.154** | 1.106 | 0.590 | 1.207 | 1.224 |
| `Compress::LZWDecode` | **0.255** | 0.473 | 0.541 | 1.063 | 1.631 |

Notes:
- The 49-task scope excludes `Etc::TextRaytracer`, which is invalid for Avant (`--final` hoists its pure render)
- JSON, Base64, hashing, and regex are internally based on C libraries

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
/path/to/avant compile compiler/main.av bin/avant --debug
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant --debug   # rebuild with itself
```

`AVANT_ROOT` overrides where the compiler looks for `runtime/build`. Codegen splits by verb: `avant run` with no flag uses myc-llvm `--debug` (fast), and `avant compile` with no flag uses myc's default codegen, which measured 75.5% faster at runtime on the 49-task gate scope for kept artifacts. On either verb, `--debug` selects the fast codegen explicitly and `--release` (or `--final`) selects `--final`, the scored mode used in [Benchmarks](#benchmarks); an explicit flag always wins over the verb default, and no environment variable selects codegen. Safepoints stay on in every mode. `dump FILE.av` prints myc IR without linking.

Tests are the native suite, run on the compiler you just built (B) and, for a release, on the next one (C):

```
AVANT_COMPILER=./bin/avant-b ./bin/avant-b test
./bin/avant-b compile compiler/main.av bin/avant-c --debug
AVANT_COMPILER=./bin/avant-c ./bin/avant-c test
```

Each file in `tests/cases/` named `test_*.av` is one program. A helper shared by two of them lives in `tests/cases/harness.av` and is imported. Run one file with `avant test tests/cases/test_ffi.av`. `--shard 2/4` runs one slice. A shard that would include a file listed in `[test] exclusive` needs `--skip-exclusive`, and those files run in a command that does not pass `--shard`. The exclusive files are the ones that compile the compiler, probe the collector, rewrite `PATH`, use git, or write a coverage report of their own. `--jobs` defaults to the number of online processors. `--junit PATH` writes one case per file. `--coverage` (or `AVANT_COVERAGE=1`) on a program writes that program's coverage; the compiler's own report is the hit file of the instrumented compiler, and `lcov.info` is written once after the processes that update it have finished. Goldens live in `tests/goldens/`.

## License

[MIT](LICENSE).
