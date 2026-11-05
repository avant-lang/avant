#!/bin/sh
# Out-of-checkout smoke test of a release tarball (Wave D, D147, D148).
#
#   sh tools/smoke.sh TARBALL
#
# Extracts TARBALL into a temporary prefix and, from a temporary project
# directory with an empty environment (only HOME, PATH=/usr/bin:/bin, and
# TMPDIR, all inside the work directory), compiles and runs programs that
# use every std module, runs `avant test` on a small project, and runs
# `avant bind` when the bundled helper's libraries are present. Nothing
# outside the work directory is read from the checkout or written; the
# work directory is removed at the end (SMOKE_KEEP=1 keeps it).
#
# Exit status 0 only when every case passes.
set -u

if [ "$#" -ne 1 ]; then
  echo "usage: sh tools/smoke.sh TARBALL" >&2
  exit 2
fi
tarball=$1
if [ ! -f "$tarball" ]; then
  echo "smoke: no such tarball: $tarball" >&2
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

mkdir -p "$work/prefix" "$work/proj" "$work/home" "$work/tmp" "$work/logs"
tar -C "$work/prefix" -xzf "$tarball" || { echo "smoke: cannot extract $tarball" >&2; exit 1; }
avant="$work/prefix/avant-linux-x86_64/avant"
if [ ! -x "$avant" ]; then
  echo "smoke: the tarball has no avant-linux-x86_64/avant" >&2
  exit 1
fi
before=$(cd "$work/prefix" && find . -printf '%p %s %T@\n' | sort)

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
expect run-hello "hello from avant" "$avant" run hello.av

# Started through PATH, as an install is used: argv[0] has no directory.
expect path-hello "hello from avant" env PATH="$work/prefix/avant-linux-x86_64:/usr/bin:/bin" avant run hello.av

if run compile-hello "$avant" compile hello.av hello; then
  expect exec-hello "hello from avant" ./hello
else
  fail compile-hello "exit status $?"
fi

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
    dummy = handle.join
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

helper="$work/prefix/avant-linux-x86_64/avant-bind"
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
  if run path-bind env PATH="$work/prefix/avant-linux-x86_64:/usr/bin:/bin" avant bind add.h; then
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

after=$(cd "$work/prefix" && find . -printf '%p %s %T@\n' | sort)
if [ "$before" = "$after" ]; then
  pass install-untouched
else
  fail install-untouched "the extracted install changed during the run"
fi

echo "smoke: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
