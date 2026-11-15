# Avant

Avant is a compiled, garbage-collected programming language that aims for the density of Crystal, with the braces of C and speed in the neighbourhood of C and Rust. You write short, type-inferred code; `avant` compiles it ahead of time, through LLVM, to a native binary; a generational Immix collector manages memory. No borrow checker, no manual `free`, no interpreter, no exceptions.

For a long time a programmer picked one pair: simplicity and productivity, or performance and efficiency. Avant's bet is that you no longer have to choose, and its rule is to show the evidence, the losses included: [Benchmarks](#benchmarks) prints the rows where Avant is behind beside the rows where it leads.

## A taste

<!-- check: run id=taste -->
```avant
struct Item {
  name: String
  qty: Int
  cents: Int

  fn total: Int {
    qty * cents
  }
}

struct BadLine: Error {
  message: String
}

fn parse(line: String): Item | BadLine {
  f = line.split(",")
  if f.size != 3 {
    return BadLine { message: "expected name,qty,cents: ${line}" }
  }
  qty = f[1].to_i ?? return BadLine { message: "bad quantity: ${f[1]}" }
  cents = f[2].to_i ?? return BadLine { message: "bad price: ${f[2]}" }
  Item { name: f[0], qty: qty, cents: cents }
}

fn main {
  items: Array(Item) = []
  for line in ["tea,2,450", "cake,one,300", "milk,3,120"] {
    item = parse(line) else e {
      puts("skipped: ${e.message}")
      continue
    }
    items.push(item)
  }
  totals = items.parallel { |item| item.total }
  puts("${items.size} items, ${totals.sum} cents")
}
```

<!-- check: output -->
```text
skipped: bad quantity: one
2 items, 1260 cents
```

What you just read:

- **No ceremony.** No semicolons, no parentheses around conditions, no `let`. Types appear where they name a fact (a field, a parameter, a result) and are inferred everywhere else.
- **Errors are values.** `parse` returns `Item | BadLine`. `??` supplies a fallback or leaves the function; `item = parse(line) else e { ... }` reads the failure in a block that must leave, and after it `item` is the success. Nothing unwinds, and an error cannot be dropped by accident.
- **Values and objects.** A `struct` is a value with methods. A `class` (not shown here) is a reference. You choose per type.
- **Concurrency is built in.** `.parallel` runs the block on a pool of OS threads and returns the results in input order.

And because backend work is home ground, a web server is seven lines:

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080) {
    server.get("/") { |req| Response.text("hello") }
    server.serve
  }
}
```

It is a real one: keep-alive connections, one event loop per processor, WebSockets on the same port. It is [measured here](#http-and-websocket-servers), with its method.

## Install

On Linux x86-64 with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39, or later):

```sh
curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
```

The one prerequisite is a C compiler, which Avant uses to link: `sudo apt install gcc libc6-dev` on Debian and Ubuntu, `sudo dnf install gcc glibc-devel` on Fedora. The installer tells you if it is missing, and ends by compiling and running a hello program to prove the install works. Options, manual install and what the archive contains are in [Installation in depth](#installation-in-depth).

Save this as `hello.av`:

```avant
fn main {
  puts("Hello Avant")
}
```

```sh
avant run hello.av                 # compile and run (a fast debug build)
avant compile hello.av             # build ./hello
avant compile hello.av --release   # the optimized build: use it whenever you measure
```

The three build modes are compared in [Build modes](#build-modes).

Avant is pre-1.0 and young: the language still changes between releases ([CHANGELOG](CHANGELOG.md)), and what is decided but not built is listed in [Not built yet](#not-built-yet) and said again beside each feature it limits.

## Contents

**Start here:**

- [A taste](#a-taste)
- [Install](#install)

**The language**

- [Basics](#basics):
  - [entry point](#entry-point)
  - [names and scope](#names-and-scope)
  - [constants](#constants)
  - [functions](#functions)
  - [built-in types](#built-in-types)
  - [operators](#operators)
  - [equality and order](#equality-and-order)
  - [printing](#printing)
  - [line breaks](#line-breaks)
  - [comments](#comments)
- [Control flow](#control-flow):
  - [if](#if)
  - [conditions that bind](#conditions-that-bind)
  - [while](#while)
  - [for](#for)
  - [switch](#switch)
- [Enums](#enums)
- [Nil and unions](#nil-and-unions):
  - [opening an optional](#opening-an-optional)
  - [unions](#unions)
  - [type aliases](#type-aliases)
  - [what an optional costs](#what-an-optional-costs)
- [Numbers](#numbers):
  - [integer types](#integer-types)
  - [literals](#literals)
  - [conversions](#conversions)
  - [arithmetic and overflow](#arithmetic-and-overflow)
  - [floats](#floats)
- [Strings](#strings):
  - [string literals](#string-literals)
  - [bytes, not characters](#bytes-not-characters)
  - [string operations](#string-operations)
  - [parsing numbers](#parsing-numbers)
  - [Bytes and Buf](#bytes-and-buf)
- [Arrays](#arrays):
  - [creating arrays](#creating-arrays)
  - [indexing and bounds](#indexing-and-bounds)
  - [array operations](#array-operations)
  - [size limits](#size-limits)
- [Hashes](#hashes):
  - [hash operations](#hash-operations)
  - [key and value types](#key-and-value-types)
- [Structs and classes](#structs-and-classes):
  - [struct or class](#struct-or-class)
  - [structs](#structs)
  - [classes](#classes)
  - [field initialization](#field-initialization)
  - [methods and self](#methods-and-self)
  - [external methods](#external-methods)
  - [type-level functions](#type-level-functions)
  - [overloads and defaults](#overloads-and-defaults)
  - [operator methods](#operator-methods)
  - [no inheritance](#no-inheritance)
- [Errors](#errors):
  - [propagate, replace, branch](#propagate-replace-branch)
  - [nothing or a failure](#nothing-or-a-failure)
  - [declaring an error](#declaring-an-error)
  - [causes](#causes)
  - [an enum as an error](#an-enum-as-an-error)
  - [errors cannot be dropped](#errors-cannot-be-dropped)
  - [reading the error itself](#reading-the-error-itself)
  - [traps](#traps)
  - [nil, error or trap](#nil-error-or-trap)
- [Function values](#function-values):
  - [lambdas](#lambdas)
  - [trailing blocks](#trailing-blocks)
  - [capture](#capture)
  - [builtin blocks are loops](#builtin-blocks-are-loops)
- [Generics](#generics):
  - [generic functions](#generic-functions)
  - [generic types](#generic-types)
- [Interfaces](#interfaces):
  - [opting in](#opting-in)
  - [bounds](#bounds)
- [Modules](#modules):
  - [import paths](#import-paths)
  - [visibility](#visibility)
  - [name clashes](#name-clashes)
- [Packages](#packages):
  - [the manifest](#the-manifest)
  - [fetch and lock](#fetch-and-lock)
  - [importing a dependency](#importing-a-dependency)
  - [updating and removing](#updating-and-removing)
  - [publishing a package](#publishing-a-package)
  - [package conflicts](#package-conflicts)
  - [package limits](#package-limits)
- [Concurrency](#concurrency):
  - [spawn](#spawn)
  - [when a task traps](#when-a-task-traps)
  - [parallel](#parallel)
  - [Channel and Mutex](#channel-and-mutex)
  - [choosing a tool](#choosing-a-tool)
  - [what may cross a task](#what-may-cross-a-task)
  - [the thread pool](#the-thread-pool)
- [Memory](#memory):
  - [the collector](#the-collector)
  - [what allocation costs today](#what-allocation-costs-today)
  - [collection hints](#collection-hints)
  - [collector settings](#collector-settings)
- [C interop](#c-interop):
  - [declaring C functions](#declaring-c-functions)
  - [C types](#c-types)
  - [layouts and Ptr](#layouts-and-ptr)
  - [strings in C memory](#strings-in-c-memory)
  - [callbacks](#callbacks)
  - [linking C code](#linking-c-code)
  - [avant bind](#avant-bind)
- [Macros](#macros):
  - [quote and splice](#quote-and-splice)
  - [the field walk](#the-field-walk)

**Library and tools**

- [Builtins](#builtins):
  - [output](#output)
  - [input](#input)
  - [files and directories](#files-and-directories)
  - [processes and environment](#processes-and-environment)
  - [time, hashing and math](#time-hashing-and-math)
- [Standard library](#standard-library):
  - [std.json](#stdjson)
  - [std.regex](#stdregex)
  - [std.http](#stdhttp)
  - [std.zlib](#stdzlib)
  - [std.base64](#stdbase64)
- [Testing](#testing):
  - [running tests](#running-tests)
  - [checks](#checks)
  - [golden files](#golden-files)
  - [serial and parallel tests](#serial-and-parallel-tests)
  - [choosing by name](#choosing-by-name)
  - [test settings](#test-settings)
  - [reports](#reports)
- [Formatter](#formatter)
- [Command line](#command-line):
  - [commands](#commands)
  - [build modes](#build-modes)
  - [program arguments](#program-arguments)
  - [compile errors](#compile-errors)
  - [temporary files](#temporary-files)
  - [environment variables](#environment-variables)

**Perspective**

- [Coming from another language](#coming-from-another-language):
  - [the deliberate differences](#the-deliberate-differences)
  - [by language](#by-language)
- [Not built yet](#not-built-yet)
- [Benchmarks](#benchmarks):
  - [LangArena](#langarena)
  - [where Avant loses](#where-avant-loses)
  - [HTTP and WebSocket servers](#http-and-websocket-servers)
  - [reading these numbers](#reading-these-numbers)
- [Goals](#goals)

**Going deeper**

- [Installation in depth](#installation-in-depth):
  - [installer options](#installer-options)
  - [manual install](#manual-install)
  - [requirements](#requirements)
- [Contributing](#contributing):
  - [how the compiler works](#how-the-compiler-works)
  - [building the compiler](#building-the-compiler)
  - [the gate and the suite](#the-gate-and-the-suite)
  - [checking this README](#checking-this-readme)
- [License](#license)

## Basics

A program is functions, types and one `fn main`. Braces are required; there are no semicolons and no parentheses around conditions. A type is written `name: Type`, and only where it names a fact: a parameter, a result, a field. Locals are inferred.

```avant
fn fact(n: Int): Int {
  if n <= 1 { return 1 }
  n * fact(n - 1)
}

fn main {
  total = 0
  for i in 1..6 {
    total += fact(i)
  }
  puts("1! + ... + 5! = ${total}")   // => 1! + ... + 5! = 153
}
```

The last expression of a function is its value. `1..6` stops before 6. `${...}` interpolates.

### Entry point

The entry point is `fn main` (or `fn run`); it needs no `pub`. Returning nothing exits with status 0. Declared `fn main: Int`, its result is the process's exit status, which must be in `0..255`.

<!-- check: run status=3 -->
```avant
fn main: Int {
  puts("three things went wrong")
  3
}
```

A status outside `0..255` traps (`avant runtime: exit status 256 out of range (0..255)`, status 1) instead of reaching the OS cut to its low byte, where 256 failures would read as success.

An entry point may also fail: declared `fn main: Void | E` or `fn main: Int | E`, with every `E` marked `Error` ([Errors](#errors)), a failure it returns (or passes up with `?`) prints its text, `"${e}"`, and a newline to stderr after everything written to stdout, and the process exits with status 1. Success exits 0, or with the `Int`.

<!-- check: run status=1 -->
```avant
class Missing: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn lookup(key: String): Int | Missing {
  if key == "port" {
    return 8080
  }
  Missing.new("no setting ${key}")
}

fn main: Void | Missing {
  puts(lookup("port")?)    // => 8080
  puts(lookup("host")?)    // stderr: no setting host; status 1
}
```

Command-line arguments come from `argv()` ([Program arguments](#program-arguments)).

### Names and scope

`=` binds a new name or assigns an existing one; there is no `let` or `var`. Locals are mutable. A name introduced in a block dies at its `}`; assigning a name that exists in an enclosing block writes that outer name.

```avant
fn main {
  n = 1
  if n > 0 {
    n = 2          // assigns the outer n
    y = 3          // a new name, gone at the brace
    n += y
  }
  puts(n)          // => 5
}
```

- A declaration may state its type: `xs: Array(Int) = []`, `small: Int32 = 5`. It must when the value alone does not say it, as an empty `[]` does not; an argument `[]` takes its parameter's type (`count([])` with `xs: Array(Int)`).
- `name : Type` is accepted; the [formatter](#formatter) prints `name: Type`.
- `_ = expr` computes a value and drops it on purpose; `_` binds nothing and cannot be read.
- A module has no variables, on purpose: state lives in a function's locals and is passed in values, so no two tasks can share a global by accident. A value that never changes is a [constant](#constants); `count = 0` at module level is a compile error that says so.

### Constants

`const` names a value at module level; `pub const` exports it, as `pub fn` does. The name is snake_case, as a function's (a capital means a type). The value is a scalar (an integer, a `Float64`, a `Bool` or a [character literal](#literals)), computed when the program compiles from literals, other constants and operators, and each use is that value written in place, so a constant takes its type from where it stands, as a literal does.

```avant
const max_depth = 64
const kib = 1 << 10
const page = 4 * kib
const ratio = 0.5

fn half(n: Int32): Int32 {
  n / 2
}

fn main {
  // each use takes the type its place wants: Int32, UInt32, Float64
  puts(half(max_depth))         // => 32
  small: UInt32 = page
  puts(small)                   // => 4096
  puts(ratio * max_depth)       // => 32.0
}
```

- An overflow, a division by zero or a shift outside the width while computing a constant is a compile error at the constant, and so is a value that is not a scalar: a `String`, an array or a call (constant strings and tables are not built yet). A constant used where its value does not fit is the literal's error, naming the constant (`the constant big = 3000000000 does not fit in Int32`).
- Constants may use each other in any order; one that refers to itself is an error.
- A constant may label a `switch` arm, or bound a range label, as its value written there would (`case max_depth`, `case lo..=hi`); a name that is not a constant is an error there, since a label is never a variable.
- A constant cannot be assigned (`max_depth = 3`, `max_depth += 1` and `for max_depth in ...` are compile errors), and there is no `const` inside a function: a local is `name = value`. A parameter or a binding (`if v = ...`, `case n: Int`, a block's `|x|`) may take a constant's name inside its body.
- A bare name is, in order: a local or parameter, a field of the receiver, a method of the receiver, a constant, a function. Another module's `pub const` is reached bare or as `module.name`; two constants of one name that a module sees (its own included) are ambiguous: qualify the one you mean ([Name clashes](#name-clashes)).

### Functions

`fn name(params): Result { body }`. An omitted result type means the function returns nothing (`Void`); result types are never inferred. A function without parameters is declared without parentheses, in one spelling (`fn answer: Int`; `fn answer(): Int` is refused with that hint), and so is a C function in a `lib` block (`fun avant_gc_collect`). It is still called with them (`answer()`): a bare `answer` is the function as a value. A *method* without arguments may be called with or without them (`q.sum`, `q.sum()`), the builtin ones included (`s.strip`, `xs.pop`).

```avant
fn area(w: Int, h: Int): Int {
  w * h
}

fn sign(n: Int): String {
  if n < 0 { "-" } else if n == 0 { "0" } else { "+" }
}

fn greet(name: String, times: Int = 1) {
  for i in 0..times {
    puts("hello ${name}")
  }
}

fn answer: Int {
  42
}

fn main {
  puts(area(3, 4))      // => 12
  puts(sign(-7))        // => -
  greet("Ada")          // => hello Ada
  puts(answer())        // => 42
}
```

- The last expression is the result, a final `if`/`else` or `switch` included; `return` leaves early.
- In a function that returns nothing, the value of a call at the end is dropped, as it is anywhere else in the body. (An error is never dropped silently: [Errors cannot be dropped](#errors-cannot-be-dropped).) A line that only computes a value, with no call in it (`x + 1`, a name, a literal, a field read), is an error anywhere, since nothing uses it; at the end of a function with no result type the error says to declare one (`fn two { 2 }` is refused: write `fn two: Int { 2 }`). `_ = x + 1` drops a value on purpose.
- Parameters may have trailing defaults (`times: Int = 1`); functions may be [overloaded](#overloads-and-defaults) and [generic](#generic-functions).
- A parameter with a default may be named at the call, after the positional arguments: `pad("ab", fill: "*")` ([Overloads and defaults](#overloads-and-defaults)).
- There are no variadics and no multiple results; return a struct.

### Built-in types

| Type | What it is | More |
| --- | --- | --- |
| `Int` | 64-bit signed integer, the default for every count, size and index | [Numbers](#numbers) |
| `Int32`, `UInt32`, `UInt64` | the other integer widths | [Integer types](#integer-types) |
| `Float64` | 64-bit float | [Floats](#floats) |
| `Bool` | `true` or `false`; `!`, `&&`, `\|\|`, `==`, `!=`; `to_i` gives 1 or 0 | [Conversions](#conversions) |
| `String` | immutable bytes, normally UTF-8 | [Strings](#strings) |
| `Bytes`, `Buf` | immutable bytes; a growable byte buffer | [Bytes and Buf](#bytes-and-buf) |
| `Array(T)` | growable array | [Arrays](#arrays) |
| `Hash(K, V)` | hash table | [Hashes](#hashes) |
| `T?` | `T` or `nil` | [Nil and unions](#nil-and-unions) |
| `A \| B` | one of several types; with an `Error` member, a result that can fail | [Unions](#unions), [Errors](#errors) |
| `type Ty = A \| B` | a name for a type, a union above all | [Type aliases](#type-aliases) |
| `enum E { a, b }` | one of a fixed list of named values | [Enums](#enums) |
| `fn(T): U` | a function value | [Function values](#function-values) |
| `JoinHandle(T)` | a running task | [Spawn](#spawn) |
| `Ptr(T)` | a C pointer | [Layouts and Ptr](#layouts-and-ptr) |
| `Void`, `Nil` | "returns nothing"; the type of `nil` | |

Generic types are written with parentheses: `Array(Int)`, never `Array<Int>` or `[]Int`. Your own types are [structs and classes](#structs-and-classes).

### Operators

From loosest to tightest binding; operators on one row associate left to right.

| Operators | Meaning |
| --- | --- |
| `??` | the left value, or the right side when the left is `nil` or an error ([Opening an optional](#opening-an-optional)) |
| `\|\|` | logical or (short-circuit) |
| `&&` | logical and (short-circuit) |
| `==` `!=` | equality |
| `<` `<=` `>` `>=` | ordering |
| `\|` | bitwise or |
| `^` | bitwise xor |
| `&` | bitwise and |
| `<<` `>>` | shifts |
| `+` `-` `&+` `&-` | add, subtract; the `&` forms wrap instead of trapping |
| `*` `/` `%` `&*` | multiply, divide, remainder; wrapping multiply |
| `!x` `-x` `~x` | not, negate, bitwise not |
| `x?` `x.name` `x[i]` `f(x)` | postfix: [propagate a failure](#propagate-replace-branch), member, index, call |

As in Go and Rust, and unlike C, `&`, `^` and `|` bind tighter than comparisons: `x & 1 == 0` means `(x & 1) == 0`. Assignment is a statement: `=`, and a compound form for every binary operator, `x op= v` (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `&+=`, `&-=`, `&*=`), which is `x = x op v` with the place read once. There is no `++`, no ternary (`if` is an expression) and no `**`.

### Equality and order

`==` and `!=` compare two values of one type; values of different types do not compare (a compile error), an integer and a `Float64` included: write the conversion (`n.to_f == x`, or `n == x.to_i`, which truncates), so no precision is lost silently above 2^53. A literal takes the other side's type (`x == 2`).

- Numbers, `Bool`, `String`, `Bytes` and enums compare by value; a `Float64` by IEEE, so NaN equals nothing, itself included.
- A struct compares its fields in order, each with its own `==`, unless it defines `fn ==`. A struct with a field that has no `==` (a `Hash`, a function) has none either: the error names the field.
- A class compares by identity (the same object) unless it defines `fn ==`.
- An array equals another when the sizes are equal and the elements are, in order.
- An optional equals another when both are `nil` or both hold equal values. It is never compared with `nil` itself: open it ([Opening an optional](#opening-an-optional)).
- A `Hash`, a function and a union of several types have no `==`; take a union apart with [`switch`](#unions).
- `!=` is always `!(a == b)`.

`<`, `<=`, `>` and `>=` order numbers and `String`s (byte by byte, a prefix first). A struct or a class that defines `fn <` gets `>`, `<=` and `>=` from it, and `sort`, `sorted`, `min` and `max` of its arrays use it.

```avant
struct Point {
  x: Int
  y: Int
}

struct Version {
  major: Int
  minor: Int

  fn <(other: Version): Bool {
    major < other.major || (major == other.major && minor < other.minor)
  }
}

fn main {
  p = Point { x: 1, y: 2 }
  puts(p == Point { x: 1, y: 2 })        // => true
  puts([p] != [Point { x: 2, y: 1 }])     // => true
  puts("apple" < "banana")               // => true
  vs = [Version { major: 1, minor: 4 }, Version { major: 1, minor: 2 }]
  puts(vs[0] >= vs[1])                    // => true
  if v = vs.min {
    puts("${v.major}.${v.minor}")         // => 1.2
  }
}
```

### Printing

Every scalar has one text, the same in `puts`, in interpolation and from `to_s`: integers in decimal, a `Float64` as the shortest text that reads back as the same number ([Floats](#floats)), `true` and `false`, a `String` as itself. An enum prints its member's name, an error its text (its message and its cause), and a union its member; an optional is opened first. A struct, class or enum that defines `fn to_s: String` prints that text in interpolation (a type marked `Error` cannot: its text is its message). For every type that prints, `x.to_s` is exactly `"${x}"`. `puts(x)`, like every [writer](#output), prints exactly what `"${x}"` prints.

```avant
fn size_or_word(n: Int): Int | String {
  if n < 10 {
    return n
  }
  "many"
}

struct Vec2 {
  x: Int
  y: Int

  fn to_s: String {
    "(${x}, ${y})"
  }
}

fn main {
  puts(42.to_s + "/" + 2.5.to_s)                       // => 42/2.5
  puts("${size_or_word(3)} ${size_or_word(30)}")      // => 3 many
  puts("at ${Vec2 { x: 1, y: 2 }}")                   // => at (1, 2)
}
```

### Line breaks

A line break ends a statement. To continue an expression on the next line, end the line with an operator or an open bracket, or start the next line with `.`, `?` or an infix operator other than `+` and `-`.

```avant
fn main {
  total = 1 +
    2
  ok = total == 3
    && total > 0
  size = "avant"
    .size
  puts(total)   // => 3
  puts(ok)      // => true
  puts(size)    // => 5
}
```

A line whose first token is `[`, `-` or `+` always begins a new statement: it never continues the line before as an index or a binary operation. An expression left dangling by that rule (`t = a` followed by a line `- b`, a line `[a, b]` whose value nobody uses, a line starting with `+`) is a compile error that says how to continue the line.

### Comments

`//` runs to the end of the line. There is no `/* */`.

One comment form is reserved: a comment that starts `// file: ` changes the path and line that diagnostics report. It is not an import. Do not start a comment that way.

## Control flow

`if`, `while`, `for` and `switch`, all with required braces and no parentheses. `if` and `switch` are expressions: they produce values.

```avant
fn classify(n: Int): String {
  if n % 15 == 0 {
    "fizzbuzz"
  } else if n % 5 == 0 {
    "buzz"
  } else if n % 3 == 0 {
    "fizz"
  } else {
    "${n}"
  }
}

fn main {
  for i in 9..16 {
    puts(classify(i))
  }
}
```

<!-- check: output -->
```text
fizz
buzz
11
fizz
13
14
fizzbuzz
```

### If

An `if` with an `else` is an expression: each branch ends in a value, and the branches' types must agree. A branch may leave instead (`return`, `break`, `continue`).

```avant
fn main {
  n = 12
  label = if n > 10 { "big" } else { "small" }
  puts(label)   // => big
}
```

A final `if`/`else` in a function with a result is that function's value, as in `classify` above. An `if` without `else` is a statement. Conditions are `Bool`: there is no truthiness, so `if n` on an `Int` is a compile error. A struct literal directly in a condition needs parentheses around it (`if p == (Point { x: 1, y: 2 }) { ... }`), so the `{` that opens the block is never taken for a literal's.

### Conditions that bind

A condition may bind a name: `if v = expr { ... }` runs the block when `expr` gives a value (not `nil`, not an error) and names that value `v` inside it. This is how optional results are opened, where other languages write `if x != null`.

```avant
fn main {
  ports = Hash(String, Int).new
  ports["http"] = 80
  ports["https"] = 443

  if p = ports.get("http") {
    puts(p)                          // => 80
  }
  if a = ports.get("http"), b = ports.get("https"), a < b {
    puts(a + b)                      // => 523
  } else {
    puts("missing or out of order")
  }
}
```

- The bound name exists only in the then-block: not in the `else`, not after the `if`. With one binding, `else e { ... }` names the failure for the `else` block ([Reading the error itself](#reading-the-error-itself)).
- An `if` that binds is an expression like any other: `width = if v = cfg.get("width") { v } else { 80 }`.
- A condition may be a comma list of bindings and `Bool` tests, evaluated left to right and stopping at the first that fails. Each bound name is in scope for the conditions after it. The `else` runs when any of them fails, and sees none of the names.
- `while` takes the same conditions ([While](#while)).

### While

```avant
fn main {
  n = 0
  while n < 3 {
    n += 1
  }
  puts(n)   // => 3
}
```

`while v = x { ... }` runs its body while `x` gives a value, evaluating it again before each iteration; `v` exists in the body only. It is the loop for walking a chain or draining a source:

```avant
class Node {
  value: Int
  next: Node?

  fn initialize(value: Int, next: Node?) {
    self.value = value
    self.next = next
  }
}

fn main {
  cur: Node? = Node.new(1, Node.new(2, Node.new(3, nil)))
  total = 0
  while n = cur {
    total += n.value
    cur = n.next
  }
  puts(total)   // => 6
}
```

`break` leaves the nearest loop and `continue` starts its next iteration. `while true { ... }` is the endless loop.

### For

`for` walks a range or an array.

```avant
fn main {
  sum = 0
  for i in 0..5 {          // 0, 1, 2, 3, 4
    sum += i
  }
  for i in 1..=3 {         // 1, 2, 3
    sum += i
  }
  for x in [10, 20] {
    sum += x
  }
  puts(sum)                // => 46
}
```

- **`..` excludes its end**, unlike Crystal's, Ruby's and Kotlin's: `for i in 1..n` never reaches `n`. **`..=` includes it**, as Rust's: `for i in 1..=n` ends with `n`, even when `n` is the largest `Int` (the loop stops after its last value instead of computing the next one).
- The bounds are evaluated once. The loop variable is a new name that exists only inside the loop (when a variable of that name already exists outside, the loop uses it, and it keeps its last value afterwards: pick a fresh name). `break`, `continue` and `return` behave as they do in `while`.
- **The loop variable cannot be assigned.** It is a copy of the range's counter or of the array's element, made again at each step, so `i = 5`, `x += 1` and a field of a struct element (`p.x = 1`) are compile errors: the write would be lost, or would change the iteration. To change the array, store through an index: `xs[i] = v`, or `xs[i].x = v` for one field of a struct element. For a range that skips or repeats steps, use `while` with a counter of your own. A field of a *class* element is assignable: `c.x = 1` changes the object.
- A range is only a `for` form: it is not a value you can store.
- Arrays and hashes also have `each` ([Array operations](#array-operations), [Hash operations](#hash-operations)).
- A struct literal in a `for`'s range or array needs parentheses, brackets or a call around it, as in a [condition](#if): `for p in [Point { x: 1, y: 2 }]`.

### Switch

`switch` picks an arm by an integer, an [enum](#enums) member, a string or the member a [union](#unions) holds. There is no fall-through; an arm may list several values.

```avant
fn name_of(kind: Int): String {
  switch kind {
    case 0 { "eof" }
    case 1, 2 { "ident-or-int" }
    else { "other" }
  }
}

fn main {
  puts(name_of(0))   // => eof
  puts(name_of(2))   // => ident-or-int
  puts(name_of(9))   // => other
}
```

Over an enum, the labels are the members' names written bare, and a switch without `else` must name every member: add a member and the compiler lists each switch that misses it.

```avant
enum Tok { eof, name, number }

fn describe(t: Tok): String {
  switch t {
    case eof { "end of file" }
    case name, number { "text" }
  }
}

fn main {
  puts(describe(Tok.number))   // => text
}
```

A label may be a range of integers, written as in `for`: `lo..hi` excludes `hi`, `lo..=hi` includes it, and a character literal is an integer:

```avant
fn kind(c: Int): String {
  switch c {
    case 'a'..='z', 'A'..='Z', '_' { "name" }
    case '0'..='9' { "digit" }
    case 0..=' ' { "control or space" }
    else { "other" }
  }
}

fn main {
  puts(kind('q'))    // => name
  puts(kind('7'))    // => digit
  puts(kind(' '))    // => control or space
  puts(kind('+'))    // => other
}
```

Over a `String`, the labels are string literals and the match is byte equality, as `==`:

```avant
fn code(method: String): Int {
  switch method {
    case "GET", "HEAD" { 1 }
    case "POST" { 2 }
    else { 0 }
  }
}

fn main {
  puts(code("HEAD"))   // => 1
  puts(code("get"))    // => 0
}
```

Over a union, a label names a member type: `case n: Int` binds the value as an `Int` for that arm only, `case String` names the member without binding it, and `case nil` names `nil`. A switch without `else` names every member, the failures too:

```avant
fn describe(x: Int | String | Nil): String {
  switch x {
    case n: Int { "the number ${n}" }
    case s: String { "the text ${s}" }
    case nil { "nothing" }
  }
}

fn main {
  puts(describe(4))        // => the number 4
  puts(describe("hi"))     // => the text hi
  puts(describe(nil))      // => nothing
}
```

- The value is an integer of any width, `Int`, `Int32`, `UInt32` or `UInt64` (labels: integer literals, character literals included, or [constants](#constants), each taking the value's type: `case 3000000000` over an `Int32` is `case label 3000000000 does not fit in Int32`), an enum (labels: its members, bare: `case eof`, not `case Tok.eof`), a `String` (labels: string literals, not interpolated) or a union (labels: its member types, with or without a binding, and `nil`; a binding is the arm's only label). Over a `Bool`, the labels are `true` and `false` (`case 1` is refused with the hint `case true`).
- A label may appear once, and no two labels share a value: `case 5` beside `case 0..=9` is `case 5 overlaps case 0..=9`; an empty range (`case 5..5`) is an error. A range is tested with two comparisons before the switch, as the `if` chain it replaces (one case per value ran 5% to 9% slower: LLVM built tables).
- Over an enum, a union or a `Bool`, a switch without `else` names every member (`true` and `false` for a `Bool`), as a statement and as a value (`switch over Tok does not cover name, number: name them or add else`), and an `else` beside every member is refused. Over the other types, a switch used as a value needs `else`, so every input produces a result; used as a statement, a value no arm names does nothing.
- An arm may leave instead of giving a value (`return`, `break`, `continue`); it still counts as naming its members.
- A switch used as a statement treats its arms as statements, as an `if` does: an arm may end with a value, which is dropped, unless it can be an error ([Errors cannot be dropped](#errors-cannot-be-dropped)).

## Enums

An `enum` is a type whose values are a fixed list of names. Each member has a number, written or counted (C's rule: the first is 0, each next one the previous plus one), and the members are written in lower case.

```avant
enum Tok {
  eof
  name
  number
}

enum Status { ok = 200, created, not_found = 404 }

fn main {
  t = Tok.name
  puts("${t}")                       // => name
  puts(t.to_i)                       // => 1
  if u = Tok.from_i(2) {
    puts(u == Tok.number)            // => true
  }
  puts(Tok.from_name("eof") ?? Tok.name)   // => eof
  puts(Status.created.to_i)          // => 201
  puts(Tok.members.size)             // => 3
}
```

- A member is written qualified, `Tok.eof`, everywhere except a [`case` label](#switch). Members are separated by commas or line breaks, a trailing comma allowed; a member's name is lower-case snake_case (a capital first letter is refused: a capital means a type).
- An enum is its own type: an `Int` is not a `Tok` and a `Tok` is not an `Int`. `==` and `!=` compare two values of one enum; there is no arithmetic and no `<`.
- What every enum has: `t.to_i` (its number), `t.name` (the member's exact name, as a `String`), `Tok.from_i(n)` and `Tok.from_name(s)` (a `Tok?`: `nil` when no member has that number or that exact name) and `Tok.members` (a new `Array(Tok)` in declaration order). `puts(t)` and `"${t}"` print the member's name.
- Values are distinct and fit in `Int32`; a value may be negative (`low = -2`) or a [constant](#constants) (`ok = http_ok`; the members after it count from its value).
- An enum has no zero value: a field of an enum type has a default (`kind: Tok = Tok.eof`) or is set in `initialize`, and `Array(Tok).new(n)` needs its value (`Array(Tok).new(n, Tok.eof)`).
- `fn` methods follow the members, with `self` the value:

```avant
enum Dir {
  north, east, south, west

  fn turn: Dir {
    switch self {
      case north { Dir.east }
      case east { Dir.south }
      case south { Dir.west }
      case west { Dir.north }
    }
  }
}

fn main {
  puts(Dir.west.turn)      // => north
}
```

- An enum is stored in one byte when its values are within `0..255` or `-128..127`, in two within `0..65535` or `-32768..32767`, else in four; an enum crossing to C is C's `int`. A program pays for the members' names only if it prints an enum, reads `name`, or calls `from_name`.
- An enum marked `: Error` is a failure type ([An enum as an error](#an-enum-as-an-error)); one inside a `lib` block is a C enum ([avant bind](#avant-bind)). An enum has no payload: a member that carries data is a [union](#unions) member.

## Nil and unions

`T?` is "a `T`, or `nil`". Absence is in the type, so the compiler makes you look before you use the value, and there is no null pointer error to meet at run time (a C pointer, [`Ptr(T)`](#layouts-and-ptr), is the one exception: it is C's).

```avant
fn find(names: Array(String), wanted: String): Int? {
  for i in 0..names.size {
    if names[i] == wanted { return i }
  }
  nil
}

fn main {
  names = ["ada", "bo", "cy"]
  if i = find(names, "bo") {
    puts("found at ${i}")              // => found at 1
  }
  puts(find(names, "zed") ?? -1)       // => -1
}
```

### Opening an optional

These forms take the value out of a `T?`. All of them work the same way on a result that can fail ([Errors](#errors)).

| Form | Gives | Use it when |
| --- | --- | --- |
| `x?` | the value, or returns `nil` from the current function | the caller should deal with it |
| `x ?? fallback` | the value, or `fallback` (evaluated only when needed) | there is a sensible default |
| `x ?? return`, `?? return v`, `?? break`, `?? continue` | the value, or leaves | absence ends this function or iteration, and the rest should not nest |
| `if v = x { ... } else { ... }` | `v` inside the block | absence needs its own code path |
| `x.or(fallback)` | the same as `x ?? fallback` | you prefer a method chain |

```avant
fn port_of(cfg: Hash(String, Int)): Int {
  cfg.get("port") ?? 8080
}

fn double_port(cfg: Hash(String, Int)): Int? {
  p = cfg.get("port")?        // nil here returns nil from double_port
  p * 2
}

fn main {
  cfg = Hash(String, Int).new
  puts(port_of(cfg))                 // => 8080
  puts(double_port(cfg) ?? 0)        // => 0
  cfg["port"] = 9000
  puts(double_port(cfg) ?? 0)        // => 18000
}
```

`x?` compiles only in a function whose result can carry the `nil` (here `Int?`); otherwise the compiler says what would be returned and where it cannot go. There is no `?.` chaining, no `!` to force a value out and no `x == nil`: open it. To test for absence without using the value, write `if _ = x { ... } else { ... }`.

### Unions

Any other union is written with `|`: `Int | String`, `Item | BadLine`. `T?` is shorthand for `T | Nil`, and `nil` is the only value of `Nil`. Parentheses group a type: `(Int | String)?` is `Int | String | Nil`, and `(fn(Int): Int)?` an optional [function value](#function-values).

```avant
class Config {
  port: Int | String = 8080
}

fn main {
  c = Config.new
  c.port = "auto"
  c.port = 9000
  puts("stored")   // => stored
}
```

The union you will write most is a result that can fail, `T | SomeError`, and the forms above open it. Any union is taken apart with [`switch`](#switch), one arm per member:

```avant
fn size_of(v: Int | String | Array(Int)): Int {
  switch v {
    case n: Int { n }
    case s: String { s.size }
    case xs: Array(Int) { xs.size }
  }
}

fn main {
  puts(size_of(3))           // => 3
  puts(size_of("four"))      // => 4
  puts(size_of([1, 2]))      // => 2
}
```

### Type aliases

`type Name = T` gives a type a name: every use of the name is that type, the same type and the same code, not a new one. It names a union above all, written once instead of at every use; the classes of the union may hold the name in their fields, so a recursive type (a tree, a JSON value, a type checker's types) reads as declared.

```avant
type Expr = Num | Add

class Num {
  pub n: Int

  fn initialize(n: Int) {
    self.n = n
  }
}

class Add {
  pub left: Expr
  pub right: Expr

  fn initialize(left: Expr, right: Expr) {
    self.left = left
    self.right = right
  }
}

fn eval(e: Expr): Int {
  switch e {
    case n: Num { n.n }
    case a: Add { eval(a.left) + eval(a.right) }
  }
}

fn main {
  e: Expr = Add.new(Num.new(2), Add.new(Num.new(3), Num.new(4)))
  puts(eval(e))   // => 9
}
```

- An alias is private to its module unless `pub`, like a type, and its name starts with a capital letter. It takes no type parameters.
- An alias may not reach itself except through a struct or class: `type Json = Int | Array(Json)` is refused; give the recursive part a class, as `Add` above.
- An alias names a type where a type is written (a declaration, a field, a parameter, a type argument); a value is built with the type's own name (`Num.new(2)`, not `Expr.new`).
- `type` is a keyword only at the start of a declaration; `type` stays usable as a name.

### What an optional costs

An optional reference costs nothing over the reference: `T?` where `T` is a class, `String`, `Bytes`, `Array`, `Hash` or `Buf` is one pointer, and `nil` is the null pointer, whether in a field, a local, an array element, a parameter or a result (`left: Node?` is 8 bytes). An optional function value, `(fn(Int): Int)?`, is the function value's two words, `nil` a null code word. Any other union holds a small tag and one slot per member that is not `nil`.

A `T?` that nothing assigned is `nil`, whatever `T` is: an optional field without a default reads as absent.

## Numbers

`Int` is the integer: 64-bit and signed on every machine. Arithmetic is checked: overflow and division by zero stop the program instead of producing a wrong number, and nothing converts between number types silently.

<!-- check: run -->
```avant
fn main {
  ms = 1731369600000          // an Int holds milliseconds since 1970 with room to spare
  puts(ms / 1000)             // => 1731369600
  puts(7 / 2)                 // => 3
  puts(-7 % 2)                // => -1
  half = 7.0 / 2.0
  puts(half)                  // => 3.5
  puts(0.1 + 0.2)             // => 0.30000000000000004
  n = 3
  puts(n.to_f * half)         // => 10.5
}
```

### Integer types

| Type | Size | Range | Use it for |
| --- | --- | --- | --- |
| `Int` | 64-bit signed | ±9.2 × 10^18 | everything by default: counts, sizes, indexes, time, ids |
| `Int32` | 32-bit signed | −2,147,483,648 to 2,147,483,647 | dense storage (`Array(Int32)` takes 4 bytes per element, half an `Array(Int)`); C's `int` |
| `UInt32` | 32-bit unsigned | 0 to 4,294,967,295 | dense storage; C's `unsigned int` |
| `UInt64` | 64-bit unsigned | 0 to 18,446,744,073,709,551,615 | bit patterns, hashes, C's 64-bit unsigned values |

`Int` is 64-bit because arithmetic traps on overflow: with a 32-bit default, an ordinary counter of milliseconds, bytes or ids would crash at 2.1 billion. Reach for `Int32` only when you store many of them. An index, a size and a `for` range are always `Int`, so `xs[i]` with an `Int32` index is written `xs[i.to_i]` (the conversion is free). Five narrower names (`Int8`, `Int16`, `UInt8`, `UInt16`, `Float32`) exist only at the C boundary ([C types](#c-types)).

### Literals

An integer literal is an `Int` unless its context gives it another number type: a declared type, a parameter (of any function or method, generic or an operator's included), a field, a function's result, or the other operand of an operator, on either side. An expression made only of literals takes its type the same way (`(2 - 1) + y`, `u: UInt64 = ~0`, `(1 << 63) | u`), and so does a literal argument above `Int`'s range when its parameter is a `UInt64`. The context may be a `Float64` too: with a float `f`, `f * 2` and `2 * f` are float arithmetic.

```avant
fn half(n: Int32): Int32 {
  n / 2
}

fn main {
  x: Int32 = 40
  y = x + 2                      // 2 is an Int32 here
  z = 2 + x                      // and here
  puts(half(10) + y + z)         // => 89
  mask = 0xFF                    // hexadecimal
  bits = 0b1010_0101             // binary, with a separator
  mode = 0o755                   // octal
  top = 18446744073709551615u64  // the u64 suffix makes a UInt64
  puts(mask)                     // => 255
  puts(bits)                     // => 165
  puts(mode)                     // => 493
  puts(top)                      // => 18446744073709551615
  puts(1_000_000)                // => 1000000
  puts('A')                      // => 65
  puts('\n')                     // => 10
}
```

- `0x`, `0b` and `0o` begin hexadecimal, binary and octal literals (the prefix in lower case; `0X` also works). A number does not start with 0 otherwise: `017` is a compile error that suggests `0o17` or `17`, since C reads it as octal.
- `_` may stand between two digits of any literal, a float's included (`1_000.000_5`), and nowhere else: not first, last, doubled, after a prefix or before a suffix.
- A **character literal** is the `Int` value of one byte: `'a'` is 97, and it adopts its context's type as any integer literal does. It takes the escapes of a string (below), with `\'` for the quote. A character that is more than one byte in UTF-8 (`'é'`, `'\u{e9}'`) is a compile error: write it in a `String`. There is no `Char` type.
- A literal outside its type's range is a compile error (`integer literal does not fit in Int32`) at every integer width and wherever the type comes from (a declaration, an argument, a result, a field, an array, the other operand); above `UInt64` it is `integer literal does not fit in UInt64, the widest integer type`. A float literal too large for `Float64` (`1e400`) is an error too. Float literals need a digit on each side of the point (`0.5`, `2.0`, `1e3`, `1.5e-7`).

### Conversions

Values of different number types do not mix: `x + y` with an `Int32` and an `Int`, or an `Int` and a `Float64`, is a compile error (`binary operator expects matching types, got Int32 and Int`), and the operator converts neither side. Only literals adapt. Convert explicitly, and choose what happens when the value does not fit.

| To | Keep the number, or trap | Reinterpret the bits, never trap |
| --- | --- | --- |
| `Int` | `to_i` | `wrap_i` |
| `Int32` | `to_i32` | `wrap_i32` |
| `UInt32` | `to_u32` | `wrap_u32` |
| `UInt64` | `to_u64` | `wrap_u64` |
| `Float64` | `to_f` (rounds to the nearest float) | |

A `Bool` is a number only when converted: `to_i`, `to_i32`, `to_u32` and `to_u64` give 1 for `true` and 0 for `false`, so `alive += cell.to_i` counts without a branch.

Use `to_*` by default: the result is the same number, or the program stops and says why (`avant runtime: to_i32 of Int 5000000000 out of range for Int32 at main.av:3:11`). Use `wrap_*` when you mean the two's-complement bits, as in hashing or binary formats.

```avant
fn main {
  big = 5000000000
  minus = -1
  puts(big.wrap_i32)        // => 705032704
  puts(minus.wrap_u32)      // => 4294967295
  puts(minus.wrap_u64)      // => 18446744073709551615
  f = 2.9
  puts(f.to_i)              // => 2
}
```

`Float64.to_i` truncates toward zero, and traps on NaN, an infinity, or a value outside `Int` (`avant runtime: to_i of Float64 30000000000000000000.0 out of range for Int`). From text, `"42".to_i` returns an optional ([Parsing numbers](#parsing-numbers)). To text, interpolate (`"${n}"`) or call `to_s`, the same text ([Printing](#printing)), or use `fmt_float(x, digits)` for a fixed number of decimals.

### Arithmetic and overflow

The rule: an operation gives the mathematically correct value when one exists, and traps when none does.

- `+`, `-`, `*` trap on overflow at the operands' width; `/` and `%` trap on division by zero. `abs` of the minimum value, a `sum` that overflows and a `Hash` `inc` that overflows trap the same way.
- `/` truncates toward zero and `%` takes the sign of the dividend (`-7 / 2` is `-3`, `-7 % 2` is `-1`): C's, Go's and Rust's rule, not Python's or Ruby's. `MIN % -1` is `0`; only `MIN / -1` overflows.
- `&+`, `&-`, `&*` wrap on overflow. Wrapping is always spelled, never implied.
- `<<` and `>>` take an amount in `0..63` for `Int` and `UInt64`, `0..31` for `Int32` and `UInt32`. Any other amount, a negative one included, traps (`avant runtime: shift amount 64 out of range for Int (0..63)`); a literal amount out of range is a compile error. The amount is an `Int` or the shifted value's own type.
- Integers have `abs` (unsigned types do not: it would be the value itself) and `to_f`.

A check costs a compare and a never-taken branch, and the compiler drops the ones it can prove: a literal index into a literal, a loop variable under its bound, a loop whose whole index range is validated once on entry. What a trap looks like is in [Traps](#traps).

### Floats

`Float64` has `abs`, `floor`, `sqrt`, `exp` and `to_i`, and the prelude has `sqrt(x)` and `exp(x)`. Float arithmetic follows IEEE 754 and never traps: `1.0 / 0.0` is `inf`.

The rest of C's math library is one `lib` block away and needs no flag ([C interop](#c-interop)):

```avant
lib m {
  fun pow(x: Float64, y: Float64): Float64
  fun log(x: Float64): Float64
}

fn main {
  puts(m.pow(2.0, 10.0))   // => 1024.0
  puts(m.log(1.0))         // => 0.0
}
```

`puts` and interpolation print a float as the shortest text that reads back as the same number, with `.0` on a whole value: `0.30000000000000004`, `2.0`, `1e21`, `1.5e-7`, `-0.0`, `nan`, `inf`, `-inf`. JSON output uses the same text (with `null` for NaN and the infinities). For a fixed number of decimals, `fmt_float(x, digits)` returns a `String` and `buf.push_fmt_f(x, digits)` appends to a buffer; `digits` is 0 to 64, and anything else traps.

```avant
fn main {
  x = 2.0
  puts(x.sqrt)                 // => 1.4142135623730951
  puts(fmt_float(x.sqrt, 3))   // => 1.414
  puts(x * 3.0)                // => 6.0
}
```

## Strings

A `String` is an immutable sequence of bytes, normally UTF-8. `${expr}` interpolates; `+` concatenates; every operation that "changes" a string returns a new one.

```avant
fn main {
  name = "Ada"
  line = "hello ${name}, you have ${2 + 3} messages"
  puts(line)                          // => hello Ada, you have 5 messages
  puts(line.size)                     // => 30
  puts("  padded ".strip + "!")       // => padded!
  words = "to be or not".split(" ")
  puts(words.size)                    // => 4
}
```

### String literals

- `"${expr}"` interpolates integers, floats, `Bool`s, strings, enums, [errors](#declaring-an-error) and unions of them ([Printing](#printing)). A `$` that is not followed by `{` is a dollar sign: `"it costs $5"`.
- Escapes: `\n`, `\t`, `\r`, `\\`, `\"`, `\$`, `\0`, `\xNN` (one byte, two hex digits, which may make a string that is not UTF-8, as bytes from a file can) and `\u{N}` (a Unicode code point, one to six hex digits, stored as its UTF-8 bytes: `"caf\u{e9}"`). Any other escape is a compile error (there is no `\e`: write `\x1b`).
- A raw string `r"..."` has no escapes and no interpolation, for paths and patterns: in `r"C:\tmp"` the backslash is a character.
- A literal is one line. There are no multi-line or backtick strings. A byte is a [character literal](#literals), `'A'`, or its number, `65`.

### Bytes, not characters

`size` is the byte length (at most 2,147,483,647: [Size limits](#size-limits)), and `s[i]` is the byte at `i` as an `Int`. Nothing checks that a string is UTF-8 until you ask for characters: `s.chars` returns one `String` per Unicode code point and traps on bytes that are not valid UTF-8 (`avant runtime: String.chars needs valid UTF-8`), and `String.from_utf8(bytes)` returns `nil` for them. This is Go's model: cheap, predictable indexing, and text handled deliberately.

```avant
fn main {
  s = "né"
  puts(s.size)             // => 3
  puts(s[0])               // => 110
  puts(s.chars.size)       // => 2
  puts(s.chars[1])         // => é
}
```

### String operations

| Operation | Result |
| --- | --- |
| `s.size` | byte length |
| `s[i]` | the byte at `i`, an `Int`; traps outside `0 <= i < size` |
| `s.get(i)` | the byte, or `nil` outside the string (`Int?`) |
| `a + b` | a new string |
| `a == b`, `a != b` | equality by content |
| `a < b`, `<=`, `>`, `>=` | byte order, a prefix first: the order `sort`, `min` and `max` use |
| `s.split(sep)` | `Array(String)`, empty fields kept; an empty separator traps |
| `s.fields` | split on runs of whitespace, empty fields dropped |
| `s.strip`, `s.strip_left`, `s.strip_right` | whitespace trimmed |
| `s.chars` | `Array(String)`, one per code point |
| `s.bytes` | the same bytes as [`Bytes`](#bytes-and-buf) |
| `s.to_i`, `s.to_u64`, `s.to_f` | [parsed numbers](#parsing-numbers), optional |
| `str_slice(s, start, stop)` | the bytes `start` up to `stop`; traps unless `0 <= start <= stop <= size`, so an empty result means the range was empty, never that it was wrong |
| `chr(b)` | a one-byte string; `b` must be in `0..255` |
| `repeat_byte(b, n)` | `n` copies of the byte `b` |
| `bytes_to_str(xs)` | a string from an `Array(Int)` of bytes |
| `String.from_utf8(bytes)` | `String?`: `nil` when the bytes are not valid UTF-8 |

`String` has no `contains`, `starts_with`, `index_of`, `replace`, `join` or case conversion yet ([Not built yet](#not-built-yet)); [`std.regex`](#stdregex) finds, replaces and splits. Substrings are a free function (`str_slice`) rather than a method for now.

### Parsing numbers

`s.to_i`, `s.to_u64` and `s.to_f` read the *whole* string as a number and return `Int?`, `UInt64?` and `Float64?`. Anything else is `nil`, so the fallback is yours to state.

```avant
fn main {
  puts("42".to_i ?? 0)          // => 42
  puts("-17".to_i ?? 0)         // => -17
  puts("12abc".to_i ?? -1)      // => -1
  puts(" 42".to_i ?? -1)        // => -1
  puts("ff".to_i(16) ?? -1)     // => 255
  puts("2.5".to_f ?? 0.0)       // => 2.5
  puts("1e3".to_f ?? 0.0)       // => 1000.0
}
```

- An integer is an optional sign and digits within the type's range, nothing else: `"0x1F"`, `" 42"` and `"9223372036854775808"` (one past `Int`) are `nil`.
- `s.to_i(base)` and `s.to_u64(base)` take a base from 2 to 36, with letters in either case.
- A float has the shape of a float literal (`-2.5`, `1e3`, `1.5E-7`) and a finite value: `"nan"`, `"inf"`, `"1e999"`, `".5"` and `"1."` are `nil`.
- `str_to_f_slice(s, start, stop)` parses a slice the same way without building the substring.

### Bytes and Buf

`Bytes` is the immutable binary counterpart of `String`, and `Buf` is a growable byte buffer: the way to build a large string or a binary payload without allocating a new string for every piece.

```avant
fn main {
  out = Buf.new
  for i in 1..4 {
    out.push_str("item ")
    out.push_fmt_i(i)
    out.push_byte(10)               // a newline
  }
  text = out.to_s
  puts(text.size)                   // => 21

  raw = "héllo".bytes
  puts(raw.size)                    // => 6
  puts(raw.slice(0, 1) == "h".bytes)   // => true
  puts(String.from_utf8(raw) ?? "not text")   // => héllo
}
```

| Type | Mutable | Holds | Use it for |
| --- | --- | --- | --- |
| `String` | no | bytes, normally UTF-8 | text |
| `Bytes` | no | any bytes | binary data: payloads, hashing, compression |
| `Buf` | yes, grows | any bytes | building text or binary output piece by piece |
| `Array(Int)` | yes, grows | 8 bytes per element | numbers. Not byte data: a byte table in an array costs eight times a `Buf` |

| `Bytes` operation | Result |
| --- | --- |
| `b.size`, `b[i]`, `b.get(i)` | as for `String` |
| `b.slice(start, stop)` | a new `Bytes`; traps unless `0 <= start <= stop <= size` |
| `a == b` | equality by content |
| `b.chars` | as `String.chars` |
| `b.to_buf` | a `Buf` holding a copy |

| `Buf` operation | Result |
| --- | --- |
| `Buf.new`, `Buf.new(capacity)`, `buf_new()` | an empty buffer |
| `push_str(s)`, `push_bytes(b)`, `push_byte(n)` | append a string, `Bytes`, or one byte (`0..255`, else a trap) |
| `push_slice(s, start, stop)` | append part of a string without building the substring |
| `push_fmt_i(n)`, `push_fmt_f(x, digits)` | append a formatted integer or float |
| `buf.size`, `buf[i]`, `buf.get(i)`, `buf[i] = n` | read and overwrite bytes |
| `starts_with(s)` | `Bool` |
| `clear` | empty it, keeping the storage |
| `to_s`, `to_bytes` | a `String` or `Bytes` of the contents |

A byte stored by `push_byte`, `buf[i] = n`, `chr`, `repeat_byte` or `bytes_to_str` must be in `0..255` (`avant runtime: byte 300 out of range (0..255)`); write `n & 255` to keep the low byte on purpose. Many library calls have an `_into(buf)` twin that writes into a buffer you own instead of allocating a result ([Standard library](#standard-library)).

## Arrays

`Array(T)` is a growable array. A literal's element type is the type of its elements.

```avant
fn main {
  xs = [3, 1, 2]
  xs.push(5)
  puts(xs.size)          // => 4
  puts(xs[0])            // => 3
  xs[0] = 4
  xs.sort
  puts(xs[0])            // => 1
  puts(xs.sum)           // => 12
  puts(xs.contains(5))   // => true

  total = 0
  for x in xs {
    total += x
  }
  puts(total)            // => 12
}
```

### Creating arrays

```avant
class Node {
  id: Int = 0
}

fn main {
  a = [1, 2, 3]                       // Array(Int), from its elements
  b: Array(String) = []               // an empty literal needs a declared type
  c = Array(Float64).new(4)           // four elements, each 0.0
  d = Array(String).new(2, "none")    // two elements, each "none"
  e: Array(Int32) = [1, 2, 3]         // dense: 4 bytes per element
  f = Array(Node).new(2) { |i| Node.new }                  // two elements, each its own Node
  g = Array(Array(Int)).new(3) { |y| Array(Int).new(4) }   // a grid of 3 rows of 4
  g[0][0] = 7
  puts(g[1][0])                       // => 0
  puts(a.size + b.size + c.size + d.size + e.size + f.size + g.size)   // => 17
}
```

- `Array(T).new(n)` makes `n` elements, each the element type's zero: `0`, `0.0`, `false`, `""`, `nil` for any `T?`, a struct of such fields.
- `Array(T).new(n, value)` makes `n` copies of `value`, for a `T` whose copies are independent: a number, a `Bool`, a `String`, `Bytes`, a struct of those, and `nil` for any `T?`. When `T` is, or holds, a reference to an object that can change (a class, an `Array`, a `Hash`, a `Buf`, a struct or an optional holding one) it is a compile error: every element would be the same object, and a change through one would show in all.
- `Array(T).new(n) { |i| ... }` builds each element: the block runs once per index, `0` to `n - 1` in order, and its value is that element. It is the form for a `T` with no zero (`Array(Node).new(n)` alone is a compile error that names the ways out) and for a grid, whose rows must each be their own array. `return` inside the block returns from the enclosing function, as in `each`; `break` and `continue` are compile errors, since every index gets a value. To put one shared object in every element on purpose, say so: `Array(Node).new(n) { |i| shared }`.
- A negative `n` traps (`avant runtime: Array size -5 is negative`).

### Indexing and bounds

`xs[i]` traps outside `0 <= i < size`: one line on stderr and exit status 1 ([Traps](#traps)). `xs.get(i)` returns `T?` instead, the element or `nil`. `String`, `Bytes` and `Buf` follow the same rule.

```avant
fn main {
  xs = [10, 20, 30]
  puts(xs.get(1) ?? -1)    // => 20
  puts(xs.get(9) ?? -1)    // => -1
  if last = xs.get(xs.size - 1) {
    puts(last)             // => 30
  }
}
```

There are no negative indexes and no slices of arrays yet.

### Array operations

| Operation | What it does |
| --- | --- |
| `xs.size` | number of elements |
| `xs[i]`, `xs[i] = v`, `xs.get(i)` | read (trapping), write, read (`T?`) |
| `xs.push(v)` | append; the storage grows as needed |
| `xs.pop` | remove and return the last element; traps on an empty array (`avant runtime: pop on an empty Array at main.av:7:10`) |
| `xs.clear` | remove every element |
| `xs.reserve(n)` | make room for `n` elements ahead of time |
| `xs.fill(v)` | set every element (integer and `Bool` arrays) |
| `xs.each { \|x\| ... }`, `for x in xs` | walk the elements |
| `xs.sort`, `xs.sort { \|a, b\| a < b }` | sort in place: by the element's order, or by a block that says whether `a` comes first |
| `xs.sorted`, `xs.sorted { \|a, b\| ... }` | the same, as a new array |
| `xs.reverse` | reverse in place |
| `xs.contains(v)`, `xs.index_of(v)` | `Bool`; `Int?`, the first position; by the elements' `==` ([Equality and order](#equality-and-order)), so a struct compares its fields and an array its elements, and an element type without `==` is refused |
| `xs.min`, `xs.max` | `T?`: `nil` for an empty array |
| `xs.sum` | the total (numeric arrays); traps on overflow |
| `xs.any { \|x\| ... }`, `xs.all { \|x\| ... }` | `Bool`, stopping at the first answer |
| `xs.parallel { \|x\| ... }` | a new array, computed [in parallel](#parallel) |

- The element order used by `sort`, `min` and `max` exists for integers, `Float64` (NaN after every number), `String` and `Bytes` (byte order) and `Bool`. Any other element type sorts with a comparator block. On floats, `min` is the smallest number (NaN only when every element is NaN) and `max` is NaN when any element is.
- There is no `map`, `select`, `reduce` or `join` built in yet; a loop, or a three-line generic function of your own ([Trailing blocks](#trailing-blocks)), does it.
- The blocks of `each`, `any`, `all`, `sort` and `parallel` are inlined loops, not function values ([Builtin blocks are loops](#builtin-blocks-are-loops)).

```avant
fn main {
  words = ["pear", "fig", "banana"]
  by_length = words.sorted { |a, b| a.size < b.size }
  puts(by_length[0])                          // => fig
  puts(words.index_of("fig") ?? -1)           // => 1
  puts(words.any { |w| w.size > 5 })          // => true
  puts(words.max ?? "")                       // => pear
}
```

### Size limits

A `String`, `Bytes` or `Buf` holds at most 2,147,483,647 bytes and an `Array` at most 2,147,483,647 elements (the runtime keeps 32-bit sizes); a `Hash` holds at most 1,073,741,824 slots. Growing past a limit traps before anything is written (`avant runtime: Buf would exceed 2147483647 bytes`, `Hash would exceed 1073741824 slots`), and `file_read` of a larger file is an `IoError` (`File too large`).

## Hashes

`Hash(K, V)` maps keys to values. A lookup returns `V?`: a missing key is `nil`, never a default value and never a crash. A key is an integer of any width, a `Bool`, an enum or a `String`; a value is any of those or a `Float64` ([Key and value types](#key-and-value-types)).

```avant
fn main {
  stock = Hash(String, Int).new
  stock["tea"] = 12
  stock["milk"] = 3

  if n = stock["tea"] {
    puts(n)                          // => 12
  }
  puts(stock["cake"] ?? 0)           // => 0
  stock.inc("milk")
  stock.inc("milk", 10)
  puts(stock.get_or("milk", 0))      // => 14
  puts(stock.size)                   // => 2
}
```

### Hash operations

| Operation | What it does |
| --- | --- |
| `Hash(K, V).new` | an empty hash (there is no hash literal) |
| `h[k] = v` | insert or replace |
| `h[k]`, `h.get(k)` | `V?` |
| `h.get_or(k, default)` | the value, or `default` |
| `h.has_key(k)` | `Bool` |
| `h.delete(k)` | remove the key; returns the value it held, or `nil` |
| `h.inc(k)`, `h.inc(k, by)` | add to a counter, starting from 0, and give its new value (integer values only; `by` has the value's type); traps on overflow |
| `h.size`, `h.clear` | number of keys; remove all |
| `h.keys`, `h.values` | arrays, in unspecified order |
| `h.each { \|k, v\| ... }` | walk every key with its value |

```avant
fn main {
  count = Hash(String, Int).new
  for w in "the cat and the hat and the bat".split(" ") {
    count.inc(w)
  }
  puts(count["the"] ?? 0)                 // => 3
  if old = count.delete("cat") {
    puts(old)                             // => 1
  }
  keys = count.keys
  keys.sort
  puts(keys[0])                           // => and
}
```

- **Iteration order is unspecified.** It is stable for one program but arbitrary: do not rely on it, and sort the keys when order matters. Insertion order is decided and not built.
- A key the block of `each` deletes before the walk reaches it is not visited.
- Three calls look a key up without building it first: `h.inc_slice(s, start, stop)` and `h.get_slice(s, start, stop)` use a slice of `s` as the key, and `h.get_concat(a, b)` uses `a + b`.

### Key and value types

A key is an integer of any width (`Int`, `Int32`, `UInt32`, `UInt64`), a `Bool`, an enum or a `String`. A value is any of those or a `Float64`. Keys and values are stored at their own width, not boxed: a `Hash(Int32, Int32)` holds 8 bytes an entry where a `Hash(Int, Int)` holds 16, so a narrow type is the cheaper table when its values fit.

```avant
enum Suit { clubs, hearts }

fn main {
  seen = Hash(Int32, Int32).new
  for x in [7, -2, 7, 7] {
    seen.inc(x.to_i32)
  }
  puts(seen[7.to_i32] ?? 0.to_i32)        // => 3
  names = Hash(Int, String).new
  names[404] = "not found"
  puts(names.get_or(500, "unknown"))      // => unknown
  score = Hash(Suit, Float64).new
  score[Suit.hearts] = 2.5
  puts(score[Suit.hearts] ?? 0.0)         // => 2.5
}
```

- A `Float64` key is a compile error: `==` on `Float64` follows IEEE 754 (`NaN` is not equal to itself, `-0.0` equals `0.0`), so no hash agrees with it. Key by an integer instead.
- Struct keys and class, array, hash or struct values are not built yet ([Not built yet](#not-built-yet)); a hash from the key to an index into an array does the same.

## Structs and classes

Avant has both kinds of user type, and you choose per type: a `struct` is a value, copied when assigned or passed; a `class` is a reference to one shared object.

```avant
struct Point {
  x: Int
  y: Int

  fn sum: Int {
    x + y
  }
}

class Counter {
  value: Int = 0

  fn inc: Int {
    self.value += 1
    value
  }
}

fn main {
  p = Point { x: 1, y: 2 }
  q = p                  // a copy
  q.x = 50
  puts(p.x)              // => 1
  puts(q.sum)            // => 52

  c = Counter.new
  d = c                  // the same object
  c.inc
  puts(d.value)          // => 1
}
```

### Struct or class

| | `struct` | `class` |
| --- | --- | --- |
| What it is | a value | a reference to an object on the heap |
| Built with | a literal, `Point { x: 1, y: 2 }` | `Counter.new(...)`, which runs `initialize` |
| Assigning or passing it | copies the fields | shares the object |
| Lives | inline in its holder: a local, a field, an array element | in its own allocation, managed by the collector |
| May contain itself | no (it would never end) | yes: `next: Node?` |
| Reach for it when | the thing *is* its data: a point, a pair, a token, a parsed row | the thing has identity or shared, changing state: a connection, a tree node, a cache |

When in doubt, start with a struct: it allocates nothing and cannot be changed behind your back.

### Structs

A struct literal names every field, in any order, except the fields that declare a default, which it may leave out (below). A copy is independent of the original.

A method may change its struct receiver, and the change is the caller's: no `mutating` mark is needed. A struct passed as an argument is a copy, so a change made through a parameter stays in the callee. The same line runs through arrays: `xs[i].x = v` changes the element in place, while `q = xs[i]` gives a copy, and a change to `q` does not reach the array. `for p in xs` gives a copy too, and assigning to `p` or to a field of it is a compile error ([For](#for)).

```avant
struct Vec {
  x: Int
  y: Int

  fn scale(k: Int) {
    self.x = x * k
    self.y = y * k
  }
}

fn scaled_sum(v: Vec): Int {
  v.scale(100)             // changes this function's copy
  v.x + v.y
}

fn main {
  a = Vec { x: 1, y: 2 }
  a.scale(4)               // changes a
  puts(a.x)                // => 4
  puts(scaled_sum(a))      // => 1200
  puts(a.x)                // => 4
}
```

A field may declare a default, any expression that does not use `self` or another field, as a class field's may. A literal that leaves the field out gets the default, evaluated at that literal, after the fields it names; a literal leaves out only fields that declare one. A struct with such a field has no zero value of its own (its zero would not be what a literal gives it): a class field of its type has a default or is set in `initialize`, and `Array(T).new(n)` takes a value.

```avant
struct Opts {
  port: Int
  loops: Int = 4
  name: String = "srv"
}

fn main {
  a = Opts { port: 80 }
  b = Opts { port: 81, name: "api" }
  puts("${a.port} ${a.loops} ${a.name}")   // => 80 4 srv
  puts("${b.port} ${b.loops} ${b.name}")   // => 81 4 api
}
```

### Classes

`Type.new(...)` allocates the object and calls `initialize` with the arguments. A class whose fields all have a starting value needs no `initialize` at all.

```avant
class Account {
  owner: String
  balance: Int = 0
  history: Array(Int) = []

  fn initialize(owner: String) {
    self.owner = owner
  }

  fn deposit(amount: Int) {
    self.balance += amount
    history.push(amount)
  }
}

fn main {
  a = Account.new("ada")
  a.deposit(40)
  a.deposit(2)
  puts("${a.owner}: ${a.balance} in ${a.history.size} deposits")   // => ada: 42 in 2 deposits
}
```

A parameter of `initialize` written `self.name` is a **field parameter**: it takes the field's type and is assigned to the field before the body runs, in parameter order, so `fn initialize(self.owner) { }` is `fn initialize(owner: String) { self.owner = owner }`. It may have a default like any parameter (`self.limit = 10`), and a body may follow for the rest. Only a class's `initialize` has field parameters.

```avant
class Span {
  start: Int
  stop: Int
  label: String = ""

  fn initialize(self.start, self.stop, self.label = "span") {
  }

  fn size: Int {
    stop - start
  }
}

fn main {
  s = Span.new(2, 9)
  t = Span.new(0, 1, label: "t")
  puts("${s.label} ${s.size} ${t.label}")   // => span 7 t
}
```

`Type { fields }` builds a struct only; a class is always built with `.new`.

### Field initialization

This section is about classes: a struct has no `initialize`, and its literal names every field that has no default ([Structs](#structs)). No class field is ever read unset. For each field, in order:

1. **A default**, if the field has one: any expression that does not use `self` (`xs: Array(Int) = []`, `seen: Hash(String, Int) = Hash(String, Int).new`, `at: Span = Span.new(0, 0)`). Defaults are evaluated at each construction, in declaration order, before `initialize`. A default cannot read another field; compute such a value in `initialize`.
2. **The type's zero**, if it has one and there is no default: `0`, `0.0`, `false`, `""`, `nil` for any `T?`, a null `Ptr`, a struct of such fields.
3. **`initialize`**, for every other field (a class, `Array`, `Hash`, `Buf`, `Bytes`, a `fn`, a union without `nil`, a struct holding one): it must set the field on every path, before reading it and before `self` is used as a value or a method of `self` is called.

A class that breaks rule 3 is refused with an error that names the field and the ways out: set it, give it a default, or make it optional (`left: Node?`, which is one pointer; a function as `cb: (fn(Int): Int)?`).

### Methods and self

A method is a `fn` inside the type's body. Inside it, `self` is the receiver.

- A field is **read** bare (`x`) or as `self.x`, and **written** only as `self.x = ...`. A bare `x = ...` where `x` is a field is a compile error that names both ways out (write `self.x =`, or give the local another name), so a write to the receiver is always visible on its line.
- A method of the same type is called bare (`sum`, `sum()`) or as `self.sum`.
- A bare name means, in order: a local or parameter, a field of the receiver, a method of the receiver, then a function. Nothing is chosen silently when two of those fit:
  - a bare call that fits both a method and a function is an error that asks for `self.name(...)` or the function's qualified name;
  - a module name used as a qualifier (`util.twice(3)`) that is also a method of the receiver is an error that asks for `self.util` or another name for the method.
- A method without arguments is called with or without parentheses: `q.sum` and `q.sum()` are the same call.
- Methods, like fields, are private to their module unless marked `pub` ([Visibility](#visibility)).
- Methods are not values: `box.f()` is always a method call. To call a field that holds a function, parenthesize it: `(box.f)()`.

### External methods

A method can also be declared outside the type's body, as a function with a receiver parameter. Inside it the receiver is implicit, as in a method of the type's body: a field is read bare and written through the receiver's name (`p.x = ...`), a method of the type is called bare, and a bare call that fits both a method and a function is an error asking for `p.name(...)` or the function's qualified name. A lambda inside an external method cannot capture its receiver (copy it into a local first). The receiver of a method of a builtin type (`String`, `Bytes`, `Buf`, an integer type, `Float64`, `Bool`) is always named.

<!-- check: run pre=point_decl -->
```avant
fn (p: Point) scaled(s: Int): Point {
  Point { x: x * s, y: y * s }
}

fn main {
  p = Point { x: 3, y: 4 }
  puts(p.scaled(2).x)   // => 6
}
```

<!-- check: hidden id=point_decl
struct Point {
  x: Int
  y: Int
}
-->

This is how a module adds methods to a type it does not own: an enum, and the builtin `String`, `Bytes`, `Buf`, `Int`, `Int32`, `UInt32`, `UInt64`, `Float64` and `Bool` included (the standard library's `bytes.compress` is one; `fn (n: Int) clamp(lo: Int, hi: Int): Int` is called `n.clamp(0, 10)`). `Array` and `Hash` take none yet.

- A builtin type keeps its own methods and fields: `fn (s: String) to_s`, `fn (b: Bytes) size` or `fn (n: Int) abs` is refused where it is declared, so a builtin is never replaced and a method is never silently left uncalled.
- An external method of a builtin type is seen like a function: in its own module and, when `pub`, in the modules that import it. A `pub` external method of a type declared in the program is part of that type, seen wherever the type is.
- Two modules may each add a method of one name to a builtin type. Each module's calls reach the one it sees; a call that sees both (two imports, or an import and the module's own) is an error that names the two modules, since a method call cannot name its module: call it from a module that imports only one of them. On a type declared in the program, two modules' methods of one name and parameter types meet wherever the type is, so the second is refused where it is declared, naming the first's module, unless both are private.

### Type-level functions

`fn self.name(...)` in a type's body declares a function called on the type itself, the usual home of named constructors.

```avant
struct Money {
  cents: Int

  fn self.zero: Money {
    Money { cents: 0 }
  }

  fn self.of(units: Int): Money {
    Money { cents: units * 100 }
  }
}

fn main {
  puts(Money.zero.cents)     // => 0
  puts(Money.of(3).cents)    // => 300
}
```

`Json.parse` and `Regex.compile` in the standard library are type-level functions.

### Overloads and defaults

Two functions or methods may share a name when their arity or argument types differ; the result type alone does not overload. Defaults are trailing, and the call site fills them in.

```avant
fn show(n: Int): String {
  "int ${n}"
}

fn show(s: String): String {
  "string ${s}"
}

fn pad(s: String, width: Int = 8, fill: String = "."): String {
  out = s
  while out.size < width {
    out = out + fill
  }
  out
}

fn main {
  puts(show(7))            // => int 7
  puts(show("x"))          // => string x
  puts(pad("ab"))          // => ab......
  puts(pad("ab", 4))       // => ab..
  puts(pad("ab", 4, "*"))  // => ab**
  puts(pad("ab", fill: "-"))            // => ab------
  puts(pad("ab", fill: "+", width: 3))  // => ab+
}
```

- **Named arguments:** a parameter with a default may be given by its name, after the positional arguments, in any order (`pad("ab", fill: "-")`). The positional arguments choose the overload, and the names fill its defaults; a name is given only to a parameter with a default (`pad(s: "ab")` is refused: `s` is passed by position), once, and not to one the positional arguments already reach. The arguments are evaluated as written, left to right, then the defaults of the parameters left out. A default's parameter name is part of the function's public surface. Builtins and function values take their arguments by position.
- A default is evaluated at each call that leaves its argument out. It belongs to the function's own module: it may call that module's private functions, and a name in it never means something of the caller's (a function, a method or a local of the same name), however the call is written.
- A default may name an earlier parameter (`fn span(a: Int, b: Int = a + 1)`). It reads that parameter's value, with its declared type: the argument is evaluated once, so `span(next())` calls `next` once.
- A method whose parameters all have defaults may be called without parentheses, like a method without parameters: `conn.recv` is `conn.recv()`.

### Operator methods

An operator is a method whose name is the operator, declared like any other. The operators that can be defined are `+`, `-`, `*`, `/`, `%`, `&+`, `&-`, `&*`, the bitwise `&`, `|`, `^`, `<<`, `>>`, the comparisons `==`, `<`, `<=`, `>` and `>=`, negation (`fn -` with no parameter, for `-x`), and indexing: `fn []`, which takes the index, reads `c[i]`, and `fn []=`, which takes the index and then the value, writes `c[i] = v`, with one index or several (`g[x, y]`). Each binary operator takes one parameter, the right operand. `!`, `&&`, `||` and `~` keep their one meaning and cannot be defined. `!=` is always `!(a == b)`, and a type that defines `<` gets `>`, `<=` and `>=` from it ([Equality and order](#equality-and-order)); a struct without `fn ==` compares its fields.

A compound assignment works through the type's operator: `at += delta` is `at = at + delta`, the same call, and `g[i] += 1` reads with `[]` and writes with `[]=`. The place is read twice in that form, so it must be one whose reading has no side effect: a name, a field, or an element at a name or a literal (`xs[i] += d`, not `xs[next()] += d`).

```avant
struct Vec2 {
  x: Int
  y: Int

  fn +(other: Vec2): Vec2 {
    Vec2 { x: x + other.x, y: y + other.y }
  }

  fn ==(other: Vec2): Bool {
    x == other.x && y == other.y
  }
}

fn main {
  v = Vec2 { x: 1, y: 2 } + Vec2 { x: 10, y: 20 }
  puts(v.x)                           // => 11
  puts(v == Vec2 { x: 11, y: 22 })    // => true
  v += Vec2 { x: 1, y: 1 }
  puts(v.y)                           // => 23
}
```

```avant
class Grid {
  cells: Array(Int)
  width: Int

  fn initialize(width: Int, height: Int) {
    self.width = width
    self.cells = Array(Int).new(width * height)
  }

  fn [](x: Int, y: Int): Int {
    cells[y * width + x]
  }

  fn []=(x: Int, y: Int, v: Int) {
    self.cells[y * width + x] = v
  }
}

fn main {
  g = Grid.new(3, 2)
  g[2, 1] = 7
  puts(g[2, 1])     // => 7
}
```

### No inheritance

There is no `class B < A`, no abstract method and no dynamic dispatch. Avant shares behavior in three other ways:

- **Composition**: a type holds another as a field and forwards what it needs.
- **[Interfaces](#interfaces) as bounds**: a generic function accepts any type that opted into an interface, and is compiled once per type, with no vtable.
- **Unions** for "one of several types", once their `switch` is built ([Unions](#unions)); today, a `kind` field.

What this buys: every call is direct, every type's layout is known, and reading a method tells you which code runs.

## Errors

A function that can fail says so in its result type: a union of its result and its failures. A failure is a value of a type marked `Error`, or `nil`. Nothing is thrown and nothing unwinds, so the type of a function tells you everything that can come out of it.

<!-- check: run id=errors_intro -->
```avant
class ParseError: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn parse_age(s: String): Int | ParseError {
  n = s.to_i ?? return ParseError.new("not a number: ${s}")
  if n < 0 || n > 150 {
    return ParseError.new("out of range: ${n}")
  }
  n
}

fn age_gap(a: String, b: String): Int | ParseError {
  x = parse_age(a)?            // a failure here returns from age_gap
  y = parse_age(b)?
  (x - y).abs
}

fn main {
  puts(age_gap("40", "31") ?? -1)       // => 9
  if gap = age_gap("40", "old") {
    puts(gap)
  } else {
    puts("could not compare")           // => could not compare
  }
}
```

One rule says what fails: **in any union, the failures are `Nil` and every member marked `Error`; the rest is the success value**. Everything below follows from it.

The `else` above does not see the error; an `else` that names it does ([Reading the error itself](#reading-the-error-itself)).

### Propagate, replace, branch

| Form | On success | On failure | Use it when |
| --- | --- | --- | --- |
| `x?` | gives the value | returns the failure from the current function | the caller should decide |
| `x ?? fallback` | gives the value | gives `fallback`, evaluated only then | a default makes sense |
| `x ?? return v` (also `?? return`, `?? break`, `?? continue`) | gives the value | leaves | the failure ends this function or iteration |
| `v = x else e { ...; continue }` | gives the value | runs the block with `e`, which must leave | the failure is reported, then ends this function or iteration |
| `if v = x { ... } else { ... }` | runs the block with `v` | runs the `else` (`else e` names the failure) | each outcome needs code |

These are the forms of [Opening an optional](#opening-an-optional): `T?` is the union whose only failure is `nil`.

- `x?` compiles only where the failure can be returned: the function's result must hold every failure of the value (`nil` included). Otherwise the compiler says what would be returned:

<!-- check: error pre=parse_error_decl -->
```avant
fn double(s: String): Int {
  parse_age(s)? * 2
}

fn main {
  puts(double("21"))
}
```

<!-- check: stderr -->
```text
error[E4000]: postfix ? on Int | ParseError would return ParseError, but the function returns Int; give it a result that includes ParseError, or handle the failure with if-assign or ??
```

- In a lambda, `?` returns from the lambda. A `spawn` or `parallel` block cannot return, so `?` is refused there. Inside `Mutex.with`, `?` returns from the function as it would outside the block, and the lock is released.
- A union with no failure member (`Int | String`) has nothing for these forms to open, and one whose every member is a failure (`IoError | ParseError`) has no value to give: both are compile errors.
- An optional error (`ParseError?`) is read like any `T?`: if-assign and `??` give the error when it is not `nil`. `?` is refused on it.

<!-- check: hidden id=parse_error_decl
class ParseError: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn parse_age(s: String): Int | ParseError {
  n = s.to_i ?? return ParseError.new("not a number: ${s}")
  if n < 0 || n > 150 {
    return ParseError.new("out of range: ${n}")
  }
  n
}
-->

### Nothing or a failure

A function that gives nothing when it succeeds, and an error when it fails, returns `Void | E` (Rust's `Result<(), E>`). The end of its body and a bare `return` are the success; `return e` is the failure. `?` passes the failure up, and `f() else e { ... }` reads it: the guard of an assignment, written without the assignment because there is no value to name.

<!-- check: run id=void_result -->
```avant
class SaveError: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn check_name(name: String): Void | SaveError {
  if name.size == 0 {
    return SaveError.new("an empty name")
  }
}

fn save(name: String, body: String): Void | SaveError {
  check_name(name)?            // a failure here returns from save
  if body.size > 10 {
    return SaveError.new("too long: ${body.size} bytes")
  }
  puts("saved ${name}")
}

fn main {
  save("notes", "hello") else e {
    puts(e.message)
    return
  }                              // => saved notes
  save("", "hi") else e {
    puts(e.message)              // => an empty name
    return
  }
}
```

- The result is an error a statement may not drop: `save("a", "b")` alone is a compile error; write `?`, `else e { ... }`, `?? return`, or `_ =` to drop it on purpose.
- There is no value to bind, so `if v = save(...)` and `v = save(...) else e` are refused; a `switch` reads the failure (`case e: SaveError { ... } else { ... }`).
- `Void` stands only beside failures marked `Error` (`Void | IoError`, `Void | A | B`): `Void | Int` and `Void | E?` are refused. A `Void | E` whose `E` is a class is one pointer, `Void` its null, as an optional class is.
- `dir_make` and `file_delete` return `Void | IoError`, and so does a [`Mutex.try_with`](#channel-and-mutex) whose block ends without a value.

### Declaring an error

An error type is a class or a struct marked `: Error`. It must make a message readable, as a `String` field named `message` or a method `fn message: String`. `e.message` reads it, and interpolating the error prints it.

```avant
struct TooLong: Error {
  limit: Int

  fn message: String {
    "longer than ${limit} bytes"
  }
}

fn check(s: String): String | TooLong {
  if s.size > 5 {
    return TooLong { limit: 5 }
  }
  s
}

fn main {
  puts(check("short") ?? "rejected")       // => short
  puts(check("too long") ?? "rejected")    // => rejected
  e = TooLong { limit: 5 }
  puts("failed: ${e}")                     // => failed: longer than 5 bytes
}
```

`Error` is a marker written where the type is declared. It is not a type you can store or a bound, and no type may be named `Error`. The library's errors are marked: `IoError`, `ProcessError`, `JsonError`, `RegexError`, `HttpError`, `ZlibError`, `Base64Error`.

### Causes

An error may carry the error that caused it: a field named `cause` holding an `Error` type, or an optional one. It is an ordinary field; interpolation prints it after the message, outermost first.

```avant
import std.json

class ConfigError: Error {
  message: String
  cause: JsonError?

  fn initialize(message: String, cause: JsonError?) {
    self.message = message
    self.cause = cause
  }
}

fn main {
  e = ConfigError.new("bad config", JsonError.new("unexpected end"))
  puts("${e}")       // => bad config: unexpected end
}
```

### An enum as an error

An enum marked `: Error` is a failure type whose failures are its members. Its message is the member's name, unless the enum defines `fn message: String`.

```avant
enum ParseError: Error { empty, not_a_number }

fn parse_digit(s: String): Int | ParseError {
  if s.size == 0 { return ParseError.empty }
  c = s[0]
  if c < 48 || c > 57 { return ParseError.not_a_number }
  c - 48
}

fn main {
  puts(parse_digit("7") ?? -1)            // => 7
  puts(parse_digit("x") ?? -1)            // => -1
  switch parse_digit("") {
    case d: Int { puts(d) }
    case e: ParseError {
      puts("failed: ${e}")                // => failed: empty
      switch e {
        case empty { puts("nothing to read") }        // => nothing to read
        case not_a_number { puts("not a digit") }
      }
    }
  }
}
```

It follows the rules of every error: `?`, `??`, if-assign and `else e` read it, a statement that drops it is refused, `_ =` discards it, and a `switch` over the result names it as a member.

### Errors cannot be dropped

A statement whose value can be an error is a compile error. Handle it, pass it up with `?`, or discard it on purpose with `_ =`.

<!-- check: error pre=parse_error_decl -->
```avant
fn main {
  parse_age("12")
}
```

<!-- check: stderr -->
```text
error[E4000]: this statement drops a value that can be an error (Int | ParseError); handle it (if-assign or ??), pass it up with ?, or discard it on purpose with _ = ...
```

<!-- check: run body pre=parse_error_decl -->
```avant
_ = parse_age("12")     // computed, then dropped: visibly
```

### Reading the error itself

`?`, `??` and a plain `else` tell you *that* something failed. To reach the error value, to print its message or read its fields, name it with `else e`. After an assignment, `else e { ... }` is a guard: its block runs on a failure with `e` holding it, and it must leave (`return`, `break`, `continue` or [`exit`](#output)), so after it the variable holds the success and the code goes on without nesting. After an if-assign, `else e` names the failure for the `else` block, which need not leave.

<!-- check: run pre=parse_error_decl -->
```avant
fn total_age(xs: Array(String)): Int {
  total = 0
  for s in xs {
    age = parse_age(s) else e {
      puts("skipped: ${e.message}")   // => skipped: not a number: old
      continue
    }
    total += age
  }
  total
}

fn main {
  puts(total_age(["40", "old", "31"]))  // => 71
  if age = parse_age("200") {
    puts("age ${age}")
  } else e {
    puts("giving up: ${e}")             // => giving up: out of range: 200
  }
}
```

- `e` has the failure's type: the one failure member, or the union of them when there are several (`Int | IoError | ParseError` gives `IoError | ParseError`), told apart with a `switch` inside the block.
- The guard reads a failure that carries something: on a value whose only failure is `nil` (`Int?`), `else e` is refused; leave with `?? return`, `?? break` or `?? continue`. A guard without a name is refused for the same reason.
- `else e` follows a plain assignment (`v = f()`, `self.n = f()`, or `_ = f()` to read only the failure), not `+=`; after an if-assign it needs the condition's one binding, not a list.
- To handle each outcome on its own, a success included, `switch` over the result: the failure is a member like the others.

<!-- check: run pre=parse_error_decl -->
```avant
fn main {
  switch parse_age("old") {
    case age: Int { puts("age ${age}") }
    case e: ParseError {
      puts(e.message)             // => not a number: old
    }
  }
}
```

Passing an error up with `?` and wrapping it with a [cause](#causes) need no helper.

### Traps

A failure the caller can act on is a value. A *bug* (an index out of range, an overflow, a misused API) is a trap: the program prints one line and stops with exit status 1. A trap cannot be caught.

<!-- check: trap -->
```avant
fn main {
  xs = [1, 2, 3]
  puts("before")
  i = 5
  puts(xs[i])
}
```

<!-- check: stderr -->
```text
avant runtime: index 5 out of bounds (size 3) in main (main.av:1:1)
```

The line is `avant runtime: WHAT` and where. Whatever the program printed before comes first: a trap flushes stdout before it writes to stderr, so a log with both streams reads in order. How precisely the line says where depends on the trap:

| Where the trap is | The line names | Example |
| --- | --- | --- |
| at one operation: `pop` on an empty array, the number conversions, shifts, `buf[i] = n`, `Array(T).new` with a negative size, `main`'s exit status, a C string result that is NULL | that operation's position | `avant runtime: pop on an empty Array at main.av:7:10` |
| an index out of bounds, an integer overflow, a division by zero | the function, and where it starts | `avant runtime: integer overflow in twice (main.av:9:1)` |
| inside a runtime function: a slice's bounds, `push_byte`, `chr`, `repeat_byte`, `sum`, a `Hash` counter, `fmt_float`'s precision, a parse base, `split` with an empty separator, `chars` on invalid UTF-8 | the operation and its values; no position yet | `avant runtime: slice 4..2 out of range (size 5)` |

The second row is deliberate: a position for each check would cost time in hot loops.

| What traps | The form that does not |
| --- | --- |
| `xs[i]`, `s[i]`, `buf[i]` outside the bounds | `.get(i)`, which returns `T?` |
| `xs.pop` on an empty array | test `xs.size` first |
| `+`, `-`, `*`, `abs`, `sum`, `inc` overflowing | `&+`, `&-`, `&*` to wrap |
| `/`, `%` by zero | test the divisor |
| `to_i`, `to_i32`, `to_u32`, `to_u64` out of range | `wrap_i`, `wrap_i32`, `wrap_u32`, `wrap_u64` |
| a shift amount outside the width | |
| a slice with `start > stop` or outside the value | |
| a byte outside `0..255` | `n & 255` |
| `chars` on invalid UTF-8 | `String.from_utf8`, which returns `String?` |
| growing past a [size limit](#size-limits) | |

**A trap ends the whole process**, a server included: one bad index in an `std.http` handler closes every connection. Run servers under a supervisor (systemd's `Restart=`, a container restart policy), and treat request data with the non-trapping forms: `.get(i)`, `??`, the optional parsers. A trap that ends only its task, and a pre-forked server mode, are decided and not built.

**A write to standard output that fails stops the program** as a trap does: `avant runtime: cannot write to standard output: No space left on device` (a full disk, `/dev/full`), exit status 1. Every `puts` checks at once. A closed pipe (`prog | head`) is the exception: the program dies by `SIGPIPE`, status 141 in a shell, as a C program does. That holds whatever the parent left: an ignored or blocked `SIGPIPE` (a CI runner, a shell's `trap '' PIPE`) is reset to the default before `main`.

### Nil, error or trap

| The situation | Use | The caller |
| --- | --- | --- |
| Absence is normal: a lookup, a parse, an empty collection | `T?` | must open it before using the value |
| The operation can fail, and the caller should know why | `T \| SomeError` | must handle it, pass it up, or discard it visibly |
| The program is wrong: a bug, not an input | a trap | cannot continue; fix the code |

The standard library follows the same line: `h[k]` and `"42".to_i` return optionals, `file_read` and `Json.parse` return errors, and `xs[9]` on a three-element array traps.

## Function values

Functions are values. A function value has the type `fn(T): U`; a lambda is written `{ |x| ... }`; a call that ends with a block passes it as the last argument.

```avant
fn apply(xs: Array(Int), f: fn(Int): Int): Array(Int) {
  out: Array(Int) = []
  for x in xs {
    out.push(f(x))
  }
  out
}

fn double(n: Int): Int {
  n * 2
}

fn main {
  a = apply([1, 2, 3], double)                 // a named function
  b = apply([1, 2, 3], { |n| n * n })          // a lambda
  c = apply([1, 2, 3]) { |n| n + 100 }         // a trailing block
  puts(a[2])    // => 6
  puts(b[2])    // => 9
  puts(c[2])    // => 103
}
```

- `fn(): Int` takes nothing; `fn(Int, String): Bool` takes two arguments; `fn(Int)` returns nothing.
- A function's name followed by a block is a call with that block as its only argument, bare or qualified: `twice { |n| n + 1 }` is `twice() { |n| n + 1 }`, as `util.twice { |n| n + 1 }` is. The block starts on the line where the call ends: a lambda on the next line is a value of its own. In the head of an `if`, `while` or `switch` a block after the condition is taken as the call's only when it starts with its parameters (`{ |n|`).
- A bare function name in a value position is that function (`f: fn(Int): Int = double`). A function of another module is named as at a call, qualified: `f = util.double`. When a name means several functions (overloads, or two imported modules' functions of one name), the expected `fn` type picks one, or the module's name does.
- The type is invariant: `fn(Int): Int` is not `fn(Int): Int?`.
- An optional function value takes grouping parentheses: `(fn(Int): Int)?` is a function or `nil`, where `fn(Int): Int?` returns an `Int?`. It is opened like any optional (`if f = cb { f(1) }`, `cb ?? double`), and it is the function value itself, `nil` a null code word: no tag and no more room than a function value.
- There is no `==` on function values, and methods are not values.

### Lambdas

`{ |x| x + 1 }` is a lambda; its last expression is its result, a final `if`/`else` whose branches end in values included, whether the result type is expected or inferred (`apply(2) { |x| if x > 1 { x * 10 } else { 0 } }`); branches that end in statements keep the `if` a statement. A parameter's type may be written inside the pipes (`{ |a: Int, b: Int| a * 10 + b }`) and is optional when the expected `fn` type already gives it; a lambda whose parameter type is still unknown is an error.

A lambda with no parameters is `{ || 1 }`.

```avant
fn main {
  add: fn(Int, Int): Int = { |a, b| a + b }
  typed = { |a: Int, b: Int| a * 10 + b }
  seven = { || 7 }
  puts(add(1, 2))       // => 3
  puts(typed(4, 2))     // => 42
  puts(seven())         // => 7
}
```

### Trailing blocks

A block after a call fills the last parameter when that parameter is a `fn` and the written arguments are one short. This is how you write your own `map`:

```avant
fn map(xs: Array(T), f: fn(T): U): Array(U) {
  ys: Array(U) = []
  xs.each { |x| ys.push(f(x)) }
  ys
}

fn main {
  lengths = map(["a", "bb", "ccc"]) { |s| s.size }
  puts(lengths[2])    // => 3
}
```

### Capture

A lambda captures the variables it uses by reference: the variable lives in a cell both sides share, so an assignment on either side is visible to the other.

```avant
fn counter(start: Int): fn(): Int {
  n = start
  { ||
    n += 1
    n
  }
}

fn main {
  next = counter(10)
  puts(next())     // => 11
  puts(next())     // => 12

  total = 0
  add = { |k: Int| total += k }
  add(5)
  add(6)
  puts(total)      // => 11
}
```

- A lambda created in a `for` loop sees the loop's one variable, not a copy per iteration: three lambdas made in `for i in 0..3` all read 3 afterwards. Copy the variable into a new name inside the body to keep the value (`k = i`, then capture `k`); a name first assigned inside a loop body is new at each iteration. A per-iteration loop variable is decided and not built.
- A lambda inside a method may use `self`, and so its fields and methods bare. The receiver is captured by value, because `self` is never reassigned: a class receiver is its reference (the lambda sees later changes to the object); a struct receiver is a copy, and a write through `self` inside such a lambda is an error, since it would change only the copy.

```avant
class Scale {
  k: Int = 2

  fn by: fn(Int): Int {
    { |n| n * k }
  }
}

fn main {
  s = Scale.new
  f = s.by
  s.k = 10
  puts(f(4))    // => 40
}
```

### Builtin blocks are loops

The blocks of the builtins (`each`, `any`, `all`, `sort`, `sorted`, `parallel`, `spawn`, `Mutex.with`, `Array(T).new(n) { ... }`) are not function values: the compiler inlines them. The difference shows in what `return` and `break` mean.

| Inside | `return` | `break` |
| --- | --- | --- |
| a builtin's block (`xs.each { ... }`) | leaves the enclosing function | leaves the loop |
| a lambda, a trailing block of your own function | returns from the lambda | a compile error |

```avant
fn first_big(xs: Array(Int)): Int {
  found = -1
  xs.each { |x|
    if x > 10 {
      found = x
      break                  // leaves the each
    }
  }
  found
}

fn report(xs: Array(Int)) {
  xs.each { |x|
    if x < 0 {
      puts("negative: ${x}")
      return                 // leaves report
    }
  }
  puts("all good")
}

fn main {
  puts(first_big([3, 12, 40]))   // => 12
  report([1, -2, 3])             // => negative: -2
  report([1, 2])                 // => all good
}
```

`return x` inside a builtin's block leaves the enclosing function with `x`, checked against that function's result type, as it would be outside the block. A block whose value is used (`any`, `all`, `sort`, `Array(T).new(n) { ... }`) may end with an `if` with an `else`, whose branches give the value:

```avant
fn main {
  xs = [3, 1, 2]
  xs.sort { |a, b|
    if a % 2 == b % 2 {
      a < b
    } else {
      a % 2 == 1             // odd numbers first
    }
  }
  puts("${xs[0]} ${xs[1]} ${xs[2]}")   // => 1 3 2
}
```

## Generics

A function or a type can take type parameters. Each use with concrete types is compiled as its own specialized code: no boxing, no dispatch at run time.

```avant
struct Pair(A, B) {
  first: A
  second: B
}

fn swap(p: Pair(A, B)): Pair(B, A) {
  Pair { first: p.second, second: p.first }
}

fn main {
  p = Pair { first: 1, second: "one" }     // Pair(Int, String)
  q = swap(p)
  puts(q.first)     // => one
  puts(q.second)    // => 1
}
```

### Generic functions

In a signature, a single capital letter that names no type (`T`, `U`, `K`) is a type parameter; nothing declares it. The call site omits it when the arguments determine it.

```avant
fn first_or(xs: Array(T), fallback: T): T {
  xs.get(0) ?? fallback
}

fn main {
  empty: Array(String) = []
  puts(first_or([7, 8], 0))         // => 7
  puts(first_or(empty, "none"))     // => none
}
```

Any other unknown name is an error, so a misspelt type is caught where it is written: `fn f(x: Strng)` reports ``unknown type Strng (did you mean `String`?)``.

A type parameter may be a member of a union: `fn first_ok(xs: Array(T | ParseError)): T?` takes an `Array(Int | ParseError)` with `T` as `Int`, and `T?` takes an `Int?`. The one parameter left unbound takes the argument's other members. Two of them in one union (`r: T | E`) split `Int | ParseError` in two ways; a bound (`r: T | E: Error`: `E` takes the members marked `Error`, [Bounds](#bounds)), another argument (`fn or_else(r: T | E, d: T): T`) or a declared result (`e: ParseError? = failure(r)`) fixes one, and otherwise the call says it cannot infer them.

### Generic types

A struct or a class names its parameters in its declaration, where any fresh name is allowed (`class Entry(Key, Val)`). Each use is its own type with its own layout: `Box(Int)` and `Box(String)` are different types, and an unapplied `Box` is an error.

```avant
class Box(T) {
  value: T

  fn initialize(value: T) {
    self.value = value
  }
}

class Link(T) {
  value: T
  next: Link(T)?

  fn initialize(value: T) {
    self.value = value
  }
}

fn main {
  a = Box(Int).new(1)       // written out
  b = Box.new("text")       // Box(String), inferred from initialize's argument
  puts(a.value)             // => 1
  puts(b.value)             // => text

  head = Link.new(1)
  head.next = Link.new(2)
  if n = head.next {
    puts(n.value)           // => 2
  }
}
```

- A struct literal infers its arguments the same way: `Pair { first: 1, second: "x" }` is `Pair(Int, String)`.
- A parameter may take a [bound](#bounds), as a function's does: `class Top(T: Ranked)` can only be used with a type that opted into `Ranked`, and `Top(Int)` is an error where it is written, not in a method far away.
- A generic class may name itself (`next: Link(T)?`): a class is a pointer. A struct is copied inline, so a struct that contains itself, generic or not, is an error.
- `Array`, `Hash`, `Ptr` and `JoinHandle` are built in; they are not generic types you could declare.
- A generic type or function imported from another module takes the types of the module that uses it, `pub` or private: with `Box` imported, `Box(Point)` and `Box.new(Point { ... })` work for a `Point` declared here. A type argument is a name like any other where it is written: a private type of another module is unknown there, also as an element type (`Array(Hidden)`) or inside a union or a function type.

## Interfaces

An `interface` is a set of method signatures, without bodies, that a type opts into by name. A generic function can then require it. An interface is a compile-time contract, not a value: there is no `Array(Show)`, no vtable, no dynamic dispatch.

```avant
interface Show {
  fn to_s: String
}

struct Vec2: Show {
  x: Int
  y: Int

  fn to_s: String {
    "(${x}, ${y})"
  }
}

struct Celsius: Show {
  degrees: Int

  fn to_s: String {
    "${degrees} C"
  }
}

fn announce(v: T: Show): String {
  "value: ${v.to_s}"
}

fn main {
  puts(announce(Vec2 { x: 1, y: 2 }))      // => value: (1, 2)
  puts(announce(Celsius { degrees: 21 }))  // => value: 21 C
}
```

### Opting in

A struct, a class, or a `Ptr` to a C layout opts in by name; conformance is never inferred from the methods a type happens to have.

- On the declaration: `struct Vec2: Show`. Several interfaces join with `+`: `struct Vec2: Show + Eq(Vec2)`.
- Later, as a top-level line: `Vec2: Show`, or `Box(Int): Show` for one instance of a generic type. `pub` on a later opt-in says who else can see that fact.
- The type's methods must match the interface's signatures exactly: `Int` does not stand for `Int?`, and a default on a method does not make a shorter signature count.
- Builtin types do not opt in: `Array` has no `Show` clause. `Ptr(Point)` can when `Point` is a [C layout](#layouts-and-ptr).

### Bounds

`T: Show` in a signature is a bound: the call compiles only for a type that opted in. The compiler copies the function for that concrete type and emits an ordinary method call. Two bounds are written `T: Show + Eq(T)`; an interface may take type parameters (`interface Eq(T)`).

- The bound is written where the parameter is: after a parameter's type (`v: T: Show`), inside another type (`xs: Array(T: Ranked)`), or on a generic type's parameters (`class Top(T: Ranked)`). After a union it bounds the last member: `r: T | E: Error`.
- `Error` is a bound too: `E: Error` takes a type marked `Error`. In a union it settles the split: `fn failure(r: T | E: Error): E?` called with an `Int | ParseError` takes `E` as `ParseError` and `T` as `Int`, with no declared result.

<!-- check: run -->
```avant
interface Eq(T) {
  fn ==(other: T): Bool
}

struct Id: Eq(Id) {
  n: Int

  fn ==(other: Id): Bool {
    n == other.n
  }
}

fn count_of(xs: Array(T), wanted: T: Eq(T)): Int {
  total = 0
  for x in xs {
    if x == wanted { total += 1 }
  }
  total
}

fn main {
  ids = [Id { n: 1 }, Id { n: 2 }, Id { n: 1 }]
  puts(count_of(ids, Id { n: 1 }))    // => 2
}
```

## Modules

A file is a module. `import name` loads `name.av` and brings that module's `pub` names into scope. Everything else in a file is private to it.

<!-- check: file project=mods file=vec.av -->
```avant
// vec.av
pub struct Vec2 {
  pub x: Int
  pub y: Int

  pub fn length2: Int {
    x * x + y * y
  }
}

pub fn origin: Vec2 {
  Vec2 { x: 0, y: 0 }
}

fn helper: Int {       // private: only vec.av can call it
  1
}
```

<!-- check: run project=mods -->
```avant
// main.av
import vec

fn main {
  v = Vec2 { x: 3, y: 4 }
  puts(v.length2)          // => 25
  puts(origin().x)         // => 0
  puts(vec.origin().y)     // => 0
}
```

The compiler takes one root file (`avant run main.av`) and follows its imports; everything compiles as one program. There is no `module` line, no header, no build file.

### Import paths

A module's name is its path from the directory that owns it, with slashes written as dots: `import http.server` loads `http/server.av`. A directory is not a module, and a keyword after a dot is just a name (`import http.spawn` loads `http/spawn.av`).

The compiler looks for the file, in order:

1. beside the importing file;
2. in the directory of the file being compiled;
3. under the nearest `avant.toml` above that file (so a file in `tests/math/` can import the project's modules);
4. in each fetched dependency ([Packages](#packages)).

The same path found in two of those places is one file; two *different* files for one import are an error, and so is an import cycle, across directories included. The first segment `std` is reserved for the [standard library](#standard-library): `import std.json` loads the bundled module, and a project file cannot take that name.

### Visibility

Names are private to their module unless marked `pub`: functions, types, constants, **methods and fields** alike.

- Outside its module a private field cannot be read, written, or named in a literal. A type with private fields is therefore built by a function of its own module, which keeps its invariants; a read-only view is a `pub fn`, which reads like a field (`account.balance`).
- A test or another module sees `pub` names only.
- `fn main` is the entry point and needs no `pub`.
- The fields of a C layout are C's, and stay visible wherever the layout is. A [field walk](#the-field-walk) written in another module than the type's sees only its `pub` fields.

### Name clashes

Names belong to their module, so two modules may publish the same name. Only a *use* that could mean either is an error, reported at that use, and qualifying it with the module name says which.

<!-- check: file project=clash file=alpha.av -->
```avant
// alpha.av
pub fn value: Int {
  1
}
```

<!-- check: file project=clash file=beta.av -->
```avant
// beta.av
pub fn value: Int {
  2
}
```

<!-- check: run project=clash -->
```avant
// main.av
import alpha
import beta

fn main {
  puts(alpha.value() + beta.value())    // => 3
}
```

A bare use that could mean either is refused, and nothing wins silently, not even the module's own name over an imported one:

- **The same function in two modules.** A bare `value()` reports `ambiguous call to value: alpha.value and beta.value both match; write alpha.value() or beta.value()`.
- **A type in one module, a function of the same name in another.** A bare `Value` where a type is expected reports `Value is a type of alpha and a function of beta: write alpha.Value or beta.Value`.
- **The same type in two modules.** A bare `Token` reports `Token is a type of both alpha and beta: write alpha.Token or beta.Token`. A module that sees only one of them (it imports one) names it bare. Generic types follow the same rule: two modules may each declare `Cell(T)`, with methods of the same names, and `alpha.Cell(Int)` names one of them.
- **The same constant in two modules.** A bare `width` reports `width is a constant of both alpha and beta: write alpha.width or beta.width`.

Three more rules:

- Functions with different parameter types still meet as one overload set across modules: `show(7)` and `show("ab")` pick `show_int.show` and `show_str.show` by their arguments.
- A qualified call reaches only that module's `pub` functions. A module may qualify its own names (`main.helper()`).
- A nested module is qualified by its full name: `http.server.Server`.

## Packages

A package is a git repository of `.av` files. A project lists its dependencies in `avant.toml`, and `avant fetch` downloads them into `deps/`. There is no central registry: a dependency is its git URL, and `avant.lock` records the exact commit.

<!-- check: file project=pkg file=avant.toml -->
```toml
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
greeter = { git = "https://github.com/example/greeter.git", rev = "v1.0.0" }
```

<!-- check: hidden project=pkg file=deps/greeter/avant.toml
[package]
name = "greeter"
version = "1.0.0"
-->

```sh
avant fetch          # clone the dependencies into deps/, write avant.lock
avant run main.av    # no extra flags: imports find deps/ on their own
```

<!-- check: file project=pkg file=deps/greeter/greeter.av -->
```avant
// deps/greeter/greeter.av, fetched
pub fn greeting: String {
  "hello from greeter"
}
```

<!-- check: run project=pkg -->
```avant
// main.av
import greeter

fn main {
  puts(greeting())    // => hello from greeter
}
```

The manifest is tooling only. Compilation still starts at one root file and follows `import`, and a dependency is source that compiles together with your program.

### The manifest

`avant.toml` sits beside your root file. `[package]` names your project; `[dependencies]` names what to fetch. Each dependency gives a `git` URL (anything `git clone` can read, a local path included) and a `rev`: a full commit SHA, a tag, or a branch. Pinning a SHA or a tag is the reproducible choice. A dependency may also be its own section:

```toml
[dependencies.greeter]
git = "https://github.com/example/greeter.git"
rev = "v1.0.0"
```

A dependency with no `git`, a duplicate dependency, or an unknown key is an error with the file and line named. So is a bare version string (`greeter = "1.0"`): there is no registry to resolve it against.

### Fetch and lock

`avant fetch` (or `avant fetch path/to/project`) clones each dependency into `deps/`, checks out the pinned commit, fetches the dependencies' own dependencies, and writes `avant.lock`. It prints one line per dependency:

```text
fetched greeter 58a9ab1a2db9803a031edf34adb9e16c74f9d126
```

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

```toml
# avant.lock - generated by avant fetch. Do not edit.
version = 1

[[package]]
name = "greeter"
source = "git+https://github.com/example/greeter.git#58a9ab1a2db9803a031edf34adb9e16c74f9d126"
```

- **Commit** `avant.toml` and `avant.lock`. **Do not commit** `deps/`: the lockfile already records exactly what belongs there, and a fresh clone is built with `avant fetch`, then `avant run main.av`.
- A plain `avant fetch` is reproducible: when the lockfile records a commit for a dependency, fetch checks out that commit again, even if the branch or tag has since moved.
- If any dependency cannot be cloned or checked out, fetch prints git's output, exits non-zero and writes no lockfile, so a failed fetch never leaves a half-recorded set of versions.
- `avant fetch` needs `git` (it looks in `/usr/bin`, `/usr/local/bin`, then `/bin`), at fetch time only. Compiling afterwards works offline.

### Importing a dependency

You import a module by its path from the dependency's repository root, not by the package's name: `greeter.av` at the root of the checkout is `import greeter`, and `http/server.av` in it is `import http.server`, with no package prefix.

A file inside a dependency uses its own checkout as its root: it may import its own modules and those of other fetched dependencies, and it never looks back into your program. Visibility is the same as for your own files. If a module cannot be found and the project has a manifest, the error says what to run:

```text
error[E5000]: cannot find greeter.av next to main.av or in fetched deps (run: avant fetch .)
```

### Updating and removing

| To | Do |
| --- | --- |
| move one package to a chosen version | change its `rev` in `avant.toml` to the new tag or commit, then `avant fetch` |
| move every branch and tag pin to its current commit | `avant fetch --update` (deletes the checkouts, clones again, rewrites the lockfile) |
| remove a package | delete its entry from `avant.toml`, delete `deps/NAME`, then `avant fetch` |
| patch a package | clone it, commit the change on a branch, set its `git` to the clone and its `rev` to the branch, then `avant fetch` (a checkout fetched from another source is cloned again) |

Commit the new `avant.lock` afterwards, so everyone fetches the same code. Removing the manifest entry alone is not enough: until its checkout is deleted, the compiler still finds its modules under `deps/`.

### Publishing a package

A package is an ordinary git repository with its modules laid out from the repository root (the root is what imports resolve against) and an `avant.toml` naming it. Its own dependencies are declared the same way and fetched for its consumers. Mark the public surface `pub`; everything else stays private to the package.

```toml
[package]
name = "greeter"
version = "1.0.0"
```

```sh
git init
git add -A
git commit -m "greeter 1.0.0"
git tag v1.0.0
git push origin main v1.0.0
```

Publishing is pushing to any git host. A new version is a new tag or commit; existing consumers keep the commit their lockfile records until they update.

### Package conflicts

Each clash is reported, never silently resolved.

- **Two places provide the same module file.** Importing that name is a compile error that lists the files (`error[E5000]: import shared is ambiguous: ./deps/a/shared.av ./deps/b/shared.av`). There is no priority order, between dependencies or between a dependency and your own tree: a `shared.av` beside your root file clashes with a fetched one the same way, so a local copy cannot shadow a dependency. Rename one module, or stop fetching one package; to change a dependency's code, patch a clone of it ([Updating and removing](#updating-and-removing)).
- **One dependency name reaches two commits**, for example your manifest pins one and a dependency of a dependency asks for another. Fetch stops: `avant fetch: dependency greeter resolves to two different commits (...)`. Align your manifest's pin and fetch again. One name is one version in a project.
- **Two modules publish the same name.** The ordinary [name clash](#name-clashes): qualify the use.

### Package limits

No registry, no version ranges or solving, no per-package build step, one version per dependency name.

## Concurrency

Avant runs tasks on a pool of OS threads. `spawn` starts one task; `.parallel` maps a block over an array; channels and mutexes carry what tasks share. The compiler checks what a task captures, so sharing a mutable value between tasks by accident is a compile error.

```avant
fn fib(n: Int): Int {
  if n < 2 { return n }
  fib(n - 1) + fib(n - 2)
}

fn main {
  h = spawn { fib(27) }                           // one task, on the pool
  fibs = [20, 21, 22, 23].parallel { |n| fib(n) } // one task per element
  puts(h.join ?? 0)  // => 196418
  puts(fibs[3])      // => 28657
}
```

### Spawn

`spawn { ... }` runs the block as a task and returns a `JoinHandle(Int)` at once; `.join` waits for the task and gives the block's value, an `Int` for now, or the `TaskError` of a trap that ended the task: its type is `Int | TaskError` ([When a task traps](#when-a-task-traps)), read with `??`, `?`, an if-assign or `else e` like any failure. Handles are ordinary values: keep them in an array to wait for many.

```avant
fn work(id: Int): Int {
  id * id
}

fn main {
  handles: Array(JoinHandle(Int)) = []
  for i in 1..5 {
    id = i
    handles.push(spawn { work(id) })
  }
  total = 0
  for h in handles {
    total += h.join ?? 0
  }
  puts(total)    // => 30
}
```

- A value the block captures is copied when the task is sent ([What may cross a task](#what-may-cross-a-task)); the block cannot assign a captured variable.
- A captured `Array` is shared for reading: the block may not change it, and the sender may not change it until the task ends. Written through the captured name, either change is a compile error. Through any other path to the same array (another name for it, a function it is passed to, a field that holds it) the change stops the program at run time: `avant runtime: push on an array a running task reads: …`, status 1, naming the operation (`push`, `pop`, `clear`, `fill`, `sort`, `reverse`, `reserve` or an element store). The array is frozen from the `spawn` until the task ends (not until the join, so a dropped handle does not keep it frozen), at every level of an array of arrays; `.parallel`'s input and the arrays its block captures are frozen while its bodies run. A program that never spawns or uses `.parallel` keeps its code; in one that does, an element store and `clear` cost one compare more, and the runtime's mutators test the array once a call (a `push` with room only when it must grow).
- Calling `.join` or `.parallel` from inside a pool worker is refused: at compile time when it is written in the block; at run time, reached through a call, it traps (`.parallel called from a pool worker: …`), which ends the task. A nested `spawn` from a worker is allowed.

### When a task traps

A trap inside a spawned block (an index out of bounds, an overflow, a division by zero, a `pop` on an empty array, a change to a frozen array, …) ends that task, not the program. The trap is printed to standard error when it happens, as a trap that ends the program is (`avant runtime: index 7 out of bounds …`), whether or not the task is ever joined. The task's stack is dropped where it stands (no unwinding: nothing needs to run, the collector frees what the task left), and its `join` gives a `TaskError` whose `what` is the trap's text and `site` its `FILE:LINE:COL` (empty when the trap names its function instead); `"${e}"` prints both. The program and the pool go on.

```avant
fn main {
  h = spawn {
    xs: Array(Int) = []
    xs.pop
  }
  v = h.join else e {
    puts(e.what)    // => pop on an empty Array
    return
  }
  puts(v)
}
```

What the task shared is left safe:

- A `Mutex` the task held in `with` when it trapped is **poisoned**: its value may be half-changed, so every later `with` on it stops with `Mutex.with on a poisoned mutex: …` (in a task, that ends the task in turn; on the main thread, the program). `m.poisoned` tells, and `m.try_with { |x| … }` is the form for code that recovers: it gives the block's value, or a `MutexError` without running the block, so its type is `T | MutexError` (`Void | MutexError` when the block ends without a value).
- Each `Channel` the block captured is closed, so a task waiting in `recv` or `send` on it is not left waiting for ever.
- Each `Array` the block captured is thawed, as at the task's normal end.

Some traps still end the program: one in a `.parallel` body (the caller waits for every body), one inside a C callback (the jump would skip C's frames), one on the main thread or an HTTP handler's thread, and the runtime's own stops (the heap limit, the collector's checks). Tasks already share no mutable objects (only immutable `String`s, frozen arrays, `Channel`s and `Mutex`es cross), as Erlang's processes share nothing; a trap that ends only its task is the task-level half of "let it crash".

### Parallel

`items.parallel { |item| ... }` runs the block once per element, on the pool, and returns an `Array` of the results **in input order**. The caller waits outside the pool.

```avant
fn main {
  words = ["a", "bb", "ccc"]
  suffix = "!"
  loud = words.parallel { |w| "${w}${suffix}" }
  puts(loud[2])      // => ccc!
}
```

The block takes one parameter. The statement form `parallel { }`, without a receiver, is not built.

### Channel and Mutex

`Channel(T)` passes values between tasks; `Mutex(T)` guards one shared value. Both are in the standard library's `std.sync`, for any type `T`.

```avant
import std.sync

fn main {
  hits = Mutex(Int).new(0)
  handles: Array(JoinHandle(Int)) = []
  for i in 0..4 {
    handles.push(spawn {
      for j in 0..1000 {
        hits.with { |n| n += 1 }
      }
      0                          // a spawn block ends with an Int, for now
    })
  }
  for h in handles {
    _ = h.join
  }
  puts(hits.with { |n| n })    // => 4000
}
```

- `Mutex(T).new(value)` holds `value`. `m.with { |x| ... }` holds the lock while the block runs; `x` names the guarded value, and assigning `x` changes it. Used as a value, the call gives the block's last expression; used as a statement, the block simply ends. `return`, `break`, `continue` and `?` leave the block as they leave any block, and the lock is released on every way out.
- **Nothing the lock guards leaves the block.** Inside it, the guarded value and every reference reached from it (a field, an element, a name bound to one) may only be a receiver (`b.items.push(x)`, `b.n += 1`) or an operand whose result is a plain value (`b.n`, `b.items.size`, `"${b}"`): never an argument, a stored value or a captured name, which would outlive the lock. Two stores keep it under the lock and are allowed: into the guarded value itself (`b.root = b.root.left`), and into a name the block declares (`node = b.head`, a `for` variable over `b.items`), which then follows the same rule. A plain value (a number, a `Bool`, a `String`, `Bytes`, a struct or an optional of those, a `Channel` or a `Mutex`) is a copy and goes anywhere; the block's value, when it is used, must be one. To take a copy out, fill an array declared before the block with plain values (`b.items.each { |it| sizes.push(it.n) }`).
- **What a `Mutex` may guard.** A method of the guarded class runs under the lock too, and the block may call it, or a method of any class the guarded value holds (`b.inner.add(c)`), with values from outside. So `Mutex(T)` is accepted only when no method of `T`, nor of any class or struct a `T` holds through its fields, elements and members, keeps a reference from outside or hands out its own: a parameter that can hold a reference is never stored (into a field, an element, a hash or an array by `push` or `fill`, also through a new object it is put in), never passed to a function or a method of another type, and never the method's result; `self` and what is reached from it are stored only into the object itself (`self.root = self.root.left`, `self.head = Node.new(v, self.head)`) and passed only to the methods of those same classes. A method may still read its parameters (`self.n == other.n`) and build new values. The error names the mutex, the class, the method and the store: `Mutex(Box) (at app.av:16:17): Box.add stores a parameter's reference (or a value reached from one) into an array (push); …`. `initialize` is not read: its object is new, and what is given to a constructor is checked where it is called.
- **Nothing from outside comes in either.** A value stored into the guarded value (`b.child = c`, `b.items.push(c)`) must be new (a constructor call or a literal, whose parts are new or plain), reached from the guarded value already, or plain: a reference the code outside keeps would reach it without the lock.
- **`Mutex(T).new(v)` consumes `v`.** When `T` can hold a reference (it is not plain), the value given to the mutex is its own from then on: give it a new value (`Mutex(Box).new(Box.new)`, `Mutex(Array(Int)).new([])`), or a local variable, whose name cannot be used after the call. Before the call that variable is used as the guarded value is inside `with` (a receiver or a plain operand: `b.n = 5`, `b.items.push(Item.new)`), every value it holds is new, and the call is not in a loop the variable was declared outside of; so no other reference to the value exists when the mutex takes it. A field, an element, a parameter or a call's result is refused as the argument: another reference may reach it. The rule follows the order of the source, not its paths: a use after the call in the other branch of an `if` is refused too.

A channel hands values from one task to another:

```avant
import std.sync

fn main {
  jobs = Channel(Int).new(4)          // buffered: four values may wait
  producer = spawn {
    for i in 1..6 {
      jobs.send(i * i)
    }
    jobs.close
    0
  }
  total = 0
  while v = jobs.recv {               // nil once the channel is closed and empty
    total += v
  }
  _ = producer.join
  puts(total)    // => 55
}
```

- `Channel(T).new` is a rendezvous channel (a send waits for its receive); `Channel(T).new(n)` buffers `n` values. `send(value)` sends and returns whether the value went into the channel, `recv` returns `T?` (`nil` once the channel is closed and empty, so `while v = ch.recv { ... }` drains it), `close` closes.
- A `send` on a closed channel returns `false` at once, buffered or not, full or not; a send waiting for room or for its receiver returns `false` when the channel closes, and its value is not received. The values sent before the close stay for the receivers to drain. A program that only closes after its last send can ignore the result (`ch.send(v)` as a statement).
- What a `send` carries crosses to another task, by the same rule as a `spawn` capture ([What may cross a task](#what-may-cross-a-task)): a class instance, a `Hash` or a struct holding one is a compile error. An `Array` is copied at the send, at every level of an array of arrays, so the receiver owns its own array and the sender may go on changing its one; its elements must cross in turn (`Array(Box)` is refused). `T` may be any type, so a channel can carry other channels (a reply channel per request) or mutexes.
- A `Channel` and a `Mutex` hold no memory outside the object: the program can make as many as it likes.

### Choosing a tool

| You have | Use | Why |
| --- | --- | --- |
| an array, and the same independent work for each element | `.parallel` | one line; results come back in order; nothing to join |
| a few different jobs to run beside the main one | `spawn` and `.join` | each task has its own code and its own result |
| a stream of values from producers to consumers | `Channel(T)` | hand-off without shared state; `recv` ends the loop when the channel closes |
| one value several tasks update (a counter, a table) | `Mutex(T)` | the value is read and changed under its lock |
| work too small to be worth a thread (a few microseconds per item) | nothing: a plain loop | a task costs a thread hop; measure first |

### What may cross a task

What a `spawn` or `.parallel` block captures is checked at compile time:

| A captured value that is | Crosses as |
| --- | --- |
| an integer, a `Bool`, a `Float64` | a copy |
| a struct whose every field may cross | a copy |
| a `String` | a shared pointer: it is immutable, and both tasks keep it alive |
| an `Array` whose elements may cross | shared for reading and frozen until the task ends ([Spawn](#spawn)); a `send` copies it |
| a `Channel` or a `Mutex` (`std.sync`) | shared: that is their purpose |
| a class instance, a `Hash`, a `fn` value, a `JoinHandle`, a struct holding one, or an `Array` of those | **refused**: it is not copied, and sharing it would be a race (an `Array(Box)` shared for reading still hands each task the same `Box` objects) |

A mutable object reaches another task only inside a `Mutex`, or as the results of `.parallel`.

### The thread pool

The pool runs at most one task per CPU the process may use at once (its affinity: all online processors, or those `taskset`, `numactl` or a container's cpuset leave it; a cgroup's CPU quota is not read), a number counted once when the pool starts. `--threads N` (or `--threads=N`) on **your program's** command line replaces that number; the runtime removes the flag before `argv()` sees it (it is to become `--avant-threads=N`, leaving `--threads` to programs: [Not built yet](#not-built-yet)). Tasks beyond it wait in a queue. `std.http`'s event loops take the same count ([HTTP server](#http-server)).

A task that waits (in a channel's `recv` or `send`, or for a `Mutex`) stops counting as running while it waits, so a queued task may start meanwhile: on a free pool thread, or on a new one when every thread holds a task, up to 1,024 threads (or `N`, if larger). So two consumers waiting in `recv` with `--threads 2` do not keep a producer spawned after them from starting. When the wait ends, the task goes on as soon as fewer than `N` tasks run. Past the thread limit, the runtime says so once on standard error (`avant runtime: tasks are queued while the pool's 1024 threads are all taken, …`) and the queued tasks wait for a thread to be free.

## Memory

Memory is managed for you: allocate with `Type.new`, literals and `push`, and nothing is freed by hand. No program needs a setting from this section. What is worth knowing is where the collector costs time today.

### The collector

The collector is a precise, generational Immix: new objects are allocated in a young generation that is collected by copying the survivors; what survives moves to a mature space of Immix blocks, collected less often and defragmented during full collections. "Precise" means the collector knows exactly which values are references, in every build mode, so nothing is kept alive by accident. Objects may move; the only code that can observe it is C ([Strings in C memory](#strings-in-c-memory)).

Structs live inline in their holder and optional references are plain pointers ([What an optional costs](#what-an-optional-costs)), so idiomatic code allocates less than its class-only equivalent would.

### What allocation costs today

Allocation-heavy code is where Avant is weakest today, and it is being worked on.

- **Many small objects in a tight loop.** Creating a class instance costs more instructions than it should (a function call, where it should be a few inlined instructions), so a loop that does little besides `T.new` runs slower than it will.
- **A large structure that all stays alive** (a big tree, a long list, a graph built once). While it grows, the collector copies its objects out of the young generation and traces them again: the build pays for the same objects more than once. [Collection hints](#collection-hints) exist for this shape.
- **A large live heap.** A full collection traces everything that is alive, so its cost grows with the live data, not with the garbage.

LangArena shows it: `Binarytrees::Arena` is at 1.42× the best time and 1.92× the best memory, and `Binarytrees::Obj` and `Calculator::Ast` post good times only because their source carries hints ([Reading these numbers](#reading-these-numbers)).

Until this improves:

- keep small values in structs: a struct allocates nothing;
- reuse a `Buf` or an array, with `clear`, instead of making one per iteration;
- use the `_into(buf)` calls of the standard library;
- hint where a structure is built and dropped whole.

### Collection hints

`std.gc` holds two hints for a program that knows something the collector cannot: it builds a large structure that stays alive while it is built, then drops it whole. That is the shape the collector handles worst today, so the hints are worth their two lines there.

```avant
import std.gc

class Tree {
  left: Tree?
  right: Tree?
}

fn build(depth: Int): Tree {
  t = Tree.new
  if depth > 0 {
    t.left = build(depth - 1)
    t.right = build(depth - 1)
  }
  t
}

fn count(t: Tree): Int {
  n = 1
  if l = t.left { n += count(l) }
  if r = t.right { n += count(r) }
  n
}

fn main {
  nodes = 0
  gc.hold {
    nodes = count(build(16))     // everything stays alive until counted: collecting meanwhile is wasted work
  }
  gc.collect()                   // and now it is all dead
  puts(nodes)                    // => 131071
}
```

- **`gc.hold { ... }`** is a statement: no collection that this thread would start runs while the block runs. Its allocations still count: a collection that falls due inside runs when the block ends, and a hold whose own allocations pass `AVANT_HEAP_MAX_MB` stops the program with `heap limit in a no-GC region (...)`. The hold ends on every way out of the block (its end, `return`, `break`, `continue`, a `?` that returns). Holds nest as one. Other threads still collect while one holds, so a hold never makes another thread wait.
- **`gc.collect()`** is a complete collection: when it returns, every object nothing reaches has been freed, whether it was allocated a moment ago or long ago. It costs a young collection when the collector can prove that no old object died since its last full collection (no reference to an old object was removed from an object or from a local, and nothing became old), and a full collection, which traces everything alive, otherwise. So it is cheap after work whose objects are short-lived and already dead, and costs as much as the live data after a long-lived structure was dropped or changed. Inside a hold it runs when the hold ends. With `AVANT_NURSERY=0` or with more than one thread it is always a full collection.
- Put the end of a hold where the structure is dead, and `gc.collect()` where it dies.
- `gc` names these two forms in a module that imports `std.gc`, unless a local or a field takes the name; without the import, `gc.hold` is an error. Under `AVANT_GC_STRESS=1` a hold is inactive, so a stress run checks the code inside it like any other.

There is no arena or bump allocator in the language: `T.new` is the one way to allocate.

### Collector settings

No setting is needed. Every one is an environment variable read once when the program starts; a switch is on for `1` and off for `0`, a size is a whole number, and any other value is reported on stderr and ignored.

| Variable | Default | Effect |
| --- | --- | --- |
| `AVANT_HEAP_MAX_MB` | `0` (no limit) | the heap limit in MiB; a program that passes it stops with a "heap limit" message |
| `AVANT_YOUNG_MB` | `0` (adaptive, 4 to 64 MiB) | a fixed budget for the young generation, in MiB |
| `AVANT_NURSERY` | `1` | `0` turns the young generation off: full, non-moving collections only |
| `AVANT_GC_DEFRAG` | `1` | `0` turns off defragmentation of the mature space |
| `AVANT_GC_PROOF` | `1` | `0` makes every complete collection (`gc.collect()`) a full one, even when the collector can prove that only young objects can have died |
| `AVANT_HEAP_RESERVE_GB` | `64` | the address space reserved for the heap, in GiB (halved automatically if the OS refuses) |
| `AVANT_GC_STATS` | `0` | `1` prints one line of collector statistics on stderr at exit |
| `AVANT_GC_VERIFY` | `0` | `1` runs the collector's self-checks (slow; for debugging the runtime) |
| `AVANT_GC_STRESS` | `0` | `1` collects before every allocation (very slow; for testing) |

The two a user is likely to meet are `AVANT_HEAP_MAX_MB`, to cap a service, and `AVANT_GC_STATS`, to see what the collector did.

## C interop

C is one block away. Declare the functions you need in a `lib` block and call them through its name; the standard library itself is built this way, on yyjson, PCRE2, zlib and uSockets.

```avant
lib c {
  fun abs(n: Int32): Int32
  fun getenv(name: String): String?
}

fn main {
  puts(c.abs(-5))                                    // => 5
  puts(c.getenv("AVANT_SURELY_NOT_SET") ?? "unset")  // => unset
}
```

### Declaring C functions

A `lib NAME { ... }` block holds `fun` declarations: `fun` for a C function, `fn` for an Avant one, so a C declaration is always visible as one. Calls go through the lib's name (`c.abs`). Hand-written and generated bindings ([avant bind](#avant-bind)) are the same grammar; there is no `extern`.

- A `String` argument is passed as a `char *`.
- A result declared `String?` is a `char *` that may be NULL: NULL reads as `nil`, and passing `nil` passes NULL. A result declared `String` is a promise that the string is there: a NULL traps, naming the function (`avant runtime: C function c.getenv returned NULL for String (declare String?)`).
- The only unions a `fun` signature may use are the optional references, each one pointer with `nil` as NULL: `String?`, `Bytes?`, and an array, a hash, a `Buf` or a class with `?`. Any other union (`Int?`, `Int | String`, `Ptr(T)?`) is a compile error there: C has one word in that place, and such a union is a tag and a payload.
- C++ is reached only through `extern "C"` functions.

### C types

Every C scalar type has an exact name at the boundary.

| C | In a `fun` or a layout | In Avant code it is |
| --- | --- | --- |
| `int`, `int32_t` | `Int32` | `Int32` |
| `unsigned int`, `uint32_t` | `UInt32` | `UInt32` |
| `long`, `long long`, `int64_t` | `Int` | `Int` |
| `unsigned long`, `uint64_t`, `size_t` | `UInt64` | `UInt64` |
| `double` | `Float64` | `Float64` |
| `bool` | `Bool` | `Bool` |
| `signed char`, `int8_t`; `short`, `int16_t` | `Int8`; `Int16` | `Int` |
| `unsigned char`, `uint8_t`; `unsigned short`, `uint16_t` | `UInt8`; `UInt16` | `Int` |
| `float` | `Float32` | `Float64` |
| `char *` | `String` or `String?` | the same |
| a pointer | `Ptr(Layout)` or `Ptr(Void)` | the same |

The five narrow names (`Int8`, `Int16`, `UInt8`, `UInt16`, `Float32`) are written only in a `fun` signature and a C layout's field. A value from C converts exactly. A value to C keeps its value or traps (`avant runtime: 256 out of range for the C type UInt8 (0..255)`); a `Float64` rounds to the nearest `Float32`.

<!-- check: fragment needs a C library of its own; the tested twin is tests/fixtures/ffi/cscalars.av -->
```avant
lib img {
  fun img_scale(n: UInt32, gain: Float32): UInt8
}
```

### Layouts and Ptr

A `struct` inside a `lib` block is a C layout: C's size, alignment and field offsets. It is not an Avant value and is never built with `{ }`; you read and write its fields through a `Ptr(T)`, and give it methods with a `Ptr(T)` receiver.

```avant
lib c {
  struct Pair {
    a: Int32
    b: Int32
  }
  fun malloc(size: Int): Ptr(Pair)
  fun free(p: Ptr(Pair))
}

fn (p: Ptr(Pair)) sum: Int32 {
  p.a + p.b
}

fn main {
  p = c.malloc(8)
  p.a = 40
  p.b = 2
  puts(p.sum)     // => 42
  c.free(p)
}
```

`Ptr(T)` is for a layout `T`, and `Ptr(Void)` for a pointer you only pass around. A `Ptr` of anything else (`Ptr(Int)`) has no fields to read and cannot be given methods. Memory you got from C is C's: the collector neither moves nor frees it. There is no `sizeof` yet: the size passed to `malloc` is yours to state.

A `Ptr` can be NULL (an unchecked `malloc`), and Avant has no test for that yet. A field read or written through a NULL `Ptr` is a segmentation fault, as in C: the program ends with no Avant message, and what it had printed without flushing is lost. Check in C before handing the pointer over. A `fun` signature cannot say `Ptr(T)?` to get a test: it is refused, because `Ptr(T)?` is not a nullable pointer today (it would be read as a tag and a pointer). A `String` field that C left NULL (or that `release` made null) reads as `""`.

### Strings in C memory

The collector moves objects, so a heap value handed to C must stay put while C can see it.

- **For a call's duration**, nothing is needed: arguments are safe while the C function runs.
- **For a bounded time longer than a call**, pin the value for exactly that span: `pin(value) { ... }`. It stays put for the block and is unpinned on every way out. Pins are counted, so nested pins compose.
- **Stored into a C field** (`h.name = s`), a `String` is pinned until you say C is done with that field, with `release`. Overwriting the field keeps the old value pinned too, because C may still hold it.

<!-- check: fragment needs C functions of its own; the tested twin is tests/fixtures/ffi/release.av -->
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

- `release(h.name)` gives back the pin of every value Avant stored into that field, the current one and every overwritten one. If the field still holds a value Avant stored, it becomes null, so C never sees a pointer the collector may free or move.
- `release(h.at)` does the same for every `String` field of a nested layout, and `release(h)` for every field of the struct `h` points to. Write `release(h)` before C frees the struct.
- Releasing twice, or releasing a field Avant never stored, does nothing. A value C stored itself is left in place. A pin taken by `pin(x) { }`, or by a store of the same value into another field, stays.
- `release` is a statement. It takes a C-layout field that holds a `String` (directly or in a nested layout) or a `Ptr` to such a layout; anything else is a compile error.
- **Forgetting a release costs memory, never correctness** (the value stays pinned for the rest of the process). **Releasing too early is a dangling C pointer**: the next collection may move or free the value, and C then reads memory that is no longer that string. Neither the compiler nor `AVANT_GC_VERIFY=1` can see C's reads.

### Callbacks

A `fun` parameter may be an Avant `fn` type: that declares a C callback. Pass a function name, or a lambda that captures nothing, and C calls it with exactly the declared arguments.

```avant
lib c {
  struct Two {
    a: Int32
    b: Int32
  }
  fun malloc(size: Int): Ptr(Two)
  fun free(p: Ptr(Two))
  fun qsort(base: Ptr(Two), count: Int, size: Int, compare: fn(Ptr(Two), Ptr(Two)): Int32)
}

fn ascending(x: Ptr(Two), y: Ptr(Two)): Int32 {
  x.a - y.a
}

fn main {
  two = c.malloc(8)          // to C, two Int32 side by side are an array of two ints
  two.a = 9
  two.b = 4
  c.qsort(two, 2, 4, ascending)
  puts(two.a)    // => 4
  puts(two.b)    // => 9
  c.free(two)
}
```

A capturing lambda, a `fn` value held in a variable, and a C function that returns a function pointer do not compile. A callback's own parameters and result use the everyday types (`Int32`, `Float64`, `Bool`, `Ptr`), not the five narrow names, for now.

### Linking C code

```sh
avant compile app.av --lib z            # link a system library: -lz
avant compile app.av --cc helper.c      # compile and link a C file (or link a .o)
avant run app.av --lib z
```

The C library and the math library need no flag. The C libraries the standard library stands on (yyjson, PCRE2, zlib, a Base64 codec, uSockets, picohttpparser) are compiled into the runtime that ships with `avant`: a program that uses them needs nothing installed.

### avant bind

`avant bind HEADER.h` reads a C header with libclang and prints a `lib` block for you to keep and edit. It builds nothing and writes nothing into your project. Given this header,

```c
int avant_add(int a, int b);
void *avant_sentinel(void);
int avant_is_null(void *p);
```

`avant bind add.h` prints:

<!-- check: compile decls -->
```avant
// generated by avant bind from add.h
lib add {
  fun avant_add(a: Int32, b: Int32): Int32
  fun avant_sentinel: Ptr(Void)
  fun avant_is_null(p: Ptr(Void)): Int32
}
```

- Each C scalar type maps to its exact name (`unsigned char` and `uint8_t` to `UInt8`, `short` to `Int16`, `int` to `Int32`, `unsigned int` to `UInt32`, `float` to `Float32`). A type with no Avant name (`long double`) fails the bind with a message.
- A `char *` result is bound as `String?`, since C returns NULL for "absent" or "failed"; a `char *` parameter is `String`.
- A named C enum of four bytes becomes an `enum` inside the `lib` block: the name in CamelCase without a trailing `_t` (`enum http_method` is `HttpMethod`, `color_t` is `Color`), the constants lower-cased with their values (`HTTP_GET = 0` is `http_get = 0`). Functions and C layout fields of that type use it. An enum declared in a `lib` block keeps C's `int` wherever it is stored, and every value C gives (a result, a field, a callback's argument) is checked: one that is no member traps, naming the enum and the number. An alias (a second constant with the same value) is dropped and named in a comment; an anonymous enum, a value outside `Int32`, a constant that is no Avant member name and two constants that are equal after lower-casing leave the enum out with a comment, and its functions take `Int32`.
- `--lib NAME` names the block.
- It needs libclang 20 on the machine (`sudo apt install libclang1-20`); a release ships the `avant-bind` helper beside `avant`.

## Macros

Avant's macros are deliberately small: `quote` pastes code, `#()` splices a value into it, and one compile-time loop walks a type's fields. There is no general compile-time interpreter.

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

fn main {
  p = Point { x: 3, y: 4 }
  puts(x(p))    // => 3
  puts(y(p))    // => 4
}
```

That pastes two functions, `fn x(p: Point): Int` and `fn y(p: Point): Int`.

### Quote and splice

`quote { ... }` pastes functions at the top level or in a type's body, and statements inside a function. `#(expr)` splices, and only inside a `quote`: a name, a type, or a string that becomes a name.

```avant
quote {
  fn #("tripled")(n: Int): Int {
    n * 3
  }
}

fn main {
  puts(tripled(7))    // => 21
}
```

Names a quote introduces are hygienic: they do not capture names at the paste site. A `quote` inside a `quote` is an error.

### The field walk

`comptime { for f in Type.fields { quote { ... } } }` runs its quote once per field, with `f.name` and `f.type` to splice. `Type` must be a concrete, non-generic name: `for f in Box.fields` on a generic type is an error. Written in another module than the type's, the walk sees only `pub` fields, the ones its generated code could name. It is the one compile-time loop; the run-time [`for`](#for) is a different statement.

## Builtins

The prelude is the set of names every program can use without an import. The types and their methods are covered above ([Strings](#strings), [Arrays](#arrays), [Hashes](#hashes), [Bytes and Buf](#bytes-and-buf)); what remains is a small set of free functions.

| Area | Names |
| --- | --- |
| Output and input | `puts`, `print`, `eputs`, `eprint`, `puts_u`, `exit`, `read_line` |
| Files and directories | `file_read`, `file_write`, `file_open`, `file_exists`, `file_size`, `file_delete`, `dir_list`, `dir_walk`, `dir_exists`, `dir_make` |
| Processes and environment | `argv`, `env_get`, `process_run`, `process_run_out`, `process_output` |
| Time | `now_ms`, `now_us` |
| Hashing and math | `sha256`, `sqrt`, `exp`, `fmt_float` |
| Strings and buffers | `str_slice`, `str_to_f_slice`, `chr`, `repeat_byte`, `bytes_to_str`, `buf_new` ([String operations](#string-operations)) |

Runnable programs that use them live in [examples/](examples): in the repository, and in the release archive beside `avant`.

### Output

| Call | Writes |
| --- | --- |
| `puts(x)` | `x` and a newline to stdout; `puts()` an empty line |
| `print(x)` | `x` alone to stdout |
| `eputs(x)` | `x` and a newline to stderr; `eputs()` an empty line |
| `eprint(x)` | `x` alone to stderr |
| `exit(status)` | nothing: it ends the program with that exit status |

```avant
enum Level { low, high }

fn main {
  print("level: ")
  puts(Level.high)                    // => level: high
  n = "12".to_i ?? 0
  puts(n * 2)                         // => 24
  eputs("this line goes to stderr")
}
```

- `x` is anything interpolation prints, and the writers print exactly what `"${x}"` prints ([Printing](#printing)): a number, a `Bool`, a `String` (every byte of it, a NUL included), a type with its own `to_s`, an enum, an error, a union of those. An optional is opened first: `puts(x)` of an `Int?` is a compile error that names the ways to open it.
- stdout is buffered and written in large pieces; stderr is not. A write to stderr first sends what stdout holds, so the two keep their order when they go to one place (a terminal, `2>&1`). A failed write to either stops the program with a message, status 1.
- `exit(status)` writes what stdout still holds, removes the program's temporary directory and ends the process, whichever task calls it. The status is an `Int` from 0 to 255; any other value traps at the call (`exit status 256 out of range (0..255)`). `exit` never returns, so a block that ends with it leaves, as one that ends with `return`: it may end an [`else e` guard](#reading-the-error-itself), a function with a result, or an arm of a `switch` that gives a value. Without it, the exit status is `main`'s result.
- `puts_u(n)` prints an integer as the unsigned number its bits spell.

### Input

`read_line()` reads one line of standard input: a `String` without its newline (a `\r` before the newline goes too), `nil` at the end of the input, or an `IoError` if the read fails. A last line without a newline is a line. `while line = read_line() { ... }` walks the lines and stops at the end or at an error; to tell the two apart, read the failure with a guard: it is `nil` at the end.

```avant
fn main {
  count = 0
  while true {
    line = read_line() else e {
      if err = e {
        eputs("cannot read: ${err}")
        exit(1)
      }
      break
    }
    count += line.size
  }
  puts(count)                         // => 0
}
```

Run with an empty standard input, as here, it prints 0. `read_line() ?? ""` reads one answer and takes the end of the input as an empty one. When stdout is a terminal, `read_line` first writes what stdout holds, so a prompt printed with `print` shows before the program waits. `file_read("/dev/stdin")` reads the whole of standard input at once.

### Files and directories

Every call returns its result or an error value that says what failed.

| Call | Result |
| --- | --- |
| `file_read(path)` | `String \| IoError`: the whole file |
| `file_write(path, body)` | `Int \| IoError`: the bytes written; the file is created or replaced |
| `file_open(path)` | `FileReader \| IoError`: the file, to read line by line |
| `file_size(path)` | `Int \| IoError`: its size in bytes |
| `file_delete(path)` | `Void \| IoError`: nothing when the file is removed (a directory is an error) |
| `file_exists(path)` | `Bool`: a file or a directory is there |
| `dir_list(path)` | `Array(String) \| IoError`: the names, sorted, without `.` and `..` |
| `dir_walk(path)` | `Array(String) \| IoError`: every name below it, as a path relative to it |
| `dir_make(path)` | `Void \| IoError`: nothing when the directory and its missing parents are made; one that is there already is fine |
| `dir_exists(path)` | `Bool`: a directory is there |

```avant
fn copy(from: String, to: String): Int | IoError {
  text = file_read(from)?
  file_write(to, text)?
}

fn main {
  _ = file_write("notes.txt", "remember the milk\n")
  puts(copy("notes.txt", "copy.txt") ?? -1)       // => 18
  puts(copy("missing.txt", "copy.txt") ?? -1)     // => -1
  puts(file_exists("copy.txt"))                   // => true
  names = dir_list(".") ?? []
  puts(names.contains("copy.txt"))                // => true
}
```

A `FileReader` reads its file a line at a time, as [`read_line`](#input) reads standard input: `r.read_line()` gives the next line without its newline, `nil` after the last one, or an `IoError`. The reader closes its file when it reaches the end or meets an error; `r.close` stops it early (a reader dropped before its end keeps its file open until the program ends).

```avant
fn longest(path: String): Int | IoError {
  r = file_open(path)?
  best = 0
  while line = r.read_line() {
    if line.size > best {
      best = line.size
    }
  }
  best
}

fn main {
  _ = file_write("poem.txt", "one\nthree\n")
  puts(longest("poem.txt") ?? -1)                 // => 5
  puts(longest("missing.txt") ?? -1)              // => -1
}
```

`file_read` returns the whole file; `file_open` reads it line by line without holding it all.

The file system's basics make a tool's directory walk and its housekeeping:

```avant
fn remove(path: String) {
  file_delete(path) else e {
    puts(e.message)
    return
  }
  puts("removed ${path}")
}

fn main {
  dir_make("logs/old") ?? return
  _ = file_write("logs/old/a.txt", "one\n")
  names = dir_walk("logs") ?? []
  puts(names.size)                                // => 2
  puts(names[1])                                  // => old/a.txt
  puts(dir_exists("logs/old"))                    // => true
  puts(file_size("logs/old/a.txt") ?? -1)         // => 4
  remove("logs")                                  // => cannot delete logs: Is a directory
  remove("logs/old/a.txt")                        // => removed logs/old/a.txt
  puts(file_exists("logs/old/a.txt"))             // => false
}
```

- `dir_walk` lists a directory before what it holds and the names of each directory sorted; a link to a directory is listed and not followed.
- An `IoError` has `op` (`"read"`, `"write"`, `"list"`, `"open"`, `"stat"`, `"delete"` or `"make"`), `path` and `errno` (the OS error number); its message reads `cannot read notes.txt: No such file or directory`. Dropping one is a compile error, like any [error](#errors-cannot-be-dropped).
- The names `IoError`, `ProcessError`, `FileReader` and `ProcessOutput` are taken only in a program that calls these functions.

### Processes and environment

| Call | Result |
| --- | --- |
| `argv()` | `Array(String)`: the program's name, then its arguments ([Program arguments](#program-arguments)) |
| `env_get(name)` | `String?`: `nil` when the variable is not set |
| `process_run(program, args)` | `Int \| ProcessError`: runs the program, waits, and gives its exit status |
| `process_run_out(program, args, out)` | the same, with stdout and stderr written to the file `out` |
| `process_output(program, args, input)` | `ProcessOutput \| ProcessError`: runs the program with `input` on its stdin (`""` when left out), waits, and gives its `status`, `stdout` and `stderr` |

```avant
fn main {
  status = process_run("sh", ["-c", "exit 3"]) ?? -1
  puts(status)                                           // => 3
  puts(process_run("no-such-program", ["x"]) ?? -1)      // => -1
  if home = env_get("HOME") {
    puts(home.size > 0)                                  // => true
  }
  puts(argv().size)                                      // => 1
  if r = process_output("tr", ["a-z", "A-Z"], "shout") {
    puts(r.stdout)                                       // => SHOUT
    puts(r.status)                                       // => 0
  }
}
```

A program name without `/` is looked up on `PATH`. `process_run` gives the child the program's own stdin, stdout and stderr; `process_output` gives it `input` and then the end of its input, and reads its stdout and stderr as they come, so neither can fill and stall it (a child that stops reading its input early is not an error). **A non-zero exit status is not an error**: it is the `Int`. A `ProcessError` is a program that could not start (`errno`, and `output` when it is the output file that could not be opened) or that a signal killed (`signal`): `cannot run ls: No such file or directory`, `sh killed by signal 9 (Killed)`.

### Time, hashing and math

```avant
fn main {
  t0 = now_us()
  puts(now_us() >= t0)       // => true
  puts(sha256("abc"))        // => ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  puts(sqrt(144.0))          // => 12.0
}
```

- `now_ms()` and `now_us()` read a monotonic clock as `Int` milliseconds and microseconds; they do not wrap. They measure intervals; they are not the time of day. The prelude has no time-of-day call and no `sleep` yet; C's `usleep` is one `lib` block away (`lib c { fun usleep(us: UInt32): Int32 }`).
- `sha256(s)` is the SHA-256 of a string, as 64 hexadecimal characters.
- `sqrt(x)`, `exp(x)` and `fmt_float(x, digits)` are in [Floats](#floats).
- These names are also taken (they serve the benchmark programs): `checksum_str`, `checksum_f64`, `crc32_bytes`, `sha256_word0` (each an `Int32`), `wide_op`, `wide_hi` and `hash_last_key`.

## Standard library

The standard library is imported module by module, and importing is what links the C library behind a module: a program that never imports `std.regex` does not carry PCRE2.

| Module | Gives you | Stands on |
| --- | --- | --- |
| [`std.json`](#stdjson) | `Json.parse`, a document to walk, `JsonBuilder` | yyjson |
| [`std.regex`](#stdregex) | `Regex.compile`, `Match` values, replace and split | PCRE2, and re2c scanners for literal patterns |
| [`std.http`](#stdhttp) | HTTP server and client, WebSocket server and client | uSockets and picohttpparser |
| [`std.zlib`](#stdzlib) | `Bytes.compress`, `Bytes.uncompress` | zlib |
| [`std.base64`](#stdbase64) | `Bytes.to_base64`, `Bytes.from_base64` | the aklomp Base64 codec |
| `std.gc` | `gc.hold { }`, `gc.collect()` | the collector ([Collection hints](#collection-hints)) |
| `std.testing` | `Test`, the checks of a test file | — ([Testing](#testing)) |

These modules are fast because of the C libraries under them: that speed is the libraries' merit, reached through Avant's bindings, and the benchmark notes say so ([Reading these numbers](#reading-these-numbers)). The libraries are compiled into the runtime, so nothing needs installing. A module's file is its import name: `std.regex` is `lib/std/regex.av`.

Two conventions run through the library: a failure is an error value (`Json.parse` returns `Json | JsonError`), and a call that allocates its result usually has an `_into(buf)` twin that writes into a `Buf` you own.

### std.json

```avant
import std.json

fn main {
  text = "{\"name\": \"ada\", \"langs\": [\"avant\", \"c\"], \"age\": 36}"
  if doc = Json.parse(text) {
    puts(doc.str("name") ?? "?")       // => ada
    puts(doc.int("age") ?? 0)          // => 36
    if langs = doc.get("langs"), first = langs.at(0) {
      puts(langs.len)                  // => 2
      puts(first.str ?? "?")           // => avant
    }
  }

  out = Json.obj
  out.set("ok", true)
  out.set("count", 2)
  out.key("tags").arr.add("x").add("y").end
  out.end
  puts(out.to_s)                       // => {"ok":true,"count":2,"tags":["x","y"]}
}
```

**Reading.** `Json.parse(text)` returns `Json | JsonError`. A `Json` is a position in the parsed document; the document owns its values and its text, and there is nothing to close.

| On a `Json` | Gives |
| --- | --- |
| `get(key)`, `at(index)`, `first` | `Json?`: a member, an element |
| `len`, `key_at(index)` | the number of members or elements; a member's key (`String?`) |
| `iter`, then `next` on it | each element in turn (`Json?`; `while v = it.next { ... }`) |
| `int`, `f64`, `str`, `bool` | this value as that type, or `nil` when it is another type |
| `int(key)`, `f64(key)`, `str(key)`, `bool(key)` | the same, for a member |
| `is_null`, `is_bool`, `is_int`, `is_float`, `is_str`, `is_arr`, `is_obj` | `Bool` |
| `f64s(key)` | `Array(Float64)?`: the numeric member `key` of every element of an array of objects |
| `text` | the value written back as JSON text |
| `to_value` | a plain `JsonValue` tree (an Avant class) that outlives the document |

`int` is a JSON integer (no fraction, no exponent) within `Int`, and `nil` for anything else: a real such as `3.7` or `3.0`, an integer beyond `Int`, a non-number. `f64` reads any number.

**Errors.** A `JsonError` says what yyjson found and where: `invalid JSON at byte 7: trailing comma is not allowed`, with the byte offset in `e.offset` (`-1` when there is none).

**Writing.** `Json.obj` and `Json.arr` start a `JsonBuilder`; `JsonBuilder.into(buf)` starts one that writes into your `Buf`.

| On a `JsonBuilder` | Does |
| --- | --- |
| `set(name, v)`, `set_null(name)` | a member of the open object |
| `add(v)`, `null` | an element of the open array |
| `key(name)`, then `obj`, `arr` or `value(v)` | a member that is itself a container or a value |
| `end` | closes the innermost open object or array |
| `to_s` | the text |

`v` is an `Int`, a `Float64`, a `String`, a `Bool`, a `Json` or another `JsonBuilder`. Floats are written in the language's one float text (`1.5e-7`, `2.0`), and NaN and the infinities as `null`. A builder used against its shape traps and says how: `end` with nothing open (`avant runtime: JsonBuilder.end: nothing open`), `key` outside an object, `to_s` with containers still open (`JsonBuilder.to_s: 2 containers still open`).

### std.regex

```avant
import std.regex

fn main {
  if re = Regex.compile(r"(\w+)@(\w+)\.com") {
    if m = re.find("write to ada@example.com today") {
      puts(m.start)                            // => 9
      puts(m.group(1) ?? "")                   // => ada
      puts(m.group(2) ?? "")                   // => example
    }
    puts(re.count("a@b.com, c@d.com"))         // => 2
    puts(re.replace("a@b.com", "$2 at $1"))    // => b at a
    re.close
  }
}
```

Compiling this program prints one note on stderr, `re2c: pattern stays on PCRE2 (...)`: see "Two engines" below.

| Call | Gives |
| --- | --- |
| `Regex.compile(pattern, caseless = false)` | `Regex \| RegexError` |
| `re.find(text, from = 0)` | `Match?`: the first match at or after byte `from` |
| `m.start`, `m.end` | the match's bounds, in bytes |
| `m.group(i)`, `m.group("name")` | `String?`: a group's text, `nil` when it did not take part |
| `m.span(i)`, `m.group_count` | a group's bounds (`Span?` with `lo` and `hi`); the number of groups |
| `re.count(text)` | the number of non-overlapping matches |
| `re.matches(text)`, then `next` on it | each match in turn (`Match?`) |
| `re.replace(text, repl)` | a new string with every match replaced; `re.replace_into(buf, text, repl)` appends it to a `Buf` |
| `re.split(text)` | `Array(String)`: the pieces between matches, empty pieces kept |
| `re.close`, `re.is_closed`, `re.scoped { \|r\| ... }` | release the pattern; ask; run a block and close on exit |

- A `Match` is a value: there are no global match cursors.
- In a replacement, `$1` to `$9`, `${1}` and `${name}` insert a group (nothing when it did not take part) and `$$` is a dollar sign.
- A pattern that does not compile is a `RegexError` with PCRE2's reason and the offset it points at (`invalid regular expression at offset 9: missing closing parenthesis`, `e.offset`).
- A compiled pattern is a handle, and copies of a `Regex` are the same handle: `re.close` through any of them releases it and closes them all. An operation on a closed `Regex` traps (`avant runtime: Regex used after close`), because a use after close is a program error, not a "no match"; so do a `Match`'s named groups and its groups past the third, which are read from the pattern. Compiling the same pattern again is cheap: a bounded cache keeps compiled patterns.
- **At PCRE2's limits a match traps; it is never read as "no match".** A search that backtracks past PCRE2's match limit (ten million steps), or runs into its depth or memory limits, ends the program with the operation and PCRE2's reason: `avant runtime: Regex.replace: match limit exceeded (PCRE2 error -47)`. There is no way yet to set a limit or to get the failure as a value, so a pattern or a text that comes from outside is yours to bound (its length, its shape) before matching.
- A match that only nests deeply (a group repeated many thousands of times) is not a failure: it is answered, on a larger stack and then by PCRE2's interpreter, at a cost in memory that grows with the text. A pattern may have any number of groups PCRE2 accepts.

**Two engines.** A string-literal pattern that needs at most one capture group and no named groups is compiled to a native scanner at compile time, by re2c. Any other pattern runs on PCRE2 with its JIT; when the pattern (and its `caseless` argument, if given) is a literal, the compiler says so on stderr as it compiles your program, so `avant run` prints only the program's own output on stdout. Both are inside the release: a program needs neither library on the machine. (Without any `re2c` binary, literal patterns run on PCRE2 too; `AVANT_RE2C` names another binary.) PCRE2 backtracks, and backtracking can be catastrophic on adversarial patterns and texts.

### std.http

Cleartext HTTP/1.1 and WebSockets: a server, a blocking HTTP client and a blocking WebSocket client. **There is no TLS yet**: `https` and `wss` URLs are an `HttpError`, never a silent downgrade.

```avant
import std.http

fn main {
  if server = Http.listen(0) {                 // port 0: the OS picks a free port
    server.get("/hello") { |req|
      server.close                             // this example answers once, then stops
      Response.text("hello ${req.query ?? "nobody"}")
    }
    port = server.port
    client = spawn {
      if resp = Http.get("http://127.0.0.1:${port}/hello?ada") {
        puts(resp.status)                               // => 200
        puts(String.from_utf8(resp.body) ?? "")         // => hello ada
      }
      0                                        // a spawn block ends with an Int, for now
    }
    server.serve
    _ = client.join
  }
}
```

#### HTTP server

`Http.listen(port)` binds at once and returns `Server | HttpError`, so a bad port fails there. Register routes, then `serve`, which runs until `close` is called.

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080, "0.0.0.0") {
    server.get("/") { |req|
      Response.text("hello")
    }
    server.post("/echo") { |req|
      Response.bytes(req.body).header("X-Echo", "1")
    }
    server.route("OPTIONS", "/") { |req|
      Response.text("", 204)
    }
    server.serve
  }
}
```

- **Listening**: `Http.listen(port, host = "127.0.0.1", max_body = 1048576, loops = 0, max_message = 1048576)`. `server.port` is the bound port.
- **Routes** are exact paths: `get`, `post`, `put`, `delete`, `patch`, or `route(method, path)`. There is no pattern router, no middleware and no streaming. Register every route before `serve`: the loop threads read the routes without a lock, so a route method (the WebSocket ones too) called while the server serves, from a handler, traps (`avant runtime: route GET /x registered while the server is serving: register every route before serve`, status 1).
- **A handler** gets a `Request` (`method`, `path`, `query` as `String?` without the `?`, `headers`, `body` as `Bytes`) and returns a `Response`: `Response.text(body, status = 200)`, `Response.bytes(body, status = 200)`, then `.header(name, value)`; a response is built only this way. Its `status` must be 100 to 599: another traps when the answer is written (`avant runtime: a response's status 99 is not 100 to 599 (RFC 9110)`). `Content-Length` and `Connection` are the server's to set. A header's name must be a token (letters, digits and the punctuation RFC 9110 allows) and its value may hold no control character but tab: anything else traps (`avant runtime: Response.header value of X-A holds a control character (byte 13 at 1): …`), because a CR or LF there would add header lines or a second answer (response splitting). Check text taken from a request before putting it in a header.
- **Headers** (`req.headers`, `resp.headers`): `get(name)` (`String?`), `has(name)`, `get_all(name)`, `count`, `name_at(i)`, `value_at(i)`.
- **Bodies** are buffered up to `max_body` (1 MiB by default): a larger one is answered 413, a chunked request body 411, a malformed request 400, and so is one with more than one `Content-Length` value (two lines or a list in one, even when they agree), an HTTP/1.1 request without `Host`, a request with two `Host` lines, and one whose `Host` is not `host[:port]` (RFC 3986's host, a name or an address, then an optional port); each of these closes the connection.
- **Connections** are kept alive, and pipelined requests are answered in order, each with its own write for now. One event loop runs per thread of the [pool's count](#the-thread-pool) (`loops` overrides it), all on the same port.
- **Handlers must not block the loop.** Hand slow work to `spawn`.
- **An idle connection is closed**: an HTTP connection on which nothing has happened for about 30 seconds (no request bytes arrived, no answer bytes left). A WebSocket connection has no such timeout.
- **A trap in a handler ends the server**, like any [trap](#traps). Run it under a supervisor, and read request data with the non-trapping forms.
- `close` stops the server and lets `serve` return. A server that is never closed keeps its socket until the process exits; there is no finalizer.

#### HTTP client

One blocking request at a time. Redirects are followed (at most five), chunked responses are decoded, and there is no connection pooling.

<!-- check: compile -->
```avant
import std.http

fn main {
  if resp = Http.get("http://127.0.0.1:8080/") {
    puts(resp.status)
    puts(resp.headers.get("Content-Type") ?? "")
    puts(String.from_utf8(resp.body) ?? "")
  }
  if created = Http.post("http://127.0.0.1:8080/echo", "payload") {
    puts(created.status)
  }
}
```

`Http.get(url, follow = true, timeout_ms = 10000)`, `Http.post(url, body, ...)` with a `String` body, and `Http.request(method, url, body, headers, ...)` with `body` as `Bytes` and `headers` as an `Array(Header)` (`Header { name: "Accept", value: "text/plain" }`), return `Response | HttpError`. A request is sent only when its method and every header name are tokens, every header value holds no control character but tab (the rules of `Response.header`), and the URL holds no space or control character; otherwise the call returns an `HttpError` naming the part and the byte (`invalid request header value of X-A (byte 13 at 1): …`) and sends nothing, because a CR or LF there would add request lines or a second request. `Content-Length` and `Connection` are the client's to set: a caller's are skipped. A call blocks its thread until the response is complete, so do not make one from a server handler: `spawn` first. The response is complete when its framing says so (its `Content-Length`, the last chunk of a chunked body, or the head alone for a HEAD request, a 204 and a 304), whether or not the server then closes the connection; a response with neither length nor chunks ends when the server closes. Interim answers (`100 Continue`, `103 Early Hints`) are read past: the response is the final answer after them. A response with more than one `Content-Length` value (identical or not, as the server refuses for requests) or one that is not a number is an `HttpError` (`invalid HTTP response body from …`).

`timeout_ms` is a deadline for the whole call: the connection, the request, the complete response, and every redirect the call follows. When it passes first, the call returns an `HttpError` (`request to 127.0.0.1:8080 timed out`), whatever part of a response had arrived; a server that keeps sending slowly does not extend it. The deadline is kept within a few milliseconds. Zero or less means the default, 10000.

#### WebSocket server

WebSocket routes share the HTTP server and its port. A handler runs once per complete message.

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080) {
    server.websocket("/echo") { |conn, msg|
      conn.send_message(msg)                   // echo, text or binary
    }
    server.websocket_close("/echo") { |conn, code, reason|
      puts("closed with ${code}")
    }
    server.serve
  }
}
```

- A message is text (`msg.text` is its `String`, `msg.is_text`) or binary (`msg.body`, `msg.is_binary`). `msg.body` is always the payload.
- On a connection: `send_text`, `send_bytes`, `send_message`, `close(code = 1000, reason = "")`; each returns `Bool`: `false` when the connection is closed or its send queue is full (a message that cannot go out at once and would leave more than 1 MiB waiting is refused whole; one the socket has begun to take is always sent whole). A `WsConn` kept after its connection closed, or after `serve` returned, answers `false` to all four.
- `websocket_open(path) { |conn| ... }` and `websocket_close(path) { |conn, code, reason| ... }` run once per connection (code 1006 when it dropped without a close frame).
- Ping, the close handshake and fragmentation are handled for you. A message over `max_message` (1 MiB by default) closes the connection with code 1009 and never reaches the handler. `server.close` sends code 1001 to open connections.
- A connection stays on its loop thread. To answer from a `spawn` block, capture `conn.sender` and send through that.

#### WebSocket client

<!-- check: compile -->
```avant
import std.http

fn main {
  if conn = Ws.connect("ws://127.0.0.1:8080/echo") {
    conn.send_text("hi")
    if msg = conn.recv() {
      puts(msg.text ?? "")
    }
    conn.close
  }
}
```

`Ws.connect(url, timeout_ms = 10000)` returns `WsConnection | HttpError` (an `HttpError` without connecting for a URL with a space or a control character); its `timeout_ms` is the deadline for the connection and its handshake. `recv(timeout_ms = -1)` blocks and returns `WsMessage?`: `nil` when the connection closed or the timeout passed, and `close_code` says which (1006 until a close frame arrives). Both timeouts are kept on the monotonic clock: setting the system's clock during the wait does not move them. Never call `recv` from a server handler. A connection that is never closed keeps its thread until the process exits.

After `close`, every copy of the connection answers as closed, whatever connections are opened later: `send_text` and `send_bytes` return `false`, `recv` returns `nil`, `close_code` is 1006, and another `close` does nothing. A `WsConnection` may be captured by a `spawn` block, so one task can wait in `recv` while another sends; a `close` makes the waiting `recv` return `nil`.

### std.zlib

```avant
import std.zlib

fn main {
  packed = "avant avant avant avant".bytes.compress
  if back = packed.uncompress, text = String.from_utf8(back) {
    puts(text)       // => avant avant avant avant
  }
}
```

`bytes.compress` returns `Bytes`; `bytes.uncompress` returns `Bytes | ZlibError`. `compress_into(buf)` and `uncompress_into(buf)` write into a `Buf` and return `ZlibError?`.

### std.base64

```avant
import std.base64

fn main {
  enc = "avant".bytes.to_base64
  puts(enc)                                    // => YXZhbnQ=
  if raw = enc.bytes.from_base64 {
    puts(String.from_utf8(raw) ?? "")          // => avant
  }
}
```

Encoding never fails (`to_base64` returns a `String`); `from_base64` returns `Bytes | Base64Error` on malformed input. `to_base64_into(buf)` and `from_base64_into(buf)` are the `Buf` twins.

## Testing

A test file is an ordinary program: it imports `std.testing`, checks what it cares about, and returns `t.finish` from `fn main: Int`. `avant test` finds the files, runs them in parallel and reports.

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

<!-- check: output -->
```text
PASS greet uses the name
PASS greet handles empty
0 failed, 2 passed
```

Save it as `tests/test_greet.av` and run `avant test`. A failing check prints `FAIL` and its name, then the run continues, so one file reports every failure.

The process status is the verdict: `0` when every check passed, `1` when any failed. A failed check fails the process whatever `main` returns, even from a void `main` or one that never calls `finish`. The runner trusts the status; it does not read the words `PASS` and `FAIL`.

Put the code under test in its own module and `import` it; a test sees its `pub` names only.

### Running tests

| Command | What it runs |
| --- | --- |
| `avant test` (or `avant t`) | every `test_*.av` under `tests/`, recursively |
| `avant test DIR` | the same walk, starting at `DIR` |
| `avant test FILE.av` | that file, even when its name does not match `test_*.av` |
| `avant test A.av B.av` | those files, and no directory walk |

With no path, `tests/` is relative to the nearest `avant.toml`, or to the current directory when there is none. The walk skips names that start with `.` and directories named `deps`. Results are sorted by path. An empty selection exits 1.

| Flag | Meaning |
| --- | --- |
| `--jobs N` | how many test processes run at once (default: the CPUs the process may use, as the thread pool counts them; `--jobs 1` runs one file at a time; `--jobs 0` is an error) |
| `--shard I/N` | run slice `I` of `N`, counted from 1; it does not launch the other slices. May be combined with `--jobs` |
| `--junit PATH` | write a JUnit report: one case per check `std.testing` records, one per file that records none, and one for a failed file whose checks all passed |
| `--name PATTERN` | run only the checks, `t.test` bodies and `t.parallel` bodies whose names contain `PATTERN` ([Choosing by name](#choosing-by-name)) |
| `--coverage` | instrument the tests, print a coverage table after the summary, write `coverage/lcov.info` |
| `--skip-exclusive` | leave out the files listed in `[test] exclusive` (required when `--shard` would include one) |
| `-h`, `--help` | print the flags and exit 0 |

`--jobs=N`, `--shard=I/N`, `--junit=PATH` and `--name=PATTERN` are the same flags. An unknown flag is an error. `avant test` does not pass arguments to the test programs, and a `--` on the command is an error.

For each file the runner prints its output, in source order, then `ok tests/test_greet.av`, or `FAIL tests/test_greet.av:` and how its process ended (`the test process exited with status 1`, `the test process killed by signal 13 (Broken pipe)`, `the compile exited with status 1`, `the parallel test "other" killed by signal 11 (Segmentation fault)`), and a total at the end (`0 failed, 1 passed`). Its own status is 1 when any file failed, when nothing was selected, or when the command was refused. Ctrl-C or `SIGTERM` stops every test process it started and removes its temporary files.

### Checks

`Test.new` makes the value the checks hang on. Every check takes a name: name it after the case, so a `FAIL` line alone says what broke.

| Method | Passes when |
| --- | --- |
| `check(cond, name)` | `cond` is true |
| `check_eq(got, want, name)` | `got == want`, for any type that has `==` and prints ([Equality and order](#equality-and-order), [Printing](#printing)); on failure it prints both under the `FAIL` line |
| `golden(path, got)` | the bytes in the file `path` equal `got` ([Golden files](#golden-files)) |
| `run_golden(program, want)` | the program compiles (with `--debug`), exits 0, and prints exactly the bytes in the file `want` |
| `rejects(source, substring)` | compiling `source` fails, and the compiler's stderr contains `substring` |
| `temp_dir` | (not a check) a new, empty directory for files the test writes |
| `finish` | prints `N failed, M passed`; returns `0` when every check passed, `1` otherwise |

For a `Float64`, `check_eq` is IEEE's `==` (NaN equals nothing): compare with a tolerance in a `check` when that is what equality means.

Several cases over one function read better as a table: inputs and expected values in two arrays, checked in one loop.

```avant
import std.testing

fn greet(name: String): String {
  "Hello, ${name}"
}

fn main: Int {
  t = Test.new
  names = ["Ada", "Bo", "Cy"]
  wants = ["Hello, Ada", "Hello, Bo", "Hello, Cy"]
  for i in 0..names.size {
    t.check(greet(names[i]) == wants[i], "greet ${names[i]}")
  }
  t.finish
}
```

`t.temp_dir` returns a new directory at each call. They live in the test's private temporary directory and are removed when it exits, so a test never cleans up after itself.

```avant
import std.testing

fn main: Int {
  t = Test.new
  dir = t.temp_dir
  t.check((file_write("${dir}/notes.txt", "hi") ?? -1) == 2, "write in temp_dir")
  t.finish
}
```

### Golden files

`golden` and `run_golden` compare against saved files, byte for byte, a trailing newline included. A missing file fails the check, and nothing ever rewrites one: update a saved file by hand, after reading the new output.

<!-- check: skip needs want.txt, hello.av, hello.want and bad.av beside it; exercised by tests/cases/test_testing_docs.av -->
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

`run_golden` and `rejects` compile a program, so they need the environment variable `AVANT_COMPILER` set to the compiler under test. A project that only calls `check` does not.

### Serial and parallel tests

Checks written in the file, and bodies passed to `t.test`, run one at a time in source order. `t.parallel` bodies run after the file's serial work, each in a new process of the same binary, overlapping one another.

<!-- check: compile -->
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

In each of those processes the file runs again, but serial checks and `t.test` bodies are skipped and only that one parallel body runs; code in `main` outside those bodies runs in every process. Only `avant test` starts those processes. `avant run` on the file, or the compiled file run as a program, runs it once with every body in that one process, in source order: the same checks, without the overlap.

### Choosing by name

`avant test tests/test_greet.av --name empty` runs only what is named with `empty` in it: a check whose name contains the pattern, and a `t.test` or `t.parallel` body whose name contains it, which then runs whole, every check inside it included. A body whose name does not contain the pattern does not run, so a check inside a body is chosen by the body's name. The pattern is plain text, matched case for case anywhere in the name. Code in `main` outside the bodies still runs, as it does in a parallel test's process. The JUnit report lists only the checks that ran.

Over several files, a file where nothing matches is left out of the report and the total. A pattern that matches nothing in any selected file is an error that names the pattern and the files: `--name nothing matches no check, test or parallel test in tests/test_greet.av`.

### Test settings

An optional `[test]` section in `avant.toml` adjusts the walk. Paths are relative to the manifest.

```toml
[test]
dir = "tests"
exclusive = ["tests/test_bench.av"]
first = ["tests/test_slow.av"]
```

| Key | Meaning |
| --- | --- |
| `dir` | one directory, replacing the default `tests/` walk |
| `entry` | one file, replacing the walk (`dir` and `entry` together are an error) |
| `exclusive` | files that run alone, while no other test file is compiling or running. A string or a list |
| `first` | files that start before the others, in the listed order. A string or a list |

- A path on the command overrides `dir` and `entry`. There is no `jobs` key and no command string.
- `exclusive` is for a benchmark that needs a quiet machine. `first` is for the slowest files, so a long file does not start last (a file runs as soon as it has compiled).
- The lists do not discover files: on a command with no path, every listed file must be one of the discovered tests.
- `--shard` is an error when the selection contains an exclusive file, unless `--skip-exclusive` is passed; it picks its slice before `first` is applied, so `first` never moves a file between shards.
- With `--jobs 1` every file is already alone, and `exclusive` changes nothing.

### Reports

**JUnit** (`--junit PATH`) is for test analytics. A test file that uses `std.testing` contributes one `<testcase>` per check, named by the check; any other test file contributes one case, named by its path. A file that failed without a failed case (its process killed by a signal, or exiting with a status and no failed check) adds one case named by its path, whose failure message says how the process ended; a failed case named by its path carries the file's output. A byte XML cannot hold, in that output or in a check's name (a control character such as a terminal colour's escape, or a byte that is not UTF-8), is written as `\xNN` (`\x1b`), so the report always parses. It is off unless you ask; a project does not gain a `junit.xml` by surprise.

**Coverage** (`--coverage`) compiles each selected file with counters, runs them, prints one table after the summary and writes `coverage/lcov.info` in the current directory (one file, overwritten by each run). Without the flag nothing is instrumented and nothing is written.

By default the report covers each test file and what it imports. An optional `[coverage]` section widens that to whole directories, so a file no test loaded is listed at 0%:

```toml
[coverage]
include = ["src"]
exclude = ["tests", "deps"]
```

`include` and `exclude` are directories relative to the manifest (a string or a list; no globs), walked recursively for `*.av`, skipping directories whose name starts with `.`. A file under `exclude` is left out, a test file included when `exclude` names `tests`. A file no test loaded has no branch map.

The runner sets what the instrumented programs need. These variables exist for a program instrumented on its own:

| Variable | Read by | Effect |
| --- | --- | --- |
| `AVANT_COVERAGE=1` | the compiler | emit hit counters for this compilation (`0` turns it off). The test runner sets it; a person rarely does |
| `AVANT_COVERAGE_MAP` | the compiler | the path of the coverage map baked into the binary. Unset, `compile` writes it beside the binary as `OUT.covmap` (the program adds `OUT.covmap.hits` when it runs); `run` and `dump` keep it in their temporary directory |
| `AVANT_COVERAGE_LCOV` | the program | where it writes the LCOV report; `0` writes none. Unset: `coverage/lcov.info` |
| `AVANT_COVERAGE_REPORT` | the program | `0` hides the text table, `1` prints it. Unset, a program prints the table and the compiler's command line does not |
| `AVANT_COVERAGE_COLOR` | the table | `0` or `1` forces color; unset, it follows the terminal |

Several processes of one instrumented binary add into one shared map, and `avant test --coverage` merges every test into one report. `compile`, `run` and `dump` take no `--coverage` flag.

## Formatter

`avant format` (or `avant f`) prints a file in the canonical style: two-space indent, `name: Type` with no space before the colon and one after, runs of blank lines collapsed to one. Your comments and your line breaks stay.

```sh
avant format main.av            # print the canonical text
avant format src --write        # rewrite every file that changes
avant format . --check          # CI: list files that would change, exit 1 if any
```

- A path is a `.av` file or a directory, walked like `avant test` walks `tests/` (names starting with `.` and directories named `deps` are skipped).
- With one file and no flag, the text goes to stdout. `--check` writes nothing, prints `would change PATH` per file (`canonical` when none would), and exits 1 if any would.
- The formatter works on tokens: it does not parse, typecheck or follow imports, so it formats a file that does not compile yet. It runs as its own command, never as part of a build, so a formatting bug cannot change what a program means.
- A file that does not lex is reported and left alone (status 1); the others are still formatted. A usage error (no path, an unknown flag, `--check` with `--write`, several files without either) exits 1.

## Command line

```sh
avant run app.av                   # compile and run
avant run app.av -- input.txt      # the program's arguments go after --
avant compile app.av               # build ./app
avant compile app.av --release     # optimized
avant test                         # run tests/
avant format . --check
```

### Commands

| Command | Short | What it does |
| --- | --- | --- |
| `avant run FILE.av [-- ARGS...]` | `avant r`, or just `avant FILE.av` | compiles to a temporary binary and runs it; exits with the program's status |
| `avant compile FILE.av [OUT]` | `avant c` | builds a binary: `OUT`, or the file's name without `.av`. Prints `compiled to OUT` |
| `avant test [PATH...]` | `avant t` | runs test files ([Testing](#testing)) |
| `avant format PATH...` | `avant f` | prints or rewrites files in the canonical style ([Formatter](#formatter)) |
| `avant fetch [DIR] [--update]` | | downloads a project's dependencies ([Packages](#packages)) |
| `avant bind HEADER.h` | `avant b` | prints a `lib` block for a C header ([avant bind](#avant-bind)) |
| `avant dump FILE.av` | `avant d` | prints the program's mid-level IR without linking |
| `avant help` | `-h`, `--help` | the usage text |
| `avant version` | `--version` | the version and the commit the binary was built from, as `avant 0.42.0 (commit e030096…)`; `, with uncommitted changes` when its `compiler/`, `lib/` or `runtime/` differed from that commit |

`FILE.av` is always one root file; the compiler follows its imports. Flags for `run` and `compile`:

| Flag | Meaning |
| --- | --- |
| `--debug`, `--release` | the [build mode](#build-modes) (`--final` is another spelling of `--release`) |
| `--cpu=LEVEL` | the x86-64 level of the generated code: `baseline`, `v2`, `v3`, `v4` or `native` |
| `--lib NAME` | link a system library ([Linking C code](#linking-c-code)); `--lib=NAME` is the same |
| `--cc FILE.c` | compile and link a C file, or link a `.o` |
| `--keep-tmp` | keep the command's [temporary files](#temporary-files) and print where |

Options may sit before or after the file, up to `--`.

### Build modes

| | `--debug` | (no flag on `compile`) | `--release` |
| --- | --- | --- | --- |
| Chosen by default for | `avant run`, `avant test` | `avant compile` | nothing: ask for it |
| Optimization | none (LLVM `-O0`, its fast instruction selector) | a short pass list | full (`-O3`) |
| Compiles | fastest | fast | slowest |
| Use it for | the edit-and-run loop | a binary to keep around | **anything you measure, and anything you ship** |

- **Time a program only with `--release`.** `avant run` builds at `-O0`; a first benchmark run that way measures the debug build, not Avant. The [Benchmarks](#benchmarks) use `--release --cpu=v3`.
- An explicit flag always wins over the verb's default. No environment variable selects the mode.
- The language is the same in every mode: bounds, overflow and conversion checks are never turned off, and the collector's bookkeeping is precise in all three.
- `--cpu`: `compile` targets `v2` by default, a level every x86-64 processor of the last decade supports, so the binary runs elsewhere. `run` and `test` target the highest level of the machine they run on. For a binary that will only run on recent hardware, `--cpu=v3` (AVX2) is worth trying; `native` is this machine exactly.

### Program arguments

Arguments after `--` are the program's. `argv()` returns the program's name at index 0 and the arguments from index 1.

<!-- check: run args=hello -->
```avant
fn main: Int {
  args = argv()
  if args.size > 1 {
    puts(args[1])      // => hello
  }
  0
}
```

```sh
avant run prog.av -- hello
```

`avant run` exits with the program's own status: what `main` returned, 128 + N when signal N killed the program, 127 when it could not be started. `--threads N` among a program's arguments sets its [thread pool](#the-thread-pool) and is not passed on.

### Compile errors

Compile errors go to stderr with a stable code, the offending line and a caret; the compiler exits non-zero.

<!-- check: error -->
```avant
fn main {
  count = 3
  puts(cuont)
}
```

<!-- check: stderr -->
```text
error[E4001]: unknown name cuont (did you mean `count`?)
  --> main.av:3:8
3 |   puts(cuont)
  |        ^
```

- The compiler recovers and keeps going, per statement: one run reports every independent mistake it can find, in every function and imported file, up to 20 errors. With several, the run ends with `error: aborting due to N previous errors`, and past the cap with `error: and M more errors (limit 20)`.
- A mistake is reported once. What reads a value whose type already failed (an operator, a call, a condition, a later use of a name bound to it) adds no error of its own; where an operand does not fit, the message is its reader's: `px = pa + pn` with an `Int32` and an `Int` says `binary operator expects matching types, got Int32 and Int`, and `n: UInt32 = -1` says `a negative literal does not fit in UInt32`.
- A syntax error names what the parser expected and what it found, in source terms: ``error[E2004]: expected `{`, found a string``.
- A name the compiler cannot find gets at most one suggestion, and only one that is in scope there: a local, a field, a method, a constant, a function, a type, an enum member or a builtin method, the nearest by spelling (at most a third of the name's letters changed, two swapped letters counting as one change and a letter's case as half, and never the whole name; a tie goes to the name the compiler would find first, a local before a field, a method, a constant or a function, then to the first alphabetically). A wrong count of a call's arguments is not a misspelling: it is the next point.
- A call whose arguments fit no function of that name says what it takes: `add takes 2 arguments, given 1: add(a: Int, b: Int): Int`; with several, each signature. Two that fit equally are named both: ``ambiguous call to f: f(a: Int, b: Int?): Int and f(a: Int?, b: Int): Int both match``.
- A use of an optional as its value, and `x == nil`, say how to open it: ``cannot read field size on String?; open it with `if v = s`, `s ?? fallback` or `s?` ``.
- The code's first digit names the phase: `E1xxx` lexer, `E2xxx` parser, `E3xxx` type resolution, `E4xxx` checker, `E5xxx` module loader, `E6xxx` macro expansion.

Run-time failures are [traps](#traps).

### Temporary files

`compile`, `run`, `test`, `fetch` and `bind` keep every intermediate file (IR, objects, logs, test binaries) in one private directory, `avant-XXXXXX` under `$TMPDIR` (or `/tmp`). It is removed when the command exits, on an error, Ctrl-C, `SIGTERM`, `SIGHUP`, a closed pipe (`SIGPIPE`, as in `avant test | head`) or a crash included (`SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT`): the process still dies by the signal. Only `kill -9` or a stack overflow leaves it behind. The tools the compiler runs (`cc`, `re2c`, `git`) and the programs that `run` and `test` start see that directory as their `TMPDIR`, so their temporaries go with it.

`--keep-tmp` keeps the directory for debugging and prints its path. A program run on its own keeps its directory with `AVANT_KEEP_TMP=1`.

### Environment variables

**None needs to be set.** For the record:

| Variable | Effect |
| --- | --- |
| `TMPDIR` | where the private temporary directory is created |
| `AVANT_KEEP_TMP=1` | a program keeps its temporary directory (`--keep-tmp` sets it for the processes a command starts) |
| `AVANT_COMPILER` | the compiler that `run_golden` and `rejects` use in tests ([Golden files](#golden-files)) |
| `AVANT_ROOT` | another place to find the runtime and the standard library, instead of beside the `avant` binary |
| `AVANT_RE2C` | another `re2c` binary for regex literals |
| `AVANT_HEAP_MAX_MB`, `AVANT_GC_STATS`, ... | the collector, read by your program ([Collector settings](#collector-settings)) |
| `AVANT_COVERAGE*` | coverage ([Reports](#reports)) |

## Coming from another language

Avant will look familiar from almost anywhere, and it differs on purpose in a handful of places. This section is the list, so the differences reach you here and not in a debugger.

### The deliberate differences

| If you expect | Avant does | What that buys |
| --- | --- | --- |
| exceptions, `try`/`catch` | failures are values in the result type, opened with `?`, `??` and if-assign ([Errors](#errors)) | a signature shows everything that can come out; no hidden control flow |
| `null` in any reference | absence only in `T?`, which must be opened ([Nil and unions](#nil-and-unions)) | no null error at run time |
| a 32-bit `int` that wraps | `Int` is 64-bit and overflow traps; `&+` wraps on request; `Int32` is for dense storage ([Numbers](#numbers)) | a wrong number never passes silently |
| numbers that convert themselves | no implicit conversion, between widths or to floats ([Conversions](#conversions)) | no silent truncation or precision loss |
| integer division that floors (Python's `//`, Ruby's `/`) | `/` truncates toward zero and `%` follows the dividend, as in C, Go and Rust | |
| `1..10` including 10 (Crystal, Ruby, Kotlin) | `..` excludes its end, `..=` includes it ([For](#for)) | ranges match zero-based indexes |
| truthiness: `if n`, `if list` | conditions are `Bool`; `if v = x` opens an optional ([Conditions that bind](#conditions-that-bind)) | |
| inheritance, virtual methods, interface values | none: composition, interfaces as compile-time bounds, unions ([No inheritance](#no-inheritance)) | every call is direct, every layout known |
| `mutating` or `&mut self` on methods that change a struct | a method may change its struct receiver; an argument is a copy ([Structs](#structs)) | Go's pointer receiver, taken automatically |
| strings of characters | strings of bytes; `.chars` when you want code points ([Bytes, not characters](#bytes-not-characters)) | constant-time `size` and indexing |
| a missing hash key that raises, or reads as zero | `h[k]` is `V?` ([Hashes](#hashes)) | absence is handled where it happens |
| hashes that remember insertion order | iteration order is unspecified, for now | |
| members public by default | functions, types, methods and fields are private to their module unless `pub` ([Visibility](#visibility)) | a module's surface is what it says it is |
| `let`, `var`; a `const` inside a function; global variables | `=` declares or assigns; `const` is a module's named value, and a module has no variables ([Names and scope](#names-and-scope), [Constants](#constants)) | no state shared by accident |
| `Array<Int>`, `[]int`, `Vec<i32>` | `Array(Int)`; a single capital letter in a signature is a type parameter ([Generics](#generics)) | |
| `return` inside a block returning from the block | in a builtin's block (`each`), `return` leaves the function; in a lambda, the lambda ([Builtin blocks are loops](#builtin-blocks-are-loops)) | |
| a panic caught per request by the server | a trap ends the process: run servers under a supervisor ([Traps](#traps)) | no unwinding machinery; Erlang's "let it crash", at the one granularity Avant has today, the process |
| one optimized build (Go) | three modes: time programs only with `--release` ([Build modes](#build-modes)) | a fast edit-and-run loop |
| a power operator `**` | none; multiply, or declare C's `pow` in a `lib` block ([Floats](#floats)) | |
| `async`/`await` | OS threads and blocking calls: `spawn`, `.parallel`, channels ([Concurrency](#concurrency)) | no function coloring |

### By language

- **Crystal and Ruby.** Yours already: `puts`, `size`, `push`, `nil`, blocks, `initialize` and `.new`, `Array(Int)`, `T?`, unions, `lib` and `fun`. Different: braces instead of `end`; `..` excludes its end and there is no `...`; no exceptions, no inheritance, no mixins; `Int` is 64-bit; `String#size` counts bytes; `h[k]` returns `V?` instead of raising; methods are private unless `pub`; macros are `quote` and a field walk, not a language.
- **Go.** Yours already: structs with methods, byte strings, errors as values, no inheritance, channels, a 64-bit `int`. Different: an error is a member of the result type, passed up with `?` instead of `if err != nil`; overflow traps instead of wrapping; a missing key is `nil`, not a zero value; there are no interface values; what a task captures is checked, where a goroutine shares anything.
- **Rust.** Yours already: `T?` is `Option<T>`, `T | E` is `Result<T, E>`, `?` propagates, `..` is exclusive, generics are monomorphized with trait-like bounds, things are private by default. Different: a garbage collector instead of ownership, so no lifetimes and no borrow checker; enums carry no data (a union member does, and `switch` takes it apart as `match` does); no trait objects; overflow traps in release builds too.
- **JavaScript, TypeScript and Python.** Yours already: `${}` interpolation, closures that capture by reference, no semicolons, types you rarely write. Different: compiled ahead of time and statically typed; integers are integers (`7 / 2` is `3`); no truthiness, no `undefined`, no exceptions; strings are bytes; threads instead of an event loop.
- **C and C++.** Yours already: braces, value structs, most of the operator precedence, C libraries one `lib` block away. Different: no manual memory, no headers, no preprocessor, no implicit conversions; `&`, `^` and `|` bind tighter than comparisons, so `x & 1 == 0` is `(x & 1) == 0`; what is undefined behavior in C (overflow, an index out of bounds) is a trap.
- **Java, C#, Kotlin and Swift.** Yours already: classes as references, a collector, generics, optionals (Kotlin, Swift), value structs (C#, Swift). Different: no inheritance and no interfaces as values; no exceptions; `Int` is 64-bit where `int` is 32; an optional is opened with if-assign and `??`, not `?.` or `!!`.

## Not built yet

What is missing, said in one place, and repeated beside each feature it limits. **Decided**: the design is settled and it will be built. **Planned**: wanted; the design is open. **Not started**: no work yet.

| Missing | Status | Today |
| --- | --- | --- |
| Constant strings and tables (`const names = [...]`) | planned | a function that builds the value |
| `Hash` with struct keys, or with class, array, hash or struct values; insertion order | decided | [key and value types](#key-and-value-types): a hash from the key to an index into an array; sort the keys |
| `String` methods: `contains`, `starts_with`, `replace`, `join`, case conversion; slices of strings and arrays | planned | `str_slice`, loops, [`std.regex`](#stdregex) |
| `map`, `select`, `reduce` on arrays | planned | a loop, or [your own](#trailing-blocks) |
| The time of day; `sleep` | planned | intervals with `now_ms`; C's `usleep` through a `lib` block |
| `==` on hashes | planned | a loop |
| A loop variable of its own for each iteration's closures | decided | copy it into a new name in the body |
| A `spawn` result other than `Int`; `for v in ch` over a channel | decided | end the block with an `Int`; `while v = ch.recv { ... }` |
| `parallel { }` as a statement, with no receiver | decided | `.parallel` on an array |
| The pool's size from `--avant-threads=N` only (a bad value an error) and a call in `std` to set it, every other argument the program's | decided | `--threads N`, which the runtime takes out of `argv()` |
| TLS for `std.http` (`https`, `wss`) | decided | a TLS-terminating proxy in front |
| A trap that ends only its task; a pre-forked server mode | decided | a supervisor that restarts the process |
| Positions in traps raised inside runtime functions | planned | the message names the operation and its values |
| Platforms other than Linux x86-64 with glibc (musl, ARM, macOS, Windows) | not started | |
| Editor tooling (a language server) | not started | `avant format`, and compile errors with positions |

## Benchmarks

Every number here was measured on one machine (an AMD Ryzen 7 5700G), each language built with optimizations, and the rows where Avant loses are printed as plainly as the rows where it wins. Read [Reading these numbers](#reading-these-numbers) before quoting any of them.

### LangArena

[LangArena](https://github.com/kostya/LangArena) runs the same tasks in many languages. Seconds, lower is better; **bold** is the best of the five. One process per task, timed by its wall clock, with peak memory from `/usr/bin/time`: the same wrapper for every language. Avant measured on 2026-11-12 (the development tree after release 0.41.0, `--release --cpu=v3`, one run, the machine otherwise idle, governor `performance`, all 50 task checksums correct); the other languages on 2026-11-07.

| Language | 50 tasks (s) | Average peak memory |
| --- | ---: | ---: |
| Avant | **39.200** | **19.9 MB** |
| C | 59.890 | 22.9 MB |
| Rust | 59.945 | 29.3 MB |
| Crystal | 87.011 | 65.9 MB |
| Go | 119.708 | 63.0 MB |

**The total flatters Avant.** Much of that gap is not the language: the four `Matmul` rows and `Hash::CRC32` are not comparable programs, two rows carry collection hints, and several rows measure C libraries ([Reading these numbers](#reading-these-numbers)). The honest reading is task by task.

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Binarytrees::Obj` | **0.618** | 2.736 | 2.865 | 1.325 | 1.881 |
| `Binarytrees::Arena` | 0.435 | 0.355 | **0.341** | 0.759 | 0.362 |
| `Brainfuck::Array` | 1.773 | 1.896 | 1.825 | 1.968 | **1.546** |
| `Brainfuck::Recursion` | 1.553 | 1.570 | **1.522** | 1.745 | 1.709 |
| `Matmul::Single` | **1.117** | 5.646 | 5.758 | 5.832 | 5.769 |
| `Matmul::T4` | **0.947** | 1.485 | 1.551 | 3.380 | 1.558 |
| `Matmul::T8` | **0.593** | 0.799 | 0.858 | 2.642 | 0.888 |
| `Matmul::T16` | 0.601 | **0.482** | 0.729 | 2.339 | 0.781 |
| `Base64::Encode` | 0.123 | **0.119** | 0.352 | 1.633 | 2.591 |
| `Base64::Decode` | **0.145** | 0.147 | 0.398 | 1.520 | 1.886 |
| `Json::Generate` | **0.246** | 0.661 | 0.337 | 1.173 | 1.256 |
| `Json::ParseDom` | **0.098** | **0.098** | 1.694 | 0.540 | 1.609 |
| `Json::ParseMapping` | 0.234 | **0.229** | 0.425 | 1.546 | 2.871 |
| `CSV::Parse` | **0.759** | 2.247 | 0.881 | 3.418 | 3.000 |
| `Etc::Sieve` | 1.450 | **0.920** | 0.979 | 2.360 | 1.896 |
| `Etc::TextRaytracer` | **0.999** | 1.307 | 1.019 | 1.341 | 1.831 |
| `Etc::NeuralNet` | 1.305 | **1.073** | 1.165 | 1.260 | 2.219 |
| `Etc::CacheSimulation` | **0.492** | 0.762 | 0.863 | 1.298 | 1.638 |
| `Etc::GameOfLife` | 1.433 | 1.487 | 1.503 | 2.816 | **0.980** |
| `Etc::Words` | 1.037 | 2.284 | 1.080 | **0.902** | 1.372 |
| `Etc::LogParser` | 0.467 | 0.496 | **0.348** | 1.207 | 28.911 |
| `Template::Regex` | **0.916** | 1.269 | 3.660 | 1.673 | 7.730 |
| `Template::Parse` | **0.755** | 1.132 | 1.181 | 2.737 | 0.891 |
| `Sort::Quick` | 1.312 | **1.186** | 1.193 | 1.550 | 1.305 |
| `Sort::Merge` | 1.738 | **1.165** | 1.540 | 2.309 | 1.593 |
| `Sort::Self` | **0.101** | 2.095 | 0.412 | 1.079 | 1.333 |
| `Graph::BFS` | **1.150** | 1.372 | 1.787 | 3.057 | 2.615 |
| `Graph::DFS` | **1.219** | 1.225 | 1.450 | 2.190 | 1.742 |
| `Graph::AStar` | **0.777** | 2.212 | 2.340 | 1.950 | 3.867 |
| `Hash::SHA256` | **0.249** | 1.155 | 1.218 | 1.214 | 1.243 |
| `Hash::CRC32` | **0.131** | 0.986 | 0.986 | 0.987 | 4.197 |
| `Calculator::Ast` | **0.450** | 1.041 | 0.600 | 0.494 | 0.580 |
| `Calculator::Interpreter` | **0.255** | 1.248 | 0.901 | 1.088 | 0.871 |
| `Maze::Generator` | 0.668 | **0.596** | 1.012 | 2.718 | 1.457 |
| `Maze::BFS` | 0.700 | **0.670** | 1.272 | 3.683 | 2.168 |
| `Maze::AStar` | **1.175** | 1.776 | 1.464 | 4.363 | 4.092 |
| `CLBG::Fannkuchredux` | 1.355 | 1.298 | 1.371 | 1.229 | **1.162** |
| `CLBG::Mandelbrot` | 0.982 | **0.981** | 1.025 | 0.998 | 1.013 |
| `CLBG::Nbody` | 1.256 | 1.271 | **1.200** | 1.296 | 1.445 |
| `CLBG::Spectralnorm` | **0.984** | 1.035 | 1.031 | 1.039 | 1.065 |
| `Compress::BWTEncode` | **0.330** | 0.505 | 0.418 | 1.332 | 2.140 |
| `Compress::BWTDecode` | **0.685** | 0.721 | 1.024 | 1.152 | 1.938 |
| `Compress::HuffEncode` | **0.634** | 0.795 | 0.717 | 1.104 | 0.908 |
| `Compress::HuffDecode` | 0.521 | **0.445** | 0.556 | 0.907 | 1.265 |
| `Compress::ArithEncode` | 1.218 | 1.036 | **0.977** | 1.139 | 1.015 |
| `Compress::ArithDecode` | 0.961 | **0.895** | 1.359 | 0.960 | 0.910 |
| `Compress::LZWEncode` | **0.196** | 1.413 | 0.707 | 1.052 | 1.244 |
| `Compress::LZWDecode` | **0.238** | 0.659 | 0.659 | 0.633 | 1.634 |
| `Distance::Jaro` | 0.963 | **0.894** | 0.895 | 1.282 | 0.930 |
| `Distance::NGram` | 0.855 | 2.014 | **0.497** | 0.794 | 0.798 |

### Where Avant loses

Among these five, Avant is the fastest on 27 of the 50 tasks. Against the best of *every* language measured on this machine (C and C++ under both GCC and Clang included, so a wider field than the table above), its time is at or ahead of the best on 25 tasks, the median task is 1.01× the best, and three tasks are more than 1.5× behind. The worst rows by time:

| Task | Avant (s) | Best (s) | Behind by |
| --- | ---: | ---: | ---: |
| `Etc::GameOfLife` | 1.433 | 0.813 (C++, GCC) | 1.76× |
| `Distance::NGram` | 0.855 | 0.497 (Rust) | 1.72× |
| `Etc::Sieve` | 1.450 | 0.920 (C, Clang) | 1.58× |
| `Sort::Merge` | 1.738 | 1.165 (C, Clang) | 1.49× |
| `CLBG::Spectralnorm` | 0.984 | 0.671 (C++, GCC) | 1.47× |
| `Binarytrees::Arena` | 0.435 | 0.306 (C, GCC) | 1.42× |
| `Etc::NeuralNet` | 1.305 | 0.948 (C++, Clang) | 1.38× |
| `Etc::LogParser` | 0.467 | 0.348 (Rust) | 1.34× |

Memory, the same way: at or below the best on 32 tasks, the median task at 0.90× the best, five tasks more than 1.5× above it. The worst rows by peak memory:

| Task | Avant (MB) | Best (MB) | Above by |
| --- | ---: | ---: | ---: |
| `CLBG::Spectralnorm` | 6.1 | 2.7 (C, GCC) | 2.21× |
| `Binarytrees::Arena` | 51.3 | 26.7 (C, Clang) | 1.92× |
| `Etc::LogParser` | 17.7 | 9.9 (C, GCC) | 1.78× |
| `Etc::GameOfLife` | 53.7 | 30.4 (Go) | 1.77× |
| `Sort::Self` | 11.3 | 7.1 (C, Clang) | 1.59× |

Crystal and Go are compiled and garbage-collected, as Avant is, so Avant should never be behind either of them on any task, in time or in memory. It is behind on twelve, and each is treated as a defect until it is fixed:

| Task | Behind |
| --- | --- |
| `Etc::GameOfLife` | time 1.46× Go; memory 1.77× Go |
| `Binarytrees::Arena` | time 1.20× Go |
| `Compress::ArithEncode` | time 1.07× Crystal, 1.20× Go |
| `CLBG::Fannkuchredux` | time 1.10× Crystal, 1.17× Go |
| `Etc::Words` | time 1.15× Crystal |
| `Brainfuck::Array` | time 1.15× Go |
| `Sort::Merge` | time 1.09× Go |
| `Distance::NGram` | time 1.08× Crystal, 1.07× Go |
| `Compress::ArithDecode` | time 1.06× Go |
| `Etc::NeuralNet` | time 1.04× Crystal |
| `Distance::Jaro` | time 1.04× Go |
| `Sort::Quick` | time 1.01× Go |

These come from one run; ratios within a few percent may be that run's noise.

One weakness these tables understate is allocation-heavy code. `Binarytrees::Arena`, which allocates most, is behind in both time and memory; the two rows that would be worse still are fast only because they are hinted; and [What allocation costs today](#what-allocation-costs-today) says what to expect and what to do meanwhile.

### HTTP and WebSocket servers

The most basic server in each language: `GET /` answered with `hello`, over keep-alive connections. Every server is an optimized build, and all eight were measured in one session. The sources and the script are in `bench/http/`.

| Server | Requests per second | Peak memory |
| --- | ---: | ---: |
| Avant (`std.http`) | 491,761 | 7.3 MB |
| C (h2o) | 475,331 | 8.6 MB |
| Rust (std, thread per connection) | 499,582 | 3.3 MB |
| Rust (hyper) | 493,031 | 6.2 MB |
| Go (`net/http`) | 348,106 | 18.6 MB |
| Go (fasthttp) | 426,621 | 11.0 MB |
| Crystal (`HTTP::Server`) | 478,206 | 15.3 MB |
| Valk (`http.serve_fast`) | 472,085 | 12.3 MB |

**Method.** `wrk -t4 -c64 -d10s --latency`, with the load generator on the same machine as the server and no pipelining; one server at a time, three runs per server after a warm-up, the median printed (AMD Ryzen 7 5700G, 2026-11-12).

**This table does not rank its fastest rows.** At this load the load generator is the limit for five of the eight servers (Avant, h2o, both Rust servers and Valk): `wrk` uses all four of its threads for them, so their numbers are level. What the table shows is that none of them is the bottleneck at 64 connections, and what each costs in memory.

Pipelined loads (many requests sent in one write) are not shown. Avant's server answers each pipelined request with its own write today, which is slower than servers that answer a batch with one write; this is being worked on.

The WebSocket echo was not measured in that session. These are the earlier numbers, kept apart so the two tables never mix sessions: messages per second over 8 connections for 10 seconds; Avant's row is the median of three runs (2026-11-10), the other rows one run each (2026-11-04).

| Server | Messages per second | Peak memory |
| --- | ---: | ---: |
| Avant (`std.http`) | 247,803 | 6.9 MB |
| C (h2o) | 251,006 | 8.5 MB |
| Rust (std, thread per connection) | 252,175 | 2.6 MB |
| Go (`net/http`) | 215,594 | 12 MB |
| Crystal (`HTTP::Server`) | 264,813 | 11 MB |

### Reading these numbers

These notes are about [LangArena](#langarena).

- **Build flags.** Avant: `--release --cpu=v3` (x86-64-v3). C and C++: LangArena's `-O3 -march=native -mtune=native` (Zen 3 here, a superset of v3); the C column is Clang, the faster of the two C compilers measured (GCC: 59.499 s, 23.4 MB). Rust (`cargo build --release`), Go and Crystal (`--release`): their defaults.
- **The whole process is timed**: start-up, data preparation, warm-up and the timed iterations. LangArena's published numbers count only the timed iterations.
- **Compiled-in task sizes.** Avant's task configuration is compiled into the program as constants, while the other languages read `run.js` at run time, so LLVM can specialize Avant's loops on the sizes.
- **Rows that are not comparable programs.** The four `Matmul` tasks: Avant's port sums with eight independent accumulators, which vectorizes, and the references use one. `Hash::CRC32`: Avant calls a runtime builtin written in C with slice-by-4 tables, and every reference is the bitwise loop. `Compress::LZWEncode` and `LZWDecode`: Avant keys the dictionary by integer, C by string. Together these seven rows are 7.6 of the 20.7 seconds between Avant's total and C's.
- **Rows that measure C, not Avant.** `Hash::SHA256` and `Hash::CRC32` call runtime builtins that do the whole task in C, where the references implement it in the language; the Burrows-Wheeler transform inside `Compress::BWTEncode` and `BWTDecode` is runtime C too.
- **Rows that carry collection hints.** `Binarytrees::Obj` and `Calculator::Ast` call `gc.hold` and `gc.collect()` ([Collection hints](#collection-hints)) in their Avant source: the same algorithm as the references, with the programmer telling the collector when not to run and when everything is dead. No reference program has an equivalent. Read these two rows as what a hinted program does, not as the collector's ordinary behaviour: without the hints, `Binarytrees::Obj` is several times slower than every language in the table.
- **Library-leaning rows.** `Json::ParseDom` and `Json::ParseMapping` (yyjson in Avant and C; simdjson in C++; serde in Rust), `Base64::Encode` and `Decode` (the aklomp codec in Avant and C), `Template::Regex` and `Etc::LogParser` (PCRE2 in Avant and C, RE2 in C++, the `regex` crate in Rust): the speed is those libraries', reached through Avant's bindings. `Sort::Self` is each language's own library sort, by the task's definition. In `CSV::Parse`, Rust and C++ use CSV libraries; in `Json::Generate`, C and C++ use JSON libraries and Avant writes into its own buffer.
- **Scope.** All 50 of LangArena's tasks. `Etc::TextRaytracer` runs its whole render (0.999 s, level with Rust's 1.019 s); it was left out while an earlier build hoisted that render out of the timed loop.
- **Equivalent programs is the rule**: Avant's tasks are meant to follow the algorithms and the data widths of the C and Rust references, and a row that does not is named above until it is rewritten.

## Goals

Beginners welcome, advanced programmers not blocked, memory managed, errors as values. Simplicity is non-negotiable, and so is not having to choose between productivity and speed.

The bars are measurable, and [LangArena](#langarena) is the public scoreboard:

| Goal | Bar |
| --- | --- |
| Expressiveness | Crystal's density: as little syntax as Crystal to say the same thing |
| Run time and memory | **C and Rust, one to one**: against the best language on each task, a median of 1.0× or better and no task above 1.5× |
| The floor | never behind Crystal or Go, the other compiled, garbage-collected languages, on any task, in time or in memory |
| HTTP | at least as fast as the fastest server measured on the same machine |
| Interop | C libraries usable with little ceremony |
| Concurrency | built in, and obvious |

Where Avant stands today (2026-11-12): on LangArena, the median task is at 1.01× the best language's time and 0.90× its memory, with three tasks beyond the 1.5× bar in time and five in memory, and twelve tasks behind Crystal or Go ([Where Avant loses](#where-avant-loses)). Allocation-heavy code is the weakest area ([What allocation costs today](#what-allocation-costs-today)). On HTTP, Avant's server is level with the fastest servers at 64 connections without pipelining, and slower than the servers that batch their writes when requests are pipelined ([HTTP and WebSocket servers](#http-and-websocket-servers)). On expressiveness, Avant's LangArena sources are 5,482 lines against Crystal's 4,220 (30% more, and 18% more once compressed): not Crystal's density yet, and the forms that will close the gap are listed in [Not built yet](#not-built-yet). Each of these is treated as a defect until it is fixed, and the numbers on this page are printed as measured, the bad rows beside the good ones.

Avant is not a better TypeScript, not a borrow-checker language, not a dynamic language with types bolted on, and not a research language. It does not try to please everyone.

## Installation in depth

[Install](#install) is one command. This section is everything behind it.

### Installer options

```sh
curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
bash install.sh v0.41.0             # a specific version
bash install.sh --no-modify-path    # do not edit shell configuration
```

The script downloads the release archive, checks its SHA-256, installs it into `~/.avant`, adds that directory to `PATH` in your shell's configuration, and compiles and runs a hello program to prove the install works.

| Option | Effect |
| --- | --- |
| a version argument (`v0.41.0`), or `AVANT_VERSION` | install that release instead of the latest |
| `--no-modify-path` | leave shell configuration alone and print the `PATH` line to add |
| `AVANT_INSTALL` | install into another directory than `~/.avant` |

Running it again updates the install in place. It refuses, with a message, on anything but Linux x86-64 with glibc (musl systems included), and it needs `curl`, `tar`, and `sha256sum` or `shasum`.

### Manual install

Download `avant-linux-x86_64.tar.gz` from [Releases](https://github.com/avant-lang/avant/releases), extract it anywhere, and put the extracted `avant-linux-x86_64` directory on your `PATH`. Keep its files together: the compiler finds `re2c`, the standard library and the runtime beside itself. `avant version` prints the release and the commit it was built from, read from the archive's `versions.txt`.

### Requirements

| Need | For | Install |
| --- | --- | --- |
| Linux x86-64, glibc 2.38 or newer | everything | Ubuntu 24.04, Debian 13, Fedora 39, or later |
| a C compiler and the C library's development files | linking every program (and C interop) | `sudo apt install gcc libc6-dev`, or `sudo dnf install gcc glibc-devel` |
| `git` | `avant fetch` only | |
| libclang 20 | `avant bind` only | `sudo apt install libclang1-20` |

Nothing else. The archive carries `avant` with LLVM 20 linked in (there is no separate backend program), `re2c`, `avant-bind`, the standard library, and the runtime with PCRE2, zlib and the other C libraries inside; beside them, `examples/` (the runnable programs this page points to), `versions.txt` (the release and its commit, which `avant version` prints), Avant's `LICENSE`, and `third_party/` with the notices and licenses of those C libraries. No environment variable needs to be set: the compiler turns a program into machine code with the LLVM it carries, and links it with `cc`.

## Contributing

Everything below is for people working on Avant itself, not for writing Avant programs.

### How the compiler works

The compiler is self-hosted: it is written in Avant (`compiler/*.av`) and compiles itself. It lowers the checked program to its own mid-level IR (MIR, `compiler/mir/`), emits LLVM IR from it (`compiler/llvm/`), and compiles that with the LLVM 20 linked into the compiler (`runtime/avant_llvm.c`); `cc` links the object with the runtime (`runtime/`, C).

```text
.av  →  parser and checker (compiler/*.av)  →  MIR  →  LLVM IR  →  in-process LLVM  →  object  →  cc  →  binary
```

This is the only back end. The earlier one, which emitted [myc](https://github.com/kostya/myc) IR for an external `myc-llvm` process, was retired once every program it ran had been frozen as a golden (`tests/goldens/runs/`). The collector is `runtime/avant_gc_*.c`, one file per concern (`runtime/avant_gc_internal.h` lists them; `runtime/avant_gc.h` documents its settings). The compiler's own sources obey the language's rules like any program, field privacy included: no build is exempt.

### Building the compiler

You need a C compiler, the C library's development files and binutils (`ld`, `objcopy`); LLVM 20's static archives (Debian and Ubuntu: `llvm-20-dev`, `libzstd-dev`), which `runtime/build.sh` merges into `runtime/build/avant_llvm.o` (through `tools/build-llvm.sh`) and only the compiler links; `re2c` for regex literals (on `PATH`, or the pinned static build: `sh tools/build-re2c.sh bin/re2c`); and, for the `bind` helper, `llvm-config` and libclang 20 (without them the build skips it with a notice).

The compiler is built by a published release, the seed: **v0.41.0 or later**, unpacked at `bin/v0.43.0` (or wherever `LETTERS_SEED` points). An older release cannot build this tree: `Int` became 64-bit in v0.41.0, and the runtime has one ABI.

```sh
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o   # rebuild with itself
```

`AVANT_ROOT` overrides where a compiler looks for `runtime/build` and `lib/std`; without it, a compiler built by `tools/letters.sh` reads the `lib/` its `OUT.versions.txt` names, and any other (a release, a bare copy) the nearest `lib/` in its own directory or one above it. `avant dump FILE.av` prints a program's MIR. `tools/bind.c` is the `avant bind` translator, built by `runtime/build.sh` into `runtime/build/avant-bind`; in a checkout the compiler runs that one, elsewhere the `avant-bind` beside itself.

### The gate and the suite

| Command | What it proves |
| --- | --- |
| `sh tools/letters.sh` | builds the runtime, B (`bin/avant-b`, built by the seed) and C′ (`bin/avant-c-llvm`, built by B). `sh tools/letters.sh b`, `c-llvm`, `d-llvm` (D′, built by C′) or `c-release` (the `--release` compiler a release ships) builds one. Beside each compiler it writes `OUT.versions.txt`, the build's record that `avant version` prints (the compiler itself carries no version, so it stays byte-identical across commits that do not change it); its `lib:` line names the checkout's `lib/`, which that compiler reads its standard library from when `AVANT_ROOT` is not set, wherever the compiler is copied, and a compiler whose recorded library is gone says so instead of reading another |
| `sh tools/gate.sh` | the full gate: B, C′, the suite on both, D′, and the fixed point (C′ and D′ emit the same LLVM IR for the compiler and are byte-identical) |
| `sh tools/run-goldens.sh` | compiles and runs every example, runnable fixture and LangArena task and compares stdout, stderr and exit status with its golden (`GOLDEN_MODE=--release`, or empty, for the other modes) |

The suite is the native test runner on the compiler's own tests:

```sh
AVANT_COMPILER=./bin/avant-b ./bin/avant-b test
AVANT_COMPILER=./bin/avant-c-llvm ./bin/avant-c-llvm test
```

`tests/cases/test_probe_matrix.av` checks seven of the checker's rules, each shown by a pair of programs (an unsafe one the compiler must refuse, a safe twin it must accept) under every statement and expression wrapper of the language, against the table `tests/probe/matrix.txt`; `AVANT_COMPILER=./bin/avant-c-llvm ./bin/avant-c-llvm run tests/probe_matrix.av` writes the table again, to be committed only after its diff is read.

- Each `tests/cases/test_*.av` is one program; helpers shared by several live in `tests/cases/harness.av`. Run one file with `avant test tests/cases/test_ffi.av`, or one slice with `--shard 2/4`.
- The files listed in `[test] exclusive` (they probe the collector, rewrite `PATH`, use git, or write a coverage report of their own) run alone, in a command without `--shard`; a shard that would include one needs `--skip-exclusive`. `test_fixed_point.av`, which compiles the compiler, is listed in `[test] first` so it starts before the rest.
- For the compiler's suite, `--junit PATH` writes one case per check, with durations (its checks record them, as `std.testing`'s do); output is printed per file as each finishes, and the summary totals the run.
- The compiler's own coverage report is the hit file of an instrumented compiler build, written once after the processes that update it have finished. It is not `avant test --coverage`, which reports the tests.
- Goldens live in `tests/goldens/`.

### Checking this README

Every `avant` block in this file is compiled, and run, by `sh tools/readme-check.sh`, and what the file says a program prints is compared with what it prints; `sh tools/readme-links.sh` checks every anchor and link. Run both after any change here.

- A block with `fn main` is a complete program: compiled, run, and expected to exit 0.
- A trailing `// => text` says the program prints that line there; a `text` block marked `output` or `stderr` states a whole output or a diagnostic.
- Any other block carries a mark in an HTML comment on the line before it (`<!-- check: ... -->`): a fragment completed into a program, an expected compile error, an expected trap, a file of a multi-file project, or a block that is shown and not compiled, with its reason. The header of `tools/readme-check.sh` lists the marks.

The programs of [Testing](#testing) are also exercised by `tests/cases/test_testing_docs.av`.

## License

[MIT](LICENSE).
