#!/bin/sh
# The runtime-C rooting lint (the contract RC1-RC4 is in runtime/avant_gc.h).
#
# Over the runtime units outside the collector (runtime/avant_*.c except
# avant_gc_*.c and avant_rt_traps.c), a function is *allocating* when its
# body calls avant_alloc (or the inline avant_str_alloc of avant_rt.h) or another allocating function of any of these
# units (the closure is taken over all of them, so a helper defined in one
# file and called from another counts). Every function that calls an
# allocating function must, in its body, open a handle scope
# (AVANT_GC_SCOPE), open a no-GC region (avant_gc_nogc_begin or the ABI
# name avant_gc_defer_enter), or carry the documented exemption, a
# comment containing "gc-lint: holds nothing" (with its reason) in the
# function or on the lines just above it. A call that a return statement
# makes, alone among the allocating calls of that statement, needs none:
# nothing of the function runs after it. A violation prints
# "file:line: function calls allocating NAME with no scope, region or
# exemption" and the script exits 1.
#
# It reads C by lines, the way the runtime is formatted (clang-format):
# a function starts at a column-0 line with a parenthesis that is not a
# declaration (it does not end with ';'), its body ends at the next
# column-0 '}'. Comments are stripped before calls are looked for, so a
# name in a comment is not a call.
#
# The barrier-first lint (RC5 of
# runtime/avant_gc.h). Over the same units, a function *stores a
# reference word* when a line of its body matches one of the forms the
# runtime writes such a word in:
#   ((void **)X)[i] = ..., ((char **)X)[i] = ..., ((const char **)X)[i] = ...
#   *slot = ...  (a statement of its own), memcpy(slot, ...
#   X->buf = ..., X->data = ..., X->keys = ..., X->vals = ..., X->state = ...
#     (the reference fields of the runtime's heap layouts; read in the
#     units that define them: avant_array, avant_hash, avant_buf,
#     avant_str, avant_json, avant_io)
#   qsort(..., sizeof(char *), ...) or sizeof(void *): a sort of a
#     pointer buffer
#   a call of a function marked "gc-lint: a store helper" (it stores the
#     word for its callers, which make the barrier call).
# The first such line of a function must come after a line that calls
# avant_barrier, avant_barrier_slot (or its inline form
# avant_barrier_slot_young), avant_barrier_at or avant_gc_overwrite, or the function carries "gc-lint: stores into its
# own allocation" or "gc-lint: no reference word" (with its reason), in
# its body or on the lines just above it. A violation prints
# "file:line: function stores a reference word with no barrier before it"
# and the script exits 1. So every runtime path that overwrites a
# reference in an object it did not allocate is one of the functions this
# lint lists under --stores, each with its barrier before the store, and a
# new one without the barrier fails the gate. What a line pattern cannot
# show (an element moved by a memcpy of a computed size, as
# avant_array_reverse does) is found by AVANT_GC_VERIFY=1 wherever a test
# runs it ("a reference was overwritten with no barrier before the
# store", runtime/avant_gc_verify.c).
#
# Usage: sh tools/gc-lint.sh [--stores] [ROOT]   (default: the checkout
# this script is in). Prints "gc-lint: clean (N functions, M allocating;
# K store a reference word, each after its barrier)" on success;
# --stores lists the K functions first.
set -eu
stores=0
if [ "${1:-}" = --stores ]; then stores=1; shift; fi
root=${1:-$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)}
files=$(ls "$root"/runtime/avant_*.c | grep -v -e '/avant_gc_' -e '/avant_rt_traps\.c$')
# shellcheck disable=SC2086
awk '
function flush() {
  if (fn != "") {
    nfn++
    fname[nfn] = fn; ffile[nfn] = FILENAME; fline[nfn] = start
    fcalls[nfn] = calls; fok[nfn] = ok
  }
  fn = ""; calls = " "; ok = 0
}
FNR == 1 { flush(); incom = 0; pending_ok = 0 }
{
  raw = $0
  if (raw ~ /gc-lint: holds nothing/) {
    if (fn != "") { ok = 1 } else { pending_ok = 1 }
  }
  # Strip comments (single-line and multi-line), then strings.
  line = raw
  out = ""
  while (length(line) > 0) {
    if (incom) {
      e = index(line, "*/")
      if (e == 0) { line = ""; break }
      line = substr(line, e + 2); incom = 0
    } else {
      b = index(line, "/*")
      if (b == 0) { out = out line; line = ""; break }
      out = out substr(line, 1, b - 1); line = substr(line, b + 2); incom = 1
    }
  }
  sub(/\/\/.*/, "", out)
  gsub(/"([^"\\]|\\.)*"/, "\"\"", out)
  if (fn == "" && out ~ /^[A-Za-z_]/ && out ~ /\(/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct|enum|union|#)/) {
    s = out
    sub(/\(.*/, "", s)
    n = split(s, w, /[ \t*]+/)
    fn = w[n]; start = FNR; calls = " "; ok = pending_ok; pending_ok = 0
    next
  }
  if (fn == "" && out ~ /[^ \t]/ && raw !~ /gc-lint: holds nothing/ && out !~ /^[ \t]*$/) { pending_ok = pending_ok && (out ~ /^[ \t]*$/) }
  if (fn != "") {
    if (out ~ /AVANT_GC_SCOPE\(|avant_gc_nogc_begin\(|avant_gc_defer_enter\(/) { ok = 1 }
    t = out
    # A return statement (also after "if (...) {" on the same line, or
    # continued on the next lines): its calls are counted on this line.
    isret = out ~ /(^|[{;])[ \t]*return[ \t]/
    while (match(t, /[A-Za-z_][A-Za-z_0-9]*[ \t]*\(/)) {
      nm = substr(t, RSTART, RLENGTH)
      sub(/[ \t]*\($/, "", nm)
      nc++
      cfn[nc] = nfn + 1; cname[nc] = nm; cline[nc] = FNR; cret[nc] = isret ? FNR : 0
      if (index(calls, " " nm " ") == 0) { calls = calls nm " " }
      t = substr(t, RSTART + RLENGTH)
    }
    if (raw ~ /^}/) { flush() }
  }
}
END {
  flush()
  alloc["avant_alloc"] = 1
  # the inline string allocator of avant_rt.h allocates
  alloc["avant_str_alloc"] = 1
  changed = 1
  while (changed) {
    changed = 0
    for (i = 1; i <= nfn; i++) {
      if (fname[i] in alloc) continue
      n = split(fcalls[i], c, " ")
      for (k = 1; k <= n; k++) if (c[k] in alloc && c[k] != fname[i]) { alloc[fname[i]] = 1; changed = 1; break }
    }
  }
  bad = 0; na = 0
  for (i = 1; i <= nfn; i++) if (fname[i] in alloc) na++
  # A return statement with one allocating call holds nothing across it:
  # nothing of the function runs after the call.
  for (j = 1; j <= nc; j++) if (cret[j] && (cname[j] in alloc)) nret[cfn[j], cret[j]]++
  for (j = 1; j <= nc; j++) {
    i = cfn[j]
    if (fok[i] || !(cname[j] in alloc) || cname[j] == fname[i]) continue
    if (cret[j] && nret[i, cret[j]] == 1) continue
    key = i SUBSEP cname[j]
    if (key in told) continue
    told[key] = 1
    f = ffile[i]; sub(/.*\//, "runtime/", f)
    printf "%s:%d: %s calls allocating %s with no scope, region or exemption\n", f, cline[j], fname[i], cname[j]
    bad++
  }
  if (bad) { printf "gc-lint: %d violation(s)\n", bad; exit 1 }
  printf "gc-lint: clean (%d functions, %d allocating", nfn, na
}
' $files
# The barrier-first lint (RC5). The store helpers first (a function with
# "gc-lint: a store helper" on the lines just above it), so a caller
# defined before its helper is read with the helper's name known.
# shellcheck disable=SC2086
helpers=$(awk '
/gc-lint: a store helper/ { want = 1; next }
want && /^[A-Za-z_]/ && /\(/ && !/;[ \t]*$/ {
  s = $0; sub(/\(.*/, "", s); n = split(s, w, /[ \t*]+/); printf "%s ", w[n]; want = 0; next
}
want && /^[ \t]*$/ { want = 0 }
' $files)
# shellcheck disable=SC2086
awk -v list="$stores" -v helperlist="$helpers" '
function flush() {
  if (fn != "") {
    if (helper) { helpers[fn] = 1 }
    else if (store) {
      nst++
      f = FILENAME; sub(/.*\//, "runtime/", f)
      if (ok) { why = "exempt" } else if (bar && bar <= store) { why = "barrier at line " bar } else { why = "" }
      if (why == "") {
        printf "\n%s:%d: %s stores a reference word with no barrier before it", f, store, fn
        bad++
      } else if (list) {
        printf "\n%s:%d: %s (%s)", f, store, fn, why
      }
    }
  }
  fn = ""; store = 0; bar = 0; ok = 0; helper = 0
}
BEGIN { nh = split(helperlist, hw, " "); for (i = 1; i <= nh; i++) helpers[hw[i]] = 1 }
FNR == 1 { flush(); incom = 0; pending_ok = 0; pending_helper = 0
  fields = (FILENAME ~ /\/avant_(array|hash|buf|str|json|io)\.c$/) }
{
  raw = $0
  isok = (raw ~ /gc-lint: stores into its own allocation|gc-lint: no reference word/)
  ishelper = (raw ~ /gc-lint: a store helper/)
  if (isok) { if (fn != "") { ok = 1 } else { pending_ok = 1 } }
  if (ishelper) { if (fn != "") { helper = 1 } else { pending_helper = 1 } }
  line = raw
  out = ""
  while (length(line) > 0) {
    if (incom) {
      e = index(line, "*/")
      if (e == 0) { line = ""; break }
      line = substr(line, e + 2); incom = 0
    } else {
      b = index(line, "/*")
      if (b == 0) { out = out line; line = ""; break }
      out = out substr(line, 1, b - 1); line = substr(line, b + 2); incom = 1
    }
  }
  sub(/\/\/.*/, "", out)
  gsub(/"([^"\\]|\\.)*"/, "\"\"", out)
  if (fn == "" && out ~ /^[A-Za-z_]/ && out ~ /\(/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct|enum|union|#)/) {
    s = out
    sub(/\(.*/, "", s)
    n = split(s, w, /[ \t*]+/)
    fn = w[n]; store = 0; bar = 0; ok = pending_ok; helper = pending_helper
    pending_ok = 0; pending_helper = 0
    next
  }
  if (fn == "" && out ~ /[^ \t]/ && !isok && !ishelper) { pending_ok = 0; pending_helper = 0 }
  if (fn != "") {
    if (!bar && out ~ /avant_barrier(_slot|_slot_young|_at)?[ \t]*\(|avant_gc_overwrite[ \t]*\(/) { bar = FNR }
    if (!store) {
      st = 0
      if (out ~ /\(\((const )?(void|char) \*\*\)[^;]*\)\[[^;]*\][ \t]*=[^=]/) st = 1
      if (out ~ /^[ \t]*\*slot[ \t]*=[^=]/) st = 1
      if (out ~ /memcpy\(slot,/) st = 1
      if (fields && out ~ /->(buf|data|keys|vals|state)[ \t]*=[^=]/) st = 1
      if (out ~ /qsort\(.*sizeof\((const )?(char|void) \*\)/) st = 1
      for (h in helpers) if (index(out, h "(") > 0) st = 1
      if (st) { store = FNR }
    }
    if (raw ~ /^}/) { flush() }
  }
}
END {
  flush()
  if (bad) { printf "\ngc-lint: %d violation(s)\n", bad; exit 1 }
  printf "%s; %d store a reference word, each after its barrier)\n", (list && nst) ? "\ngc-lint: clean (as above" : "", nst
}
' $files
