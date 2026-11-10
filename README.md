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

On Linux x86_64 with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39, or later):

```sh
curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
```

The script downloads the release archive, checks its SHA-256, installs it into `~/.avant`, adds that directory to `PATH` in your shell's configuration, and compiles and runs a hello program to prove the install works. Options: a version (`bash install.sh v0.34.0`, or `AVANT_VERSION`), `--no-modify-path` to print the `PATH` line instead of editing your configuration, and `AVANT_INSTALL` to choose another directory. Running it again updates the install in place.

The one prerequisite is a C compiler with the C library's development files, which Avant uses to link programs (and which C interop needs anyway): `sudo apt install gcc libc6-dev` on Debian and Ubuntu, `sudo dnf install gcc glibc-devel` on Fedora. If it is missing, the installer says so and prints that line. Nothing else is needed: the archive carries `avant` with LLVM linked in (no separate backend program), `re2c`, the standard library, and the runtime with PCRE2 and zlib inside. `avant bind` (C header to `lib` block) also needs libclang 20 (`sudo apt install libclang1-20`).

To install by hand instead, download `avant-linux-x86_64.tar.gz` from [Releases](https://github.com/avant-lang/avant/releases), extract it anywhere, and put the extracted `avant-linux-x86_64` directory on your `PATH`. Keep its files together: the compiler finds `re2c`, the standard library, and the runtime beside itself.

No environment variable needs to be set. The compiler turns a program into machine code with the LLVM 20 it carries inside, and links it with `cc`.

Run your first program:

```sh
avant run hello.av          # compile and run
avant compile hello.av hi   # or build a binary named hi
./hi
```

### Temporary files

`compile`, `run`, `test`, `fetch`, and `bind` keep every intermediate file (the IR, objects, logs, test binaries) in one private directory, `avant-XXXXXX` under `$TMPDIR` (or `/tmp` when `TMPDIR` is unset). The directory is removed when the command exits, including on an error, Ctrl-C, or `SIGTERM`; only `kill -9` or a crash leaves it behind. The tools the compiler runs (`cc`, `re2c`, `git`) and the programs that `run` and `test` start see that directory as their `TMPDIR`, so their temporaries go with it. Nothing needs to be configured. `--keep-tmp` keeps the directory for debugging and prints its path; a program run on its own keeps its directory with `AVANT_KEEP_TMP=1`, which `--keep-tmp` also sets for the processes the command starts.

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
16. [Standard Library](#standard-library)
17. [Not built yet](#not-built-yet)
18. [Benchmarks](#benchmarks)
19. [Goals](#goals)
20. [Advanced: building the compiler](#advanced-building-the-compiler)
21. [License](#license)

## Programs

Braces are required. There are no parentheses on `if`, `while`, or `switch`, and no semicolons. `=` binds a new name or assigns an existing one. The last expression in a function is its value. A type is written `name: Type`.

The entry point is `fn main`, or `fn run` (the compiler synthesizes the C `main`). Neither needs `pub`.

```avant
fn main {
  puts("Hello Avant")
}

fn fact(n: Int): Int {
  if n <= 1 { return 1 }
  n * fact(n - 1)
}
```

### Line breaks

A line break ends a statement. A line whose first token is `[`, `-` or `+` always begins a new statement: it never continues the line before as an index or a binary operation. To continue an expression on the next line, end the line before with an operator or an open bracket. A line that starts with `.`, `?` or another infix operator (`*`, `&&`, `==`, ...) cannot begin a statement, so it continues the line before.

```avant
fn main {
  total = 1 +
    2
  bb = "y"
  [total, 3].each { |n| puts(n) }
  size = bb
    .size
  puts(size)
}
```

An expression left dangling by the rule is a compile error that says how to continue the line: `t = a` followed by a line `- b`, a line `[a, b]` whose value nobody uses, or a line starting with `+`.

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

`Int` is a 32-bit signed integer. `Int64` and `UInt64` are 64-bit. `Float64`, `Bool`, and `String` are the other everyday primitives. `Float64` has `abs`, `floor`, `sqrt`, `exp` and `to_i` (`x.exp` or `x.exp()`). Two `Bool`s compare with `==` and `!=`. `Void` is the result of a function that does not return a value. An omitted return type means `Void`.

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

A `T?` that nothing assigned is `nil`, whatever `T` is: an optional field without a default reads as absent.

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

`?` compiles only where the failure can be returned: on a `T?` the function's result must include `nil`, and on another union the result must hold its failure members. Otherwise it is a compile error that names the result type (`postfix ? would return nil, but the function returns Int; declare the result Int?, or handle the nil with if-assign or ??`). In a lambda, `?` returns from the lambda; a `spawn` or `parallel` block and `Mutex.with` cannot return, so `?` is refused there.

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

Indexing an `Array`, `String`, `Buf`, or `Bytes` outside `0 <= i < size` traps at runtime: one diagnostic on stderr and a non-zero exit. `.get(i)` returns `T?` instead — the element or nil. Integer `+`, `-`, `*` (and `/`, `%`) are checked the same way: overflow and division by zero trap. The rule is that an operation gives the mathematically correct value when one exists and traps when none does: `MIN % -1` is `0` for every signed type (only `MIN / -1` overflows); a shift `<<` or `>>` by an amount outside `0..31` for `Int` (`0..63` for `Int64` and `UInt64`), negative amounts included, traps (`avant runtime: shift amount 32 out of range for Int (0..31)`), and a literal amount outside the range is a compile error; `Float64.to_i` of NaN, an infinity, or a value outside the `Int` range traps (`avant runtime: to_i of Float64 3000000000 out of range for Int`), and inside it truncates toward zero. The wrapping spelling is explicit literal text: `&+`, `&-`, `&*` wrap on overflow. Checks the compiler can prove safe — a literal index into a literal, an induction variable under its loop bound, a loop whose whole index range is validated once on entry — are not emitted.

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

Heap allocation goes through the collector in `runtime/avant_gc_*.c` (one file per concern; `runtime/avant_gc_internal.h` lists them). The collector is precise GenImmix: a copying nursery, then an Immix mature space. The nursery is the default.

`AVANT_NURSERY=0` restores a full, non-moving collection. Every collector setting is an environment variable read once at start-up; `runtime/avant_gc.h` lists them. A switch is on for `1` and off for `0`, a size is a whole number, and any other value is reported on stderr and ignored.

A heap object stored into a C field is pinned until the program releases that field with `release(...)`, and for the rest of the process if it never does. Pinned objects are not copied. Code that hands a heap value to C for a bounded time can pin it for exactly that span with `pin(value) { ... }`: the value stays put for the block and is unpinned on every way out, and pins are counted, so a scoped pin never takes a field's pin and a release never takes a block's. See [Strings in C memory](#strings-in-c-memory).

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

`avant bind HEADER.h` reads a header with libclang and prints an editable `lib` block. The command lives on the identity compiler. The translator is `tools/bind.c`, built by `sh runtime/build.sh` into `runtime/build/avant-bind` (it needs `llvm-config` and libclang; without them the build skips it with a notice). A release ships `avant-bind` beside `avant`. In a checkout the compiler runs `runtime/build/avant-bind`; elsewhere it runs the `avant-bind` beside itself. `avant bind` builds nothing and writes nothing into the project.

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

`Ptr(Int)` is rejected. `Ptr` of a layout, and `Ptr(Void)`, are the pointer types this is for.

### Strings in C memory

A `String` stored into a C field (`h.name = s`) is pinned: the collector neither moves nor frees it, so C may read it, keep the pointer, or copy the struct. Overwriting the field keeps the old value pinned too, because C may still hold it. A C-layout store keeps its value alive until `release`.

When C is done with what Avant stored, say so with `release`:

```avant
lib cfg {
  struct Holder {
    name: String
    id: Int
  }
  fun holder_new: Ptr(Holder)
  fun holder_keep(h: Ptr(Holder))
  fun holder_forget
  fun holder_free(h: Ptr(Holder))
}

fn main {
  h = cfg.holder_new()
  h.name = "first-${1}"
  cfg.holder_keep(h)          // C keeps the pointer
  h.name = "second-${2}"      // "first-1" stays pinned
  cfg.holder_forget()         // C drops what it kept
  release(h.name)             // both values unpinned; h.name is now null
  cfg.holder_free(h)          // or release(h) before freeing it
}
```

- `release(h.name)` gives back the pin of every value Avant stored into that field, the current one and every overwritten one. If the field still holds a value Avant stored there, it becomes null, so C never sees a pointer the collector may free or move.
- `release(h.at)` does the same for every `String` field of a nested layout, and `release(h)` for every field of the struct `h` points to. Write `release(h)` before C frees the struct.
- Releasing twice, or releasing a field Avant never stored, does nothing. A value C stored itself is left in place. A pin taken by `pin(x) { }`, or by a store of the same value into another field, stays.
- `release` takes a C-layout field that holds a `String` (directly or in a nested layout) or a `Ptr` to such a layout. An Avant value, an `Int`, `Bool`, `Float64` or `Ptr` field, `Ptr(Void)`, and a layout with no `String` field are compile errors. It is a statement, not a value.
- Forgetting a release costs memory, never correctness. Releasing too early is a dangling C pointer: a value C still uses after its release may be moved or freed by the next collection, and C then reads memory that is no longer that string. Neither the compiler nor `AVANT_GC_VERIFY=1` can see C's reads.

A `fun` parameter may be an Avant `fn` type; that declares a C callback. Pass a function name or a lambda that captures nothing, and C calls it with exactly the declared arguments (libc `qsort` with an Avant comparator is the proven shape). A capturing lambda, a `fn` value held in a variable, and a C function returning a function pointer do not compile.

JSON in the examples is yyjson. HTTP in `std.http` is uSockets and picohttpparser (C, not C++). All live under `runtime/third_party/`.

## Concurrency

### spawn

`spawn { }` runs the block on a pool of OS threads. The pool size is the number of processors online, read once when the pool starts. `--threads N` (or `--threads=N`) on the program's command line replaces it. Extra tasks wait in a queue.

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

`Channel(T)` and `Mutex(T)` are ordinary generic classes. Nothing imports them for you. The proof is `tests/cases/test_tasks.av`.

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

Regex lives in the standard library: `import std.regex` is what links PCRE2. `Regex.compile(pattern, caseless = false)` returns `Regex | RegexError`, `re.find(text, from = 0)` returns a `Match?` value carrying the bounds (`m.start`, `m.end`, `m.span(i)`) and the groups (`m.group(i)`, or `m.group("name")` for a named group), `re.count(text)` counts non-overlapping matches, `re.matches(text)` iterates them, and `re.replace` / `re.split` work on every match. A compiled pattern is a handle: `re.close` releases it (the compiler keeps a bounded cache of compiled patterns, so compiling the same pattern again is cheap), `re.scoped { |r| ... }` closes on exit, and operations on a closed `Regex` report no match.

```avant
import std.regex

fn main {
  if re = Regex.compile("x(y)") {
    if m = re.find("zxy") {
      puts(m.start)       // 1
      puts(m.group(1))    // y
    }
    re.close
  }
}
```

A string-literal pattern is compiled to a native scanner with re2c at compile time when it needs at most one capture group and no named groups; any other pattern stays on PCRE2 with JIT, and the compiler says so. A release ships `re2c` beside `avant`; without any `re2c`, literals use PCRE2 too, and `AVANT_RE2C` overrides the binary. PCRE2 is compiled into the runtime, so a program that uses `std.regex` needs no PCRE2 library on the machine. PCRE2 backtracking can be catastrophic on adversarial patterns and texts; that cost is the caller's to bound.

## Testing

`avant test` (alias `avant t`) discovers and runs a project's tests. A test file is an ordinary program. It imports `std.testing`, checks what it cares about, and returns `t.finish` from `fn main: Int`. That integer is the process status: `0` when every check passed, `1` when any failed. A void `fn main` is still a successful program and does not change. The runner trusts the status. It does not read the words `PASS` and `FAIL`.

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

Save it as `tests/test_greet.av` and run `avant test`. The file prints `PASS greet uses the name`, `PASS greet handles empty`, and `0 failed, 2 passed`. A failing check prints `FAIL` and the name, then the run continues, so one file reports every failure. `finish` returns 1 when any check failed. Put the functions under test in their own module and `import` it. A test sees `pub` names only.

### Commands

| Command | What it runs |
| --- | --- |
| `avant test` | `test_*.av` under `tests/`, recursively |
| `avant t` | the same walk |
| `avant test DIR` | the same walk, starting at `DIR` |
| `avant test FILE.av` | that file, even when the name does not match `test_*.av` |
| `avant test A.av B.av` | those files, and no directory walk |

With no path, `tests/` is relative to the nearest `avant.toml`, or to the current directory when that walk finds none. The walk skips a name that starts with `.` and a directory named `deps`. Results are sorted bytewise by relative path. An empty selection exits 1.

### Flags

`avant test` accepts these flags. `--jobs=N`, `--shard=I/N`, and `--junit=PATH` are the same as the two-word form. An unknown flag is an error. `--jobs 0` is an error. `avant test` does not forward arguments to the test program, and a `--` on the command is an error.

| Flag | Meaning |
| --- | --- |
| `--jobs N` | How many test processes may run at once. Default: the number of online processors. |
| `--jobs 1` | One file at a time. |
| `--shard I/N` | Slice `I` of `N`, counted from 1. May be combined with `--jobs`. Does not launch the other slices. |
| `--junit PATH` | Write one JUnit case per file. Off unless you pass it. |
| `--skip-exclusive` | Omit files listed in `[test] exclusive`. Required when `--shard` would include one of them. |
| `--coverage` | Instrument the tests and, after the summary, print one coverage table. Also writes `coverage/lcov.info` in the current directory. |
| `-h`, `--help` | Print the flag list and exit 0. |

Ctrl-C or `SIGTERM` stops the whole run: the runner stops every test process it started, then removes its temporary files. The log is flushed in path order, and within a file in source order. The runner then prints `ok tests/test_greet.av` or `FAIL tests/test_greet.av`, and a total (`0 failed, 1 passed`). Its own status is 1 when any file failed, when nothing was selected, or when the command was refused.

### Checks

`Test` is the value from `import std.testing`. Create it with `Test.new`. Every check takes a `name: String`. Name it after the case, so a `FAIL` line alone says what broke.

| Method | Passes when |
| --- | --- |
| `check(cond, name)` | `cond` is true. |
| `check_eq(got, want, name)` | `got` equals `want`. Overloads: `Int`, `Bool`, and `String`. On failure it prints `got` and `want` under the `FAIL` line. |
| `golden(path, got)` | The bytes in `path` equal `got`. A missing file fails the check. Nothing rewrites the file. |
| `run_golden(program, want)` | `AVANT_COMPILER` compiles `program` with `--debug`, the program exits 0, and its stdout equals the bytes in `want`. |
| `rejects(source, substring)` | Compiling `source` exits non-zero, and stderr contains `substring`. |
| `finish` | Returns `0` when every check passed, `1` otherwise. Prints `N failed, M passed`. |

`Float64` has no `check_eq`. Compare it in a `check` when you have decided what equality means, including NaN.

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

Saved files compared by `golden` and `run_golden` are the exact bytes, including a trailing newline when the program prints one. Update a saved file by hand, after reading the new output.

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

`run_golden` and `rejects` need `AVANT_COMPILER` set to the compiler under test. A user project that only calls `check` does not.

`t.temp_dir` returns a new, empty directory for files a test writes. Each call makes another one. They live in the test program's private temporary directory and are removed when the program exits, so a test never cleans up after itself.

```avant
import std.testing

fn main: Int {
  t = Test.new
  dir = t.temp_dir
  t.check(file_write("${dir}/notes.txt", "hi") == 0, "write in temp_dir")
  t.finish
}
```

### Order inside a file

Checks written in the file, and bodies passed to `t.test`, run one at a time in source order. `t.parallel` overlaps other parallel tests of that same file, and only after the file's serial work. That run records each parallel name. `avant test` does not start the file an extra time only to list them. Each parallel test is then a new process of the same binary. The file runs again, serial checks and `t.test` bodies do not, and only that parallel body does. Code in `main` outside those bodies runs in every process. `avant run` on the file runs it once, so parallel bodies do not run until `avant test` starts them.

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

### Files that must run alone

A file listed in `[test] exclusive` runs only while no other selected test file is compiling or running. A benchmark that needs a quiet machine, as far as the other tests are concerned, is its own exclusive file. The list does not discover files. On a command with no path, every listed path has to be one of the discovered tests. `--shard` is an error when the selection contains an exclusive file, unless `--skip-exclusive` is also passed. With `--jobs 1`, every file is already alone and the list does not change the order.

```toml
[test]
dir = "tests"
exclusive = ["tests/test_bench.av"]
```

A file listed in `[test] first` starts before the others, in the listed order; the rest keep their sorted order. List the slowest files there so a long file does not start last and hold up the end of the run. Each file's run starts as soon as that file has compiled, and a free job slot takes a waiting run before a waiting compile. On a command with no path, every listed path has to be one of the discovered tests. `--shard` picks its slice before the list is applied, so the list does not move files between shards.

| Key | Meaning |
| --- | --- |
| `dir` | One directory. Replaces the default `tests/` walk. |
| `entry` | One file. Replaces the walk. |
| `exclusive` | Paths that run alone. A string, or a list of strings. |
| `first` | Paths that start before the other files, in the listed order. A string, or a list of strings. |

`dir` and `entry` together are an error. A path on the command overrides both. There is no command string and no `jobs` key. Paths are relative to the manifest.

### Reports

JUnit is the test-analytics file. Coverage is a separate report, for one instrumented program. They are not the same output.

`--junit PATH` writes one `<testcase>` per test file. The name is that file's path. It is off unless you pass it. A project does not gain a `junit.xml` by surprise. The file is what an analytics uploader reads. It does not contain one case per `check`.

Coverage is a report of the tests. `avant test --coverage` compiles each selected file with counters, runs them, and after the summary prints one table. It also writes `coverage/lcov.info` in the current directory, one fixed file overwritten by each run. The default `avant test` does not instrument and does not write that file. `compile`, `dump`, and `run` do not take `--coverage`.

With no `[coverage]` section, the table and `coverage/lcov.info` are the test file plus what it imports. An optional section widens that set. `include` and `exclude` are directories relative to the manifest, a string or a list of strings. The walk takes `*.av` under `include`, recursively, and skips a directory whose name starts with `.`. There are no globs. A file under `exclude` is left out, including a test file when `exclude` names `tests`. A file no test loaded is listed at 0% lines, with no branch map.

```toml
[coverage]
include = ["src"]
exclude = ["tests", "deps"]
```

| Knob | When it applies | Effect |
| --- | --- | --- |
| `AVANT_COVERAGE=1` | The compiler's environment | Emit hit counters for that compilation. `0` turns it off. The test runner sets this. A person does not. |
| `AVANT_COVERAGE_MAP` | Compile time | Path of the slot map baked into the binary. Unset, `compile` writes it beside the binary as `OUT.covmap` (the program adds `OUT.covmap.hits` when it runs); `dump` and `run` keep it in the command's temporary directory. |
| `AVANT_COVERAGE_LCOV` | The instrumented program | Where it writes the LCOV report. `0` writes nothing. Unset, it writes `coverage/lcov.info` in the current directory, creating `coverage/`. |
| `AVANT_COVERAGE_REPORT` | The instrumented program | `0` hides the text table. `1` prints it. Unset, a program prints the table and the compiler command line does not. |
| `AVANT_COVERAGE_COLOR` | The text table | `0` or `1` forces color. Unset, color follows the terminal. |

Hit counters for one binary are a shared map. Several processes of that same binary add into it. `avant test --coverage` keeps those processes from writing the report or printing a table, then merges every test by file, kind, line, and name and writes one report.

### Program arguments

`avant test` compiles each file and runs that binary, so the process status is the file's. `avant run FILE.av --` does the same when the program takes arguments. `avant run FILE.av` without `--` stays on the fast path and does not forward the status.

`avant run [RUN-OPTIONS] FILE.av [-- PROGRAM-ARGS...]` is how a program receives arguments. Options may sit before the file or after it, up to `--`, so `avant run FILE.av --debug` still works. Arguments after `--` are the program's, starting at `argv()` index 1.

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

### The compiler's own suite

Testing the compiler itself is covered for contributors under [Advanced: building the compiler](#advanced-building-the-compiler). The programs in this section are exercised by `tests/cases/test_testing_docs.av`.

## Builtins

Builtins are the prelude: names every program can use with no import. Runnable examples live in `examples/` (`hello`, `sieve`, `errors`, `collect`, `json`, `spawn`, `matmul`, and more). The core language (strings, arrays, hashes, buffers, tasks) is covered in its own sections above; this section is the free-function surface that remains.

| Area | Names |
| --- | --- |
| Time | `now_ms`, `now_us` |
| Files | `file_read`, `file_write`, `file_exists`, `dir_list` |
| Process and environment | `argv`, `env_get`, `process_run`, `process_run_out` |
| Hashing and math | `sha256`, `checksum_str`, `checksum_f64`, `sqrt`, `exp` |
### Files, process, and environment

```avant
fn main: Int {
  if text = file_read("notes.txt") {
    puts(text.size)
  }
  if home = env_get("HOME") {
    puts(home)
  }
  args = argv()
  if args.size > 1 {
    puts(args[1])
  }
  0
}
```

### Time, hashing, and math

```avant
fn main {
  t0 = now_us()
  puts(now_us() >= t0)
  puts(sha256("avant").size)
  puts(sqrt(144.0))
}
```

## Standard Library

The standard library is imported, and importing is what links the C library behind a module: a program that never imports a module does not carry its library. Modules may add methods to `Bytes`, `String`, and `Buf`, and may declare type-level functions (written `fn self.name(...)` in a type body and called on the type, as `Json.parse` and `Regex.compile` are). A module's logical name is its import name: the file that implements `std.regex` is `lib/std/regex.av`.

| Module | Adds | Links |
| --- | --- | --- |
| `std.zlib` | `Bytes.compress`, `Bytes.uncompress` (plus `Buf` counterparts) | zlib |
| `std.base64` | `Bytes.to_base64`, `Bytes.from_base64` (plus `Buf` counterparts) | vendored Base64 codec |
| `std.json` | `Json.parse` and the parsed-value walk | yyjson |
| `std.regex` | `Regex.compile` and `Match` values | PCRE2 (re2c scanners for literal patterns) |
| `std.http` | HTTP server and client, WebSocket server and client | uSockets and picohttpparser (vendored) |
| `std.testing` | the support library the test runner loads | — |

### `std.zlib`

Compression returns values, and failure to decompress is a `ZlibError` value:

```avant
import std.zlib

fn main {
  small = "avant avant avant".bytes.compress
  if back = small.uncompress {
    puts(String.from_utf8(back) ?? "")
  }
}
```

Each operation also has a `Buf` counterpart (`compress_into`, `uncompress_into`) that writes into a buffer the caller owns instead of allocating a fresh result.

### `std.base64`

```avant
import std.base64

fn main {
  enc = "avant".bytes.to_base64
  puts(enc)
  if raw = enc.bytes.from_base64 {
    puts(String.from_utf8(raw) ?? "")
  }
}
```

Encoding never fails; decoding reports a `Base64Error` on malformed input. `to_base64_into` and `from_base64_into` are the `Buf` counterparts.

### `std.json`

`Json.parse` returns a `Json` document on success or a `JsonError` value on malformed input. A parsed value walks with `get`/`at`, scans its children with `iter`, and projects a numeric field of every array element with `f64s(key)`:

```avant
import std.json

fn main {
  if doc = Json.parse("{\"n\": 7, \"name\": \"ada\"}") {
    if n = doc.int("n") {
      puts(n)
    }
    if name = doc.str("name") {
      puts(name)
    }
  }
}
```

The document owns its values and its text buffer; there is nothing to close.

### `std.regex`

`Regex.compile` returns a compiled pattern or a `RegexError`; `find` returns a `Match` value with bounds and groups, and there are no global match cursors. The full surface (`count`, the `matches` iterator, `replace`, `split`, named groups, pattern lifetimes) is in [Regex](#regex):

```avant
import std.regex

fn main {
  if re = Regex.compile("x(y)") {
    if m = re.find("zxy") {
      puts(m.group(1) ?? "")
    }
    re.close
  }
}
```

### `std.http`

Cleartext HTTP and WebSockets: a server, a blocking HTTP client, and a blocking WebSocket client.

#### Server

`Http.listen` binds now and returns `Server | HttpError`. Routes are exact paths. `serve` runs until `close` is called.

```avant
import std.http

fn main {
  if server = Http.listen(8080) {
    server.get("/") { |req|
      Response.text("hello")
    }
    server.post("/echo") { |req|
      Response.bytes(req.body)
    }
    server.serve
  }
}
```

- A handler gets a `Request` (method, path, query, headers, body) and returns a `Response` (`Response.text`, `Response.bytes`, `.header(name, value)`).
- Bodies are buffered up to `max_body` (1 MiB default): larger is answered 413, chunked request bodies 411, malformed requests 400.
- Connections are kept alive. One event loop runs per elected thread (`loops` at listen time to override).
- Handlers must not block the loop. Hand CPU work to `spawn`.
- A server that is never closed leaks its socket. There is no finalizer.

#### HTTP client

One blocking request at a time. Redirects are followed (at most five). Chunked responses are decoded.

```avant
import std.http

fn main {
  if resp = Http.get("http://127.0.0.1:8080/") {
    puts(resp.status)
    puts(String.from_utf8(resp.body) ?? "")
  }
}
```

#### WebSockets

WebSocket routes share the HTTP server and port. A handler runs once per complete message. Text reads as `msg.text`; binary stays `msg.body`.

```avant
server.websocket("/echo") { |conn, msg|
  sent = conn.send_message(msg)
}
server.websocket_close("/echo") { |conn, code, reason|
  puts(code)
}
```

- Ping, the close handshake, and fragmentation are handled for you.
- A message over `max_message` (1 MiB default) is closed with code 1009 and never reaches the handler.
- `server.close` sends close code 1001 to open connections.
- A `WsConn` stays on its loop thread. Capture `conn.sender` to answer from a `spawn` block.

#### WebSocket client

`Ws.connect` returns `WsConnection | HttpError`. `recv` blocks, so never call it from a server handler.

```avant
import std.http

fn main {
  if conn = Ws.connect("ws://127.0.0.1:8080/echo") {
    sent = conn.send_text("hi")
    if msg = conn.recv() {
      puts(msg.text ?? "")
    }
    conn.close
  }
}
```

There is no TLS yet: `https` and `wss` URLs are an `HttpError`, never a silent downgrade.

#### Hello-world servers compared

The most basic hello-world server: `GET /` answered with `hello` and the WebSocket equivalent. Every server is an optimized build (Avant `--release`); Avant's row is the median of three runs (2026-11-10).

| Language | HTTP (req/s) | HTTP peak RSS | WebSocket (msg/s) | WebSocket peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Avant (`std.http`) | 507,795 | 7.2 MB | 247,803 | 6.9 MB |
| C (h2o) | 484,026 | 8.4 MB | 251,006 | 8.5 MB |
| Rust (std, thread per connection) | 507,603 | 3.4 MB | 252,175 | 2.6 MB |
| Go (`net/http`) | 349,436 | 18 MB | 215,594 | 12 MB |
| Crystal (`HTTP::Server`) | 521,997 | 15 MB | 264,813 | 11 MB |

### `std.testing`

`std.testing` is the support library the `avant test` runner itself loads (output plumbing for test binaries). Test files use the `Checks` style shown in [Testing](#testing); `std.testing` is not an assertion API of its own.

## Not built yet

These are decided and currently do not compile.

| What | Status |
| --- | --- |
| `parallel { }` with no receiver | decided, not shipping |
| TLS for `std.http` | decided, not shipping |
| An inclusive range (`a..=b`) | not part of the language yet; write `a..b + 1` |

## Benchmarks

Seconds, lower is better. **Bold** is best. Measured on an AMD Ryzen 7 5700G, one process per task, timed by its wall clock, with peak memory from `/usr/bin/time`, the same wrapper for every language.

| Language | 49 tasks (s) | Average peak memory |
| --- | ---: | ---: |
| Avant | **39.023** | **19.0 MB** |
| C | 58.583 | 23.4 MB |
| Rust | 58.926 | 29.8 MB |
| Crystal | 85.670 | 67.1 MB |
| Go | 117.876 | 64.3 MB |

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Binarytrees::Obj` | **0.623** | 2.736 | 2.865 | 1.325 | 1.881 |
| `Binarytrees::Arena` | 0.520 | 0.355 | **0.341** | 0.759 | 0.362 |
| `Brainfuck::Array` | 1.635 | 1.896 | 1.825 | 1.968 | **1.546** |
| `Brainfuck::Recursion` | 1.906 | 1.570 | **1.522** | 1.745 | 1.709 |
| `Matmul::Single` | **1.152** | 5.646 | 5.758 | 5.832 | 5.769 |
| `Matmul::T4` | **0.949** | 1.485 | 1.551 | 3.380 | 1.558 |
| `Matmul::T8` | **0.582** | 0.799 | 0.858 | 2.642 | 0.888 |
| `Matmul::T16` | 0.596 | **0.482** | 0.729 | 2.339 | 0.781 |
| `Base64::Encode` | 0.127 | **0.119** | 0.352 | 1.633 | 2.591 |
| `Base64::Decode` | 0.154 | **0.147** | 0.398 | 1.520 | 1.886 |
| `Json::Generate` | **0.283** | 0.661 | 0.337 | 1.173 | 1.256 |
| `Json::ParseDom` | 0.099 | **0.098** | 1.694 | 0.540 | 1.609 |
| `Json::ParseMapping` | 0.238 | **0.229** | 0.425 | 1.546 | 2.871 |
| `CSV::Parse` | **0.758** | 2.247 | 0.881 | 3.418 | 3.000 |
| `Etc::Sieve` | 1.423 | **0.920** | 0.979 | 2.360 | 1.896 |
| `Etc::NeuralNet` | 1.301 | **1.073** | 1.165 | 1.260 | 2.219 |
| `Etc::CacheSimulation` | **0.493** | 0.762 | 0.863 | 1.298 | 1.638 |
| `Etc::GameOfLife` | 1.438 | 1.487 | 1.503 | 2.816 | **0.980** |
| `Etc::Words` | 1.125 | 2.284 | 1.080 | **0.902** | 1.372 |
| `Etc::LogParser` | 0.473 | 0.496 | **0.348** | 1.207 | 28.911 |
| `Template::Regex` | **0.992** | 1.269 | 3.660 | 1.673 | 7.730 |
| `Template::Parse` | **0.809** | 1.132 | 1.181 | 2.737 | 0.891 |
| `Sort::Quick` | 1.434 | **1.186** | 1.193 | 1.550 | 1.305 |
| `Sort::Merge` | 1.739 | **1.165** | 1.540 | 2.309 | 1.593 |
| `Sort::Self` | **0.103** | 2.095 | 0.412 | 1.079 | 1.333 |
| `Graph::BFS` | **1.141** | 1.372 | 1.787 | 3.057 | 2.615 |
| `Graph::DFS` | **1.193** | 1.225 | 1.450 | 2.190 | 1.742 |
| `Graph::AStar` | **0.821** | 2.212 | 2.340 | 1.950 | 3.867 |
| `Hash::SHA256` | **0.613** | 1.155 | 1.218 | 1.214 | 1.243 |
| `Hash::CRC32` | **0.137** | 0.986 | 0.986 | 0.987 | 4.197 |
| `Calculator::Ast` | **0.441** | 1.041 | 0.600 | 0.494 | 0.580 |
| `Calculator::Interpreter` | **0.242** | 1.248 | 0.901 | 1.088 | 0.871 |
| `Maze::Generator` | **0.570** | 0.596 | 1.012 | 2.718 | 1.457 |
| `Maze::BFS` | **0.531** | 0.670 | 1.272 | 3.683 | 2.168 |
| `Maze::AStar` | **1.089** | 1.776 | 1.464 | 4.363 | 4.092 |
| `CLBG::Fannkuchredux` | 1.361 | 1.298 | 1.371 | 1.229 | **1.162** |
| `CLBG::Mandelbrot` | **0.973** | 0.981 | 1.025 | 0.998 | 1.013 |
| `CLBG::Nbody` | 1.255 | 1.271 | **1.200** | 1.296 | 1.445 |
| `CLBG::Spectralnorm` | **0.984** | 1.035 | 1.031 | 1.039 | 1.065 |
| `Compress::BWTEncode` | 0.437 | 0.505 | **0.418** | 1.332 | 2.140 |
| `Compress::BWTDecode` | **0.685** | 0.721 | 1.024 | 1.152 | 1.938 |
| `Compress::HuffEncode` | **0.537** | 0.795 | 0.717 | 1.104 | 0.908 |
| `Compress::HuffDecode` | 0.575 | **0.445** | 0.556 | 0.907 | 1.265 |
| `Compress::ArithEncode` | 1.187 | 1.036 | **0.977** | 1.139 | 1.015 |
| `Compress::ArithDecode` | 0.984 | **0.895** | 1.359 | 0.960 | 0.910 |
| `Compress::LZWEncode` | **0.156** | 1.413 | 0.707 | 1.052 | 1.244 |
| `Compress::LZWDecode` | **0.247** | 0.659 | 0.659 | 0.633 | 1.634 |
| `Distance::Jaro` | 1.011 | **0.894** | 0.895 | 1.282 | 0.930 |
| `Distance::NGram` | 0.901 | 2.014 | **0.497** | 0.794 | 0.798 |

<small>Notes:</small>
- <small>Build flags: Avant `--release --cpu=v3` (x86-64-v3); C and C++ use LangArena's `-O3 -march=native -mtune=native` (Zen 3 here, a superset of v3); Rust (`cargo build --release`), Go and Crystal (`--release`) use their defaults. C is C compiled with Clang, the faster of the two C compilers measured here (GCC: 59.499 s, 23.4 MB).</small>
- <small>Each task's time is its whole process: start-up, data preparation, warm-up and the timed iterations; LangArena's published numbers count only the timed iterations.</small>
- <small>Avant's task configuration is compiled into the program as constants, while the other languages read `run.js` at run time, so LLVM can specialize Avant's loops on the sizes; on a few tasks (`Matmul::Single` above all) that, not the language, explains most of the gap.</small>
- <small>The 49-task scope excludes `Etc::TextRaytracer`, which is invalid for Avant (`--release` hoists its pure render).</small>
- <small>JSON, Base64, hashing, and regex are internally based on C libraries.</small>

## Goals

Beginners welcome, advanced programmers unblocked, memory managed, errors as values. The target: Crystal-class expressiveness, runtime nearer C/Rust than Crystal is today, clearly faster than Go, memory better than Crystal without giving up the collector. LangArena is the public scoreboard.

## Advanced: building the compiler

Everything below is for compiler developers, not for writing Avant programs.

The compiler is self-hosted: it is written in Avant (`compiler/*.av`) and compiles itself. It lowers the checked program to its own mid-level IR (MIR, `compiler/mir/`), emits LLVM IR from it (`compiler/llvm/`), and compiles that with the LLVM 20 linked into the compiler (`runtime/avant_llvm.c`); `cc` links the object with the runtime.

```
.av  →  parser and checker (compiler/*.av)  →  MIR  →  LLVM IR  →  in-process LLVM  →  object  →  cc  →  binary
```

This is the compiler's only back end. The earlier one, which emitted [myc](https://github.com/kostya/myc) IR for an external `myc-llvm` process, was retired once every program it ran had been frozen as a golden (`tests/goldens/runs/`, checked by `sh tools/run-goldens.sh`).

Building from a checkout needs a C compiler, the C library development files, and binutils (`ld`, `objcopy`); the compiler itself also needs LLVM 20's static archives (Debian/Ubuntu: `llvm-20-dev`, `libzstd-dev`), which `runtime/build.sh` merges into `runtime/build/avant_llvm.o` (`tools/build-llvm.sh`) and only the compiler links; the `bind` helper also needs `llvm-config` and libclang 20, and regex literals need `re2c` (on `PATH`, or the pinned static build: `sh tools/build-re2c.sh bin/re2c`). Build the next compiler with a published release, v0.35.0 or later (`tools/letters.sh` uses `bin/v0.36.0` unless `LETTERS_SEED` names another; older releases compiled through a `myc-llvm` process, and the compiler they build carries LLVM stack maps the runtime no longer walks, so they cannot be seeds):

```
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o   # rebuild with itself
```

`sh tools/letters.sh` does the same for the letters the gate uses: the runtime, B (`bin/avant-b`, built by the seed), and C′ (`bin/avant-c-llvm`, built by B); `sh tools/letters.sh b`, `c-llvm`, `d-llvm` (D′, built by C′), or `c-release` (the `--release` build a release ships) builds one. `sh tools/gate.sh` is the full gate (B, C′, the suite on both, D′, and the fixed point: C′ and D′ emit the same LLVM IR for the compiler and are byte-identical). `sh tools/run-goldens.sh` compiles and runs every example, runnable fixture, and LangArena task in `test` mode and compares each one's stdout, stderr, and exit status with its golden (`GOLDEN_MODE=--release` or empty for the other modes).

`AVANT_ROOT` overrides where the compiler looks for `runtime/build`. Codegen splits by verb: `avant run` with no flag uses `--debug` (fast: LLVM at `-O0` with its fast instruction selector), and `avant compile` with no flag uses the default optimization (a short pass list, for kept artifacts). On either verb, `--debug` selects the fast codegen explicitly and `--release` (or `--final`) selects full optimization (`-O3`), the scored mode used in [Benchmarks](#benchmarks); an explicit flag always wins over the verb default, and no environment variable selects codegen. The garbage collector's roots are kept precisely in every mode. `dump FILE.av` prints the program's MIR without linking.

Tests are the native suite, run on the compiler you just built (B) and on the next one (C′):

```
AVANT_COMPILER=./bin/avant-b ./bin/avant-b test
./bin/avant-b compile compiler/main.av bin/avant-c-llvm --debug runtime/build/avant_llvm.o
AVANT_COMPILER=./bin/avant-c-llvm ./bin/avant-c-llvm test
```

Each file in `tests/cases/` named `test_*.av` is one program. A helper shared by two of them lives in `tests/cases/harness.av` and is imported. Run one file with `avant test tests/cases/test_ffi.av`. `--shard 2/4` runs one slice. A shard that would include a file listed in `[test] exclusive` needs `--skip-exclusive`, and those files run in a command that does not pass `--shard`. `test_fixed_point.av`, which compiles the compiler, is listed in `[test] first` so it starts before the rest; it no longer runs alone. The exclusive files are the ones that probe the collector, rewrite `PATH`, use git, or write a coverage report of their own. `--jobs` defaults to the number of online processors. `--junit PATH` writes one case per check, with durations, for files whose checks record them (the compiler suite and `std.testing` both do), and one case per file otherwise. Each file's output is printed as soon as that file finishes, in completion order; the summary line still totals the whole run. The compiler's own report is the hit file of an instrumented compiler build, and `coverage/lcov.info` for that build is written once after the processes that update it have finished. `avant test --coverage` is a separate report, of the tests. Goldens live in `tests/goldens/`.

## License

[MIT](LICENSE).
