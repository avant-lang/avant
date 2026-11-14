#!/bin/sh
# Out-of-checkout smoke test of a release tarball (Wave D, D147, D148).
#
#   sh tools/smoke.sh TARBALL | INSTALL_DIR
#
# Extracts TARBALL into a temporary prefix (or uses INSTALL_DIR, an
# installed release such as ~/.avant, in place) and, from a temporary project
# directory with an empty environment (only HOME, PATH=/usr/bin:/bin, and
# TMPDIR, all inside the work directory), compiles and runs programs that
# use every std module, compiles and runs programs in each codegen mode
# (--debug, bare, --release), runs `avant test` on a small project, checks
# that examples/, LICENSE and the third-party notices are there and that
# `avant version` prints versions.txt's version and commit, and runs
# `avant bind` when the bundled helper's libraries are present. It first
# proves that the release carries no myc-llvm (Stage 28 W10; the
# compiler's one back end is the in-process LLVM since W11): none in the
# install, none on the PATH the cases use, none named by the environment
# (which is empty); and AVANT_BACKEND=myc is refused, naming the variable.
# Nothing
# outside the work directory is read from the checkout or written; the
# work directory is removed at the end (SMOKE_KEEP=1 keeps it).
#
# Exit status 0 only when every case passes.
set -u

if [ "$#" -ne 1 ]; then
  echo "usage: sh tools/smoke.sh TARBALL | INSTALL_DIR" >&2
  exit 2
fi
tarball=$1
if [ ! -e "$tarball" ]; then
  echo "smoke: no such tarball or directory: $tarball" >&2
  exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/avant-smoke-XXXXXX")
cleanup() {
  if [ "${SMOKE_KEEP:-0}" = 1 ]; then
    echo "smoke: kept $work"
  else
    rm -rf "$work"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$work/proj" "$work/home" "$work/tmp" "$work/logs"
if [ -d "$tarball" ]; then
  inst=$(CDPATH= cd -- "$tarball" && pwd)
else
  mkdir -p "$work/prefix"
  tar -C "$work/prefix" -xzf "$tarball" || { echo "smoke: cannot extract $tarball" >&2; exit 1; }
  inst="$work/prefix/avant-linux-x86_64"
fi
avant="$inst/avant"
if [ ! -x "$avant" ]; then
  echo "smoke: no avant in $inst" >&2
  exit 1
fi
before=$(cd "$inst" && find . -printf '%p %s %T@\n' | sort)

passed=0
failed=0

# run NAME -- CMD...: CMD in the project directory with the empty
# environment; stdout and stderr go to logs/NAME.log; returns CMD's status.
run() {
  name=$1
  shift
  (
    cd "$work/proj" &&
      exec env -i HOME="$work/home" PATH=/usr/bin:/bin TMPDIR="$work/tmp" "$@"
  ) > "$work/logs/$name.log" 2>&1 < /dev/null
}

pass() {
  passed=$((passed + 1))
  echo "ok   $1"
}

fail() {
  failed=$((failed + 1))
  echo "FAIL $1: $2"
  if [ -f "$work/logs/$1.log" ]; then
    sed -n '1,12s/^/     | /p' "$work/logs/$1.log"
  fi
}

# expect NAME EXPECTED -- CMD...: CMD exits 0 and its output is EXPECTED.
expect() {
  name=$1
  want=$2
  shift 2
  if run "$name" "$@"; then
    got=$(cat "$work/logs/$name.log")
    if [ "$got" = "$want" ]; then
      pass "$name"
    else
      fail "$name" "output differs (want: $(printf '%s' "$want" | tr '\n' '|'))"
    fi
  else
    fail "$name" "exit status $?"
  fi
}

src() {
  cat > "$work/proj/$1"
}

src hello.av <<'EOF'
fn main {
  puts("hello from avant")
}
EOF
# No myc-llvm reachable: not in the install, not on the cases' PATH (nor
# the install directory, which path-hello adds), and the environment is
# empty (no AVANT_MYC_LLVM). The old back end's name is refused (it
# retired at Stage 28 W11): the compiler has no other back end.
mycs=$(find "$inst" -name 'myc-llvm*' 2>/dev/null)
if [ -n "$mycs" ]; then
  fail no-myc-llvm "the install has $mycs"
elif run no-myc-llvm-path sh -c "PATH=/usr/bin:/bin:$inst; command -v myc-llvm"; then
  fail no-myc-llvm "a myc-llvm is on PATH: $(cat "$work/logs/no-myc-llvm-path.log")"
else
  pass no-myc-llvm
fi
if run old-backend-absent env AVANT_BACKEND=myc "$avant" run hello.av; then
  fail old-backend-absent "AVANT_BACKEND=myc was accepted"
elif grep -q "AVANT_BACKEND must be unset, empty, or llvm" "$work/logs/old-backend-absent.log"; then
  pass old-backend-absent
else
  fail old-backend-absent "no AVANT_BACKEND message"
fi

expect run-hello "hello from avant" "$avant" run hello.av

# Started through PATH, as an install is used: argv[0] has no directory.
expect path-hello "hello from avant" env PATH="$inst:/usr/bin:/bin" avant run hello.av

if run compile-hello "$avant" compile hello.av hello; then
  expect exec-hello "hello from avant" ./hello
else
  fail compile-hello "exit status $?"
fi

# The three codegen modes (D174), each compiled and run: hello, a tree
# of class objects (allocation and collection), and arrays, hashes,
# strings, and a block.
src trees.av <<'EOF'
class Node {
  item: Int
  left: Node?
  right: Node?

  fn initialize(item: Int) {
    self.item = item
  }
}

fn make(item: Int, depth: Int): Node {
  n = Node.new(item)
  if depth > 0 {
    n.left = make(item &* 2, depth - 1)
    n.right = make(item &* 2 &+ 1, depth - 1)
  }
  n
}

fn check(n: Node, depth: Int): Int {
  if depth == 0 {
    return n.item
  }
  if l = n.left, r = n.right {
    return check(l, depth - 1) &+ check(r, depth - 1) &+ n.item
  }
  n.item
}

fn main {
  sum = 0
  for i in 0..64 {
    sum = sum &+ check(make(i, 10), 10)
  }
  puts(sum)
}
EOF
src collect.av <<'EOF'
fn main {
  xs: Array(Int) = [1, 2]
  xs.push(3)
  sum = 0
  xs.each { |n| sum += n }
  puts(sum)
  puts(xs.size)

  h = Hash(String, Int).new
  h["a"] = 10
  h["b"] = 20
  if v = h.get("a") {
    puts(v)
  }
  puts(h.size)
  puts("hi " + "avant")
}
EOF
for mode in debug bare release; do
  flag="--$mode"
  [ "$mode" = bare ] && flag=""
  for prog in hello trees collect; do
    case $prog in
      hello) want="hello from avant" ;;
      trees) want="2863245344" ;;
      collect) want="6
3
10
2
hi avant" ;;
    esac
    # shellcheck disable=SC2086 # flag is one word or empty
    if run "compile-$prog-$mode" "$avant" compile $prog.av "out-$mode/$prog" $flag; then
      expect "exec-$prog-$mode" "$want" "./out-$mode/$prog"
    else
      fail "compile-$prog-$mode" "exit status $?"
    fi
  done
done

src threads.av <<'EOF'
fn main {
  h = spawn {
    21 * 2
  }
  puts(h.join)
}
EOF
expect std-threads "42" "$avant" run threads.av

src json.av <<'EOF'
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
EOF
expect std-json "7
ada" "$avant" run json.av

# A literal pattern must take re2c: the compile log must not say that it
# stayed on PCRE2 (the suite's test_std_regex uses the same signal).
src regex_literal.av <<'EOF'
import std.regex

fn main {
  if re = Regex.compile("a+") {
    if m = re.find("xxaa") {
      puts(m.start)
      puts(m.end)
    }
    re.close
  }
}
EOF
if run compile-regex-literal "$avant" compile regex_literal.av regex_literal; then
  if grep -q "stays on PCRE2" "$work/logs/compile-regex-literal.log"; then
    fail compile-regex-literal "the literal pattern stayed on PCRE2 (re2c not used)"
  else
    pass compile-regex-literal
  fi
  expect exec-regex-literal "2
4" ./regex_literal
else
  fail compile-regex-literal "exit status $?"
fi

src regex_computed.av <<'EOF'
import std.regex

fn main {
  p = "a"
  p = p + "+"
  if re = Regex.compile(p) {
    if m = re.find("xxaa") {
      puts(m.start)
      puts(m.end)
    }
    re.close
  }
}
EOF
expect std-regex-computed "2
4" "$avant" run regex_computed.av

src zlib.av <<'EOF'
import std.zlib

fn main {
  if back = "hello zlib".bytes.compress.uncompress {
    puts(String.from_utf8(back) ?? "")
  }
}
EOF
expect std-zlib "hello zlib" "$avant" run zlib.av

src base64.av <<'EOF'
import std.base64

fn main {
  puts("aaa".bytes.to_base64)
}
EOF
expect std-base64 "YWFh" "$avant" run base64.av

src http.av <<'EOF'
import std.http

fn main {
  if server = Http.listen(0) {
    server.get("/") { |req|
      server.close
      Response.text("Hello Avant")
    }
    port = server.port
    handle = spawn {
      if resp = Http.get("http://127.0.0.1:${port}/") {
        puts(String.from_utf8(resp.body) ?? "")
      }
      0
    }
    server.serve
    _ = handle.join
  }
}
EOF
if run compile-http "$avant" compile http.av http_bin; then
  pass compile-http
else
  fail compile-http "exit status $?"
fi

mkdir -p "$work/proj/tests"
src avant.toml <<'EOF'
[package]
name = "smoke"
version = "0.1.0"
EOF
src tests/test_ok.av <<'EOF'
import std.testing

fn main: Int {
  t = Test.new
  t.check(1 + 1 == 2, "arithmetic")
  t.finish
}
EOF
if run avant-test "$avant" test; then
  if grep -q "ok tests/test_ok.av" "$work/logs/avant-test.log"; then
    pass avant-test
  else
    fail avant-test "no ok line for tests/test_ok.av"
  fi
else
  fail avant-test "exit status $?"
fi

# What the archive carries for its user (O294, Stage 29 W-FIX1e): the
# examples the README points to (examples/hello.av run from the
# install), Avant's LICENSE, and the third-party notices with each
# vendored library's license.
if [ -f "$inst/examples/hello.av" ] && [ -f "$inst/examples/c/add.c" ]; then
  pass examples-shipped
else
  fail examples-shipped "examples/hello.av or examples/c/add.c is missing from the install"
fi
expect example-hello "Hello Avant" "$avant" run "$inst/examples/hello.av"
missing=""
for f in LICENSE third_party/NOTICE.md third_party/base64/LICENSE third_party/pcre2/LICENCE.md third_party/pcre2/deps/sljit/LICENSE third_party/uSockets/LICENSE third_party/zlib/LICENSE third_party/yyjson/LICENSE third_party/picohttpparser/LICENSE; do
  [ -s "$inst/$f" ] || missing="$missing $f"
done
if [ -z "$missing" ]; then
  pass licenses-shipped
else
  fail licenses-shipped "missing or empty:$missing"
fi

# `avant version` and `--version` (O534) print the version and the
# commit of the archive's versions.txt (its `version:` line, else a
# release's `tag: vX`).
want_v=$(sed -n 's/^version: //p' "$inst/versions.txt" | head -n 1)
[ -n "$want_v" ] || want_v=$(sed -n 's/^tag: v//p' "$inst/versions.txt" | head -n 1)
want_c=$(sed -n 's/^avant: //p' "$inst/versions.txt" | head -n 1)
if run version "$avant" version && run dash-version "$avant" --version; then
  got_v=$(cat "$work/logs/version.log")
  case "$got_v" in
    "avant $want_v (commit $want_c)" | "avant $want_v (commit $want_c, with uncommitted changes)")
      if [ "$got_v" = "$(cat "$work/logs/dash-version.log")" ]; then
        pass version
      else
        fail version "--version prints something else than version"
      fi
      ;;
    *) fail version "prints '$got_v', not avant $want_v (commit $want_c)" ;;
  esac
else
  fail version "exit status $?"
fi

helper="$inst/avant-bind"
if [ -x "$helper" ] && ! ldd "$helper" 2>&1 | grep -q "not found"; then
  src add.h <<'EOF'
int add(int a, int b);
EOF
  if run avant-bind "$avant" bind add.h; then
    if grep -q "add" "$work/logs/avant-bind.log"; then
      pass avant-bind
    else
      fail avant-bind "the lib block does not name add"
    fi
  else
    fail avant-bind "exit status $?"
  fi
  if run path-bind env PATH="$inst:/usr/bin:/bin" avant bind add.h; then
    if grep -q "add" "$work/logs/path-bind.log"; then
      pass path-bind
    else
      fail path-bind "the lib block does not name add"
    fi
  else
    fail path-bind "exit status $?"
  fi
else
  echo "skip avant-bind (the helper's libraries are not all present)"
fi

after=$(cd "$inst" && find . -printf '%p %s %T@\n' | sort)
if [ "$before" = "$after" ]; then
  pass install-untouched
else
  fail install-untouched "the extracted install changed during the run"
fi

echo "smoke: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
