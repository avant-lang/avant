#!/bin/sh
# The runtime-C rooting lint (Stage 28 W16a, gc-design.md section 4.2
# check 3; the contract RC1-RC4 is in runtime/avant_gc.h).
#
# Over the runtime units outside the collector (runtime/avant_*.c except
# avant_gc_*.c and avant_rt_traps.c), a function is *allocating* when its
# body calls avant_alloc or another allocating function of any of these
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
# Usage: sh tools/gc-lint.sh [ROOT]   (default: the checkout this script
# is in). Prints "gc-lint: clean (N functions, M allocating)" on success.
set -eu
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
  printf "gc-lint: clean (%d functions, %d allocating)\n", nfn, na
}
' $files
