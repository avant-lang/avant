# Avant

Avant is a compiled, garbage-collected language aiming for Crystal's density, C's braces and speed near C and Rust. `avant` compiles short, type-inferred code ahead of time, through LLVM, to a native binary; a generational Immix collector manages memory. No borrow checker, no manual `free`, no interpreter, no exceptions.

Avant's bet is that you need not choose between productivity and speed, and its rule is to show the evidence: [Benchmarks](#benchmarks) prints the rows where Avant is behind beside those where it leads.

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
  qty = f[1].to_i() ?? return BadLine { message: "bad quantity: ${f[1]}" }
  cents = f[2].to_i() ?? return BadLine { message: "bad price: ${f[2]}" }
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
  totals = items.parallel (item) { item.total() }
  puts("${items.size} items, ${totals.sum()} cents")
}
```

<!-- check: output -->
```text
skipped: bad quantity: one
2 items, 1260 cents
```

What you just read:

- **No ceremony.** No semicolons, no parentheses around conditions, no `let`; types are written on fields, parameters and results, inferred elsewhere.
- **Errors are values.** `parse` returns `Item | BadLine`; `??` gives a fallback or leaves the function; `item = parse(line) else e { ... }` handles the failure in a block that must leave, after which `item` is the success. Nothing unwinds; an error cannot be dropped by accident.
- **Values and objects.** A `struct` is a value with methods, a `class` (not shown) a reference; you choose per type.
- **Concurrency is built in.** `.parallel` runs the block on a pool of OS threads and returns the results in input order.

A web server is seven lines:

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080) {
    server.get("/", (req) { Response.text("hello") })
    server.serve()
  }
}
```

It has keep-alive connections, one event loop per processor and WebSockets on the same port ([measured here](#http-and-websocket-servers)).

## Install

On Linux x86-64 with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39, or later):

```sh
curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
```

The one prerequisite is a C compiler, to link: `sudo apt install gcc libc6-dev` on Debian and Ubuntu, `sudo dnf install gcc glibc-devel` on Fedora. The installer says if it is missing and ends by compiling and running a hello program. Options, manual install and the archive's contents: [Installation in depth](#installation-in-depth).

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

The two build modes are compared in [Build modes](#build-modes).

Avant is pre-1.0: the language changes between releases ([CHANGELOG](CHANGELOG.md)); what is decided but not built is in [Not built yet](#not-built-yet) and beside each feature it limits.

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
- [Sets](#sets)
- [Structs and classes](#structs-and-classes):
  - [struct or class](#struct-or-class)
  - [structs](#structs)
  - [classes](#classes)
  - [clone](#clone)
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
  - [capture](#capture)
  - [lambdas given to builtins](#lambdas-given-to-builtins)
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
- [Contributing](#contributing)
- [License](#license)

## Basics

A program is functions, types and one `fn main`. Braces are required; no semicolons, no parentheses around conditions. Types are written `name: Type` on parameters, results and fields; locals are inferred.

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

The entry point is `fn main` (or `fn run`), without `pub`. It exits with status 0, or, declared `fn main: Int`, with its result, which must be in `0..255`.

<!-- check: run status=3 -->
```avant
fn main: Int {
  puts("three things went wrong")
  3
}
```

A status outside `0..255` traps (`avant runtime: exit status 256 out of range (0..255)`, status 1), never cut to its low byte.

Declared `fn main: Void | E` or `fn main: Int | E` (every `E` marked `Error`: [Errors](#errors)), a failure it returns or passes up with `?` prints `"${e}"` and a newline to stderr after all stdout, and exits with status 1.

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

`=` binds a new name or assigns an existing one (no `let` or `var`); locals are mutable. A name introduced in a block ends at its `}`; assigning a name of an enclosing block writes that name.

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

- A declaration may state its type (`xs: Array(Int) = []`, `small: Int32 = 5`), and must when the value does not, as an empty `[]`; an argument `[]` takes its parameter's type (`count([])` with `xs: Array(Int)`).
- `name : Type` is accepted; the [formatter](#formatter) prints `name: Type`.
- `_ = expr` computes a value and drops it on purpose; `_` binds nothing and cannot be read.
- A module has no variables, so no two tasks share a global: `count = 0` at module level is a compile error; a value that never changes is a [constant](#constants).

### Constants

`const` names a module-level value; `pub const` exports it. The name is snake_case. The value is a scalar (an integer, a `Float64`, a `Bool` or a [character literal](#literals)) computed at compile time from literals, constants and operators; each use is the value written in place, typed by its place as a literal is.

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

- Overflow, division by zero, a shift outside the width, or a non-scalar value (a `String`, an array, a call; constant strings and tables are not built yet) is a compile error at the constant. A constant whose value does not fit its place gets the literal's error, naming it (`the constant big = 3000000000 does not fit in Int32`).
- Constants may use each other in any order; one that refers to itself is an error.
- A constant may label a `switch` arm or bound a range label (`case max_depth`, `case lo..=hi`); any other name there is an error.
- A constant cannot be assigned (`max_depth = 3`, `max_depth += 1` and `for max_depth in ...` are compile errors); there is no `const` in a function. A parameter or a binding (`if v = ...`, `case n: Int`, a block's `|x|`) may take a constant's name in its body.
- A bare name is, in order: a local or parameter, a field of the receiver, a method of the receiver, a constant, a function. Another module's `pub const` is reached bare or as `module.name`; two constants of one name a module sees (its own included) are ambiguous: qualify it ([Name clashes](#name-clashes)).

### Functions

`fn name(params): Result { body }`; no result type means `Void`, and result types are never inferred.

- A function without parameters is declared without parentheses (`fn answer: Int`; `fn answer(): Int` is refused with that hint), a `lib` block's C function too (`fun avant_gc_collect`), and called with them (`answer()`); a bare `answer` is the function as a value.
- **Every method call takes parentheses**, argumentless and builtin ones included (`q.sum()`, `s.strip()`, `xs.sort()`, `Point.new()`), so a mutation never reads as a field. After a dot, a name without them is a field or one of two builtin read-only properties: `size` (of a `String`, `Bytes`, `Buf`, `Array` or `Hash`) and `bytes` (a `String`'s bytes as `Bytes`). `xs.sort` says `a method call takes parentheses: write sort()`; `s.size()` says `size is read without parentheses: write .size`. `avant format --calls FILE...` adds missing parentheses ([Formatter](#formatter)).

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
- A call's unused value is dropped, at the end of a `Void` function too (an error never silently: [Errors cannot be dropped](#errors-cannot-be-dropped)). A line that computes a value without a call (`x + 1`, a name, a literal, a field read) is an error; at the end of a function with no result type the error says to declare one (`fn two { 2 }` is refused: write `fn two: Int { 2 }`). `_ = x + 1` drops a value on purpose.
- Parameters may have trailing defaults (`times: Int = 1`), named at the call after the positional arguments (`pad("ab", fill: "*")`); functions may be [overloaded](#overloads-and-defaults) and [generic](#generic-functions).
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
| `Set(T)` | a set of distinct values | [Sets](#sets) |
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

`&`, `^` and `|` bind tighter than comparisons: `x & 1 == 0` means `(x & 1) == 0`. Assignment is a statement: `=`, and `x op= v` for every binary operator (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `&+=`, `&-=`, `&*=`), which is `x = x op v` reading the place once. No `++`, no ternary (`if` is an expression), no `**`.

### Equality and order

`==` and `!=` compare two values of one type; different types, an integer and a `Float64` included, are a compile error: convert (`n.to_f() == x`, or `n == x.to_i()`, which truncates), so no precision is lost silently above 2^53. A literal takes the other side's type (`x == 2`).

- Numbers, `Bool`, `String`, `Bytes` and enums compare by value; a `Float64` by IEEE, so NaN equals nothing, itself included.
- A struct compares its fields in order, each with its own `==`, unless it defines `fn equals(other: T): Bool`, which `==` then calls (with its `fn hash`: [Key and value types](#key-and-value-types)); one with a field that has no `==` (a `Hash`, a function) has none either, and the error names the field.
- A class compares by identity (the same object) unless it defines `fn equals`.
- An array equals another when the sizes are equal and the elements are, in order.
- An optional equals another when both are `nil` or both hold equal values; it is never compared with `nil`: open it ([Opening an optional](#opening-an-optional)).
- A `Hash`, a function and a union of several types have no `==`; take a union apart with [`switch`](#unions).
- `!=` is always `!(a == b)`.

`<`, `<=`, `>` and `>=` order numbers and `String`s (byte by byte, a prefix first). A struct or a class that defines `fn compare(other: T): Int` (negative, zero or positive) gets `<`, `<=`, `>` and `>=` from it, and `sort`, `sorted`, `min` and `max` of its arrays use it, so its orders cannot disagree. `a.equals(b)` and `a.compare(b)` can be called as written too.

```avant
struct Point {
  x: Int
  y: Int
}

struct Version {
  major: Int
  minor: Int

  fn compare(other: Version): Int {
    if major != other.major {
      return major - other.major
    }
    minor - other.minor
  }
}

fn main {
  p = Point { x: 1, y: 2 }
  puts(p == Point { x: 1, y: 2 })        // => true
  puts([p] != [Point { x: 2, y: 1 }])     // => true
  puts("apple" < "banana")               // => true
  vs = [Version { major: 1, minor: 4 }, Version { major: 1, minor: 2 }]
  puts(vs[0] >= vs[1])                    // => true
  if v = vs.min() {
    puts("${v.major}.${v.minor}")         // => 1.2
  }
}
```

### Printing

`puts(x)`, `x.to_s()` and every [writer](#output) give exactly `"${x}"`: integers in decimal, a `Float64` as in [Floats](#floats), `true` and `false`, a `String` itself, an enum its member's name, an error its text (message and cause), a union its member; an optional is opened first. A struct, class or enum may define `fn to_s: String` to give its text (not a type marked `Error`: its text is its message).

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
  puts(42.to_s() + "/" + 2.5.to_s())                       // => 42/2.5
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

A line starting with `[`, `-` or `+` always begins a new statement; an expression left dangling by that (`t = a` then a line `- b`, an unused line `[a, b]`, a line starting with `+`) is a compile error that says how to continue the line.

### Comments

`//` runs to the end of the line. There is no `/* */`.

A comment starting `// file: ` is reserved: it changes the path and line that diagnostics report (it is not an import).

## Control flow

`if`, `while`, `for` and `switch` take braces and no parentheses; `if` and `switch` are expressions.

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

An `if` with an `else` is an expression: each branch ends in a value of one type, or leaves (`return`, `break`, `continue`).

```avant
fn main {
  n = 12
  label = if n > 10 { "big" } else { "small" }
  puts(label)   // => big
}
```

A final `if`/`else` is the function's value (`classify` above); an `if` without `else` is a statement. Conditions are `Bool`, with no truthiness: `if n` on an `Int` is a compile error. A struct literal in a condition needs parentheses: `if p == (Point { x: 1, y: 2 }) { ... }`.

### Conditions that bind

`if v = expr { ... }` runs the block when `expr` gives a value (not `nil`, not an error), named `v` in it.

```avant
fn main {
  ports = Hash(String, Int).new()
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

- The bound name exists only in the then-block. With one binding, `else e { ... }` names the failure in the `else` ([Reading the error itself](#reading-the-error-itself)).
- It is an expression like any `if`: `width = if v = cfg.get("width") { v } else { 80 }`.
- A condition may be a comma list of bindings and `Bool` tests, evaluated left to right until one fails, each name in scope for the later ones; the `else` runs when any fails and sees none of the names.
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

`while v = x { ... }` runs while `x`, evaluated before each iteration, gives a value, bound to `v` in the body:

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

- **`..` excludes its end** (`for i in 1..n` never reaches `n`); **`..=` includes it** (`for i in 1..=n` ends with `n`, even the largest `Int`, without overflow).
- The bounds are evaluated once. The loop variable is a new name, only inside the loop: a name that already exists there (a local, a parameter, an outer loop's variable) is a compile error, since the loop would otherwise assign it at every step and leave its last value behind; give the loop's variable a name of its own. Two loops one after the other may use the same name. `break`, `continue` and `return` work as in `while`.
- **Over an array of structs the loop variable is the element itself**, in place: `p.x = 1`, `p.pos.x += 1` and a method that changes its receiver (`p.step()`) change the element in the array, as a *class* element's field changes its object. A copy is still a copy (`q = p`, an argument, `ys.push(p)`). While the loop runs, the array's size and order cannot change through its own name (`xs.push`, `pop`, `clear`, `reserve`, `sort` and `reverse` there are compile errors; collect the changes and apply them after the loop); a change through another name that removes the element stops the program at the variable's next use, as a bad index does. A method call on the variable (`p.step(b)`) that may reach the array freezes it for the call ([Structs](#structs)). `p = other` replaces the element in the array, as `xs[i] = other` does. The variable cannot be captured by a lambda or a task (copy it first: `q = p`). The parameter of `each`, `any` and `all` over an array of structs is the same place.
- **Any other loop variable cannot be assigned** (it is a fresh copy at each step): `i = 5` and `x += 1` are compile errors. Store through an index (`xs[i] = v`), or use `while` for a range that skips or repeats steps.
- A walk over an array of plain values (numbers, strings, classes; `for`, `each`, `any`, `all`) reads each element when it reaches it, and reads the size again at each step: an element pushed during the walk is visited, and a `pop` or `clear` ends the walk sooner. A `sort` or `reverse` of the walked array through its own name inside the walk is a compile error (the walk would read some elements twice and miss others); sort before or after the loop.

```avant
struct Ball {
  x: Int
  dx: Int

  fn step {
    self.x += self.dx
  }
}

fn main {
  balls = [Ball { x: 0, dx: 1 }, Ball { x: 10, dx: -2 }]
  for b in balls {
    b.step()
    b.dx *= 2
  }
  balls.each (b) { b.x += 100 }
  puts("${balls[0].x} ${balls[1].x}")   // => 101 108
  puts(balls[1].dx)                     // => -4
}
```

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

Over an enum:

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

A label may be an integer range, as in `for` (`lo..hi`, `lo..=hi`); a character literal is an integer:

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

Over a `String`, the match is byte equality:

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

Over a union, `case n: Int` binds the value as an `Int` in that arm, `case String` names a member without binding it, `case nil` names `nil`:

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

| The value | Its labels |
| --- | --- |
| an integer of any width: `Int`, `Int32`, `UInt32`, `UInt64` | integer literals, character literals included, or [constants](#constants), each taking the value's type: `case 3000000000` over an `Int32` is `case label 3000000000 does not fit in Int32` |
| an enum | its members, bare: `case eof`, not `case Tok.eof` |
| a `String` | string literals, not interpolated |
| a union | its member types, with or without a binding, and `nil`; a binding is the arm's only label |
| a `Bool` | `true` and `false` (`case 1` is refused with the hint `case true`) |

- No two labels share a value (`case 5` beside `case 0..=9` is `case 5 overlaps case 0..=9`); an empty range (`case 5..5`) is an error. A range label costs two comparisons, as the `if` chain it replaces.
- Over an enum, a union (its failures too) or a `Bool`, a switch without `else` names every member (`true` and `false` for a `Bool`), as a statement and as a value, so a new member is reported at each switch that misses it (`switch over Tok does not cover name, number: name them or add else`); an `else` beside every member is refused. Over other types, a switch used as a value needs `else`; as a statement, a value no arm names does nothing.
- An arm may leave instead of giving a value (`return`, `break`, `continue`); it still counts as naming its members.
- As a statement, a switch's arms are statements, as an `if`'s: an arm's final value is dropped, unless it can be an error ([Errors cannot be dropped](#errors-cannot-be-dropped)).

## Enums

An `enum`'s values are a fixed list of lower-case names, each with a number, written or counted (the first 0, each next the previous plus one).

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
  puts(t.to_i())                       // => 1
  if u = Tok.from_i(2) {
    puts(u == Tok.number)            // => true
  }
  puts(Tok.from_name("eof") ?? Tok.name)   // => eof
  puts(Status.created.to_i())          // => 201
  puts(Tok.members().size)             // => 3
}
```

- A member is written qualified, `Tok.eof`, everywhere except a [`case` label](#switch). Members are separated by commas or line breaks (a trailing comma allowed) and named in lower-case snake_case; a capital first letter is refused.
- An enum is its own type, not an `Int`: it has `==` and `!=`, no arithmetic and no `<`.
- What every enum has: `t.to_i()` (its number), `t.name()` (the member's exact name, as a `String`), `Tok.from_i(n)` and `Tok.from_name(s)` (a `Tok?`: `nil` when no member has that number or that exact name) and `Tok.members()` (a new `Array(Tok)` in declaration order). It prints as its name ([Printing](#printing)).
- Values are distinct and fit in `Int32`; a value may be negative (`low = -2`) or a [constant](#constants) (`ok = http_ok`; the members after it count from its value).
- An enum has no zero value: an enum field has a default (`kind: Tok = Tok.eof`) or is set in `initialize`, and `Array(Tok).new(n)` needs its value: `Array(Tok).new(n, Tok.eof)`.
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
  puts(Dir.west.turn())      // => north
}
```

- An enum takes one byte when its values fit `0..255` or `-128..127`, two within `0..65535` or `-32768..32767`, else four; crossing to C it is C's `int`. The members' names are stored only in a program that prints an enum, reads `name` or calls `from_name`.
- An enum marked `: Error` is a failure type ([An enum as an error](#an-enum-as-an-error)); one inside a `lib` block is a C enum ([avant bind](#avant-bind)). An enum has no payload: a member that carries data is a [union](#unions) member.

## Nil and unions

`T?` is a `T` or `nil`. The compiler makes you open it before use, so there is no null pointer error at run time (except a C pointer, [`Ptr(T)`](#layouts-and-ptr), whose NULL traps with a message).

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

These forms open a `T?`, and a result that can fail the same way ([Errors](#errors)).

| Form | Gives | Use it when |
| --- | --- | --- |
| `x?` | the value, or returns `nil` from the current function | the caller should deal with it |
| `x ?? fallback` | the value, or `fallback` (evaluated only when needed) | there is a sensible default |
| `x ?? return`, `?? return v`, `?? break`, `?? continue` | the value, or leaves | absence ends this function or iteration, and the rest should not nest |
| `if v = x { ... } else { ... }` | `v` inside the block | absence needs its own code path |
| `x.or(fallback)` | the same as `x ?? fallback` | you prefer a method chain |
| `x.expect("why")` | the value, or stops the program printing `why` and the call's position | absence is a bug: the message says what the program assumed |

```avant
fn port_of(cfg: Hash(String, Int)): Int {
  cfg.get("port") ?? 8080
}

fn double_port(cfg: Hash(String, Int)): Int? {
  p = cfg.get("port")?        // nil here returns nil from double_port
  p * 2
}

fn main {
  cfg = Hash(String, Int).new()
  puts(port_of(cfg))                 // => 8080
  puts(double_port(cfg) ?? 0)        // => 0
  cfg["port"] = 9000
  puts(double_port(cfg) ?? 0)        // => 18000
}
```

- `x?` compiles only in a function whose result can carry the `nil` (here `Int?`); otherwise the error says what would be returned and where it cannot go.
- `x.expect("why")` traps on `nil` (`avant runtime: why at main.av:7:12`, status 1; in a spawned block it ends the task: [When a task traps](#when-a-task-traps)), its message evaluated only then. It opens only an optional, not a result that can fail.
- There is no `?.`, no `!` or `!!`, and no `x == nil`. To test absence without the value: `if _ = x { ... } else { ... }`.

### Unions

A union is written with `|`: `Int | String`, `Item | BadLine`. `T?` is `T | Nil`, `nil` the only value of `Nil`. Parentheses group a type: `(Int | String)?` is `Int | String | Nil`, `(fn(Int): Int)?` an optional [function value](#function-values).

```avant
class Config {
  port: Int | String = 8080
}

fn main {
  c = Config.new()
  c.port = "auto"
  c.port = 9000
  puts("stored")   // => stored
}
```

The forms above open a result that can fail, `T | SomeError`; any union is taken apart with [`switch`](#switch), one arm per member:

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

`type Name = T` names a type (the same type, not a new one), a union above all; the union's classes may hold the name in their fields, so a type can be recursive (a tree, a JSON value).

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

- An alias is private to its module unless `pub`, starts with a capital letter and takes no type parameters.
- An alias may not reach itself except through a struct or class: `type Json = Int | Array(Json)` is refused; give the recursive part a class, as `Add` above.
- An alias stands where a type is written (a declaration, a field, a parameter, a type argument); a value is built with the type's own name (`Num.new(2)`, not `Expr.new()`).
- `type` is a keyword only at the start of a declaration; elsewhere it is a usable name.

### What an optional costs

`T?` of a class, `String`, `Bytes`, `Array`, `Hash`, `Buf` or `Ptr(T)` is one pointer, `nil` the null pointer, wherever it is stored (`left: Node?` is 8 bytes). `(fn(Int): Int)?` is the function value's two words, `nil` a null code word. Any other union holds a small tag and one slot per non-`nil` member.

A `T?` that nothing assigned is `nil`: an optional field without a default reads as absent.

## Numbers

`Int` is 64-bit and signed on every machine. Overflow and division by zero trap instead of giving a wrong number, and nothing converts between number types silently.

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
  puts(n.to_f() * half)         // => 10.5
}
```

### Integer types

| Type | Size | Range | Use it for |
| --- | --- | --- | --- |
| `Int` | 64-bit signed | ±9.2 × 10^18 | everything by default: counts, sizes, indexes, time, ids |
| `Int32` | 32-bit signed | −2,147,483,648 to 2,147,483,647 | dense storage (`Array(Int32)` takes 4 bytes per element, half an `Array(Int)`); C's `int` |
| `UInt32` | 32-bit unsigned | 0 to 4,294,967,295 | dense storage; C's `unsigned int` |
| `UInt64` | 64-bit unsigned | 0 to 18,446,744,073,709,551,615 | bit patterns, hashes, C's 64-bit unsigned values |

Use `Int32` only to store many: a 32-bit counter would trap at 2.1 billion. An index, a size and a `for` range are always `Int`: with an `Int32` index write `xs[i.to_i()]` (free). Five narrower names (`Int8`, `Int16`, `UInt8`, `UInt16`, `Float32`) exist only at the C boundary ([C types](#c-types)).

### Literals

An integer literal is an `Int` unless its context gives another number type: a declared type, any parameter (generic and operators' included), a field, a result, or an operator's other operand, on either side. So does an expression of literals only (`(2 - 1) + y`, `u: UInt64 = ~0`, `(1 << 63) | u`), and a literal above `Int`'s range passed to a `UInt64` parameter. With a float `f`, `f * 2` and `2 * f` are float arithmetic.

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

- `0x`, `0b` and `0o` begin hexadecimal, binary and octal literals (the prefix in lower case; `0X` also works). No other number starts with 0: `017` is a compile error that suggests `0o17` or `17`.
- `_` may stand between two digits of any literal, a float's included (`1_000.000_5`), and nowhere else: not first, last, doubled, after a prefix or before a suffix.
- A **character literal** is one byte's integer value (`'a'` is 97), typed by its context like any integer literal, with a [string's escapes](#string-literals) and `\'` for the quote. A character of more than one UTF-8 byte (`'é'`, `'\u{e9}'`) is a compile error: write it in a `String`. There is no `Char` type.
- A literal outside its type's range is a compile error wherever the type comes from (`integer literal does not fit in Int32`; above `UInt64`, `integer literal does not fit in UInt64, the widest integer type`), and so is a float literal too large for `Float64` (`1e400`). Float literals need a digit on each side of the point (`0.5`, `2.0`, `1e3`, `1.5e-7`).

### Conversions

Different number types do not mix: `x + y` with an `Int32` and an `Int`, or an `Int` and a `Float64`, is a compile error (`binary operator expects matching types, got Int32 and Int`); only literals adapt. Convert explicitly:

| To | Keep the number, or trap | Reinterpret the bits, never trap |
| --- | --- | --- |
| `Int` | `to_i` | `wrap_i` |
| `Int32` | `to_i32` | `wrap_i32` |
| `UInt32` | `to_u32` | `wrap_u32` |
| `UInt64` | `to_u64` | `wrap_u64` |
| `Float64` | `to_f` (rounds to the nearest float) | |

`to_*` gives the same number or traps (`avant runtime: to_i32 of Int 5000000000 out of range for Int32 at main.av:3:11`); `wrap_*` gives the two's-complement bits, for hashing or binary formats. A `Bool`'s `to_i`, `to_i32`, `to_u32` and `to_u64` give 1 for `true`, 0 for `false` (`alive += cell.to_i()`).

```avant
fn main {
  big = 5000000000
  minus = -1
  puts(big.wrap_i32())        // => 705032704
  puts(minus.wrap_u32())      // => 4294967295
  puts(minus.wrap_u64())      // => 18446744073709551615
  f = 2.9
  puts(f.to_i())              // => 2
}
```

`Float64.to_i` truncates toward zero and traps on NaN, an infinity, or a value outside `Int` (`avant runtime: to_i of Float64 30000000000000000000.0 out of range for Int`). From text, `"42".to_i()` returns an optional ([Parsing numbers](#parsing-numbers)); to text, `"${n}"` or `to_s` ([Printing](#printing)), or `fmt_float(x, digits)` for fixed decimals.

### Arithmetic and overflow

An operation gives the mathematically correct value, or traps when none exists.

- `+`, `-`, `*` trap on overflow at the operands' width; `/` and `%` trap on division by zero. `abs` of the minimum value, a `sum` that overflows and a `Hash` `inc` that overflows trap the same way.
- `/` truncates toward zero and `%` takes the sign of the dividend (`-7 / 2` is `-3`, `-7 % 2` is `-1`). `MIN % -1` is `0`; only `MIN / -1` overflows.
- `&+`, `&-`, `&*` wrap on overflow. Wrapping is always spelled: an unsigned value has no unary minus (`-u` with `u: UInt64` is a compile error, as `-1u64` is); its two's complement is `0u64 &- u`.
- `<<` and `>>` take an amount in `0..63` for `Int` and `UInt64`, `0..31` for `Int32` and `UInt32`, an `Int` or the shifted value's type. Any other amount, negative included, traps (`avant runtime: shift amount 64 out of range for Int (0..63)`); a literal one is a compile error.
- Integers have `abs` (not unsigned types) and `to_f`.

A check is a compare and a never-taken branch, dropped where the compiler proves it: a literal index into a literal, a loop variable under its bound, a loop whose index range is validated once on entry. What a trap prints: [Traps](#traps).

### Floats

`Float64` has `abs`, `floor`, `sqrt`, `exp` and `to_i`, and the prelude has `sqrt(x)` and `exp(x)`. Float arithmetic follows IEEE 754 and never traps: `1.0 / 0.0` is `inf`.

The rest of C's math library is a `lib` block away, with no flag ([C interop](#c-interop)):

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

A float prints as the shortest text that reads back as the same number, with `.0` on a whole value: `0.30000000000000004`, `2.0`, `1e21`, `1.5e-7`, `-0.0`, `nan`, `inf`, `-inf`; JSON too (`null` for NaN and the infinities). For fixed decimals, `fmt_float(x, digits)` returns a `String` and `buf.push_fmt_f(x, digits)` appends to a buffer; `digits` outside 0 to 64 traps.

```avant
fn main {
  x = 2.0
  puts(x.sqrt())                 // => 1.4142135623730951
  puts(fmt_float(x.sqrt(), 3))   // => 1.414
  puts(x * 3.0)                // => 6.0
}
```

## Strings

A `String` is immutable bytes, normally UTF-8. `${expr}` interpolates, `+` concatenates, and every operation that changes a string returns a new one.

```avant
fn main {
  name = "Ada"
  line = "hello ${name}, you have ${2 + 3} messages"
  puts(line)                          // => hello Ada, you have 5 messages
  puts(line.size)                     // => 30
  puts("  padded ".strip() + "!")       // => padded!
  words = "to be or not".split(" ")
  puts(words.size)                    // => 4
}
```

### String literals

- `"${expr}"` interpolates integers, floats, `Bool`s, strings, enums, [errors](#declaring-an-error) and unions of them ([Printing](#printing)). A `$` that is not followed by `{` is a dollar sign: `"it costs $5"`.
- Escapes: `\n`, `\t`, `\r`, `\\`, `\"`, `\$`, `\0`, `\xNN` (one byte, two hex digits; may make a string that is not UTF-8) and `\u{N}` (a code point, one to six hex digits, stored as UTF-8: `"caf\u{e9}"`). Any other escape is a compile error (no `\e`: write `\x1b`).
- A raw string `r"..."` has no escapes and no interpolation: `r"C:\tmp"`.
- A literal is one line: no multi-line or backtick strings. A byte is a [character literal](#literals), `'A'`, or its number, `65`.

### Bytes, not characters

`size` is the byte length ([Size limits](#size-limits)), and `s[i]` is the byte at `i` as an `Int`. UTF-8 is checked only when you ask for characters: `s.chars()` returns one `String` per code point and `s.code_points()` each code point's number, and both trap on invalid UTF-8 (`avant runtime: String.chars needs valid UTF-8`); `String.from_utf8(bytes)` returns `nil` for it. `String.from_code_point(n)` is the UTF-8 of one code point; a number outside `0..0x10FFFF`, or a surrogate (`0xD800..0xDFFF`), traps.

```avant
fn main {
  s = "né"
  puts(s.size)             // => 3
  puts(s[0])               // => 110
  puts(s.chars().size)       // => 2
  puts(s.chars()[1])         // => é
  puts(s.code_points()[1])   // => 233
  puts(String.from_code_point(8364))   // => €
}
```

A string knows its length (`size` is a read, never a scan), and a long slice shares its string's bytes instead of copying them: `str_slice(s, start, stop)` of 64 bytes or more that is at least a quarter of `s` keeps `s` alive as long as the slice lives; a shorter slice is a copy.

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
| `s.fields()` | split on runs of whitespace, empty fields dropped |
| `s.strip()`, `s.strip_left()`, `s.strip_right()` | whitespace trimmed |
| `s.chars()` | `Array(String)`, one per code point |
| `s.code_points()` | `Array(Int)`, each code point's number |
| `s.bytes` | the same bytes as [`Bytes`](#bytes-and-buf) |
| `s.to_i()`, `s.to_u64()`, `s.to_f()` | [parsed numbers](#parsing-numbers), optional |
| `str_slice(s, start, stop)` | the bytes `start` up to `stop`; traps unless `0 <= start <= stop <= size` |
| `chr(b)` | a one-byte string; `b` must be in `0..255` |
| `repeat_byte(b, n)` | `n` copies of the byte `b` |
| `bytes_to_str(xs)` | a string from an `Array(Int)` of bytes |
| `String.from_utf8(bytes)` | `String?`: `nil` when the bytes are not valid UTF-8 |
| `String.from_code_point(n)` | the UTF-8 of one code point; traps outside `0..0x10FFFF` and on a surrogate |

`String` has no `contains`, `starts_with`, `index_of`, `replace`, `join` or case conversion yet ([Not built yet](#not-built-yet)); [`std.regex`](#stdregex) finds, replaces and splits.

### Parsing numbers

`s.to_i()`, `s.to_u64()` and `s.to_f()` read the *whole* string as a number and return `Int?`, `UInt64?` and `Float64?`; anything else is `nil`.

```avant
fn main {
  puts("42".to_i() ?? 0)          // => 42
  puts("-17".to_i() ?? 0)         // => -17
  puts("12abc".to_i() ?? -1)      // => -1
  puts(" 42".to_i() ?? -1)        // => -1
  puts("ff".to_i(16) ?? -1)     // => 255
  puts("2.5".to_f() ?? 0.0)       // => 2.5
  puts("1e3".to_f() ?? 0.0)       // => 1000.0
}
```

- An integer is an optional sign and digits within the type's range, nothing else: `"0x1F"`, `" 42"` and `"9223372036854775808"` (one past `Int`) are `nil`.
- `s.to_i(base)` and `s.to_u64(base)` take a base from 2 to 36, with letters in either case.
- A float has the shape of a float literal (`-2.5`, `1e3`, `1.5E-7`) and a finite value: `"nan"`, `"inf"`, `"1e999"`, `".5"` and `"1."` are `nil`.
- `str_to_f_slice(s, start, stop)` parses a slice the same way without building the substring.

### Bytes and Buf

`Bytes` is immutable binary data; `Buf` is a growable byte buffer, to build text or a payload without a new string per piece.

```avant
fn main {
  out = Buf.new()
  for i in 1..4 {
    out.push_str("item ")
    out.push_fmt_i(i)
    out.push_byte(10)               // a newline
  }
  text = out.to_s()
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
| `b.chars()` | as `String.chars` |
| `b.to_buf()` | a `Buf` holding a copy |

| `Buf` operation | Result |
| --- | --- |
| `Buf.new()`, `Buf.new(capacity)`, `buf_new()` | an empty buffer |
| `push_str(s)`, `push_bytes(b)`, `push_byte(n)` | append a string, `Bytes`, or one byte (`0..255`, else a trap) |
| `push_slice(s, start, stop)` | append part of a string without building the substring |
| `push_fmt_i(n)`, `push_fmt_f(x, digits)` | append a formatted integer or float |
| `buf.size`, `buf[i]`, `buf.get(i)`, `buf[i] = n` | read and overwrite bytes |
| `starts_with(s)` | `Bool` |
| `clear` | empty it, keeping the storage |
| `to_s`, `to_bytes` | a `String` or `Bytes` of the contents |

A byte stored by `push_byte`, `buf[i] = n`, `chr`, `repeat_byte` or `bytes_to_str` must be in `0..255` (`avant runtime: byte 300 out of range (0..255)`); `n & 255` keeps the low byte on purpose. Many library calls have an `_into(buf)` twin writing into your buffer instead of allocating ([Standard library](#standard-library)).

## Arrays

`Array(T)` is a growable array. A literal's element type is the type of its elements.

```avant
fn main {
  xs = [3, 1, 2]
  xs.push(5)
  puts(xs.size)          // => 4
  puts(xs[0])            // => 3
  xs[0] = 4
  xs.sort()
  puts(xs[0])            // => 1
  puts(xs.sum())           // => 12
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
  f = Array(Node).new(2, (i) { Node.new() })                  // two elements, each its own Node
  g = Array(Array(Int)).new(3, (y) { Array(Int).new(4) })   // a grid of 3 rows of 4
  g[0][0] = 7
  puts(g[1][0])                       // => 0
  puts(a.size + b.size + c.size + d.size + e.size + f.size + g.size)   // => 17
}
```

- `Array(T).new(n)` makes `n` elements, each the element type's zero: `0`, `0.0`, `false`, `""`, `nil` for any `T?`, a struct of such fields.
- `Array(T).new(n, value)` makes `n` copies of `value`: a number, a `Bool`, a `String`, `Bytes`, a struct of those, or `nil` for any `T?`. A `T` that is or holds a mutable object (a class, an `Array`, a `Hash`, a `Buf`, a struct or optional holding one) is a compile error, since every element would be that one object.
- `Array(T).new(n, (i) { ... })` runs the block once per index, `0` to `n - 1` in order, its value the element: the form for a `T` with no zero (`Array(Node).new(n)` alone is a compile error naming the ways out), for a grid's separate rows, and for one shared object on purpose (`Array(Node).new(n, (i) { shared })`). `return` in the block returns from the enclosing function, as in `each`; `break` and `continue` are compile errors.
- A negative `n` traps (`avant runtime: Array size -5 is negative`).

### Indexing and bounds

`xs[i]` traps outside `0 <= i < size` ([Traps](#traps)); `xs.get(i)` returns the element or `nil`. `String`, `Bytes` and `Buf` follow the same rule.

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
| `xs.pop()` | remove and return the last element, as `T?`: `nil` on an empty array (`while v = stack.pop() { ... }`); `xs.pop().expect("why")` where the array cannot be empty. The emptied slot is cleared, so the array keeps nothing alive |
| `xs.clear()` | remove every element |
| `xs.reserve(n)` | make room for `n` elements ahead of time |
| `xs.fill(v)` | set every element (integer and `Bool` arrays) |
| `xs.each (x) { ... }`, `for x in xs` | walk the elements; over structs, `x` is the element in place ([For](#for)) |
| `xs.sort()`, `xs.sort (a, b) { a < b }` | sort in place: by the element's order, or by a lambda that says whether `a` comes first |
| `xs.sorted()`, `xs.sorted (a, b) { ... }` | the same, as a new array |
| `xs.reverse()` | reverse in place |
| `xs.contains(v)`, `xs.index_of(v)` | `Bool`; `Int?`, the first position; by the elements' `==` ([Equality and order](#equality-and-order)), so a struct compares its fields and an array its elements, and an element type without `==` is refused |
| `xs.min()`, `xs.max()` | `T?`: `nil` for an empty array |
| `xs.sum()` | the total (numeric arrays); traps on overflow |
| `xs.any (x) { ... }`, `xs.all (x) { ... }` | `Bool`, stopping at the first answer |
| `xs.parallel (x) { ... }` | a new array, computed [in parallel](#parallel) |

- The element order used by `sort`, `min` and `max` exists for integers, `Float64` (NaN after every number), `String` and `Bytes` (byte order) and `Bool`. Any other element type sorts with a comparator block. On floats, `min` is the smallest number (NaN only when every element is NaN) and `max` is NaN when any element is.
- No `map`, `select`, `reduce` or `join` is built in yet: write a loop, or a three-line generic function ([Lambdas](#lambdas)).
- The lambdas given to `each`, `any`, `all`, `sort` and `parallel` are compiled as inlined loops; `return` in one ends that element's turn, and a `for` leaves early ([Lambdas given to builtins](#lambdas-given-to-builtins)).
- The parameters of a comparator (`sort`, `sorted`) and of a `parallel` body are copies of the elements: writing one, or calling a method on it that changes its receiver, is a compile error (a comparator only compares; a `parallel` body returns the new element). A block given to a function of your own is a function value: its parameters are copies, as any function's are.

```avant
fn main {
  words = ["pear", "fig", "banana"]
  by_length = words.sorted (a, b) { a.size < b.size }
  puts(by_length[0])                          // => fig
  puts(words.index_of("fig") ?? -1)           // => 1
  puts(words.any (w) { w.size > 5 })          // => true
  puts(words.max() ?? "")                       // => pear
}
```

### Size limits

A `String`, `Bytes` or `Buf` holds at most 2,147,483,647 bytes, an `Array` 2,147,483,647 elements (32-bit sizes), a `Hash` 1,073,741,824 slots. Growing past a limit traps before anything is written (`avant runtime: Buf would exceed 2147483647 bytes`, `Hash would exceed 1073741824 slots`); `file_read` of a larger file is an `IoError` (`File too large`).

## Hashes

`Hash(K, V)` maps keys to values ([Key and value types](#key-and-value-types)). A lookup returns `V?`: a missing key is `nil`, never a default or a crash.

```avant
fn main {
  stock = ["tea": 12, "milk": 3]     // Hash(String, Int)

  if n = stock["tea"] {
    puts(n)                          // => 12
  }
  puts(stock["cake"] ?? 0)           // => 0
  stock.inc("milk")
  stock.inc("milk", 10)
  puts(stock["milk"] ?? 0)           // => 14
  puts(stock.size)                   // => 2
}
```

### Hash operations

| Operation | What it does |
| --- | --- |
| `["tea": 12, "milk": 3]` | a hash of these entries, set in order (a repeated key keeps its last value); its types are the first entry's, or the declared type's |
| `Hash(K, V).new()`, `[:]` | an empty hash; `[:]` takes its type from a declaration or a parameter (`h: Hash(String, Int) = [:]`) |
| `h[k] = v` | insert or replace |
| `h[k]`, `h.get(k)` | `V?` |
| `h[k] ?? default` | the value, or `default` for a missing key (one lookup) |
| `h.has(k)` | `Bool`: whether `k` is a key |
| `h.delete(k)` | remove the key; returns the value it held, or `nil` |
| `h.inc(k)`, `h.inc(k, by)` | add to a counter, starting from 0, and give its new value (integer values only; `by` has the value's type); traps on overflow |
| `h.size`, `h.clear()` | number of keys; remove all |
| `h.keys()`, `h.values()` | arrays, in unspecified order |
| `h.each (k, v) { ... }` | walk every key with its value |

```avant
fn main {
  count = Hash(String, Int).new()
  for w in "the cat and the hat and the bat".split(" ") {
    count.inc(w)
  }
  puts(count["the"] ?? 0)                 // => 3
  if old = count.delete("cat") {
    puts(old)                             // => 1
  }
  keys = count.keys()
  keys.sort()
  puts(keys[0])                           // => and
}
```

- **Iteration order is unspecified** (stable for one program); sort the keys when order matters. Insertion order is decided, not built.
- **Changing a hash during its walk:** assigning an existing key's value, deleting a key (the one being visited or another) and `clear()` are allowed, and a key deleted before the walk reaches it is not visited. Adding a key, through the walked name or any other, would be missed by the walk, so it stops the program after that step (`avant runtime: a key was added to a Hash during its walk, …`); collect the new entries and add them after the walk.
- Without building the key, `h.inc_slice(s, start, stop)` and `h.get_slice(s, start, stop)` look up a slice of `s`, and `h.get_concat(a, b)` looks up `a + b`.

### Key and value types

A key is an integer of any width (`Int`, `Int32`, `UInt32`, `UInt64`), a `Bool`, an enum, a `String`, or a struct whose fields are all of those (or such structs); a value any of those, a `Float64`, a class, an `Array`, a `Hash`, `Bytes`, a `Buf` or any struct. Numbers are stored at their own width, unboxed: a `Hash(Int32, Int32)` entry is 8 bytes, a `Hash(Int, Int)` one 16.

```avant
enum Suit { clubs, hearts }

struct Cell {
  x: Int
  y: Int
}

class Room {
  pub name: String

  fn initialize(name: String) {
    self.name = name
  }
}

fn main {
  seen = Hash(Int32, Int32).new()
  for x in [7, -2, 7, 7] {
    seen.inc(x.to_i32())
  }
  puts(seen[7.to_i32()] ?? 0.to_i32())        // => 3
  names = Hash(Int, String).new()
  names[404] = "not found"
  puts(names[500] ?? "unknown")           // => unknown
  score = Hash(Suit, Float64).new()
  score[Suit.hearts] = 2.5
  puts(score[Suit.hearts] ?? 0.0)         // => 2.5
  walls = Hash(Cell, Bool).new()
  walls[Cell { x: 1, y: 2 }] = true
  puts(walls.has(Cell { x: 1, y: 2 })) // => true
  rooms = Hash(String, Room).new()
  rooms["hall"] = Room.new("Hall")
  if r = rooms["hall"] {
    puts(r.name)                          // => Hall
  }
}
```

- A struct key's hash and `==` come from its fields, as the struct's own `==` does when it defines none: each field in turn, a `String` by its bytes.
- A struct that defines its own equality (`fn equals`) defines its own hash too, `fn hash(h: Hasher)`, which adds to the hasher the values that `equals` compares (`h.add(v)` takes an integer, a `Bool`, an enum, a `String`, or a struct, added by its own hash or field by field); the table then hashes and compares the key by those two methods, and its other fields may be anything. A type with `equals` and no `hash`, or `hash` and no `equals`, is refused at its declaration: equal values must hash alike. The runtime gives the hasher, with its per-process seed and its mixing; it exists only as that parameter (it is not made, stored, returned or captured).
- A `Float64` key, or a struct key that holds one, is a compile error, since IEEE `==` (`NaN` unequal to itself, `-0.0` equal to `0.0`) agrees with no hash: key by an integer.
- A class key is a compile error, since a class compares by identity and the collector moves objects, and a class's own `equals` reads fields that can change while it is in the table: key by a value the object holds (an id or a name), or by its index in an array. Every refused key or value names what to use instead.
- A struct key of integers, `Bool`s and enums whose size is 1, 2, 4 or 8 bytes (two `Int32` fields, say) is kept as one integer, so it costs what a combined integer key costs.
- A class, an `Array` or a `Hash` value is shared with the table, as a variable would share it; a struct value is a copy, as in an `Array`.
- A value cannot be optional (`Hash(Int, String?)` is refused): `h[k]` is already `nil` for a missing key, so a stored `nil` could not be told from it.

```avant
struct Ticket {
  id: Int
  note: String

  fn equals(other: Ticket): Bool {
    id == other.id
  }

  fn hash(h: Hasher) {
    h.add(id)
  }
}

fn main {
  open = Set(Ticket).new()
  open.add(Ticket { id: 7, note: "printer" })
  puts(open.has(Ticket { id: 7, note: "" }))       // => true
  puts(open.add(Ticket { id: 7, note: "again" }))  // => false
}
```

## Sets

`Set(T)` holds distinct values, each at most once. Its elements are what a `Hash` takes as keys ([Key and value types](#key-and-value-types)), by the same rules: a `Float64`, a class or a struct with its own `equals` and no hash is refused with the reason.

```avant
fn main {
  seen = Set(String).new()
  for w in "the cat and the hat".split(" ") {
    if !seen.add(w) {
      puts("again: ${w}")        // => again: the
    }
  }
  puts(seen.size)                // => 4
  puts(seen.has("cat"))          // => true
  puts(seen.delete("cat"))       // => true
  words = seen.values()
  words.sort()
  puts(words[0])                 // => and

  primes: Set(Int) = [2, 3, 5, 7, 5]
  total = 0
  for p in primes {
    total += p
  }
  puts(total)                    // => 17
}
```

| Operation | What it does |
| --- | --- |
| `Set(T).new()`, `[]` | an empty set; `[]` and an array literal make a set where the type says `Set(T)` (`s: Set(Int) = [1, 2, 3]`, a `Set` parameter, a result) |
| `s.add(v)` | put `v` in; `true` when it was not there |
| `s.has(v)` | `Bool`: whether `v` is in the set |
| `s.delete(v)` | take `v` out; `true` when it was there |
| `s.size`, `s.clear()` | number of elements; remove all |
| `s.values()` | the elements as an array, in unspecified order |
| `s.each (v) { ... }`, `for v in s` | walk every element (over the array `values()` makes when the walk starts); an element added during the walk stops the program after that step, as in a hash's walk |

- A set is stored as a `Hash` whose values are one byte nobody reads, and costs what that hash costs; a set is shared as a hash is.
- **Iteration order is unspecified**, as a hash's; sort `values()` when order matters.
- A non-empty array literal passed straight to a `Set` parameter stays an array: give it a name with a declared type first.
- Not built yet: `==` on sets, and union, intersection and difference (a loop over `for` does them).

## Structs and classes

A `struct` is a value, copied when assigned or passed; a `class` is a reference to one shared object.

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
  puts(q.sum())            // => 52

  c = Counter.new()
  d = c                  // the same object
  c.inc()
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

When in doubt, use a struct: it allocates nothing, and no other holder can change it.

### Structs

A struct literal names every field without a default, in any order; a copy is independent of the original.

A method's change to its struct receiver is the caller's (no `mutating` mark); a struct argument is a copy, so a change through a parameter stays in the callee. A struct reached through an index or a field is changed where it is stored: `xs[i].x = v`, `xs[i].scale(4)`, `xs[i].pos.x = v`, `obj.pos.x = v` and `obj.pos.scale(4)` change the element or the field, and so does the variable of `for p in xs` and the parameter of `xs.each (p) { ... }` ([For](#for)); `q = xs[i]` copies it. While a method that changes its receiver runs on an array's element, and the call may reach that array (an argument, a lambda or the receiver's own fields may lead to it), the array is frozen: a change of its size or order through any path (`push`, even with room, `pop`, `clear`, `reserve`, `sort`, `reverse`) stops the program, naming the method call (`avant runtime: push on an array while Pos.grow, called on one of its elements, runs: …`), since the method's receiver is the element's place in the buffer. Element stores and reads through other paths, and reassigning the field or the variable that holds the array, stay allowed (the old array keeps the write). To change the array inside such a method, copy the element, call the method on the copy and store it back (`p = b.ps[i]; p.grow(b); b.ps[i] = p`). Cost: none for a call that cannot reach the array; otherwise two atomic operations a call. A Hash gives its value as an optional copy (`h[k]` is `nil` when the key is absent), so `h[k].x = v` is a compile error: open it, change it and store it back (`if v = h[k] { v.x = 1; h[k] = v }`); the parameters of `h.each (k, v) { ... }` are copies too, and writing one is a compile error.

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

A field default is any expression that uses neither `self` nor another field, evaluated at each literal that leaves the field out, after the fields it names. A struct with a default has no zero value: a class field of its type needs a default or `initialize`, and `Array(T).new(n)` needs a value.

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

`Type.new(...)` allocates the object and calls `initialize` with the arguments; a class whose fields all have a starting value needs no `initialize`.

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

A parameter of a class's `initialize` written `self.name` is a **field parameter**: it has the field's type and is assigned to it before the body, in parameter order (`fn initialize(self.owner) { }` is `fn initialize(owner: String) { self.owner = owner }`); it may have a default (`self.limit = 10`).

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
  puts("${s.label} ${s.size()} ${t.label}")   // => span 7 t
}
```

`Type { fields }` builds a struct only; a class is always built with `.new()`.

### Clone

`x.clone()` is a deep copy: a new object whose fields are copies, so a change to one side is never seen on the other.

```avant
class Cart {
  owner: String
  items: Array(String) = []
  counts: Hash(String, Int) = Hash(String, Int).new()

  fn initialize(self.owner) {
  }
}

fn main {
  a = Cart.new("ada")
  a.items.push("tea")
  b = a.clone()
  b.items.push("milk")
  b.counts["tea"] = 2
  puts("${a.items.size} ${b.items.size} ${a.counts.size}")   // => 1 2 0
}
```

- Every class and struct has it, derived from its fields: a class field is cloned in turn, an `Array`, `Hash` or `Set` is copied at every level (its class elements cloned), a struct is copied with its class fields cloned, an optional is cloned by its value. `initialize` does not run: the copy's fields are the original's, copied.
- What cannot change is shared, since a copy would read the same: numbers, `Bool`, enums, `String`, `Bytes`. A `Channel` or a `Mutex` field is shared too, as a task shares one: the copy talks to the same channel and locks the same value.
- `clone` exists on arrays, hashes, sets and optionals too (`grid.clone()`), and on every other value, where it is the value itself.
- Refused at compile time, naming the field: a `fn` field (a function may capture mutable state the copy would share), a `Ptr` (C memory), a `JoinHandle`, a `Buf`, and a union of several types holding something to copy (`Int | Box`; an optional `Box?` is cloned).
- A type cannot define its own `clone`: a clone is always this deep copy.
- The copy follows the type, recursively, as a recursive function would: an object reached twice is copied twice, a chain is copied one call deeper per link (a linked list of 100,000 nodes runs out of stack in a `--debug` build, as any recursion that deep does), and a cycle never ends. Copy such structures with a loop of your own.

### Field initialization

No class field is ever read unset (a struct has no `initialize`: [Structs](#structs)). Each field gets, in order:

1. **A default**: any expression that uses neither `self` nor another field (`xs: Array(Int) = []`, `seen: Hash(String, Int) = Hash(String, Int).new()`, `at: Span = Span.new(0, 0)`), evaluated at each construction, in declaration order, before `initialize` (compute a value from other fields in `initialize`).
2. **The type's zero**, if it has one and there is no default: `0`, `0.0`, `false`, `""`, `nil` for any `T?`, a null `Ptr`, a struct of such fields.
3. **`initialize`**, for every other field (a class, `Array`, `Hash`, `Buf`, `Bytes`, a `fn`, a union without `nil`, a struct holding one): it must set the field on every path, before reading it and before `self` is used as a value or a method of `self` is called.

Breaking rule 3 is an error naming the field and the ways out: set it, give it a default, or make it optional (`left: Node?`, one pointer; a function as `cb: (fn(Int): Int)?`).

### Methods and self

A method is a `fn` in the type's body; `self` is its receiver.

- A field is **read** bare (`x`) or as `self.x`, and **written** only as `self.x = ...`: a bare `x = ...` on a field is an error naming both ways out (`self.x =`, or another name for the local).
- A method of the same type is called bare (`sum()`) or as `self.sum()`.
- A bare name is, in order: a local or parameter, a field, a method of the receiver, then a function. When two fit, it is an error:
  - a bare call fitting both a method and a function asks for `self.name(...)` or the function's qualified name;
  - a module qualifier (`util.twice(3)`) that is also a method of the receiver asks for `self.util` or another name for the method.
- Every method call takes parentheses: `q.sum()`, not `q.sum` ([Functions](#functions)).
- Methods are not values. `box.f(x)` calls the method `f`, or the function in the field `f` (as `(box.f)(x)` does); a field and a method of one name are an error at the declaration (`Box.f is both a field and a method`).

### External methods

A method may be declared outside the type, as a function with a receiver parameter, under the body's rules with the receiver's name for `self` (`p.x = ...`; an ambiguous bare call asks for `p.name(...)`). A lambda in it cannot capture the receiver (copy it into a local). A builtin type's receiver (`String`, `Bytes`, `Buf`, an integer type, `Float64`, `Bool`, an `Array`, a `Hash`, a `Set`) is always named.

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

So a module adds methods to a type it does not own: an enum, and the builtin `String`, `Bytes`, `Buf`, `Int`, `Int32`, `UInt32`, `UInt64`, `Float64` and `Bool` (the standard library's `bytes.compress` is one; `fn (n: Int) clamp(lo: Int, hi: Int): Int` is called `n.clamp(0, 10)`), and `Array`, `Hash` and `Set`, on one element type or on all of them:

```avant
fn (xs: Array(T)) second: T? {
  xs.get(1)
}

fn (xs: Array(Int)) total: Int {
  t = 0
  for x in xs {
    t += x
  }
  t
}

fn (h: Hash(String, V)) has_all(ks: Array(String)): Bool {
  for k in ks {
    if !h.has(k) {
      return false
    }
  }
  true
}

fn main {
  puts([7, 8].second() ?? 0)                   // => 8
  puts([1, 2, 3].total())                      // => 6
  puts(["a": 1, "b": 2].has_all(["a", "b"]))   // => true
}
```

- A container's method is declared on its receiver's type: `Array(Int)` is one element type (another method of the same name may serve `Array(String)`), `Array(T)` every one, the type parameters taken from the receiver at each call. A call on a receiver no declaration fits names the types the method is declared on; a generic and a specific declaration of one name are refused as declared twice.

- A builtin type keeps its own methods and fields: `fn (s: String) to_s`, `fn (b: Bytes) size` or `fn (n: Int) abs` is refused where declared.
- An external method of a builtin type is seen like a function (in its module, and in importers when `pub`); a `pub` one of a program's type is seen wherever the type is.
- Two modules may each add a same-named method to a builtin type; a call that sees both (two imports, or an import and its own) is an error naming the two modules: call from a module that sees one. On a program's type, a second method of one name and parameter types is refused where declared, naming the first's module, unless both are private.

### Type-level functions

`fn self.name(...)` in a type's body declares a function called on the type itself, the usual home of named constructors (the library's `Json.parse`, `Regex.compile`).

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
  puts(Money.zero().cents)     // => 0
  puts(Money.of(3).cents)    // => 300
}
```

### Overloads and defaults

Functions or methods may share a name when their arity or argument types differ (the result type alone does not overload). Defaults are trailing, filled in at the call site.

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

- **Named arguments:** a defaulted parameter no positional argument reaches may be named once, after the positional arguments, in any order (`pad("ab", fill: "-")`; `pad(s: "ab")` is refused). The positional arguments choose the overload. Arguments are evaluated left to right as written, then the omitted defaults. A defaulted parameter's name is part of the function's public surface; builtins and function values take arguments by position.
- A default is evaluated at each call that omits it, in the function's module: it may call that module's private functions, and its names never mean the caller's.
- A default may name an earlier parameter (`fn span(a: Int, b: Int = a + 1)`), with its declared type; the argument is evaluated once (`span(next())` calls `next` once).
- A method whose parameters all have defaults is called `conn.recv()`.

### Operator methods

An operator is a method named by it: `+`, `-`, `*`, `/`, `%`, `&+`, `&-`, `&*`, `&`, `|`, `^`, `<<`, `>>` (one parameter, the right operand); `fn -` with no parameter (`-x`); `fn []`, taking the index or several (`g[x, y]`), reads `c[i]`, and `fn []=`, taking the index, then the value, writes `c[i] = v`. `!`, `&&`, `||` and `~` cannot be defined; equality and order are the named methods `fn equals(other: T): Bool` and `fn compare(other: T): Int`, which `==`, `!=` (always `!(a == b)`), `<`, `<=`, `>` and `>=` call ([Equality and order](#equality-and-order)); a struct without `fn equals` compares its fields. A method declared `fn ==` or `fn <` is refused with the one to write.

`at += delta` is `at = at + delta`, and `g[i] += 1` reads with `[]` and writes with `[]=`; the place is read twice, so it must be a name, a field, or an element at a name or a literal (`xs[i] += d`, not `xs[next()] += d`).

```avant
struct Vec2 {
  x: Int
  y: Int

  fn +(other: Vec2): Vec2 {
    Vec2 { x: x + other.x, y: y + other.y }
  }

  fn equals(other: Vec2): Bool {
    x == other.x && y == other.y
  }

  fn hash(h: Hasher) {
    h.add(x)
    h.add(y)
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

No `class B < A`, abstract methods or dynamic dispatch, so every call is direct and every layout known. Behaviour is shared by:

- **Composition**: a field of another type, forwarding what it needs.
- **[Interfaces](#interfaces) as bounds**: a generic function takes any type that opted in, compiled once per type, with no vtable.
- **Unions** for "one of several types", taken apart with `switch` ([Unions](#unions)).

## Errors

A function that can fail returns a union of its result and its failures (types marked `Error`, or `nil`); nothing is thrown or unwinds.

<!-- check: run id=errors_intro -->
```avant
class ParseError: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn parse_age(s: String): Int | ParseError {
  n = s.to_i() ?? return ParseError.new("not a number: ${s}")
  if n < 0 || n > 150 {
    return ParseError.new("out of range: ${n}")
  }
  n
}

fn age_gap(a: String, b: String): Int | ParseError {
  x = parse_age(a)?            // a failure here returns from age_gap
  y = parse_age(b)?
  (x - y).abs()
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

**In any union, the failures are `Nil` and every member marked `Error`; the rest is the success value.** A plain `else` does not see the error; `else e` does ([Reading the error itself](#reading-the-error-itself)).

### Propagate, replace, branch

| Form | On success | On failure | Use it when |
| --- | --- | --- | --- |
| `x?` | gives the value | returns the failure from the current function | the caller should decide |
| `x ?? fallback` | gives the value | gives `fallback`, evaluated only then | a default makes sense |
| `x ?? return v` (also `?? return`, `?? break`, `?? continue`) | gives the value | leaves | the failure ends this function or iteration |
| `v = x else e { ...; continue }` | gives the value | runs the block with `e`, which must leave | the failure is reported, then ends this function or iteration |
| `if v = x { ... } else { ... }` | runs the block with `v` | runs the `else` (`else e` names the failure) | each outcome needs code |

These are the forms of [Opening an optional](#opening-an-optional): `T?` is the union whose only failure is `nil`.

- `x?` needs the function's result to hold every failure of the value (`nil` included); otherwise:

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

- `?` returns from a lambda whose result type holds the failure (`(): Int | ParseError { ... }`); it is refused in a `spawn` block and in a lambda given to a builtin (`each`, `parallel`, `Mutex.with`, ...), where it has nowhere to return the failure: a `for` loop's `?` returns from the function.
- These forms are compile errors on a union with no failure (`Int | String`) or only failures (`IoError | ParseError`).
- An optional error (`ParseError?`) is a `T?`: if-assign and `??` give the error when not `nil`; `?` is refused.

<!-- check: hidden id=parse_error_decl
class ParseError: Error {
  message: String

  fn initialize(message: String) {
    self.message = message
  }
}

fn parse_age(s: String): Int | ParseError {
  n = s.to_i() ?? return ParseError.new("not a number: ${s}")
  if n < 0 || n > 150 {
    return ParseError.new("out of range: ${n}")
  }
  n
}
-->

### Nothing or a failure

A function with no success value returns `Void | E`: the end of its body or a bare `return` succeeds, `return e` fails; `?` passes the failure up and `f() else e { ... }` reads it (a guard without an assignment).

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

- `save("a", "b")` alone is a compile error ([Errors cannot be dropped](#errors-cannot-be-dropped)): write `?`, `else e { ... }`, `?? return`, or `_ =`.
- `if v = save(...)` and `v = save(...) else e` are refused (no value to bind); a `switch` reads the failure (`case e: SaveError { ... } else { ... }`).
- `Void` stands only beside failures marked `Error` (`Void | IoError`, `Void | A | B`; `Void | Int` and `Void | E?` are refused). `Void | E` with a class `E` is one pointer, `Void` its null.
- `dir_make` and `file_delete` return `Void | IoError`, and so does a [`Mutex.try_with`](#channel-and-mutex) whose block ends without a value.

### Declaring an error

An error type is a class or struct marked `: Error` with a `String` field `message` or a method `fn message: String`; `e.message` reads it and interpolation prints it.

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

`Error` is a marker on the declaration and a [bound](#bounds), not a type to store; no type may be named `Error`. The library's errors are marked: `IoError`, `ProcessError`, `JsonError`, `RegexError`, `HttpError`, `ZlibError`, `Base64Error`.

### Causes

A field `cause` of an `Error` type, or an optional one, is printed by interpolation after the message, outermost first.

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

An enum marked `: Error` has its members as failures, each one's message its name unless the enum defines `fn message: String`.

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

It is read, refused as dropped and discarded like any error; a `switch` over the result names it as a member.

### Errors cannot be dropped

A statement whose value can be an error is a compile error:

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

`else e` names the failure. After an assignment it is a guard: the block runs on a failure and must leave (`return`, `break`, `continue` or [`exit`](#output)), so after it the variable holds the success. After an if-assign, `else e` names the failure for an `else` block that need not leave.

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

- `e` is the one failure member, or the union of several (`Int | IoError | ParseError` gives `IoError | ParseError`), told apart with a `switch`.
- Where the only failure is `nil` (`Int?`), a guard, named or not, is refused: use `?? return`, `?? break` or `?? continue`.
- `else e` follows a plain assignment (`v = f()`, `self.n = f()`, or `_ = f()` to read only the failure), not `+=`; after an if-assign it needs the condition's one binding, not a list.
- A `switch` over the result handles each outcome, the failure a member like the others.

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

### Traps

A *bug* (an index out of range, an overflow, a misused API) is a trap: the program prints one line and exits with status 1; it cannot be caught.

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

The line, written after stdout is flushed, is `avant runtime: WHAT` and where:

| Where the trap is | The line names | Example |
| --- | --- | --- |
| at one operation: `expect` on `nil`, the number conversions, shifts, `buf[i] = n`, `Array(T).new` with a negative size, `main`'s exit status, a C string result that is NULL | that operation's position | `avant runtime: xs has an element at main.av:7:15` |
| an index out of bounds, an integer overflow, a division by zero | the function, and where it starts | `avant runtime: integer overflow in twice (main.av:9:1)` |
| inside a runtime function: a slice's bounds, `push_byte`, `chr`, `repeat_byte`, `sum`, a `Hash` counter, `fmt_float`'s precision, a parse base, `split` with an empty separator, `chars` on invalid UTF-8 | the operation and its values; no position yet | `avant runtime: slice 4..2 out of range (size 5)` |

The second row has no position so that hot loops stay fast.

| What traps | The form that does not |
| --- | --- |
| `xs[i]`, `s[i]`, `buf[i]` outside the bounds | `.get(i)`, which returns `T?` |
| `x.expect("why")` on `nil` | `if v = x`, `x ?? fallback`, `x?` |
| `+`, `-`, `*`, `abs`, `sum`, `inc` overflowing | `&+`, `&-`, `&*` to wrap |
| `/`, `%` by zero | test the divisor |
| `to_i`, `to_i32`, `to_u32`, `to_u64` out of range | `wrap_i`, `wrap_i32`, `wrap_u32`, `wrap_u64` |
| a shift amount outside the width | |
| a slice with `start > stop` or outside the value | |
| a byte outside `0..255` | `n & 255` |
| `chars` on invalid UTF-8 | `String.from_utf8`, which returns `String?` |
| growing past a [size limit](#size-limits) | |

**A trap ends the whole process**, except in a spawned task ([When a task traps](#when-a-task-traps)) and in an `std.http` handler, where it fails only its request ([HTTP server](#http-server)). The exceptions keep their own limits (a trap inside a C callback, and the runtime's own stops, still end the process), so run servers under a supervisor too (systemd's `Restart=`, a container restart policy). A pre-forked server mode is decided, not built.

**A failed write to standard output stops the program** like a trap (`avant runtime: cannot write to standard output: No space left on device` on a full disk or `/dev/full`, status 1), checked at every `puts`. A closed pipe (`prog | head`) kills it by `SIGPIPE` (status 141 in a shell), reset to the default before `main` even if the parent ignored or blocked it (a CI runner, a shell's `trap '' PIPE`).

### Nil, error or trap

| The situation | Use | The caller |
| --- | --- | --- |
| Absence is normal: a lookup, a parse, an empty collection | `T?` | must open it before using the value |
| The operation can fail, and the caller should know why | `T \| SomeError` | must handle it, pass it up, or discard it visibly |
| The program is wrong: a bug, not an input | a trap | cannot continue; fix the code |

In the library, `h[k]` and `"42".to_i` return optionals, `file_read` and `Json.parse` errors, and `xs[9]` on three elements traps.

## Function values

A function value has the type `fn(T): U`; a lambda is a function without a name, `(x) { ... }`, and is passed like any other argument.

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
  b = apply([1, 2, 3], (n) { n * n })          // a lambda
  puts(a[2])    // => 6
  puts(b[2])    // => 9
}
```

- `fn(): Int` takes nothing; `fn(Int, String): Bool` takes two arguments; `fn(Int)` returns nothing.
- A bare function name in a value position is that function (`f: fn(Int): Int = double`); another module's is qualified, as at a call (`f = util.double`). When a name means several functions (overloads, or two imported modules' of one name), the expected `fn` type or the module's name picks one.
- The type is invariant: `fn(Int): Int` is not `fn(Int): Int?`.
- An optional function value is `(fn(Int): Int)?` (`fn(Int): Int?` returns an `Int?`), opened like any optional (`if f = cb { f(1) }`, `cb ?? double`), no larger than a function value (`nil` is a null code word).
- There is no `==` on function values, and methods are not values.

### Lambdas

`(x) { x + 1 }` is a lambda: a function's parameters and body without `fn` and a name. Its last expression is its result, the result type expected or inferred, including a final `if`/`else` whose branches end in values. A parameter's type, and the lambda's result, are written as in a function (`(a: Int, b: Int): Int { a * 10 + b }`, `(v: Int | String) { ... }`) unless the expected `fn` type gives them; a parameter left unknown is an error. `() { 7 }` takes nothing.

```avant
fn main {
  add: fn(Int, Int): Int = (a, b) { a + b }
  typed = (a: Int, b: Int): Int { a * 10 + b }
  seven = () { 7 }
  puts(add(1, 2))       // => 3
  puts(typed(4, 2))     // => 42
  puts(seven())         // => 7
}
```

- `return` returns from the lambda; when the result is inferred, every `return` and the end give one type (`return` with nothing beside an `Int` end is an error that names both: write the result type, as `(n: Int): Int? { ... }`).
- A postfix `?` in a lambda returns its failure from the lambda, so the result type must hold it: `(): Int | ParseError { parse_age(s)? }`; in a lambda whose result is inferred it is refused with that hint.
- `break` and `continue` in a lambda have no loop of the lambda's to leave: a compile error, unless inside a loop of its own body.
- **When the lambda is a call's only argument**, the call's own parentheses may be dropped: `xs.each (x) { puts(x) }` is `xs.each((x) { puts(x) })`, `users.map (u) { u.name }`, `t.group () { ... }`. Not directly in the head of an `if`, `while`, `for` or `switch`, where the `{` begins the statement's body: there, write `if xs.any((x) { x > 3 }) { ... }`. A call with another argument writes the lambda among them: `fold(xs, 0, (acc, x) { acc + x })`.
- An operator or a method after the call applies to its result: `twice (x) { x - 5 }.abs()`, `count (x) { x > 2 } > 1`.
- The braces with pipes, `{ |x| ... }`, are the older spelling, still accepted in this release with their old meaning (a builtin's block in pipes is a loop body whose `return` leaves the function); the next release refuses them. `avant format --lambdas FILE...` rewrites them: a block whose body leaves its function with `return`, `break`, `continue` or `?` becomes a `for`.

Your own `map`:

```avant
fn map(xs: Array(T), f: fn(T): U): Array(U) {
  ys: Array(U) = []
  xs.each (x) { ys.push(f(x)) }
  ys
}

fn main {
  lengths = map(["a", "bb", "ccc"], (s) { s.size })
  puts(lengths[2])    // => 3
}
```

### Capture

A lambda captures the variables it uses by reference, in a cell both sides share: an assignment on either side is seen by the other.

```avant
fn counter(start: Int): fn(): Int {
  n = start
  () {
    n += 1
    n
  }
}

fn main {
  next = counter(10)
  puts(next())     // => 11
  puts(next())     // => 12

  total = 0
  add = (k: Int) { total += k }
  add(5)
  add(6)
  puts(total)      // => 11
}
```

- A lambda made in a `for` loop gets that iteration's variable: lambdas made in `for i in 0..3` read 0, 1 and 2 (a new cell each iteration, made only when a lambda captures the variable). A name first assigned in a loop body is new at each iteration too.
- A lambda in a method may use `self` and its fields and methods bare; it captures the receiver by value: a class's reference (later changes to the object are seen) or a struct's copy (a write through `self` is an error).
- A parameter's default may be a lambda that reads the parameters before it; one that assigns to them is refused (it would change a copy no one else sees).

```avant
class Scale {
  k: Int = 2

  fn by: fn(Int): Int {
    (n) { n * k }
  }
}

fn main {
  s = Scale.new()
  f = s.by()
  s.k = 10
  puts(f(4))    // => 40
  fs: Array(fn(): Int) = []
  for i in 0..3 {
    fs.push(() { i })
  }
  puts((fs[0])() + (fs[2])())   // => 2
}
```

### Lambdas given to builtins

`each`, `any`, `all`, `sort`, `sorted`, `parallel`, `Mutex.with` and `Array(T).new(n, (i) { ... })` take a lambda like any function, and the compiler inlines it as a plain loop. A lambda is a function of its own there too: `return` ends this element's turn (with the lambda's value, for `any`, `all`, a comparator or a building lambda), and `break`, `continue` and a `?` with nowhere to return its failure are compile errors with a hint. To leave the walk or the function early, write a `for`:

| Inside | `return` | `break` |
| --- | --- | --- |
| a lambda (`xs.each (x) { ... }`, your own function's) | returns from the lambda: the next element | a compile error |
| a `for` loop | leaves the enclosing function | leaves the loop |

The parameters of a lambda given to a builtin are new names, as a `for` variable is: one named like a variable that already exists there (`x = 1`, then `xs.each (x) { ... }`) is a compile error, since the compiler inlines the lambda as a loop of the function around it. The parameters of any other lambda are a function's, and may take any name.

```avant
fn first_big(xs: Array(Int)): Int {
  for x in xs {
    if x > 10 {
      return x                 // leaves first_big
    }
  }
  -1
}

fn positives(xs: Array(Int)): Int {
  sum = 0
  xs.each (x) {
    if x < 0 {
      return                   // skips this element
    }
    sum += x
  }
  sum
}

fn main {
  puts(first_big([3, 12, 40]))   // => 12
  puts(positives([1, -2, 3]))    // => 4
}
```

A lambda whose value is used (`any`, `all`, `sort`, `Array(T).new(n, (i) { ... })`) may end with a value-giving `if`/`else`; for `parallel` and `with`, whose value is the lambda's last expression, a `return` needs the lambda's result type written (`xs.parallel((x): Int { ... })`):

```avant
fn main {
  xs = [3, 1, 2]
  xs.sort (a, b) {
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

Functions and types take type parameters; each concrete use compiles to its own code, with no boxing or run-time dispatch.

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

In a signature, a single capital letter that names no type (`T`, `U`, `K`) is a type parameter, undeclared, omitted at a call whose arguments determine it.

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

Any other unknown type name is an error where written: `fn f(x: Strng)` reports ``unknown type Strng (did you mean `String`?)``.

A union member may be a type parameter, which takes the argument's other members: `fn first_ok(xs: Array(T | ParseError)): T?` takes an `Array(Int | ParseError)` with `T` as `Int` (and `T?` an `Int?`). Two in one union (`r: T | E`) are split by a bound (`r: T | E: Error`: [Bounds](#bounds)), another argument (`fn or_else(r: T | E, d: T): T`) or a declared result (`e: ParseError? = failure(r)`); otherwise the call says it cannot infer them.

### Generic types

A struct or class declares its parameters, any fresh names (`class Entry(Key, Val)`); each use is its own type and layout (`Box(Int)` is not `Box(String)`), and an unapplied `Box` is an error.

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
- A parameter may take a [bound](#bounds): `class Top(T: Ranked)` takes only a type that opted into `Ranked`, and `Top(Int)` is an error where written.
- A generic class may contain itself (`next: Link(T)?`); a struct, generic or not, may not ([Struct or class](#struct-or-class)).
- `Array`, `Hash`, `Ptr` and `JoinHandle` are built in; they are not generic types you could declare.
- An imported generic type or function takes the using module's types, private ones included (`Box(Point)`, `Box.new(Point { ... })`); another module's private type is unknown as a type argument, an element type (`Array(Hidden)`) or inside a union or function type.

## Interfaces

An `interface` is a set of method signatures a type opts into by name and a generic function can require: a compile-time contract, not a value (no `Array(Show)`, no vtable).

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
  "value: ${v.to_s()}"
}

fn main {
  puts(announce(Vec2 { x: 1, y: 2 }))      // => value: (1, 2)
  puts(announce(Celsius { degrees: 21 }))  // => value: 21 C
}
```

### Opting in

A struct, a class, or a `Ptr` to a C layout opts in by name, never by merely having the methods.

- On the declaration: `struct Vec2: Show`. Several interfaces join with `+`: `struct Vec2: Show + Eq(Vec2)`.
- Later, as a top-level line: `Vec2: Show`, or `Box(Int): Show` for one instance of a generic type; `pub` on it makes importers see it.
- Methods must match the signatures exactly: `Int` is not `Int?`, and a defaulted parameter does not make a shorter signature.
- Builtin types do not opt in (`Array` has no `Show` clause); `Ptr(Point)` can when `Point` is a [C layout](#layouts-and-ptr).

### Bounds

`T: Show` is a bound: the call compiles only for a type that opted in, and the function's copy for it makes ordinary method calls. Two bounds: `T: Show + Eq(T)`; an interface may take type parameters (`interface Eq(T)`).

- The bound is written where the parameter is: after a parameter's type (`v: T: Show`), inside another type (`xs: Array(T: Ranked)`), or on a generic type's parameters (`class Top(T: Ranked)`). After a union it bounds the last member: `r: T | E: Error`.
- `E: Error` takes a type marked `Error`, and settles a union's split: `fn failure(r: T | E: Error): E?` on an `Int | ParseError` takes `E` as `ParseError` and `T` as `Int`.
- A bound by an interface that declares `fn equals(other: T): Bool` (`Eq(T)` below) or `fn compare(other: T): Int` (an order, say `interface Ordered(T)`) lets the body write `a == b`, or `a < b`, `<=`, `>` and `>=`: each type's copy calls its own `equals` (or its derived one) or `compare`.

<!-- check: run -->
```avant
interface Eq(T) {
  fn equals(other: T): Bool
}

struct Id: Eq(Id) {
  n: Int

  fn equals(other: Id): Bool {
    n == other.n
  }

  fn hash(h: Hasher) {
    h.add(n)
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

A file is a module: `import name` loads `name.av` and brings its `pub` names into scope; the rest is private.

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
  puts(v.length2())        // => 25
  puts(origin().x)         // => 0
  puts(vec.origin().y)     // => 0
}
```

The compiler follows one root file's imports (`avant run main.av`) into one program; there is no `module` line, header or build file.

### Import paths

A module's name is its path with dots for slashes: `import http.server` loads `http/server.av`. A directory is not a module; a keyword after a dot is a name (`import http.spawn` loads `http/spawn.av`).

The compiler looks for the file, in order:

1. beside the importing file;
2. in the directory of the file being compiled;
3. under the nearest `avant.toml` above that file (so a file in `tests/math/` can import the project's modules);
4. in each fetched dependency ([Packages](#packages)).

One file found twice is one file; two *different* files for one import, or an import cycle, are errors. The first segment `std` is reserved for the [standard library](#standard-library) (`import std.json`).

### Visibility

Names are private to their module unless marked `pub`: functions, types, constants, **methods and fields** alike.

- A private field cannot be read, written or named in a literal outside its module, so such a type is built by its module's functions; a read-only view is a `pub fn` (`account.balance()`).
- A test or another module sees `pub` names only.
- `fn main` is the entry point and needs no `pub`.
- A C layout's fields are C's, visible wherever the layout is. A [field walk](#the-field-walk) in another module than the type's sees only its `pub` fields.

### Name clashes

Two modules may publish the same name; a *use* that could mean either is an error there: qualify it with the module name.

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

Nothing wins silently, not even the module's own name:

| Two modules have | A bare use reports |
| --- | --- |
| the same function | `value()`: `ambiguous call to value: alpha.value and beta.value both match; write alpha.value() or beta.value()` |
| a type and a function of one name | `Value` where a type is expected: `Value is a type of alpha and a function of beta: write alpha.Value or beta.Value` |
| the same type | `Token`: `Token is a type of both alpha and beta: write alpha.Token or beta.Token` |
| the same constant | `width`: `width is a constant of both alpha and beta: write alpha.width or beta.width` |

A module importing only one names it bare. Generic types alike: two modules may each declare `Cell(T)` with same-named methods, and `alpha.Cell(Int)` names one.

- Functions with different parameter types still meet as one overload set across modules: `show(7)` and `show("ab")` pick `show_int.show` and `show_str.show` by their arguments.
- A qualified call reaches only that module's `pub` functions. A module may qualify its own names (`main.helper()`).
- A nested module is qualified by its full name: `http.server.Server`.

## Packages

A package is a git repository of `.av` files, named by its git URL (no registry). A project lists its dependencies in `avant.toml`; `avant fetch` clones them into `deps/` and records the exact commits in `avant.lock`.

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

The manifest is for fetching only: a dependency is source compiled with your program through `import`.

### The manifest

`avant.toml` sits beside your root file: `[package]` names the project; each `[dependencies]` entry has a `git` URL (anything `git clone` reads, a local path included) and a `rev` (a full commit SHA, a tag or a branch; a SHA or tag is reproducible). A dependency may also be its own section:

```toml
[dependencies.greeter]
git = "https://github.com/example/greeter.git"
rev = "v1.0.0"
```

A dependency with no `git`, a duplicate, an unknown key, or a bare version string (`greeter = "1.0"`: no registry resolves it) is an error naming the file and line.

### Fetch and lock

`avant fetch` (or `avant fetch path/to/project`) clones each dependency and its own dependencies into `deps/` at the pinned commits, writes `avant.lock`, and prints one line per dependency:

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

- **Commit** `avant.toml` and `avant.lock`, **not** `deps/`: a fresh clone builds with `avant fetch`, then `avant run main.av`.
- A plain `avant fetch` takes the lockfile's commits, even if a branch or tag moved.
- If any dependency cannot be cloned or checked out, fetch prints git's output, exits non-zero and writes no lockfile.
- Fetching needs `git` (looked for in `/usr/bin`, `/usr/local/bin`, then `/bin`); compiling works offline.

### Importing a dependency

A dependency's module is imported by its path from the checkout's root, with no package prefix: `greeter.av` is `import greeter`, `http/server.av` is `import http.server`.

A dependency's file imports from its own checkout and the other fetched dependencies, never your program; visibility is as for your files. A module not found in a project with a manifest says what to run:

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

Commit the new `avant.lock`. A removed entry's checkout left in `deps/` is still found by the compiler.

### Publishing a package

A package is a git repository with its modules laid out from the root and an `avant.toml` naming it; its own dependencies are declared the same way and fetched for its consumers; its public surface is marked `pub`.

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

Publishing is pushing to any git host; a new version is a new tag or commit, which consumers take when they update.

### Package conflicts

Each clash is reported, never silently resolved.

- **Two places provide the same module file.** Importing it is a compile error listing the files (`error[E5000]: import shared is ambiguous: ./deps/a/shared.av ./deps/b/shared.av`), with no priority: a `shared.av` beside your root file clashes with a fetched one. Rename one module or stop fetching one package; to change a dependency's code, patch a clone ([Updating and removing](#updating-and-removing)).
- **One dependency name reaches two commits** (your pin and a dependency's): fetch stops with `avant fetch: dependency greeter resolves to two different commits (...)`; align your pin and fetch again.
- **Two modules publish the same name.** The ordinary [name clash](#name-clashes): qualify the use.

### Package limits

No registry, no version ranges or solving, no per-package build step, one version per dependency name.

## Concurrency

Tasks run on a pool of OS threads: `spawn` starts one, `.parallel` maps a block over an array, channels and mutexes carry what tasks share. The compiler checks what a task captures: sharing a mutable value by accident is a compile error.

```avant
fn fib(n: Int): Int {
  if n < 2 { return n }
  fib(n - 1) + fib(n - 2)
}

fn main {
  h = spawn { fib(27) }                           // one task, on the pool
  fibs = [20, 21, 22, 23].parallel (n) { fib(n) } // one task per element
  puts(h.join() ?? 0)  // => 196418
  puts(fibs[3])      // => 28657
}
```

### Spawn

`spawn { ... }` runs the block as a task and returns a `JoinHandle(T)` at once, `T` the block's value: any type, `Void` for a block that ends with a statement or a call that gives nothing. `.join` waits and gives a `T | TaskError`: the block's value or the trap that ended the task ([When a task traps](#when-a-task-traps)), read like any failure (`??`, `?`, an if-assign, `else e`); a second join gives the same. Handles are ordinary values: an array of them waits for many.

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
    total += h.join() ?? 0
  }
  puts(total)    // => 30
}
```

A `?` in the block ends the task with the failure, as it returns one from a function: the block's value is then `T | E` (`T?` for a `nil`), and the join gives `T | E | TaskError`.

```avant
fn parse_port(s: String): Int? {
  n = s.to_i()?
  if n < 1 || n > 65535 {
    return nil
  }
  n
}

fn main {
  h = spawn { parse_port("8080")? + 1 }
  puts(h.join() ?? 0)                         // => 8081
  bad = spawn { parse_port("http")? + 1 }
  puts(bad.join() ?? 0)                       // => 0
  name = spawn { "task ${1 + 1}" }
  puts(name.join() ?? "")                     // => task 2
}
```

- A value the block captures is copied when the task is sent ([What may cross a task](#what-may-cross-a-task)); the block cannot assign a captured variable. Its value goes back by the join whatever its type: the task made it or captured it, and nothing else holds it once the join returns.
- **A task completes when its block and every task it spawned have ended.** A task spawned inside a task, or inside a `.parallel` body, counts in it: the worker goes back to the pool when the block ends, and the join returns once the last of them has ended. So a join guarantees that every array the task captured, directly or through the tasks it spawned, is thawed; a value received through a channel does not.
- A captured `Array` is shared for reading and frozen, for the block and the sender, from the `spawn` until the task ends, with the tasks it spawned (not the join: a dropped handle does not keep it frozen), at every level of an array of arrays; `.parallel`'s input and its block's captured arrays are frozen while its bodies run. A change through the captured name is a compile error; through another path (another name, a function it is passed to, a field holding it) it stops the program with status 1, naming the operation (`push`, `pop`, `clear`, `fill`, `sort`, `reverse`, `reserve` or an element store): `avant runtime: push on an array a running task reads: …`. Cost: none without `spawn` or `.parallel`; otherwise one compare per element store and `clear`, and one test per runtime mutator call (a `push` with room tests only when it must grow).
- A captured `Hash` (and a `Set`) follows the same rule: shared for reading and frozen until the task ends, at every level of a hash of arrays or an array of hashes. A store (`h[k] = v`), `delete`, `clear`, `inc` or a `Set`'s `add` through the captured name is a compile error; through another path it stops the program, naming the operation (`a store`, `delete` or `clear`): `avant runtime: a store on a hash a running task reads: …`. Cost: one test per runtime call that changes a hash, in every program; the count takes padding the hash already had, so a hash is no larger.
- `.join` and `.parallel` work inside a task, written in the block or reached through a call: a joined task that no worker has taken yet runs on the joining worker, as do the `.parallel` bodies still queued, and the caller waits for the rest giving its place in the pool back, as a task waiting on a channel does ([The thread pool](#the-thread-pool)). So a recursive split (spawn one half, compute the other, join) needs no more threads than the pool's count. A task that joins a task waiting on a channel only it would send to still waits for good, as anywhere.

### When a task traps

A trap in a spawned block (an index out of bounds, an overflow, a division by zero, an `expect` on `nil`, a change to a frozen array or hash, …) ends that task; the program and the pool go on. The trap is printed to standard error when it happens, joined or not (`avant runtime: index 7 out of bounds …`); the task's stack is dropped without unwinding (the collector frees what it left), and `join` gives a `TaskError`: `what` is the trap's text, `site` its `FILE:LINE:COL` (empty when the trap names its function instead), `"${e}"` prints both.

```avant
fn main {
  h = spawn {
    xs: Array(Int) = []
    xs.pop().expect("xs has an element")
  }
  v = h.join() else e {
    puts(e.what)    // => xs has an element
    return
  }
  puts(v)
}
```

What the task shared is left safe:

- A `Mutex` the task held in `with` is **poisoned**, its value maybe half-changed: every later `with` on it traps with `Mutex.with on a poisoned mutex: …`. `m.poisoned()` tells; `m.try_with (x) { … }` gives the block's value or, without running the block, a `MutexError` (type `T | MutexError`, or `Void | MutexError` for a block without a value). It heals by `recover: f`, `m.recover(v)` or `m.clear_poison()` ([Channel and Mutex](#channel-and-mutex)).
- Each `Channel` the block captured is closed, which wakes a task waiting on it in `recv` or `send`.
- Each `Array` and `Hash` the block captured is thawed, as at the task's normal end.

A trap in a `.parallel` body ends that body (it is printed when it happens, and what the body held is given back as above); the other bodies run to their end, and then the `.parallel` traps in its caller, naming how many bodies trapped and the first one's trap: `avant runtime: .parallel: 2 of 8 bodies trapped; the first: index 9 out of bounds …`. In a task that ends the task, whose join gives it; on the main thread it ends the program.

These traps still end the program: inside a C callback, on the main thread outside an `std.http` handler (a handler's trap fails its request: [HTTP server](#http-server)), and the runtime's own stops (the heap limit, the collector's checks).

### Parallel

`items.parallel (item) { ... }` runs the block once per element, on the pool, and returns an `Array` of the results **in input order**. The caller waits outside the pool; inside a task it runs the bodies still queued itself ([Spawn](#spawn)). A `?` in the body makes that element the failure, as in a `spawn` block: the result is an `Array(T | E)`.

```avant
fn main {
  words = ["a", "bb", "ccc"]
  suffix = "!"
  loud = words.parallel (w) { "${w}${suffix}" }
  puts(loud[2])      // => ccc!
}
```

The block takes one parameter; the statement form `parallel { }`, without a receiver, is not built.

### Channel and Mutex

`Channel(T)` passes values between tasks and `Mutex(T)` guards one shared value, for any type `T`; both are in `std.sync`.

```avant
import std.sync

fn main {
  hits = Mutex(Int).new(0)
  handles: Array(JoinHandle(Void)) = []
  for i in 0..4 {
    handles.push(spawn {
      for j in 0..1000 {
        hits.with (n) { n += 1 }
      }
    })
  }
  for h in handles {
    _ = h.join()
  }
  puts(hits.with (n) { n })    // => 4000
}
```

- `Mutex(T).new(value)` holds `value`. `m.with (x) { ... }` runs the block under the lock, `x` naming the guarded value (assigning `x` changes it); as a value, the call gives the lambda's last expression. `return` ends the lambda (its value needs the result type written, `m.with((x): Int { ... })`); `break`, `continue` and a `?` have nowhere to go in it; every way out releases the lock.
- **Leaving early keeps the changes made so far** (only a trap poisons), unchecked by the compiler: leave only where the guarded value is whole, after the update (`n += 1`, then `return`) or before it (`if q.size == 0 { return nil }`, then the change).
- **A poisoned mutex heals** ([When a task traps](#when-a-task-traps)) by one of three operations, each under the lock:
  - `Mutex(T).new(value, recover: f)`: a `with` or `try_with` that meets the poison, on whichever task, calls `f()`, makes its result the guarded value, clears the poison and runs the block. `f` is a named function or a lambda written there (`recover: () { Stats.new() }`), whose captures are plain values (a number, `Bool`, `String`, an enum, or a struct, optional or union of those), copied when the mutex is made: `recover: () { Stats.new(size) }` rebuilds with `size` as it was at the call, whatever is assigned to `size` later, and a captured class, `Array`, `Hash`, `fn`, `Channel` or `Mutex`, or a function value from a variable, is refused, since the rebuild runs on whichever task meets the poison; a trap inside it is an ordinary trap and leaves the mutex poisoned. `f` costs a lock nothing until it meets the poison; without `recover:`, a poisoned lock traps.
  - `m.recover(value)` replaces the value and clears the poison; it takes `value` as `Mutex(T).new(value)` does (a new value, or a local variable it consumes).
  - `m.clear_poison()` keeps the value as the trapped task left it: for a value whose every state is valid.

```avant
import std.sync

fn fresh: Array(Int) {
  [0, 0, 0]
}

fn main {
  counts = Mutex(Array(Int)).new([0, 0, 0], recover: fresh)
  h = spawn {
    counts.with (c) {
      c[0] += 1
      c[7] += 1                          // traps: the task ends, the mutex is poisoned
    }
  }
  _ = h.join()
  puts(counts.poisoned())                  // => true
  total = counts.with (c) { c.sum() }      // fresh rebuilds the value first
  puts(total)                            // => 0
  puts(counts.poisoned())                  // => false
}
```

- **Nothing the lock guards leaves the block.** The guarded value and every reference reached from it (a field, an element, a name bound to one) may be a receiver (`b.items.push(x)`, `b.n += 1`) or an operand with a plain result (`b.n`, `b.items.size`, `"${b}"`), never an argument, a stored value or a captured name. It may be stored only into the guarded value itself (`b.root = b.root.left`) or into a name the block declares (`node = b.head`, a `for` variable over `b.items`), which follows the same rule. A plain value (a number, `Bool`, `String`, `Bytes`, a struct or optional of those, a `Channel` or `Mutex`) is a copy and goes anywhere; the block's value, when used, must be one. To take data out, push plain values into an array declared before the block (`b.items.each (it) { sizes.push(it.n) }`).
- **What a `Mutex` may guard.** The block may call methods of the guarded class, or of any class it holds (`b.inner.add(c)`), with outside values, under the lock; so `Mutex(T)` is accepted only when no method of `T`, or of any class or struct `T` holds through fields, elements and members, keeps an outside reference or hands out its own:
  - a parameter that can hold a reference is never stored (into a field, an element, a hash, an array by `push` or `fill`, or a new object), passed to a function or another type's method, or returned; reading it (`self.n == other.n`) and building new values is allowed;
  - `self` and what it reaches are stored only into the object itself (`self.root = self.root.left`, `self.head = Node.new(v, self.head)`) and passed only to those same classes' methods;
  - `initialize` is not read: its object is new, and a constructor's arguments are checked at the call;
  - the error names the mutex, class, method and store: `Mutex(Box) (at app.av:16:17): Box.add stores a parameter's reference (or a value reached from one) into an array (push); …`.
- **Nothing from outside comes in.** A value stored into the guarded value (`b.child = c`, `b.items.push(c)`) must be new (a constructor call or literal whose parts are new or plain), already reached from the guarded value, or plain.
- **`Mutex(T).new(v)` consumes `v`** when `T` is not plain: pass a new value (`Mutex(Box).new(Box.new())`, `Mutex(Array(Int)).new([])`) or a local variable, unusable after the call by source order, not paths (a use in the other branch of an `if`, or after a `return` that follows the call, is refused too: declare the value in the branch that gives it to the mutex); a field, an element, a parameter or a call's result is refused. Before the call, the local is used only as the guarded value is inside `with` (a receiver or plain operand: `b.n = 5`, `b.items.push(Item.new())`), holds only new values, and no other name holds a reference reached from it, even one no longer used (`inner = b.inner` is refused: write `b.inner.n += 1`); the call is not in a loop or a block (another mutex's `with` among them) the variable was declared outside of (declare it inside, or pass `Box.new()`).

A channel hands values from one task to another:

```avant
import std.sync

fn main {
  jobs = Channel(Int).new(4)          // buffered: four values may wait
  producer = spawn {
    for i in 1..6 {
      jobs.send(i * i)
    }
    jobs.close()
  }
  total = 0
  while v = jobs.recv() {               // nil once the channel is closed and empty
    total += v
  }
  _ = producer.join()
  puts(total)    // => 55
}
```

- `Channel(T).new()` is a rendezvous channel (a send waits for its receive); `Channel(T).new(n)` buffers `n` values. `send(value)` returns whether the value went in; `recv()` returns `T?`, `nil` once the channel is closed and empty (so `while v = ch.recv() { ... }` drains it); `close` closes.
- A `send` on a closed channel returns `false` at once, buffered or not, full or not; a waiting send returns `false` when the channel closes, its value not received. Values sent before the close stay to be drained. A program that closes only after its last send can ignore the result (`ch.send(v)` as a statement).
- A sent value follows the `spawn` capture rule ([What may cross a task](#what-may-cross-a-task)), except that an `Array` is copied at every level, so each side owns its own. `T` may be any type: a channel can carry channels (a reply channel per request) or mutexes.
- A `Channel` or `Mutex` holds no memory outside the object: make as many as you like.

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
| a `Hash` or `Set` whose keys are plain values (the rows above but an `Array`) and whose values may cross | shared for reading and frozen until the task ends ([Spawn](#spawn)); a `send` refuses it, and an `Array` holding one (a send copies arrays only) |
| a `Channel` or a `Mutex` (`std.sync`) | shared: that is their purpose |
| a class instance, a `fn` value, a `JoinHandle`, a struct holding one, or an `Array` or `Hash` of those | **refused**: it is not copied, and sharing it would be a race (an `Array(Box)` shared for reading still hands each task the same `Box` objects) |

A mutable object reaches another task only inside a `Mutex`, or as the results of `.parallel`.

### The thread pool

The pool runs at most `N` tasks at once, its size; further tasks queue. `N` is the number of CPUs the process may use (its affinity: all online processors, or those `taskset`, `numactl` or a container's cpuset leave it; a cgroup's CPU quota is not read), unless one of these sets it:

- `--avant-threads=N` on **your program's** command line, `N` from 1 to 1,024. The runtime takes it out before `argv()` sees it; a bare `--avant-threads` or a bad value stops the program before `main` with one line on standard error and status 1 (`avant runtime: --avant-threads=0: the thread pool's size is --avant-threads=N, N from 1 to 1024`). Every other argument, `--threads` included, is the program's. After a literal `--` nothing is taken.
- `set_pool_size(n)` from `std.sync`, for a program with its own option (a `--threads`, a config file); `pool_size()` reads the size.

<!-- check: run args=--threads,2 -->
```avant
import std.sync

fn main {
  args = argv()
  if args.size >= 3 && args[1] == "--threads" {
    set_pool_size(args[2].to_i() ?? 1)
  }
  puts(pool_size())                        // => 2
  squares = [1, 2, 3, 4].parallel (x) { x * x }
  puts(squares[3])                         // => 16
}
```

The size is fixed when the pool starts, at the first `spawn` or `.parallel`, or when `Http.listen` without `loops:` takes it as its count of event loops ([HTTP server](#http-server)). A `set_pool_size` after that, or with `n` outside 1 to 1,024, traps (`set_pool_size(3) after the thread pool's size was fixed: …`): a program never runs on a size it did not get. Call it at the start of `main`; it replaces a size given by `--avant-threads=N`.

A task waiting in a channel's `recv` or `send`, or for a `Mutex`, stops counting as running, so a queued task may start, on a free pool thread or a new one, up to 1,024 threads (or `N`, if larger): two consumers waiting in `recv` with `--avant-threads=2` do not block a producer spawned after them. A task whose wait ends goes on once fewer than `N` tasks run. Past the thread limit, the runtime says so once on standard error (`avant runtime: tasks are queued while the pool's 1024 threads are all taken, …`) and queued tasks wait for a free thread.

## Memory

Allocate with `Type.new()`, literals and `push`; there is no arena or bump allocator, nothing is freed by hand, and no program needs a setting from this section.

### The collector

The collector is a precise, generational Immix: new objects go to a young generation, collected by copying the survivors into a mature space of Immix blocks, which is collected less often and defragmented during full collections. In every build mode it knows exactly which values are references, so nothing is kept alive by accident. Objects may move; only C can observe it ([Strings in C memory](#strings-in-c-memory)).

Structs live inline in their holder and optional references are plain pointers ([What an optional costs](#what-an-optional-costs)), so idiomatic code allocates less than a class-only equivalent.

### What allocation costs today

Allocation-heavy code is where Avant is weakest:

- **Many small objects.** A class instance is allocated inline (a compare, a store, the header, the zeroing) and a young collection touches only survivors, but many small live objects cost more time and memory than C's arena: `Binarytrees::Arena` is at 1.22× the best time and 1.93× the best memory.
- **A large structure that all stays alive** (a big tree, a long list, a graph built once). With one thread, after two young collections in a row that keep nearly everything, the collector promotes each full young budget where it lies, without tracing or copying it; with several threads it still traces it (dense young blocks are aged in place, not copied). Either way it takes more memory than a hinted build ([Collection hints](#collection-hints)).
- **A large live heap.** A full collection traces everything alive: its cost grows with the live data, not the garbage.

`Binarytrees::Obj` and `Calculator::Ast` carry hints in their LangArena source ([Reading these numbers](#reading-these-numbers)). Here they are with the hints removed and nothing else (`bench/valk-gc/trees/`), built by release 0.46.0 and measured on the LangArena server on 2026-11-17 (`--release --cpu=v3`, median of 5 rounds); the other languages' rows are the LangArena table's (2026-11-14):

| Program | Avant | Fastest other | Smallest other |
| --- | ---: | ---: | ---: |
| `Binarytrees::Obj` with hints | 1.108 s, 67.6 MB | 2.925 s (Crystal) | 66.5 MB (C, Clang) |
| `Binarytrees::Obj` without hints | 2.882 s, 114.3 MB | 2.925 s (Crystal) | 66.5 MB (C, Clang) |
| `Calculator::Ast` with hints | 1.185 s, 8.1 MB | 1.333 s (Crystal) | 10.5 MB (C, GCC) |
| `Calculator::Ast` without hints | 1.541 s, 17.4 MB | 1.333 s (Crystal) | 10.5 MB (C, GCC) |

Unhinted, `Binarytrees::Obj` is faster than Crystal, the fastest other (2.882 s to 2.925 s), and 2.1 to 2.4× faster than C, Rust and Go (6.05 to 6.85 s), at 1.72× C's (the smallest) memory: 1.09× Go's 104.7 MB, below Crystal's 117.9 MB. Unhinted, `Calculator::Ast` is 1.16× Crystal's time (Rust 1.640 s, Go 2.132 s, C 2.739 s are slower) at 1.66× the smallest memory, below Rust, Crystal and Go (19.1 to 25.8 MB). Both trail Crystal or Go (`Binarytrees::Obj` Go's memory, `Calculator::Ast` Crystal's time): defects, like the rows in [Where Avant loses](#where-avant-loses). Until this improves:

- keep small values in structs: a struct allocates nothing;
- reuse a `Buf` or an array, with `clear`, instead of making one per iteration;
- use the `_into(buf)` calls of the standard library;
- hint where a structure is built and dropped whole.

### Collection hints

`std.gc` holds two hints for a large structure that stays alive while it is built, then is dropped whole: the shape the collector handles worst.

```avant
import std.gc

class Tree {
  left: Tree?
  right: Tree?
}

fn build(depth: Int): Tree {
  t = Tree.new()
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

- **`gc.hold { ... }`** is a statement: this thread starts no collection during the block; one that falls due inside runs when the block ends. A hold whose own allocations pass `AVANT_HEAP_MAX_MB` stops the program with `heap limit in a gc.hold block: its own allocations passed AVANT_HEAP_MAX_MB (in MODULE/FUNCTION)`. The hold ends on every way out (the block's end, `return`, `break`, `continue`, a `?` that returns); holds nest as one. Other threads still collect, so a hold never makes another thread wait.
- **`gc.collect()`** is a complete collection: when it returns, every object nothing reaches is freed, however young or old. It is a young collection, cheap, when the collector can prove no old object died since its last full collection (no reference to an old object was removed from an object or a local, and nothing became old), otherwise a full one, tracing everything alive. Inside a hold it runs when the hold ends; with `AVANT_NURSERY=0` or more than one thread it is always full.
- Put the end of a hold where the structure is dead, and `gc.collect()` where it dies.
- `gc` names these two forms in a module that imports `std.gc`, unless a local or a field takes the name; without the import, `gc.hold` is an error. Under `AVANT_GC_STRESS=1` a hold is inactive, so a stress run checks the code inside it like any other.

### Collector settings

Each setting is an environment variable read once at start: a switch is `1` (on) or `0` (off), a size a whole number; any other value is reported on stderr and ignored.

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

The useful two: `AVANT_HEAP_MAX_MB` to cap a service, `AVANT_GC_STATS` to see what the collector did.

## C interop

Declare C functions in a `lib` block and call them through its name; the standard library is built this way, on yyjson, PCRE2, zlib and uSockets.

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

A `lib NAME { ... }` block holds `fun` declarations (`fun` for a C function, `fn` for an Avant one), called through the lib's name (`c.abs`). Hand-written and generated bindings ([avant bind](#avant-bind)) share the grammar; there is no `extern`.

- A `String` argument is passed as a `char *` to its bytes, terminated: a long slice that ends its string's bytes is passed in place, any other slice as a copy for the call.
- A string C gives the program (a `String` result, a `String` field of C memory or of a struct C returns, a callback's `String` argument) is copied on the way in: C may change or free its memory afterwards.
- A `String?` result is a `char *` that may be NULL, read as `nil` (passing `nil` passes NULL); a `String` result that is NULL traps, naming the function (`avant runtime: C function c.getenv returned NULL for String (declare String?)`).
- A `fun` signature's only unions are optional references, one pointer with `nil` as NULL: `String?`, `Bytes?`, `Ptr(T)?` (the nullable C pointer, [Layouts and Ptr](#layouts-and-ptr)), and an array, a hash, a `Buf` or a class with `?`. Any other union (`Int?`, `Int | String`, a tag and a payload where C has one word) is a compile error there.
- An `Array` (or a `Hash`, a `Buf`) passed to C is the value itself, not a copy: C may change it, except an array or a hash a running task reads ([What may cross a task](#what-may-cross-a-task)), which C must leave as it is; the runtime's own C functions that write an array or a hash trap on such a value, as Avant code does.
- The runtime's own C functions (`avant_c_str`, `avant_alloc_slow`, every entry the compiler calls) are reserved: a `fun` of that name is a compile error (`C function avant_c_str is reserved: ...`).
- A C function that answers through a pointer (`int *out`, `natsMsg **msg`) takes `&x`, the address of a local: C writes it during the call, and afterwards `x` holds what C wrote (`msg: Ptr(Msg)? = nil`, then `nats.natsMsg_Create(&msg, ...)`). `x` is a local whose type is the parameter's pointee (`Int32` for `Ptr(Int32)`, `Ptr(Msg)?` for `Ptr(Ptr(Msg)?)`; a narrow pointee such as `Ptr(UInt8)` takes an `Int`, range-checked on the way in); `&x` is only a `fun` argument, never a field, an element or a value, so C never holds the address of anything the collector moves.
- C++ is reached only through `extern "C"` functions.

```avant
lib m {
  fun frexp(x: Float64, exp: Ptr(Int32)): Float64
}

fn main {
  e: Int32 = 0
  f = m.frexp(48.0, &e)
  puts("${f} ${e}")     // => 0.75 6
}
```

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
| a pointer | `Ptr(Layout)`, `Ptr(Void)`, or a pointer to a scalar or a pointer (`int *` is `Ptr(Int32)`, `uint8_t *` is `Ptr(UInt8)`, `T **` is `Ptr(Ptr(T)?)`); `Ptr(T)?` where C may give NULL | the same |

The five narrow names (`Int8`, `Int16`, `UInt8`, `UInt16`, `Float32`) are written only in a `fun` signature, a C layout's field and a `Ptr`'s type (`Ptr(UInt8)` anywhere). A value from C converts exactly. A value to C keeps its value or traps (`avant runtime: 256 out of range for the C type UInt8 (0..255)`); a `Float64` rounds to the nearest `Float32`.

<!-- check: fragment needs a C library of its own; the tested twin is tests/fixtures/ffi/cscalars.av -->
```avant
lib img {
  fun img_scale(n: UInt32, gain: Float32): UInt8
}
```

### Layouts and Ptr

A `struct` inside a `lib` block is a C layout (C's size, alignment and field offsets), not an Avant value: it is never built with `{ }`; its fields are read and written through a `Ptr(T)`, and its methods take a `Ptr(T)` receiver.

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
  p = c.malloc(Pair.size)
  p.a = 40
  p.b = 2
  puts(p.sum())     // => 42
  c.free(p)
}
```

`Ptr(T)` is for a layout `T`, a C scalar or a pointer (the elements of a C array), `Ptr(Void)` for a pointer you only pass around or convert with `.as(T)`; a `Ptr` of an Avant type (`Ptr(String)`, `Ptr(Array(Int))`) is a compile error. Memory from C is C's: the collector neither moves nor frees it. `Pair.size` is the layout's C size (`sizeof(Pair)`), known when compiling, so `c.malloc(Pair.size)` replaces a size counted by hand; it takes the integer type its place wants, as a literal does.

`p[i]` reads element `i` of the C array a `Ptr` points to (`p[0]` its one value): `Ptr(UInt8)`, `Ptr(Int32)`, `Ptr(Ptr(Reply)?)` and the rest; `p[i] = v` writes it. A narrow element reads as its Avant type and is range-checked when written, as a layout's field. C gave no length, so nothing checks `i`: the count comes from C (`elements`, `len`) and stays your code's to respect. A NULL `p` traps.

```avant
lib c {
  fun calloc(n: Int, size: Int): Ptr(Int32)
  fun free(p: Ptr(Int32))
}

fn main {
  xs = c.calloc(3, 4)       // C's int[3]
  for i in 0..3 {
    xs[i] = (i * 10).to_i32()
  }
  xs[2] += 1
  puts(xs[1] + xs[2])       // => 31
  c.free(xs)
}
```

A field `message: Int8[504]` is C's `char message[504]` (C's order, `T[N]`: `T` a C scalar, a `Ptr` or a layout, `N` an integer literal or a module constant written as one): it takes C's room and alignment, so the fields after it sit where C puts them. It is no value: `e.message[i]` reads or writes one element (checked against `N`), `e.message.size` is `N`, and an `Int8` or `UInt8` array's `e.message.to_s()` is its text up to the first NUL (at most `N` bytes).

```avant
lib c {
  struct Err {
    code: Int32
    tag: UInt8[4]
    next: Int32
  }
  fun calloc(n: Int, size: Int): Ptr(Err)
}

fn main {
  e = c.calloc(1, Err.size)
  e.tag[0] = 111
  e.tag[1] = 107
  e.next = 7
  puts("${e.tag.to_s()} ${e.tag.size} ${Err.size}")   // => ok 4 12
}
```

`Ptr(T)?` is the nullable pointer: one word, `nil` is C's NULL, in a `fun` result or parameter, a layout's field, and anywhere in Avant code. Declare it where C may give NULL (a failed `malloc`, the end of a list) and test it as any optional:

```avant
lib c {
  struct Node {
    v: Int32
    next: Ptr(Node)?
  }
  fun calloc(n: Int, size: Int): Ptr(Node)?
}

fn length(head: Ptr(Node)?): Int {
  n = 0
  cur = head
  while node = cur {
    n += 1
    cur = node.next
  }
  n
}

fn main {
  if a = c.calloc(1, Node.size), b = c.calloc(1, Node.size) {
    a.next = b
    puts(length(a))         // => 2
  }
  puts(length(c.calloc(1, 1 << 62)))   // => 0
}
```

`v.as(T)` reads a `Ptr(Void)` as a `Ptr(T)` (C's cast of a `void *`; `T` any type a `Ptr` points to, `v.as(Ptr(Node)?)` for an array of pointers), and any `Ptr` passes as a `Ptr(Void)`, as C converts to `void *`: one `malloc` serves every layout.

```avant
lib c {
  struct Pair {
    a: Int32
    b: Int32
  }
  struct Node {
    v: Int32
    next: Ptr(Node)?
  }
  fun malloc(size: Int): Ptr(Void)?
  fun free(p: Ptr(Void))
}

fn main {
  if v = c.malloc(Pair.size), w = c.malloc(Node.size) {
    p = v.as(Pair)
    n = w.as(Node)
    p.a = 2
    n.v = 40
    puts(p.a + n.v)     // => 42
    c.free(p)
    c.free(n)
  }
}
```

A plain `Ptr(T)` can still hold NULL when C gives one: a field read or written through it traps, naming the layout and the field, after the program's output is flushed (`avant runtime: the field a read or written through a NULL Ptr(Pair) (...) at app.av:7:8`); inside a task or an HTTP handler it fails that task or request alone. A `String?` field reads a NULL as `nil` and stores `nil` as NULL; a plain `String` field that holds NULL (C left it unset, or `release` made it null) traps when read, naming the field (`avant runtime: the C field Holder.name is NULL (declare it String? to read NULL as nil)`).

### Strings in C memory

The collector moves objects, so a heap value handed to C must stay put while C can see it.

- **For a call**, nothing is needed: arguments stay put while the C function runs.
- **For a bounded span longer than a call**, `pin(value) { ... }` keeps it put for the block and unpins it on every way out; pins are counted, so nested pins compose.
- **Stored into a C field** (`h.name = s`), a `String` stays pinned until `release` says C is done with that field; overwriting the field keeps the old value pinned too. A slice that shares its string's bytes is stored as a copy of its own.

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

- `release(h.name)` unpins every value Avant stored into that field, current and overwritten, and makes the field null if it still holds one of them.
- `release(h.at)` does the same for every `String` field of a nested layout, `release(h)` for every field of the struct `h` points to: write it before C frees the struct.
- Releasing twice, or a field Avant never stored, does nothing. A value C stored itself is left in place. A pin taken by `pin(x) { }`, or by storing the same value into another field, stays.
- `release` is a statement taking a C-layout field that holds a `String` (directly or in a nested layout) or a `Ptr` to such a layout; anything else is a compile error.
- **Forgetting a release costs memory, never correctness** (the value stays pinned until exit). **Releasing too early is a dangling C pointer**: the next collection may move or free the value under C. Neither the compiler nor `AVANT_GC_VERIFY=1` can see C's reads.

A C buffer with a length (a `char *str` with its `size_t len`, a document's bytes, a message's payload) is copied out with `Bytes.from_c(p, n)`, or `String.from_c(p, n)`: `n` bytes at `p` (any `Ptr`, or a `Ptr(T)?`) into a new value, NUL bytes kept; a `String` result or field stops at its first NUL. A negative `n`, or a NULL `p` with `n` above 0, traps. As Go's `C.GoBytes` and `C.GoStringN`, it is a copy: C may free its buffer afterwards.

```avant
lib c {
  fun calloc(n: Int, size: Int): Ptr(UInt8)
  fun free(p: Ptr(UInt8))
}

fn main {
  p = c.calloc(4, 1)
  p[0] = 104
  p[2] = 105
  b = Bytes.from_c(p, 4)
  c.free(p)
  puts(b.size)     // => 4
  puts(b[2])       // => 105
}
```

### Callbacks

A `fun` parameter of an Avant `fn` type declares a C callback: pass a function name or a lambda that captures nothing, and C calls it with exactly the declared arguments.

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

A capturing lambda, a `fn` value held in a variable, and a C function returning a function pointer do not compile. A callback's type may use the five narrow names (`fn(UInt8, Float32): Int16`): the Avant function takes and returns their Avant types (`fn(Int, Float64): Int`), each argument converts exactly on the way in, and the result is checked on the way out (out of range traps; a `Float64` rounds to the nearest `Float32`), as for a `fun`'s own parameters.

### Linking C code

```sh
avant compile app.av --lib z            # link a system library: -lz
avant compile app.av --cc helper.c      # compile and link a C file (or link a .o)
avant run app.av --lib z
```

The C and math libraries need no flag; the standard library's C libraries (yyjson, PCRE2, zlib, a Base64 codec, uSockets, picohttpparser) are compiled into the runtime shipped with `avant`.

### avant bind

`avant bind HEADER.h` reads a C header with libclang and prints a `lib` block to keep and edit; it builds and writes nothing. Given this header,

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

- Each C scalar type maps to its exact name ([C types](#c-types)); a type with no Avant name (`long double`) fails the bind with a message.
- A `char *` result is bound as `String?` (C returns NULL for "absent" or "failed"), a `char *` parameter as `String`.
- Any other pointer is bound as `Ptr` of its pointee: `int *` as `Ptr(Int32)`, `unsigned char *` as `Ptr(UInt8)`, a pointer to a pointer as `Ptr(Ptr(T)?)` (`char **` is `Ptr(Ptr(Int8)?)`, `void **` is `Ptr(Ptr(Void)?)`), in a function and in a layout's field.
- A named four-byte C enum becomes an `enum` in the `lib` block, used by the functions and layout fields of that type: the name in CamelCase without a trailing `_t` (`enum http_method` is `HttpMethod`, `color_t` is `Color`), the constants lower-cased with their values (`HTTP_GET = 0` is `http_get = 0`). It is stored as C's `int`, and every value C gives (a result, a field, a callback's argument) is checked: a non-member traps, naming the enum and the number. An alias (a second constant with the same value) is dropped, named in a comment; a value outside `Int32`, a constant that is not a valid Avant member name, or two constants equal after lower-casing leave the enum out with a comment, its functions taking `Int32`.
- An anonymous enum (`enum { FLAG_A = 1, FLAG_B = 2 };`, how C spells constants) becomes [module constants](#constants) after the `lib` block, lower-cased, with C's values (`pub const flag_a = 1`); a name that is no Avant name, or one an earlier constant took after lower-casing, is left out with a comment.
- `--lib NAME` names the block. It needs libclang 20 (`sudo apt install libclang1-20`); a release ships the `avant-bind` helper beside `avant`.

## Macros

`quote` pastes code, `#()` splices a value into it, and one compile-time loop walks a type's fields; there is no general compile-time interpreter.

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

`comptime { for f in Type.fields { quote { ... } } }` runs its quote once per field, with `f.name` and `f.type` to splice. `Type` must be a concrete, non-generic name (`for f in Box.fields` on a generic type is an error). In another module than the type's, the walk sees only `pub` fields.

## Builtins

The prelude, used without an import, is the types and methods above ([Strings](#strings), [Arrays](#arrays), [Hashes](#hashes), [Sets](#sets), [Bytes and Buf](#bytes-and-buf)) and these free functions:

| Area | Names |
| --- | --- |
| Output and input | `puts`, `print`, `eputs`, `eprint`, `puts_u`, `exit`, `read_line` |
| Files and directories | `file_read`, `file_write`, `file_open`, `file_exists`, `file_size`, `file_delete`, `dir_list`, `dir_walk`, `dir_exists`, `dir_make` |
| Processes and environment | `argv`, `env_get`, `process_run`, `process_run_out`, `process_output` |
| Time | `now_ms`, `now_us` |
| Hashing and math | `sha256`, `sqrt`, `exp`, `fmt_float` |
| Strings and buffers | `str_slice`, `str_to_f_slice`, `chr`, `repeat_byte`, `bytes_to_str`, `buf_new` ([String operations](#string-operations)) |

Runnable programs using them are in [examples/](examples), in the repository and in the release archive beside `avant`. In the tables below, a dash means there is nothing to say: no parameter, no result, or no failure.

### Output

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `puts(x)` | `x`, any printable value; none for `puts()` | — | a failed write stops the program with a message, status 1 | writes `x` and a newline to stdout; `puts()` an empty line |
| `print(x)` | `x` | — | as `puts` | writes `x` alone to stdout |
| `eputs(x)` | `x`, or none | — | as `puts` | writes `x` and a newline to stderr; `eputs()` an empty line |
| `eprint(x)` | `x` | — | as `puts` | writes `x` alone to stderr |
| `puts_u(n)` | `n`, an integer | — | as `puts` | prints an integer as the unsigned number its bits spell |
| `exit(status)` | `status`, 0 to 255 | never returns | a status outside 0 to 255 traps at the call (`exit status 256 out of range (0..255)`) | writes nothing: flushes stdout, removes the program's temporary directory and ends the process with that exit status, from any task |

```avant
enum Level { low, high }

fn main {
  print("level: ")
  puts(Level.high)                    // => level: high
  n = "12".to_i() ?? 0
  puts(n * 2)                         // => 24
  eputs("this line goes to stderr")
}
```

`x` is written exactly as `"${x}"` ([Printing](#printing)): a number, a `Bool`, a `String` (every byte, a NUL included), a type with its own `to_s`, an enum, an error, or a union of those; `puts(x)` of an `Int?` is a compile error naming the ways to open it. stdout is buffered, stderr is not; a write to stderr first flushes stdout, so both keep their order (a terminal, `2>&1`). As `exit` never returns, like `return` it may end an [`else e` guard](#reading-the-error-itself), a function with a result, or a value-giving `switch` arm. Without it, the exit status is `main`'s result.

### Input

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `read_line()` | — | `String \| IoError \| Nil`: the next line of standard input without its newline (or a `\r` before it), an unterminated last line included | `nil` at the end; an `IoError` if the read fails | when stdout is a terminal, `read_line` flushes it first, so a `print` prompt shows |
| `file_read("/dev/stdin")` | — | `String \| IoError` | as [`file_read`](#files-and-directories) | reads all of standard input |

`while line = read_line() { ... }` stops at either; a guard tells them apart (the failure is `nil` at the end).

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

With an empty input it prints 0. `read_line() ?? ""` reads one answer, the end as empty.

### Files and directories

Every `path` is a `String`.

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `file_read(path)` | `path` | `String \| IoError`: the whole file | an `IoError` | |
| `file_write(path, body)` | `path`, `body: String` | `Int \| IoError`: the bytes written | an `IoError` | the file is created or replaced |
| `file_open(path)` | `path` | `FileReader \| IoError`: the file, to read line by line | an `IoError` | `file_open` never holds the whole file |
| `r.read_line()` | — | `String \| IoError \| Nil` | as [`read_line`](#input) | a `FileReader`'s `r.read_line()` is [`read_line`](#input) on its file |
| `r.close()` | — | — | — | closes the file; it also closes at the end, or on an error; a reader dropped earlier keeps it open until the program ends |
| `file_size(path)` | `path` | `Int \| IoError`: its size in bytes | an `IoError` | |
| `file_delete(path)` | `path` | `Void \| IoError`: nothing when the file is removed | an `IoError`; a directory is an error | |
| `file_exists(path)` | `path` | `Bool`: a file or a directory is there | — | |
| `dir_list(path)` | `path` | `Array(String) \| IoError`: the names, sorted, without `.` and `..` | an `IoError` | |
| `dir_walk(path)` | `path` | `Array(String) \| IoError`: every name below it, as a path relative to it | an `IoError` | `dir_walk` lists a directory before what it holds and the names of each directory sorted; a link to a directory is listed and not followed |
| `dir_make(path)` | `path` | `Void \| IoError`: nothing when the directory and its missing parents are made | an `IoError` | one that is there already is fine |
| `dir_exists(path)` | `path` | `Bool`: a directory is there | — | |

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

A directory walk and housekeeping:

```avant
fn remove(path: String) {
  file_delete(path) else e {
    puts(e.message())
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

An `IoError` has `op` (`"read"`, `"write"`, `"list"`, `"open"`, `"stat"`, `"delete"` or `"make"`), `path` and `errno` (the OS error number); its message reads `cannot read notes.txt: No such file or directory`. The names `IoError`, `ProcessError`, `FileReader` and `ProcessOutput` are taken only in a program that calls these functions.

### Processes and environment

A `program` is a `String` and `args` an `Array(String)`; a program name without `/` is looked up on `PATH`.

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `argv()` | — | `Array(String)`: the program's name, then its arguments ([Program arguments](#program-arguments)) | — | |
| `env_get(name)` | `name: String` | `String?` | `nil` when the variable is not set | |
| `process_run(program, args)` | `program`, `args` | `Int \| ProcessError`: its exit status | a `ProcessError` (below) | runs the program and waits; `process_run`'s child shares the program's stdin, stdout and stderr |
| `process_run_out(program, args, out)` | `program`, `args`, `out: String` | the same | the same, with `output` when the output file could not be opened | the same, with stdout and stderr written to the file `out` |
| `process_output(program, args, input)` | `program`, `args`, `input: String` (`""` when left out) | `ProcessOutput \| ProcessError`: its `status`, `stdout` and `stderr` | a `ProcessError` | runs the program with `input` on its stdin, then end of input, waits, and reads its stdout and stderr as they come, so it never stalls (a child that stops reading early is not an error) |

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

**A non-zero exit status is not an error**, only the `Int`. A `ProcessError` is a program that could not start (`errno`; `output` when the output file could not be opened) or a signal killed (`signal`): `cannot run ls: No such file or directory`, `sh killed by signal 9 (Killed)`.

### Time, hashing and math

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `now_ms()`, `now_us()` | — | `Int` milliseconds, microseconds | — | a monotonic clock, without wrapping: intervals, not the time of day |
| `sha256(s)` | `s: String` | `String`: 64 hexadecimal characters | — | the SHA-256 of a string |
| `sqrt(x)`, `exp(x)`, `fmt_float(x, digits)` | | | | in [Floats](#floats) |
| `checksum_str`, `checksum_f64`, `crc32_bytes`, `sha256_word0` | | each an `Int32` | | also taken, for the benchmark programs |
| `wide_op`, `wide_hi` and `hash_last_key` | | | | also taken, for the benchmark programs |

```avant
fn main {
  t0 = now_us()
  puts(now_us() >= t0)       // => true
  puts(sha256("abc"))        // => ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  puts(sqrt(144.0))          // => 12.0
}
```

No time-of-day call or `sleep` yet; bind C's `usleep` (`lib c { fun usleep(us: UInt32): Int32 }`).

## Standard library

Each module is imported on its own and links only its own C library: a program that never imports `std.regex` does not carry PCRE2.

| Module | Gives you | Stands on |
| --- | --- | --- |
| [`std.json`](#stdjson) | `Json.parse`, a document to walk, `JsonBuilder` | yyjson |
| [`std.regex`](#stdregex) | `Regex.compile`, `Match` values, replace and split | PCRE2, and re2c scanners for literal patterns |
| [`std.http`](#stdhttp) | HTTP server and client, WebSocket server and client | uSockets and picohttpparser |
| [`std.zlib`](#stdzlib) | `Bytes.compress`, `Bytes.uncompress` | zlib |
| [`std.base64`](#stdbase64) | `Bytes.to_base64`, `Bytes.from_base64` | the aklomp Base64 codec |
| `std.sync` | `Channel`, `Mutex`, `pool_size`, `set_pool_size` | the runtime's tasks ([Concurrency](#concurrency)) |
| `std.gc` | `gc.hold { }`, `gc.collect()` | the collector ([Collection hints](#collection-hints)) |
| `std.testing` | `Test`, the checks of a test file | — ([Testing](#testing)) |

The libraries are compiled into the runtime, nothing to install; their speed is theirs ([Reading these numbers](#reading-these-numbers)). A module's file is its import name: `std.regex` is `lib/std/regex.av`. A failure is an error value (`Json.parse` returns `Json | JsonError`); a call that allocates its result usually has an `_into(buf)` twin writing into your `Buf`.

### std.json

```avant
import std.json

fn main {
  text = "{\"name\": \"ada\", \"langs\": [\"avant\", \"c\"], \"age\": 36}"
  if doc = Json.parse(text) {
    puts(doc.str("name") ?? "?")       // => ada
    puts(doc.int("age") ?? 0)          // => 36
    if langs = doc.get("langs"), first = langs.at(0) {
      puts(langs.len())                  // => 2
      puts(first.str() ?? "?")           // => avant
    }
  }

  out = Json.object()
  out.set("ok", true)
  out.set("count", 2)
  out.key("tags").arr().add("x").add("y").end()
  out.end()
  puts(out.to_s())                       // => {"ok":true,"count":2,"tags":["x","y"]}
}
```

**Reading.** A `Json` is a position in the parsed document, which owns its values and text: nothing to close.

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Json.parse(text)` | `text: String` | `Json \| JsonError` | a `JsonError` says what yyjson found and where: `invalid JSON at byte 7: trailing comma is not allowed`, with the byte offset in `e.offset` (`-1` when there is none) | |
| `get(key)`, `at(index)`, `first` | `key: String`, `index: Int` | `Json?`: a member, an element | `nil` when there is none | |
| `len` | — | `Int`: the number of members or elements | — | |
| `key_at(index)` | `index: Int` | `String?`: a member's key | `nil` when there is none | |
| `arr`, then `next` on it | — | `JsonIter?`: the elements of an array, each in turn (`Json?`) | `nil` for any value but an array, so a walk over a string, a number, an object or `null` is not an empty walk; `next` is `nil` after the last | `if it = v.arr() { while e = it.next() { ... } }` |
| `arr(key)`, then `next` on it | `key: String` | `JsonIter?`: the elements of the member `key` | `nil` when there is no such member or it is not an array | `it = doc.arr("numbers") ?? return bad_request()` |
| `int`, `f64`, `str`, `bool` | — | this value as that type | `nil` when it is another type | `int` is `nil` for anything but a JSON integer (no fraction, no exponent) within `Int`: `3.7`, `3.0`, an integer beyond `Int`, a non-number. `f64` reads any number |
| `int(key)`, `f64(key)`, `str(key)`, `bool(key)` | `key: String` | the same, for a member | the same | |
| `is_null`, `is_bool`, `is_int`, `is_float`, `is_str`, `is_arr`, `is_obj` | — | `Bool` | — | |
| `f64s(key)` | `key: String` | `Array(Float64)?`: the numeric member `key` of every element of an array of objects | `nil` otherwise | |
| `text` | — | `String`: the value written back as JSON text | — | |
| `to_value` | — | a plain `JsonValue` tree (an Avant class) | — | it outlives the document; an object's members are in `members`, a `Hash` by name (a repeated name keeps its first value), and their names in `keys`, in the document's order |

**Writing.** Each call on a `JsonBuilder` but `to_s` returns the builder, so calls chain.

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Json.object`, `Json.array` | — | a `JsonBuilder` | — | start a `JsonBuilder` with an object, an array open (not `obj` and `arr`: those read a parsed value) |
| `JsonBuilder.into(buf)` | `buf: Buf` | a `JsonBuilder` | — | starts one that writes into your `Buf` |
| `set(name, v)`, `set_null(name)` | `name: String`, `v` (below) | the builder | — | a member of the open object |
| `add(v)`, `null` | `v` (below) | the builder | — | an element of the open array |
| `key(name)`, then `obj`, `arr` or `value(v)` | `name: String`, `v` (below) | the builder | `key` outside an object traps | a member that is itself a container or a value |
| `end` | — | the builder | `end` with nothing open traps (`avant runtime: JsonBuilder.end: nothing open`) | closes the innermost open object or array |
| `to_s` | — | `String`: the text | `to_s` with containers open traps (`JsonBuilder.to_s: 2 containers still open`) | |

`v` is an `Int`, `Float64`, `String`, `Bool`, `Json` or another `JsonBuilder`. Floats use the language's float text (`1.5e-7`, `2.0`); NaN and the infinities are `null`. A builder used against its shape traps, saying how (the Errors column).

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
    re.close()
  }
}
```

Compiling it prints `re2c: pattern stays on PCRE2 (...)` on stderr (**Two engines**, below).

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Regex.compile(pattern, caseless = false, match_limit = 10000000, depth_limit = 10000000, heap_limit = 268435456)` | `pattern: String`, `caseless: Bool`, the limits `Int` (below) | `Regex \| RegexError` | a pattern that does not compile is a `RegexError` with PCRE2's reason and the offset it points at (`invalid regular expression at offset 9: missing closing parenthesis`, `e.offset`, `e.kind` is `RegexErrorKind.pattern`); a limit below 1 traps | every copy of a `Regex` shares one compiled pattern; a bounded cache makes compiling a pattern again cheap |
| `re.find(text, from = 0)` | `text: String`, `from: Int` | `Match?`: the first match at or after byte `from` | `nil` when there is none; a limit reached traps | |
| `re.try_find(text, from = 0)` | `text: String`, `from: Int` | `Match? \| RegexError`: `find`'s answer, or why there is none | a limit reached is a `RegexError` (`Regex.try_find: match limit exceeded (PCRE2 error -47)`; `e.kind`: `match_limit`, `depth_limit`, `heap_limit`, `memory` or `failed`) | for a pattern or a text from outside |
| `m.start`, `m.end` | — | `Int`: the match's bounds, in bytes | — | a `Match` is a value: there are no global match cursors |
| `m.group(i)`, `m.group("name")` | `i: Int`, or a group's name | `String?`: a group's text | `nil` when it did not take part | |
| `m.span(i)`, `m.group_count()` | `i: Int` | a group's bounds (`Span?` with `lo` and `hi`); the number of groups | — | |
| `re.count(text)` | `text: String` | `Int`: the number of non-overlapping matches | — | |
| `re.matches(text)`, then `next` on it | `text: String` | each match in turn (`Match?`) | `nil` after the last | |
| `re.replace(text, repl)` | `text`, `repl: String` | a new string with every match replaced | — | `re.replace_into(buf, text, repl)` appends it to a `Buf`. In a replacement, `$1` to `$9`, `${1}` and `${name}` insert a group (nothing when it did not take part) and `$$` is a dollar sign |
| `re.split(text)` | `text: String` | `Array(String)`: the pieces between matches, empty pieces kept | — | |
| `re.close()`, `re.is_closed()`, `re.scoped (r) { ... }` | a lambda for `scoped` | `is_closed` a `Bool` | — | release the pattern; ask; run a block and close on exit. `re.close()` through any copy closes all |

**After close.** Any operation on a closed `Regex` traps (`avant runtime: Regex used after close`), never "no match", as do a `Match`'s named groups and groups past the third (read from the pattern). A `Regex` that is dropped without `close` keeps its compiled pattern until the program ends (the collector runs no finalizer), but not its place in the cache: compiling new patterns stays cached.

**Limits.** Each `Regex` has its own, set at `compile` (`Regex.compile(p, match_limit: 100000, heap_limit: 1 << 20)`): `match_limit` bounds the backtracking steps of one match (the default, ten million, is reached in about 40 ms by `(a+)+$` on the JIT), `depth_limit` the interpreter's backtracking depth, and `heap_limit` the bytes one deep match may take (256 MiB by default: the JIT's larger stack and the interpreter's frames). Deep nesting alone (a group repeated many thousands of times) is not a failure: a larger stack, then PCRE2's interpreter, answers it within `heap_limit`. A limit past what PCRE2 holds (32 bits; `heap_limit` in KiB) is the largest it holds. A literal pattern on re2c's scanner never backtracks; its `count` runs on PCRE2 and keeps the limits. A pattern may have any number of groups PCRE2 accepts.

**At a limit a match is never "no match"**: `try_find` returns a `RegexError`, and `find`, `count`, `replace`, `split` and `matches` trap with the operation and PCRE2's reason (`avant runtime: Regex.replace: match limit exceeded (PCRE2 error -47)`). For a pattern or a text from outside, set the limits and match with `try_find`.

**Two engines.** re2c compiles a string-literal pattern with at most one capture group and no named groups to a native scanner. Any other pattern runs on PCRE2's JIT; a literal one (with a literal `caseless`, if given) gets a note on stderr, so `avant run`'s stdout stays the program's. Both ship inside the release. Without a `re2c` binary literal patterns run on PCRE2 too; `AVANT_RE2C` names another binary. PCRE2 backtracks: adversarial patterns and texts can be catastrophic.

### std.http

Cleartext HTTP/1.1 and WebSockets: a server, a blocking HTTP client and a blocking WebSocket client. **No TLS** ([Not built yet](#not-built-yet)): `https` and `wss` URLs are an `HttpError`, never a silent downgrade.

```avant
import std.http

fn main {
  if server = Http.listen(0) {                 // port 0: the OS picks a free port
    server.get("/hello", (req) {
      server.close()                             // this example answers once, then stops
      Response.text("hello ${req.query ?? "nobody"}")
    })
    port = server.port()
    client = spawn {
      if resp = Http.get("http://127.0.0.1:${port}/hello?ada") {
        puts(resp.status)                               // => 200
        puts(String.from_utf8(resp.body) ?? "")         // => hello ada
      }
    }
    server.serve()
    _ = client.join()
  }
}
```

#### HTTP server

`Http.listen(port)` binds at once (a bad port fails there); register routes, then `serve` runs until `close`.

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080, "0.0.0.0") {
    server.get("/", (req) {
      Response.text("hello")
    })
    server.post("/echo", (req) {
      Response.bytes(req.body).header("X-Echo", "1")
    })
    server.route("OPTIONS", "/", (req) {
      Response.text("", 204)
    })
    server.serve()
  }
}
```

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Http.listen(port, host = "127.0.0.1", max_body = 1048576, loops = 0, max_message = 1048576)` | `port: Int`, `host: String`; `max_body`, `loops` and `max_message` are `Int`s | `Server \| HttpError` | an `HttpError` when it cannot bind | `max_body` bounds a request body; `loops` overrides the event-loop count; `max_message` bounds a WebSocket message |
| `server.port()` | — | `Int`: the bound port | — | |
| `get`, `post`, `put`, `delete`, `patch`, `route(method, path)` | `path: String` (and `method: String`), then the handler, a lambda | the `Server` | one registered from a handler while serving traps (`avant runtime: route GET /x registered while the server is serving: register every route before serve`; the handler's request fails, answered 500) | **routes** are exact paths; no pattern router, middleware or streaming. Register every route, WebSocket ones too, before `serve` |
| a handler, `(req) { ... }` | a `Request` | a `Response` | — | gets `method`, `path`, `query` as `String?` without the `?`, `headers`, `body` as `Bytes` |
| `Response.text(body, status = 200)`, `Response.bytes(body, status = 200)` | `body`: a `String`, `Bytes`; `status: Int` | a `Response` | a `status` outside 200 to 599 traps when written (`avant runtime: a response's status 99 is not 200 to 599: …`): a 1xx is an interim answer, never a handler's (the WebSocket upgrade's 101 is the server's) | the only ways to build one; the server sets `Content-Length` and `Connection` |
| `.header(name, value)` | `name`, `value: String` | the `Response` | a header name must be a token (letters, digits, RFC 9110's punctuation) and its value hold no control character but tab, or it traps (`avant runtime: Response.header value of X-A holds a control character (byte 13 at 1): …`) | check request text before putting it in a header |
| `get(name)`, `has(name)`, `get_all(name)`, `count`, `name_at(i)`, `value_at(i)` | `name: String`, `i: Int` | `String?`, `Bool`, `Array(String)`, `Int`, `String?`, `String?` | — | **headers**, on `req.headers` and `resp.headers` |
| `serve` | — | — | — | runs until `close` |
| `close` | — | — | — | stops the server and lets `serve` return, at the latest at the shutdown deadline (below). An unclosed server keeps its socket until the process exits (no finalizer) |

**Bodies** are buffered up to `max_body` (1 MiB by default). Answered, then closed:

| Request | Answer |
| --- | --- |
| a larger body | 413 |
| a chunked request body | 411 |
| a malformed request; more than one `Content-Length` value (two lines, or a list in one, even agreeing); an HTTP/1.1 request without `Host`, two `Host` lines, or a `Host` that is not `host[:port]` (RFC 3986's host, a name or an address, then an optional port) | 400 |

**Limits** are on by default and set like routes, before `serve` (one set while serving traps: `avant runtime: Server.head_timeout called while the server is serving: set every limit before serve`); each setter returns the `Server`, 0 turns its limit off, and a negative value traps:

| Limit | Default | When it is passed |
| --- | --- | --- |
| `head_timeout(ms)`: the time a request head may take from its first byte | 10000 ms | 408, closed |
| `min_rate(bytes_per_second, grace_ms = 5000)`: the rate a request body must arrive at, and an answer the client reads must leave at, over each `grace_ms` from when it began to wait | 240 bytes a second over 5000 ms | a body: 408, closed; an answer: the connection is closed |
| `body_budget(bytes)`: what the bodies of all requests waiting for theirs may hold together (each its `Content-Length`; a body that arrives with its head holds none) | 268435456 (256 MiB) | 503, closed |
| `memory_ceiling(bytes)`: the process's resident memory, read about once a second | 90% of the container's memory limit (cgroup); off when there is none | new requests: 503, closed |
| `max_connections(n)`: the open connections | 10000, at most the file-descriptor limit less 64 and 8 a loop | the loop's longest idle connection is closed; with none idle, the new one |
| `shutdown_timeout(ms)`: how long `close` waits for what it left to finish (an answer being sent, a WebSocket's closing handshake) | 30000 ms | those connections are closed and `serve` returns |

<!-- check: compile -->
```avant
import std.http

fn main {
  if opened = Http.listen(8080) {
    server = opened.head_timeout(5000).min_rate(1000, 2000).max_connections(50000)
    server.get("/", (req) {
      Response.text("hello")
    })
    server.serve()
  }
}
```

- **Connections** are kept alive; pipelined requests are answered in order, each with its own write. One event loop runs per thread of the [pool's count](#the-thread-pool) (`loops` overrides it), all on one port.
- **Handlers must not block the loop**: hand slow work to `spawn`.
- **What a handler captures is checked as a task's** ([What may cross a task](#what-may-cross-a-task)), since every loop thread runs the same handlers at once: a number, `Bool`, `String`, `Bytes`, enum, struct of those, `Channel`, `Mutex` or the `Server` itself may be captured, and an `Array` or `Hash` of those; a class or a function value may not (keep what handlers change in a `Mutex`), the handler cannot assign a captured variable, and a captured `Array` or `Hash` is frozen from the handler's registration on (a change through its name is a compile error, through any other path a trap that fails the request). A handler is a lambda written at the call or a named function; a function value from a variable is refused, since what it captures was not checked.
- **An idle HTTP connection is closed** after about 30 seconds with no bytes in or out (a request in parts has the limits above too); WebSocket connections never time out.
- **A trap in a handler fails its request, not the server**: it is printed to standard error when it happens (`avant runtime: index 7 out of bounds …`), the handler's stack is dropped without unwinding, and the request is answered 500 (the connection kept or closed as any answer's would be); the next request is served. A trap in a WebSocket handler closes that connection with 1011. What the handler held is given back as for a [task that traps](#when-a-task-traps): a `Mutex` it held in `with` is poisoned, its `pin` blocks and `gc.hold`s end. A trap inside a C callback the handler made, and the runtime's own stops, still end the program.

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

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Http.get(url, follow = true, timeout_ms = 10000)` | `url: String`, `follow: Bool`, `timeout_ms: Int` | `Response \| HttpError` | below | |
| `Http.post(url, body, ...)` | `body`, a `String`; then as `get` | `Response \| HttpError` | below | |
| `Http.request(method, url, body, headers, ...)` | `method: String`, `body` as `Bytes` and `headers` an `Array(Header)` (`Header { name: "Accept", value: "text/plain" }`); then as `get` | `Response \| HttpError` | below | |

| The call returns an `HttpError` when | Message |
| --- | --- |
| its method and header names are not tokens, a header value holds a control character but tab (`Response.header`'s rules), or its URL a space or control character: the call sends nothing | naming the part and byte (`invalid request header value of X-A (byte 13 at 1): …`) |
| the response has more than one `Content-Length` value (even identical) or a non-number | `invalid HTTP response body from …` |
| `timeout_ms` passed, whatever had arrived | `request to 127.0.0.1:8080 timed out` |

- The client sets `Content-Length` and `Connection`, skipping a caller's. It sends one `Host`: the URL's, or the caller's in its place (to reach a virtual host by address).
- A call blocks its thread until the response is complete (from a server handler, `spawn` first): as the framing says (the `Content-Length`, a chunked body's last chunk, or the head alone for HEAD, 204 and 304), or, with neither length nor chunks, when the server closes. Interim answers (`100 Continue`, `103 Early Hints`) are skipped.
- `timeout_ms` is one deadline, kept within a few milliseconds, for the whole call (connection, request, response, every redirect); a slow server does not extend it. Zero or less means the default, 10000.

#### WebSocket server

WebSocket routes share the HTTP server and its port. A handler runs once per complete message.

<!-- check: compile -->
```avant
import std.http

fn main {
  if server = Http.listen(8080) {
    server.websocket("/echo", (conn, msg) {
      conn.send_message(msg)                   // echo, text or binary
    })
    server.websocket_close("/echo", (conn, code, reason) {
      puts("closed with ${code}")
    })
    server.serve()
  }
}
```

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `websocket(path, (conn, msg) { ... })` | `path: String`, a lambda | the `Server` | a message over `max_message` (1 MiB by default) closes the connection with code 1009 and never reaches the handler | runs once per complete message |
| `websocket_open(path, (conn) { ... })`, `websocket_close(path, (conn, code, reason) { ... })` | `path: String`, a lambda | the `Server` | — | run once per connection (code 1006 when it dropped without a close frame) |
| `msg.text()`, `msg.is_text` | — | `String?`: the text, `nil` for a binary message or bytes that are not valid UTF-8; `Bool` | — | a text message |
| `msg.body`, `msg.is_binary` | — | `Bytes`; `Bool` | — | `msg.body` is always the payload |
| `send_text`, `send_bytes`, `send_message`, `close(code = 1000, reason = "")` | the text, `Bytes` or message; `code: Int`, `reason: String` | `Bool` | `false` when the connection is closed or its send queue is full | a message that cannot go out at once and would leave more than 1 MiB waiting is refused whole; one the socket has begun to take is always sent whole |
| `conn.sender` | — | a sender with the same four calls | — | a connection stays on its loop thread: to answer from a `spawn` block, capture `conn.sender` and send through it |

A `WsConn` kept after its connection closed, or after `serve` returned, answers `false` to all four. Ping, the close handshake and fragmentation are handled for you. `server.close()` sends code 1001 to open connections.

#### WebSocket client

<!-- check: compile -->
```avant
import std.http

fn main {
  if conn = Ws.connect("ws://127.0.0.1:8080/echo") {
    conn.send_text("hi")
    if msg = conn.recv() {
      puts(msg.text() ?? "")
    }
    conn.close()
  }
}
```

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `Ws.connect(url, timeout_ms = 10000)` | `url: String`, `timeout_ms: Int` | `WsConnection \| HttpError` | an `HttpError` without connecting for a URL with a space or a control character | `timeout_ms` covers the connection and its handshake |
| `recv(timeout_ms = -1)` | `timeout_ms: Int` | `WsMessage?` | `nil` when the connection closed or the timeout passed, and `close_code` says which | blocks. Never call `recv` from a server handler |
| `close_code` | — | `Int` | — | 1006 until a close frame arrives |
| `send_text`, `send_bytes` | a `String`, `Bytes` | `Bool` | `false` after `close` | |
| `close` | — | — | — | after `close`, every copy of the connection answers as closed, whatever opens later: `send_text` and `send_bytes` return `false`, `recv` `nil`, `close_code` 1006, and another `close` does nothing |

Both timeouts use the monotonic clock. An unclosed connection keeps its thread until the process exits. A `spawn` block may capture a `WsConnection`, so one task can wait in `recv` while another sends; a `close` makes the waiting `recv` return `nil`.

### std.zlib

```avant
import std.zlib

fn main {
  packed = "avant avant avant avant".bytes.compress()
  if back = packed.uncompress(), text = String.from_utf8(back) {
    puts(text)       // => avant avant avant avant
  }
}
```

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `bytes.compress` | — | `Bytes` | — | |
| `bytes.uncompress` | — | `Bytes \| ZlibError` | a `ZlibError` | |
| `compress_into(buf)`, `uncompress_into(buf)` | `buf: Buf` | `ZlibError?` | a `ZlibError` | write into a `Buf` |

### std.base64

```avant
import std.base64

fn main {
  enc = "avant".bytes.to_base64()
  puts(enc)                                    // => YXZhbnQ=
  if raw = enc.bytes.from_base64() {
    puts(String.from_utf8(raw) ?? "")          // => avant
  }
}
```

| Call | Parameters | Result | Errors | Note |
| --- | --- | --- | --- | --- |
| `to_base64` | — | a `String` | — | encoding never fails |
| `from_base64` | — | `Bytes \| Base64Error` | a `Base64Error` on malformed input | |
| `to_base64_into(buf)`, `from_base64_into(buf)` | `buf: Buf` | `Base64Error?` | as above | the `Buf` twins |

## Testing

A test file is a program that imports `std.testing` and returns `t.finish()` from `fn main: Int`; `avant test` finds the files, runs them in parallel and reports.

```avant
import std.testing

fn greet(name: String): String {
  "Hello, ${name}"
}

fn main: Int {
  t = Test.new()
  t.check(greet("Ada") == "Hello, Ada", "greet uses the name")
  t.check_eq(greet(""), "Hello, ", "greet handles empty")
  t.finish()
}
```

<!-- check: output -->
```text
PASS greet uses the name
PASS greet handles empty
0 failed, 2 passed
```

Save it as `tests/test_greet.av` and run `avant test`. A failing check prints `FAIL` and its name and the file goes on, reporting every failure.

The verdict is the process status, not the words `PASS` and `FAIL`: `0` when every check passed, `1` when any failed, whatever `main` returns (a void `main`, or one that never calls `finish`, included).

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

`--jobs=N`, `--shard=I/N`, `--junit=PATH` and `--name=PATTERN` work too. An unknown flag, or a `--`, is an error: test programs get no arguments.

For each file the runner prints its output in source order, then `ok tests/test_greet.av`, or `FAIL tests/test_greet.av:` and how its process ended (`the test process exited with status 1`, `the test process killed by signal 13 (Broken pipe)`, `the compile exited with status 1`, `the parallel test "other" killed by signal 11 (Segmentation fault)`); at the end, a total (`0 failed, 1 passed`). Its status is 1 when a file failed, nothing was selected, or the command was refused. Ctrl-C or `SIGTERM` stops every test process it started and removes its temporary files.

### Checks

`Test.new()` holds the checks. Name each check after its case, so a `FAIL` line alone says what broke.

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

Cases as a table: inputs and expected values in two arrays, checked in one loop.

```avant
import std.testing

fn greet(name: String): String {
  "Hello, ${name}"
}

fn main: Int {
  t = Test.new()
  names = ["Ada", "Bo", "Cy"]
  wants = ["Hello, Ada", "Hello, Bo", "Hello, Cy"]
  for i in 0..names.size {
    t.check(greet(names[i]) == wants[i], "greet ${names[i]}")
  }
  t.finish()
}
```

`t.temp_dir` returns a new directory at each call, inside the test's private temporary directory, removed at exit: a test never cleans up.

```avant
import std.testing

fn main: Int {
  t = Test.new()
  dir = t.temp_dir()
  t.check((file_write("${dir}/notes.txt", "hi") ?? -1) == 2, "write in temp_dir")
  t.finish()
}
```

### Golden files

`golden` and `run_golden` compare against saved files byte for byte, trailing newline included. A missing file fails; nothing rewrites one: update it by hand after reading the new output.

<!-- check: skip needs want.txt, hello.av, hello.want and bad.av beside it; exercised by tests/cases/test_testing_docs.av -->
```avant
import std.testing

fn main: Int {
  t = Test.new()
  t.golden("want.txt", "Hello Avant\n")
  t.run_golden("hello.av", "hello.want")
  t.rejects("bad.av", "unknown name")
  t.finish()
}
```

`run_golden` and `rejects` compile a program, so they need `AVANT_COMPILER` set to the compiler under test; `check` alone does not. Under `avant test` it is set for you: to the value you exported, else to the path of the `avant` running the tests.

### Serial and parallel tests

Checks written in the file, and bodies passed to `t.test`, run one at a time in source order. `t.parallel` bodies run after the file's serial work, each in a new process of the same binary, overlapping one another.

<!-- check: compile -->
```avant
import std.testing

fn main: Int {
  t = Test.new()
  t.test("adds", () {
    t.check(1 + 2 == 3, "1+2")
  })
  t.parallel("other", () {
    t.check(true, "other")
  })
  t.finish()
}
```

Each such process runs only its parallel body and `main`'s code outside the bodies. Only `avant test` starts them: `avant run`, or the compiled program, runs every body once, in one process, in source order.

### Choosing by name

`avant test tests/test_greet.av --name empty` runs only the checks and the `t.test` or `t.parallel` bodies (whole) whose names contain `empty`, as plain case-sensitive text; a check inside a body goes by the body's name, and `main`'s code outside the bodies still runs. The JUnit report lists only the checks that ran.

A file where nothing matches is left out of the report and the total; a pattern that matches nothing in any selected file is an error: `--name nothing matches no check, test or parallel test in tests/test_greet.av`.

### Test settings

An optional `[test]` section of `avant.toml` adjusts the walk; paths are relative to the manifest.

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
- `exclusive` suits a benchmark that needs a quiet machine; `first`, the slowest files, so they do not start last (a file runs once compiled).
- The lists do not discover files: on a command with no path, every listed file must be one of the discovered tests.
- `--shard` is an error when the selection contains an exclusive file, unless `--skip-exclusive` is passed; it picks its slice before `first` is applied, so `first` never moves a file between shards.
- With `--jobs 1`, `exclusive` changes nothing.

### Reports

**JUnit** (`--junit PATH`, off unless asked): a file that uses `std.testing` gives one `<testcase>` per check, named by the check; any other file one case, named by its path. A file that failed without a failed case (a signal, or a failing status with no failed check) adds a case named by its path whose failure message says how the process ended; a failed case named by its path carries the file's output. A byte XML cannot hold, there or in a check's name (a control character such as a colour escape, or non-UTF-8), is written `\xNN` (`\x1b`), so the report always parses.

**Coverage** (`--coverage`) compiles each selected file with counters, runs them, prints one table after the summary and writes `coverage/lcov.info` in the current directory (overwritten by each run). Without the flag nothing is instrumented or written.

By default the report covers each test file and what it imports. An optional `[coverage]` section widens that to whole directories, so a file no test loaded is listed at 0%:

```toml
[coverage]
include = ["src"]
exclude = ["tests", "deps"]
```

`include` and `exclude` are directories relative to the manifest (a string or a list; no globs), walked recursively for `*.av`, skipping names that start with `.`. A file under `exclude` is left out, test files too when `exclude` names `tests`. A file no test loaded has no branch map.

For a program instrumented on its own (the runner sets these itself):

| Variable | Read by | Effect |
| --- | --- | --- |
| `AVANT_COVERAGE=1` | the compiler | emit hit counters for this compilation (`0` turns it off) |
| `AVANT_COVERAGE_MAP` | the compiler | the path of the coverage map baked into the binary. Unset, `compile` writes it beside the binary as `OUT.covmap` (the program adds `OUT.covmap.hits` when it runs); `run` and `dump` keep it in their temporary directory |
| `AVANT_COVERAGE_LCOV` | the program | where it writes the LCOV report; `0` writes none. Unset: `coverage/lcov.info` |
| `AVANT_COVERAGE_REPORT` | the program | `0` hides the text table, `1` prints it. Unset, a program prints the table and the compiler's command line does not |
| `AVANT_COVERAGE_COLOR` | the table | `0` or `1` forces color; unset, it follows the terminal |

Several processes of one instrumented binary add into one shared map, and `avant test --coverage` merges every test into one report. `compile`, `run` and `dump` take no `--coverage` flag.

## Formatter

`avant format` (or `avant f`) prints a file in the canonical style: two-space indent, `name: Type` (no space before the colon, one after), runs of blank lines collapsed to one. Comments and line breaks stay.

```sh
avant format main.av            # print the canonical text
avant format src --write        # rewrite every file that changes
avant format . --check          # CI: list files that would change, exit 1 if any
```

- A path is a `.av` file or a directory, walked like `avant test` walks `tests/` (names starting with `.` and directories named `deps` are skipped, and a symbolic link to a directory is not followed).
- With one file and no flag, the text goes to stdout. `--check` writes nothing, prints `would change PATH` per file (`canonical` when none would), and exits 1 if any would.
- The formatter works on tokens (no parsing, typechecking or imports), so it formats a file that does not compile yet; it is never part of a build.
- A file that does not lex is reported and left alone (status 1); the others are still formatted. A usage error (no path, an unknown flag, `--check` with `--write`, several files without either) exits 1.
- `avant format --calls FILE...`, the one mode that typechecks, checks each FILE as a program's root and gives `()` to every method call written without them in it and its imports (`x.sum`, a bare `sum` on the receiver, `Point.new`), leaving fields and the read-only properties (`size`, `bytes`) alone. It prints each changed file with its count; a second run changes nothing. It rewrites only what the checker reaches (a generic body, for the instances the program makes); a program with errors is rewritten where it was checked, with status 1. It writes the files itself, so `--write` beside it changes nothing; `--check` is refused.

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

| | `--debug` | `--release` |
| --- | --- | --- |
| Chosen by default for | `avant run`, `avant test` | `avant compile` |
| Optimization | none (LLVM `-O0`, its fast instruction selector) | full (`-O3`) |
| Compiles | fastest | about 1.5 times `--debug` on a small program, three and a half times on a very large one |
| Use it for | the edit-and-run loop | **anything you keep, measure or ship** |

- **Time a program only with `--release`** (`avant run` builds at `-O0`; `avant compile` builds `--release`); the [Benchmarks](#benchmarks) use `--release --cpu=v3`.
- An explicit flag wins over the verb's default; no environment variable selects the mode.
- Every option that takes a value takes it either way: `--cpu=v3` or `--cpu v3`, `--jobs=4` or `--jobs 4`.
- The language is the same in every mode: bounds, overflow and conversion checks are never off, and the collector is precise in both.
- `--cpu`: `compile` targets `v2` by default (every x86-64 processor of the last decade); `run` and `test`, the machine's highest level. `--cpu=v3` (AVX2) is for recent hardware only; `native` is this machine exactly.

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

`avant run` exits with the program's own status: what `main` returned, 128 + N when signal N killed the program, 127 when it could not be started. `--avant-threads=N` among a program's arguments sets its [thread pool](#the-thread-pool)'s size and is not passed on; every other argument is.

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

- The compiler recovers per statement: one run reports every independent mistake, in every function and imported file, up to 20 errors. With several, it ends with `error: aborting due to N previous errors`, past the cap with `error: and M more errors (limit 20)`.
- A mistake is reported once: what reads a value whose type already failed (an operator, a call, a condition, a later use of its name) adds no error. Where an operand does not fit, the message is its reader's: `px = pa + pn` with an `Int32` and an `Int` says `binary operator expects matching types, got Int32 and Int`; `n: UInt32 = -1` says `a negative literal does not fit in UInt32`.
- A syntax error names what the parser expected and found, in source terms: ``error[E2004]: expected `{`, found a string``.
- An unknown name gets at most one suggestion, in scope there (a local, field, method, constant, function, type, enum member or builtin method), the nearest by spelling: at most a third of the letters changed (a swap counts as one change, a case change as half), never the whole name; a tie goes to what the compiler would find first (a local, then a field, a method, a constant, a function), then alphabetically. A wrong argument count gets no suggestion.
- A call whose arguments fit no function of that name says what it takes: `add takes 2 arguments, given 1: add(a: Int, b: Int): Int`; with several, each signature. Two that fit equally are named both: ``ambiguous call to f: f(a: Int, b: Int?): Int and f(a: Int?, b: Int): Int both match``.
- A use of an optional as its value, and `x == nil`, say how to open it: ``cannot read field size on String?; open it with `if v = s`, `s ?? fallback` or `s?` ``.
- The code's first digit names the phase: `E1xxx` lexer, `E2xxx` parser, `E3xxx` type resolution, `E4xxx` checker, `E5xxx` module loader, `E6xxx` macro expansion.

Run-time failures are [traps](#traps).

### Temporary files

`compile`, `run`, `test`, `fetch` and `bind` keep every intermediate file (IR, objects, logs, test binaries) in one private directory, `avant-XXXXXX` under `$TMPDIR` (or `/tmp`), removed when the command exits, also on an error, Ctrl-C, `SIGTERM`, `SIGHUP`, a closed pipe (`SIGPIPE`, as in `avant test | head`) or a crash (`SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT`), the process still dying by the signal; only `kill -9` or a stack overflow leaves it behind. The tools the compiler runs (`cc`, `re2c`, `git`) and the programs `run` and `test` start get it as their `TMPDIR`.

`--keep-tmp` keeps the directory for debugging and prints its path. A program run on its own keeps its directory with `AVANT_KEEP_TMP=1`.

### Environment variables

**None needs to be set.**

| Variable | Effect |
| --- | --- |
| `TMPDIR` | where the private temporary directory is created |
| `AVANT_KEEP_TMP=1` | a program keeps its temporary directory (`--keep-tmp` sets it for the processes a command starts) |
| `AVANT_COMPILER` | the compiler that `run_golden` and `rejects` use in tests ([Golden files](#golden-files)); `avant test` passes a value you exported to the tests as it is, and its own path when there is none |
| `AVANT_ROOT` | another place to find the runtime and the standard library, instead of beside the `avant` binary |
| `AVANT_RE2C` | another `re2c` binary for regex literals |
| `AVANT_HEAP_MAX_MB`, `AVANT_GC_STATS`, ... | the collector, read by your program ([Collector settings](#collector-settings)) |
| `AVANT_COVERAGE*` | coverage ([Reports](#reports)) |

## Coming from another language

Where Avant differs on purpose:

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
| strings of characters | strings of bytes; `.chars` or `.code_points()` when you want code points ([Bytes, not characters](#bytes-not-characters)) | constant-time `size` and indexing |
| a missing hash key that raises, or reads as zero | `h[k]` is `V?` ([Hashes](#hashes)) | absence is handled where it happens |
| hashes that remember insertion order | iteration order is unspecified, for now | |
| members public by default | functions, types, methods and fields are private to their module unless `pub` ([Visibility](#visibility)) | a module's surface is what it says it is |
| `let`, `var`; a `const` inside a function; global variables | `=` declares or assigns; `const` is a module's named value, and a module has no variables ([Names and scope](#names-and-scope), [Constants](#constants)) | no state shared by accident |
| `Array<Int>`, `[]int`, `Vec<i32>` | `Array(Int)`; a single capital letter in a signature is a type parameter ([Generics](#generics)) | |
| `return` inside a lambda returning from the lambda | the same, also in a lambda a builtin inlines (`xs.each (x) { ... }`); `for` is the loop that leaves the function early ([Lambdas given to builtins](#lambdas-given-to-builtins)) | one meaning of `return` |
| a panic caught per request by the server | a trap ends the process: run servers under a supervisor ([Traps](#traps)) | no unwinding machinery |
| one optimized build (Go) | two modes: `--debug` for the edit-and-run loop, `--release` for the rest, and only it is timed ([Build modes](#build-modes)) | a fast edit-and-run loop |
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

What is missing, also said beside each feature it limits. **Decided**: designed, to be built. **Planned**: wanted, design open. **Not started**: no work yet.

| Missing | Status | Today |
| --- | --- | --- |
| Constant strings and tables (`const names = [...]`) | planned | a function that builds the value |
| `Hash` insertion order | decided | sort the keys ([hash operations](#hash-operations)) |
| `String` methods: `contains`, `starts_with`, `replace`, `join`, case conversion; slices of strings and arrays | planned | `str_slice`, loops, [`std.regex`](#stdregex) |
| `map`, `select`, `reduce` on arrays | planned | a loop, or [your own](#lambdas) |
| The time of day; `sleep` | planned | intervals with `now_ms`; C's `usleep` through a `lib` block |
| `==` on hashes | planned | a loop |
| A loop variable of its own for each iteration's closures | decided | copy it into a new name in the body |
| `for v in ch` over a channel | decided | `while v = ch.recv() { ... }` |
| `parallel { }` as a statement, with no receiver | decided | `.parallel` on an array |
| TLS for `std.http` (`https`, `wss`) | decided | a TLS-terminating proxy in front |
| A trap in an `std.http` handler that ends only its request; a pre-forked server mode | decided | a supervisor that restarts the process |
| Positions in traps raised inside runtime functions | planned | the message names the operation and its values |
| Platforms other than Linux x86-64 with glibc (musl, ARM, macOS, Windows) | not started | |
| Editor tooling (a language server) | not started | `avant format`, and compile errors with positions |

## Benchmarks

Each table names its machine and day (LangArena on a server, AMD EPYC 7351P; the HTTP servers on a desktop, AMD Ryzen 7 5700G; never compared). Every language is an optimized build; losses are printed as plainly as wins. Read [Reading these numbers](#reading-these-numbers) before quoting any.

### LangArena

[LangArena](https://github.com/kostya/LangArena) runs the same tasks in many languages. Seconds, lower is better; **bold** is the best of the five. Each task is one process: wall-clock time and peak memory (`ru_maxrss`), read by `/usr/bin/time` under LangArena's harness for the others, by a spawner without a wrapper's start-up for Avant. The server: AMD EPYC 7351P (Zen 1, 16 cores, 32 threads), boost off at a fixed 2.4 GHz, governor `performance`. Avant: 2026-11-17, release 0.46.0, `--release --cpu=v3`, the median of 5 rounds on an idle machine, all 50 checksums correct; the others: 2026-11-14.

**The Avant build** is release 0.46.0 as users download it: the release job's build on Ubuntu 26.04, gcc 15.2 for the runtime and LLVM 22.1.8 for the compiler, for the baseline x86-64. The numbers were measured with the same source built on this server with the same toolchain; the release job's build gives byte-identical task binaries.

| Language | 50 tasks (s) | Average peak memory | time × Avant | peak memory × Avant | time, geometric mean × Avant |
| --- | ---: | ---: | ---: | ---: | ---: |
| Avant | **90.359** | **19.3 MB** | 1.00× | 1.00× | 1.00× |
| C (Clang) | 138.008 | 23.0 MB | 1.53× | 1.19× | 1.57× |
| C (GCC) | 143.231 | 23.0 MB | 1.59× | 1.19× | 1.64× |
| Rust | 137.709 | 29.3 MB | 1.52× | 1.52× | 1.60× |
| Crystal | 194.527 | 65.2 MB | 2.15× | 3.37× | 2.42× |
| Go | 311.401 | 62.8 MB | 3.45× | 3.25× | 2.94× |

The geometric-mean column is the mean over the 50 tasks of each task's time over Avant's, so every task weighs alike; the total is dominated by the longest tasks.

**The total flatters Avant**: the four `Matmul` rows and `Hash::CRC32` are not comparable programs, two rows carry collection hints, and several measure C libraries ([Reading these numbers](#reading-these-numbers)). Read it task by task.

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Binarytrees::Obj` | **1.100** | 6.487 | 6.849 | 2.925 | 6.155 |
| `Binarytrees::Arena` | 1.110 | 1.039 | **0.908** | 1.653 | 0.953 |
| `Brainfuck::Array` | 4.489 | **3.455** | 4.066 | 7.098 | 3.837 |
| `Brainfuck::Recursion` | 5.503 | 4.721 | 5.276 | **4.689** | 5.132 |
| `Matmul::Single` | **3.411** | 11.101 | 11.558 | 11.829 | 12.492 |
| `Matmul::T4` | **2.132** | 2.924 | 3.015 | 4.757 | 7.838 |
| `Matmul::T8` | **1.187** | 1.587 | 1.644 | 4.544 | 4.070 |
| `Matmul::T16` | 0.930 | **0.830** | 1.065 | 3.608 | 2.139 |
| `Base64::Encode` | **0.441** | 0.497 | 1.041 | 3.490 | 5.941 |
| `Base64::Decode` | **0.504** | 0.573 | 0.977 | 3.351 | 3.958 |
| `Json::Generate` | **0.569** | 1.554 | 0.670 | 2.979 | 3.592 |
| `Json::ParseDom` | **0.240** | 0.292 | 4.273 | 1.710 | 5.128 |
| `Json::ParseMapping` | **0.641** | 0.746 | 1.157 | 4.371 | 8.027 |
| `CSV::Parse` | **1.907** | 6.257 | 2.789 | 9.475 | 8.802 |
| `Etc::Sieve` | 2.483 | **1.893** | 1.916 | 4.490 | 3.792 |
| `Etc::TextRaytracer` | **2.341** | 3.250 | 2.538 | 3.525 | 4.135 |
| `Etc::NeuralNet` | 2.889 | **2.654** | 2.967 | 3.137 | 4.717 |
| `Etc::CacheSimulation` | **1.051** | 2.243 | 2.077 | 4.198 | 5.057 |
| `Etc::GameOfLife` | 3.000 | 3.577 | 3.606 | 6.274 | **2.488** |
| `Etc::Words` | 2.476 | 4.621 | 2.481 | **2.300** | 2.949 |
| `Etc::LogParser` | 1.205 | 1.345 | **0.864** | 2.942 | 79.266 |
| `Template::Regex` | **2.142** | 3.200 | 8.642 | 4.342 | 21.198 |
| `Template::Parse` | **1.645** | 2.479 | 2.802 | 6.547 | 2.551 |
| `Sort::Quick` | 2.750 | 2.651 | **2.640** | 3.635 | 2.915 |
| `Sort::Merge` | 2.966 | **2.394** | 3.303 | 5.566 | 3.589 |
| `Sort::Self` | **0.614** | 4.487 | 1.544 | 2.453 | 3.063 |
| `Graph::BFS` | **1.871** | 2.425 | 2.895 | 4.292 | 5.223 |
| `Graph::DFS` | **1.896** | 2.133 | 2.531 | 3.782 | 3.300 |
| `Graph::AStar` | **1.135** | 5.225 | 5.525 | 4.678 | 10.289 |
| `Hash::SHA256` | **0.492** | 2.246 | 2.144 | 2.263 | 2.559 |
| `Hash::CRC32` | **0.247** | 1.943 | 1.950 | 1.940 | 9.272 |
| `Calculator::Ast` | **1.181** | 2.739 | 1.639 | 1.333 | 2.132 |
| `Calculator::Interpreter` | **0.561** | 2.830 | 2.167 | 2.102 | 1.799 |
| `Maze::Generator` | **0.962** | 1.226 | 2.027 | 5.454 | 3.396 |
| `Maze::BFS` | **1.209** | 1.317 | 2.117 | 6.553 | 4.350 |
| `Maze::AStar` | **1.806** | 3.557 | 2.590 | 7.896 | 7.585 |
| `CLBG::Fannkuchredux` | **2.042** | 2.680 | 2.398 | 2.455 | 2.598 |
| `CLBG::Mandelbrot` | **2.171** | 2.188 | 2.304 | 2.251 | 2.178 |
| `CLBG::Nbody` | 3.550 | 3.516 | **3.280** | 3.566 | 4.008 |
| `CLBG::Spectralnorm` | **2.647** | 2.724 | 2.729 | 2.874 | 2.712 |
| `Compress::BWTEncode` | 1.690 | 1.717 | **0.979** | 3.335 | 4.607 |
| `Compress::BWTDecode` | **3.460** | 3.691 | 4.109 | 4.159 | 6.540 |
| `Compress::HuffEncode` | 1.847 | 1.943 | **1.605** | 2.468 | 2.520 |
| `Compress::HuffDecode` | 1.354 | **1.037** | 1.576 | 2.481 | 3.471 |
| `Compress::ArithEncode` | 2.938 | 2.679 | **2.633** | 3.306 | 2.871 |
| `Compress::ArithDecode` | 2.642 | **2.330** | 3.327 | 2.574 | 2.554 |
| `Compress::LZWEncode` | **0.439** | 3.278 | 1.616 | 2.475 | 3.937 |
| `Compress::LZWDecode` | **0.552** | 1.489 | 1.601 | 1.787 | 5.814 |
| `Distance::Jaro` | 2.089 | **2.020** | 2.090 | 2.837 | 2.047 |
| `Distance::NGram` | 1.850 | 4.216 | **1.210** | 1.779 | 1.856 |

### Where Avant loses

Avant is the fastest of these five on 31 of 50 tasks. Against the best of *every* language measured on this machine (C under both GCC and Clang included): at or ahead on 31 tasks, the median task 0.92× the best, two tasks more than 1.5× behind. The worst by time:

| Task | Avant (s) | Best (s) | Behind by |
| --- | ---: | ---: | ---: |
| `Compress::BWTEncode` | 1.690 | 0.979 (Rust) | 1.73× |
| `Distance::NGram` | 1.850 | 1.210 (Rust) | 1.53× |
| `Etc::LogParser` | 1.205 | 0.864 (Rust) | 1.39× |
| `Etc::Sieve` | 2.483 | 1.893 (C, Clang) | 1.31× |
| `Compress::HuffDecode` | 1.354 | 1.037 (C, Clang) | 1.31× |
| `Brainfuck::Array` | 4.489 | 3.455 (C, Clang) | 1.30× |
| `Brainfuck::Recursion` | 5.503 | 4.378 (C, GCC) | 1.26× |
| `Sort::Merge` | 2.966 | 2.394 (C, Clang) | 1.24× |

By peak memory: at or below the best on 33 tasks, the median task at 0.89×, five tasks more than 1.5× above. The worst:

| Task | Avant (MB) | Best (MB) | Above by |
| --- | ---: | ---: | ---: |
| `CLBG::Spectralnorm` | 6.1 | 2.9 (C, Clang) | 2.14× |
| `Binarytrees::Arena` | 51.6 | 26.8 (C, Clang) | 1.93× |
| `Etc::LogParser` | 18.1 | 9.9 (C, Clang) | 1.82× |
| `Sort::Self` | 11.6 | 7.3 (C, Clang) | 1.59× |
| `Etc::GameOfLife` | 45.6 | 29.5 (Go) | 1.55× |

Crystal and Go are compiled and garbage-collected too, so trailing either on any task, in time or memory, is a defect. Avant trails on nine:

| Task | Behind |
| --- | --- |
| `Binarytrees::Arena` | time 1.17× Go |
| `Brainfuck::Array` | time 1.17× Go |
| `Brainfuck::Recursion` | time 1.17× Crystal, 1.07× Go |
| `Etc::GameOfLife` | time 1.21× Go; memory 1.55× Go |
| `Etc::Words` | time 1.08× Crystal |
| `Compress::ArithEncode` | time 1.02× Go |
| `Compress::ArithDecode` | time 1.03× Crystal, 1.03× Go |
| `Distance::Jaro` | time 1.02× Go |
| `Distance::NGram` | time 1.04× Crystal |

Avant's numbers are the median of five rounds, the others' one run: ratios within a few percent may be noise.

The tables understate allocation-heavy code: `Binarytrees::Arena`, the heaviest allocator, trails in time and memory, and the two rows that would be worse are fast because they are hinted ([What allocation costs today](#what-allocation-costs-today)).

### HTTP and WebSocket servers

The most basic server in each language: `GET /` answered with `hello` over keep-alive connections, each an optimized build, all eight in one session. Sources and script: `bench/http/`.

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

**Method.** `wrk -t4 -c64 -d10s --latency`, the load generator on the server's machine, no pipelining; one server at a time, three runs each after a warm-up, the median shown (AMD Ryzen 7 5700G, 2026-11-12).

**The fastest rows are not ranked**: for five of the eight (Avant, h2o, both Rust servers, Valk) `wrk`'s four threads are the limit, so the table shows only that none of them is the bottleneck at 64 connections, and what each costs in memory.

Pipelined loads (many requests in one write) are not shown: Avant's server answers each with its own write, slower than servers that batch them; this is being worked on.

The WebSocket echo comes from earlier sessions on the same desktop (AMD Ryzen 7 5700G), kept apart: messages per second over 8 connections for 10 seconds; Avant the median of three runs (2026-11-10), the others one run each (2026-11-04).

| Server | Messages per second | Peak memory |
| --- | ---: | ---: |
| Avant (`std.http`) | 247,803 | 6.9 MB |
| C (h2o) | 251,006 | 8.5 MB |
| Rust (std, thread per connection) | 252,175 | 2.6 MB |
| Go (`net/http`) | 215,594 | 12 MB |
| Crystal (`HTTP::Server`) | 264,813 | 11 MB |

### Reading these numbers

These notes are about [LangArena](#langarena).

- **Build flags.** Avant: `--release --cpu=v3` (x86-64-v3). C and C++: LangArena's `-O3 -march=native -mtune=native` (Zen 1 here, a superset of v3); the C column is Clang, the faster of the two C compilers (GCC: 143.231 s, 23.0 MB). Rust (`cargo build --release`), Go and Crystal (`--release`): their defaults.
- **The whole process is timed**: start-up, data preparation, warm-up and the timed iterations; LangArena's published numbers count only the timed iterations.
- **Compiled-in task sizes.** Avant compiles the task configuration in as constants, the others read `run.js` at run time, so LLVM can specialize Avant's loops on the sizes.
- **Rows that are not comparable programs.** `Matmul` (four tasks): Avant sums with eight independent accumulators, which vectorizes; the references use one. `Hash::CRC32`: Avant calls a C runtime builtin with slice-by-4 tables; every reference is the bitwise loop. `Compress::LZWEncode` and `LZWDecode`: Avant keys the dictionary by integer, C by string. These seven rows are 14.3 of the 47.6 seconds between Avant's total and C's.
- **Rows that measure C, not Avant.** `Hash::SHA256` and `Hash::CRC32` call runtime builtins that do the whole task in C, where the references implement it; the Burrows-Wheeler transform in `Compress::BWTEncode` and `BWTDecode` is runtime C too.
- **Rows that carry collection hints.** `Binarytrees::Obj` and `Calculator::Ast` call `gc.hold` and `gc.collect()` ([Collection hints](#collection-hints)), telling the collector when not to run and when everything is dead; no reference has an equivalent. Unhinted, `Binarytrees::Obj` takes 2.882 s (Crystal, the table's fastest, 2.925 s) at 1.72× C's memory, and `Calculator::Ast` 1.541 s, 1.16× Crystal's time ([What allocation costs today](#what-allocation-costs-today)).
- **Library-leaning rows** (the library's speed, through Avant's bindings): `Json::ParseDom` and `ParseMapping` (yyjson in Avant and C, simdjson in C++, serde in Rust), `Base64::Encode` and `Decode` (the aklomp codec in Avant and C), `Template::Regex` and `Etc::LogParser` (PCRE2 in Avant and C, RE2 in C++, the `regex` crate in Rust). `Sort::Self` is each language's library sort, by the task's definition. In `CSV::Parse` Rust and C++ use CSV libraries; in `Json::Generate` C and C++ use JSON libraries, Avant its own buffer.
- **Scope.** All 50 tasks; `Etc::TextRaytracer` times its whole render.
- **Equivalent programs is the rule**: Avant's tasks follow the C and Rust references' algorithms and data widths; a row that does not is named above until rewritten.

## Goals

Beginners welcome, advanced programmers not blocked, memory managed, errors as values. Simplicity is non-negotiable, and so is not choosing between productivity and speed.

The bars are measurable, and [LangArena](#langarena) is the public scoreboard:

| Goal | Bar |
| --- | --- |
| Expressiveness | Crystal's density: as little syntax as Crystal to say the same thing |
| Run time and memory | **C and Rust, one to one**: against the best language on each task, a median of 1.0× or better and no task above 1.5× |
| The floor | never behind Crystal or Go, the other compiled, garbage-collected languages, on any task, in time or in memory |
| HTTP | at least as fast as the fastest server measured on the same machine |
| Interop | C libraries usable with little ceremony |
| Concurrency | built in, and obvious |

Where Avant stands, each gap a defect until fixed:

- **LangArena** (2026-11-17): the median task 0.92× the best's time, 0.89× its memory; beyond 1.5×, two tasks in time and five in memory; nine behind Crystal or Go ([Where Avant loses](#where-avant-loses)); allocation-heavy code is the weakest ([What allocation costs today](#what-allocation-costs-today)).
- **HTTP**: level with the fastest servers at 64 connections without pipelining, slower than servers that batch writes when requests are pipelined ([HTTP and WebSocket servers](#http-and-websocket-servers)).
- **Expressiveness**: Avant's LangArena sources are 5,482 lines to Crystal's 4,220 (30% more, 18% compressed), not Crystal's density yet; the forms that close the gap are in [Not built yet](#not-built-yet).

Avant is not a better TypeScript, a borrow-checker language, a dynamic language with types bolted on, or a research language.

## Installation in depth

### Installer options

```sh
curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
bash install.sh v0.41.0             # a specific version
bash install.sh --no-modify-path    # do not edit shell configuration
```

The script downloads the release archive, checks its SHA-256, installs it into `~/.avant`, adds it to `PATH` in your shell's configuration, and compiles and runs a hello program to prove the install.

| Option | Effect |
| --- | --- |
| a version argument (`v0.41.0`), or `AVANT_VERSION` | install that release instead of the latest |
| `--no-modify-path` | leave shell configuration alone and print the `PATH` line to add |
| `AVANT_INSTALL` | install into another directory than `~/.avant` |

Running it again updates in place. It refuses, with a message, anything but Linux x86-64 with glibc (musl included), and needs `curl`, `tar`, and `sha256sum` or `shasum`.

### Manual install

Download `avant-linux-x86_64.tar.gz` from [Releases](https://github.com/avant-lang/avant/releases), extract it anywhere, and put the `avant-linux-x86_64` directory on your `PATH`. Keep its files together: the compiler finds `re2c`, the standard library and the runtime beside itself. `avant version` prints the release and commit, from the archive's `versions.txt`.

### Requirements

| Need | For | Install |
| --- | --- | --- |
| Linux x86-64, glibc 2.38 or newer | everything | Ubuntu 24.04, Debian 13, Fedora 39, or later |
| a C compiler and the C library's development files | linking every program (and C interop) | `sudo apt install gcc libc6-dev`, or `sudo dnf install gcc glibc-devel` |
| `git` | `avant fetch` only | |
| libclang 20 | `avant bind` only | `sudo apt install libclang1-20` |

Nothing else, and no environment variable. The archive holds `avant` with LLVM 22 linked in (it links with `cc`), `re2c`, `avant-bind`, the standard library, the runtime with PCRE2, zlib and the other C libraries inside, `examples/` (the runnable programs this page points to), `versions.txt` (what `avant version` prints), Avant's `LICENSE`, and `third_party/` with those C libraries' notices and licenses.

## Contributing

Avant's compiler is written in Avant and is built by a published release of itself. How it works, building it from source, the gate and the suite, and how this README's examples are checked are in [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE).
