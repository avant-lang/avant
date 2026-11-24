#!/bin/sh
# tools/gc-lint.sh on a fake runtime:
#   lint-attribute-name: a function whose definition starts with
#     __attribute__((...)) is known by its own name, so a caller that keeps
#     two of its allocations without a scope is named;
#   lint-asm-not-function: a top-level __asm__ block and a declaration
#     renamed with __asm__("...") are neither functions nor calls, so the
#     functions after them are read as themselves and nothing else is named.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/gc-lint.sh WORK
#
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/gc-lint.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1"); f=$w/avant
rm -rf "$f"; mkdir -p "$f/tools" "$f/runtime"
cp "$root/tools/gc-lint.sh" "$f/tools/"
cat > "$f/runtime/avant_one.c" <<'EOF2'
__attribute__((noinline, cold)) static void *grow(long n) {
  return avant_alloc(n);
}

void *keeps_two(long n) {
  void *a = grow(n);
  void *b = grow(n);
  return a ? b : a;
}
EOF2
cat > "$f/runtime/avant_two.c" <<'EOF2'
extern const char *plain_ntop(int af, const void *src, char *dst,
                              int size) __asm__("inet_ntop");

__asm__(".section .tbss,\"awT\",@nobits\n\t"
        ".previous\n");

__attribute__((constructor)) static void check(void) {
  char b[8];
  plain_ntop(2, b, b, 8);
}

void *one_alloc(long n) {
  return avant_alloc(n);
}
EOF2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
out=$(sh "$f/tools/gc-lint.sh" "$f" 2>&1); rc=$?
[ $rc = 1 ] && echo "$out" | grep -q '^runtime/avant_one.c:[0-9]*: keeps_two calls allocating grow with no scope'
say lint-attribute-name $? "rc=$rc $(echo "$out" | tr '\n' ';')"
[ "$(echo "$out" | grep -c 'calls allocating')" = 1 ] && ! echo "$out" | grep -q '__asm__\|__attribute__\|plain_ntop\|avant_two'
say lint-asm-not-function $? "$(echo "$out" | tr '\n' ';')"
echo done
