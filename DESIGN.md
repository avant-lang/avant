# Avant's design contract

The rules a change to the language keeps, in three parts: the numeric rules in one place, what safe code cannot do, and the design choices with their reasons, the ones taken and the ones set aside. [README.md](README.md) teaches the language and shows each rule at work; this page is the short form a proposal is checked against. A change that breaks a rule here changes this page first, with its new reason; a proposal that brings back a choice set aside below answers the reason written beside it.

## Contents

- [The numeric rules](#the-numeric-rules):
  - [types](#types)
  - [literals](#literals)
  - [the u64 suffix](#the-u64-suffix)
  - [conversions and comparison](#conversions-and-comparison)
  - [integer arithmetic](#integer-arithmetic)
  - [shifts and bits](#shifts-and-bits)
  - [floats](#floats)
  - [constants](#constants)
  - [sizes and indexes](#sizes-and-indexes)
- [What safe code cannot do](#what-safe-code-cannot-do):
  - [where a trap is reported](#where-a-trap-is-reported)
- [Design choices and their reasons](#design-choices-and-their-reasons):
  - [kept](#kept)
  - [set aside](#set-aside)
  - [taken after a first refusal](#taken-after-a-first-refusal)
  - [not decided yet](#not-decided-yet)

## The numeric rules

One line holds them all: an operation gives the mathematically correct value or traps, and a value changes type only where the program says so.

### Types

- `Int` is a 64-bit signed integer and the type of every count, size, index, time and id by default: a 32-bit counter would trap at 2.1 billion, and one type for positions needs no conversion between them.
- `Int32` and `UInt32` are for dense storage (an `Array(Int32)` takes half an `Array(Int)`'s memory) and are C's `int` and `unsigned int`; `UInt64` is for bit patterns, hashes and C's 64-bit unsigned values; `Float64` is the float.
- `Int8`, `Int16`, `UInt8`, `UInt16` and `Float32` exist only at the C boundary, to describe C's layouts and signatures: fewer types inside Avant mean fewer conversions between them.

### Literals

- An integer literal takes its type from its context only: a declared type, a parameter (generic and operators' included), a field, a result, or an operator's other operand, on either side. An expression of literals only adopts as one literal does (`u: UInt64 = ~0`), and a literal above `Int`'s range passed to a `UInt64` parameter is a `UInt64`. With no context it is an `Int`, and a float literal a `Float64`.
- A literal is never typed by its value: editing a digit must not change a type, and the context is where a reader looks for it.
- A literal outside its type's range is a compile error for every type, wherever the type comes from.
- A float literal has a digit on each side of the point (`0.5`, `2.0`): `.5` would read as a member after a line break (a line starting with `.` continues the one before), and `1.` as a method call on a number (`1.abs()`).
- `0x`, `0b` and `0o` begin hexadecimal, binary and octal literals, and no other number starts with `0`: `017` is 15 to C and 17 to a reader, so it is refused with both readings offered. `_` stands only between two digits.
- A character literal (`'a'`) is one byte's integer value, typed as any integer literal; there is no `Char` type, since a string is bytes.

### The u64 suffix

`u64` is the one literal suffix, and it is written only where a literal has no context to type it: a local's first value (`low = 0u64`, which `low: UInt64 = 0` also says), a constant's value (`const big = 18446744073709551615u64`), a receiver (`(32800u64 << 48).wrap_i()`), a `puts` argument, an array literal's first element. Where the literal has a context, the suffix is not written: one spelling.

- **Why a suffix at all:** a constant takes no declared type, so a constant above `Int`'s range has no other spelling, and neither has a value above `Int`'s range with no context (`18446744073709551615.to_u64()` is itself an `Int` literal that does not fit). One short suffix covers every such place; a declared type on constants would add a form for a handful of uses, and typing a literal by its value would break the rule above.
- **Why only `u64`:** every `Int32` or `UInt32` value fits in an `Int`, so a literal with no context loses nothing as an `Int`, and one meant as either type is given its context where it is stored (`n: Int32 = 5`, a field, an array element, a parameter); only `UInt64` has values no `Int` holds.

### Conversions and comparison

- Number types do not mix: an `Int32` and an `Int`, or an `Int` and a `Float64`, in one operation is a compile error; only literals adapt. A conversion is where a value can be lost (truncated, or rounded above 2^53), so it is written.
- `to_i`, `to_i32`, `to_u32`, `to_u64` give the same number or trap; `wrap_i`, `wrap_i32`, `wrap_u32`, `wrap_u64` give the two's-complement bits and never trap; `to_f` rounds to the nearest float; a float's `to_i` truncates toward zero and traps on NaN, an infinity or a value outside `Int`.
- Values of different types do not compare, an `Int` and a `Float64` included, so the rule has no exception and no precision is lost in a comparison.
- `Bool` converts with `to_i` (and the other integer conversions), 1 or 0, and is never read as a number by itself.

### Integer arithmetic

- `+`, `-` and `*` trap on overflow at the operands' width; `/` and `%` trap on a zero divisor; `MIN / -1` traps and `MIN % -1` is 0; `abs` of the minimum, a `sum` and a `Hash` counter that overflow trap the same way. A wrong number never passes silently: a trap is a bug's report.
- Wrapping is always spelled: `&+`, `&-` and `&*`. An unsigned value has no unary minus (`-u` and `-1u64` are compile errors); its two's complement is `0 &- u`.
- `/` truncates toward zero and `%` takes the dividend's sign (`-7 / 2` is `-3`, `-7 % 2` is `-1`), as in C, Go, Rust and the hardware's division: the C-family hands Avant keeps.
- A check is a compare and a branch never taken, dropped where the compiler proves the value safe.

### Shifts and bits

- `<<` and `>>` take an amount in `0..63` for `Int` and `UInt64`, `0..31` for `Int32` and `UInt32`, typed `Int` or as the shifted value; any other amount traps, and a literal one is a compile error. C makes such a shift undefined and the processor masks its amount, so either way the program would get a wrong number. `x.try_shl(n)` and `x.try_shr(n)` return `nil` instead.
- `&`, `^` and `|` bind tighter than the comparisons, below the shifts (Rust's order): `x & 1 == 0` means what it reads, `(x & 1) == 0`, where C's order is a known trap.
- `count_ones`, `leading_zeros` and `trailing_zeros` count the value's own width.

### Floats

- Float arithmetic follows IEEE 754 and never traps: `1.0 / 0.0` is `inf` and `0.0 / 0.0` is `nan`. IEEE defines a value for each case, every language with a 64-bit float gives it, and the trap belongs to integer operations, which have no correct value to give.
- NaN equals nothing, itself included, and `-0.0` equals `0.0`; a `Hasher` takes no `Float64`, since no hash agrees with that equality.
- A float prints as the shortest text that reads back as the same number; JSON cannot write NaN or an infinity, so a `JsonBuilder` traps on them.
- There is no `**`: `x.pow(y)` is the method, and an operator would bring its own precedence and type rules for a rare operation.

### Constants

- A scalar constant made of literals, constants and operators is computed at compile time, and each use is its value written in place, typed by its place as a literal is. Overflow, a zero divisor or a shift outside the width in it is a compile error at the constant; a constant whose value does not fit its place gets the literal's error, naming the constant.
- Every other constant value is computed once, before `main`, and never changes at any depth, so any task reads it without a lock.

### Sizes and indexes

- An index, a size and a `for` range are `Int`; an `Int32` index is written `xs[i.to_i()]`, which costs nothing.
- The ceilings (a `String`, `Bytes` or `Buf` of 2,147,483,647 bytes, an `Array` of 2,147,483,647 elements, a `Hash` of 1,073,741,824 slots, one object's storage below 4 GiB) are the README's [Size limits](README.md#size-limits); growing past one traps before anything is written.

## What safe code cannot do

Safe code is Avant code that does not cross into C: no `lib` block, `fun` or `Ptr(T)` of its own. Past that boundary C's rules hold, and Avant checks what it can at the crossing (an enum value C returns is a member, a NULL `String` traps). Inside it:

| Safe code cannot | What happens instead |
| --- | --- |
| read or write outside an object | every index and slice is checked: a trap, or `T?` from `get` and `try_slice` |
| read a value never written | a local is born with its value (`=` binds a name to one), and no field is ever read unset: it has a default, its type's zero, or `initialize` sets it on every path; a type with no zero (an enum, a class, a struct with a default) must have one of the others |
| reach through `nil` | absence lives only in `T?`, which is opened (if-assign, `??`, `?`, `expect`) before its value is used |
| use memory after it is freed, or free it | the collector owns memory; there is no `free` |
| overflow, convert or shift into a wrong number | [the numeric rules](#the-numeric-rules): a trap or a compile error |
| read a value as another type | there are no casts between types; a union is tagged and taken apart by `switch` or if-assign; an enum value is always one of its members |
| ignore a failure | a statement whose value can be an error is refused; `_ = e` discards one on purpose |
| unwind the stack | there are no exceptions: a trap ends the program, or the task it happens in, whose `join` gives a `TaskError` |
| share a mutable object between tasks unchecked | what a task captures is checked at compile time: values cross as copies, an `Array` or `Hash` shared for reading is frozen until the task ends, a class is moved or refused, and a mutable object reaches another task only inside a `Mutex`, moved, or as `.parallel`'s results |
| change a collection in a way its walk would misread | adding a key to a `Hash` during its walk, or deleting one other than the visited key, stops the program after that step; an array of structs cannot change its size or order while a loop walks its elements in place; an array of values grown during its own `for` or `each` shows the new elements (its size is read at each step: a defined result, no invented value) |
| keep a `Hasher` | the runtime gives a `Hasher` to a `fn hash` for the call only: it may be a parameter or a local of any function (a helper that adds several values), and is never stored in a field, an optional or a collection, returned, or captured |
| meet undefined behavior | every operation has a defined result or traps; what is undefined in C (an overflow, an index out of bounds, an over-wide shift) is a trap |

### Where a trap is reported

A trap prints `avant runtime: WHAT` and where, after flushing standard output, and exits with status 1 (or ends its task); the README's [Traps](README.md#traps) gives each kind's position. Two rules hold for every builtin and library function:

- A broken precondition of a library function names the program's call, not the line inside the library that checks it.
- A trap a builtin raises for the lambda it runs (a walk that finds its collection changed, `Mutex.with` on a poisoned mutex, a task's trap as its join reports it) names the method call's position, as a trap at any other call does; a trap in the lambda's own code names that code's line.

## Design choices and their reasons

### Kept

| Choice | Reason |
| --- | --- |
| Conditions are `Bool`: no truthiness (`if n`, `if list`) | `if n` would mix up zero, empty and absent; `if v = x` already opens an optional |
| No `!` or `!!` to force an optional open: `x.expect("why")` | the message says what the program assumed, and the trap names the call |
| No `++` or `--` | assignment is a statement, and `+= 1` says it |
| Only `//` comments, no `/* */` | block comments do not nest, and editors comment lines |
| A float literal needs a digit on each side of the point | [Literals](#literals) |
| `u64` is the only literal suffix | [The u64 suffix](#the-u64-suffix) |
| Every method call takes parentheses; after a dot, a name without them is a field or one of the builtin read-only properties (`size`, `bytes`, `message`) | a mutation never reads as a field (a `pop` read as a field once escaped a check), a trailing block after a bare name was ambiguous (`twice { ... }`), and a call has one spelling |
| A function-typed field is called as `box.f()`; a class may not have a field and a method of one name | with one name meaning one thing, the call needs no extra parentheses |
| `xs.pop()` returns `T?` | as `min`, `max` and `get` do, and `while v = stack.pop() { ... }` drains a stack; the trapping form is `xs.pop().expect("why")` |
| No variadic functions | an array literal argument does it, and interpolation covers formatting |
| `..` excludes its end, `..=` includes it | ranges match zero-based indexes: `0..xs.size` is every index |
| `/` truncates and `%` takes the dividend's sign | [Integer arithmetic](#integer-arithmetic) |
| `&`, `^`, `\|` bind tighter than comparisons | [Shifts and bits](#shifts-and-bits) |
| No `**` | [Floats](#floats) |
| Type arguments in parentheses, `Array(Int)`; a single capital letter in a signature is a type parameter | parentheses parse without knowing types, where `<` and `>` in an expression could be comparisons (`f(a < b, c > d)`), and a generic signature needs no declaration list |
| A `Hash` walks its keys in insertion order, for every key type, with a per-process seed | output does not depend on the seed, which then guards against collision attacks unseen |
| A `String` is bytes, normally UTF-8, checked at `chars` and `String.from_utf8` | `size` and indexing take constant time, and byte data needs no second type |
| `h[k]` is `V?`, never a default or a crash | absence is handled where it happens |
| Functions, types, methods and fields are private to their module unless `pub` | a module's surface is what it says it is |
| A module has constants and no variables | no state shared by accident, and a task reads a constant without a lock |
| A method may change its struct receiver, and the change is the caller's; an argument is a copy | Go's pointer receiver, taken automatically, with no `mutating` mark |
| No inheritance, no interface values: composition, interfaces as compile-time bounds, unions | every call is direct and every layout known |
| `return` in a lambda leaves the lambda, also in one a builtin inlines | one meaning of `return`; `for` is the loop that leaves the function early |
| Braces required, no parentheses around a condition, no semicolons, a line break ends a statement | C's silhouette without its punctuation |
| One spelling per meaning; a second only as the short or explicit form of the same construct, parsed without types, meaning and costing the same, shown useful on real code, and decided on purpose | synonyms and style options cost every reader and grow dialects; the lambda shorthand `xs.each (x) { ... }` is the one second form, a strict shortening of `xs.each((x) { ... })` |

### Set aside

| Not taken | Reason |
| --- | --- |
| Tuples | a struct names its parts; programs that return two values are few, and a struct serves them |
| Multiple results (Go's `(T, error)`) | a struct holds several values, and a union holds a value or a failure: Go's pair keeps both halves present and forces no check |
| Named arguments for every parameter | defaults cover optional parameters, so only a defaulted parameter may be named, after the positional arguments ([taken after a first refusal](#taken-after-a-first-refusal)); required arguments stay positional, as in C |
| Variadic functions | [Kept](#kept) |
| `break 2` and labelled loops | a count of loops breaks silently when a loop is added between; a function and `return` leave several loops |
| `.member` for an enum member outside a `case` label | a line starting with `.` continues the one before, and outside a label a bare name competes with locals and functions; a `case` label takes the bare member name |
| Implicit or transitive imports | an explicit import shows where each name comes from, at the cost of a few lines a file |
| Error hierarchies (`extends`) and one implicit error type per function | unions already say "this fails with A or B", compose with every other union, and use names the programmer chose |
| Exceptions (`throw`, `try`/`catch`) | non-local control flow: a signature shows everything that can come out |
| A ternary `c ? a : b` | `?` is the failure operator, and `if` is an expression |
| First-class range values (`r = 0..n`) | no program needed one; a range is syntax in `for`, slices and `case` labels |
| Format specs inside interpolation (`"${x:.3}"`) | a small language inside strings for a few fixed-precision uses: `fmt_float(x, 3)` and `buf.push_fmt_f(x, 3)` say it |
| An in-place `map` sibling (`map!`) | `for i, x in xs { xs[i] = f(x) }` says it plainly and allocates nothing |
| Crystal's `&.method` and an implicit `it` parameter | shorthand that saves a few characters and is one more thing to learn; a lambda names its parameters |
| A trailing block on a call without parentheses (`ready { ... }`) | `if ready { ... }` could not be told from a call of `ready` taking a block; every call takes parentheses |
| A method as a value (`f = c.add`) | `c.add` would mean a call or a bound method depending on the arity; a lambda says it: `(n) { c.add(n) }` |
| Implicit numeric widening | [Conversions and comparison](#conversions-and-comparison) |
| Predicate names ending in `?` (`empty?`) | `?` is the postfix failure operator |
| A second string type for views | a view type is a second `String` for every API; slices of a `String` and `Buf` are looked up in place where it matters (a `Hash` key) |
| A trap for float division by zero | [Floats](#floats) |
| `"".split(",")` giving `[]` | `split` keeps empty fields, so the empty string is one empty field (`[""]`), as `"a,".split(",")` is `["a", ""]`; an empty separator traps |
| A probe of CPU names for `--cpu` | `--cpu` takes five fixed levels (`baseline`, `v2`, `v3`, `v4`, `native`), so no unknown name reaches code generation |
| Unwinding a trap in a server handler | a trap in an `std.http` handler fails its request with a 500 and the server goes on, with no unwinding machinery: the handler's frames are dropped |

### Taken after a first refusal

So that a review does not re-reject these from an older report:

| Now built | What changed |
| --- | --- |
| Named arguments for defaulted parameters (`pad("ab", fill: "-")`) | calls that take options (`Http.listen(8080, loops: 4)`) set one of several defaults; the positional arguments still choose the overload, the names only fill defaults, and a defaulted parameter's name is part of its function's public surface |
| A scalar constant whose expression overflows is a compile error at the constant | an earlier rule trapped at run time; a constant is computed while compiling, so the error belongs there |
| `Int` is 64-bit | once overflow trapped, a 32-bit `Int` was a crash at 2^31 waiting in counts, sizes, times and ids; `Int32` keeps dense storage |
| Module constants (`const`) | named values without module variables |
| `for` over any type with a `next` method returning `T?` | a direct call per step, the same code as the `while` loop written by hand, so it costs nothing over today's loops |
| `Set(T)` | it was deferred until the hash work it stands on; with that built, it was cheap to add |

### Not decided yet

- **Destructuring** (several locals from one struct result at once): to be decided with a count of the struct results unpacked into locals in real code.
- **Optional chaining** (`a?.b`): to be measured on real code first.
