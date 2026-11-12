#!/bin/sh
# Code placement probe (Stage 29 WP, O186): does a LangArena task's time
# depend on where its code lands? Run it alone, from the checkout root laid
# out for the harness (langarena/ copied in, LangArena/ beside the checkout):
#
#   sh tools/placement-probe.sh OUT [TASK...]
#
# TASK is a harness task name (Etc::Sieve); the default is the tasks the WP
# probe found sensitive before the placement policy. Steps:
#
#   1. the harness (measure.cr prod, AVANT_TASK_LIST) builds and runs each
#      task once with PROBE_COMPILER, through a cc wrapper that keeps the
#      program's object and its link arguments (OUT/la, OUT/save)
#   2. every task is relinked once per pad in PROBE_PADS: PROBE_MODE=before
#      puts a block of PAD bytes (int3) before the program's object, which
#      moves the program and the runtime as a bigger PLT or runtime cold
#      code would; PROBE_MODE=rt puts it between the program and the runtime,
#      which moves the runtime only. No instruction changes; pad 0 is the
#      harness's own binary, byte for byte
#   3. PROBE_ROUNDS rounds run every (task, pad) binary under perf stat
#      (cycles:u), the pads in a rotated order each round, single-threaded
#      tasks pinned to CPU PROBE_PIN (matmul_t* are not pinned: one CPU's
#      scheduling then decides less of the result); every run's checksum
#      is checked
#   4. OUT/align.txt: the .text alignment of each program object and of
#      avant_rt.o (64 under the WP policy, 16 before it);
#      OUT/spread.txt: per task, the spread (the slowest pad's median over
#      the fastest's, minus 1) and the noise (the median over pads of each
#      pad's (max - min) / median), worst first
#
# Status: 0 when every task's spread is at most PROBE_BAR percent or within
# its noise, 1 when one is over both (named on stderr), 2 on a usage error
# or a red step (a checksum, a link, the governor not `performance`).
#
# Environment: PROBE_COMPILER (bin/avant-c-llvm), PROBE_PADS ("0 16 32 48
# 64 128 256 512"), PROBE_MODE (before), PROBE_ROUNDS (3), PROBE_PIN (2),
# PROBE_BAR (2.0). Every child runs under timeout with stdin from
# /dev/null; nothing is written outside OUT except the harness's own
# langarena/tmp (as every harness run).
#
# The WP record (results/2026-11-11-s29-wp, the ledgers' records) has the
# numbers before and after the policy: spreads up to 14% before, within the
# noise after, the 50 tasks over 8 pads and 5 rounds (about an hour; the
# default set at 3 rounds takes about 15 minutes).
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
out=${1:-}
[ -n "$out" ] || { echo "usage: sh tools/placement-probe.sh OUT [TASK...]" >&2; exit 2; }
shift
[ -f langarena/harness/measure.cr ] || { echo "placement-probe: langarena/harness/measure.cr is missing (copy the harness in)" >&2; exit 2; }
comp=$(realpath -m "${PROBE_COMPILER:-bin/avant-c-llvm}")
[ -x "$comp" ] || { echo "placement-probe: compiler $comp is missing" >&2; exit 2; }
pads=${PROBE_PADS:-0 16 32 48 64 128 256 512}
mode=${PROBE_MODE:-before}
rounds=${PROBE_ROUNDS:-3}
pin=${PROBE_PIN:-2}
bar=${PROBE_BAR:-2.0}
case "$mode" in before|rt) ;; *) echo "placement-probe: PROBE_MODE is before or rt" >&2; exit 2;; esac
if [ $# -eq 0 ]; then
  set -- Sort::Self Etc::Sieve Compress::HuffEncode Brainfuck::Recursion Brainfuck::Array Sort::Merge \
    Template::Parse Json::Generate Compress::LZWEncode CSV::Parse Calculator::Ast Distance::Jaro \
    Binarytrees::Arena Compress::ArithDecode Etc::Words Sort::Quick Compress::ArithEncode
fi
mkdir -p "$out" || exit 2
out=$(realpath "$out")
gov=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ')
echo "governor: $gov" > "$out/governor.txt"
[ "$gov" = "performance " ] || { echo "placement-probe: the governor is not performance: $gov" >&2; exit 2; }

# 1. Build through the wrapper.
cat > "$out/cc-save.sh" <<'EOF'
#!/bin/sh
# A compile passes through; a link keeps the program's object (the first
# argument) and the other arguments under $CC_SAVE_DIR/<output name>/.
for a in "$@"; do [ "$a" = "-c" ] && exec /usr/bin/cc "$@"; done
out=""; prev=""
for a in "$@"; do [ "$prev" = "-o" ] && out=$a; prev=$a; done
if [ -n "${CC_SAVE_DIR:-}" ] && [ -n "$out" ]; then
  d="$CC_SAVE_DIR/$(basename "$out")"; rm -rf "$d"; mkdir -p "$d"
  cp "$1" "$d/prog.o"; : > "$d/args"; n=0; first=1; prev=""
  for a in "$@"; do
    if [ $first = 1 ]; then first=0; prev=$a; continue; fi
    if [ "$prev" = "-o" ]; then prev=$a; continue; fi
    case "$a" in
      -o) ;;
      */runtime/build/*.o) echo "RT/$(basename "$a")" >> "$d/args" ;;
      *.o|*.a) n=$((n+1)); cp "$a" "$d/extra$n.o"; echo "$d/extra$n.o" >> "$d/args" ;;
      *) printf '%s\n' "$a" >> "$d/args" ;;
    esac
    prev=$a
  done
fi
exec /usr/bin/cc "$@"
EOF
chmod +x "$out/cc-save.sh"
list=$(printf '%s,' "$@"); list=${list%,}
rm -rf "$out/save" "$out/la"; mkdir -p "$out/save" "$out/la" "$out/rt" "$out/pads" "$out/bins"
CC="$out/cc-save.sh" CC_SAVE_DIR="$out/save" AVANT_COMPILER="$comp" AVANT_CODEGEN=release AVANT_TASK_LIST="$list" \
  AVANT_RESULTS_DIR="$out/la" timeout -k 10 2700 crystal run langarena/harness/measure.cr -- prod < /dev/null > "$out/la/la-prod.log" 2>&1 \
  || { echo "placement-probe: the harness run failed (OUT/la/la-prod.log)" >&2; exit 2; }
cp runtime/build/*.o "$out/rt/"
# The policy itself, read from the objects: the .text alignment of each
# program object and of the runtime's (64 under the WP policy; 16 before).
# (The harness's own crystal build goes through the wrapper too; its
# directory is dropped.)
rm -rf "$out/save/crystal-run-measure.tmp"
text_align() { readelf -SW "$1" | awk '{for (i = 1; i <= NF; i++) if ($i == ".text") print $NF}'; }
for d in "$out"/save/*/; do
  printf '%s\t%s\n' "$(basename "$d")" "$(text_align "$d/prog.o")"
done > "$out/align.txt"
printf 'runtime avant_rt.o\t%s\n' "$(text_align "$out/rt/avant_rt.o")" >> "$out/align.txt"

# 2. Pads and links.
for p in $pads; do
  [ "$p" -gt 0 ] || continue
  printf '.section .text\n.p2align 4\n.skip %d, 0xcc\n.section .note.GNU-stack,"",@progbits\n' "$p" > "$out/pads/pad$p.s"
  as -o "$out/pads/pad$p.o" "$out/pads/pad$p.s" || exit 2
done
slugs=""
for name in "$@"; do
  s=$(echo "$name" | sed 's/::/_/' | tr 'A-Z' 'a-z')
  [ -f "$out/save/$s/prog.o" ] || { echo "placement-probe: no object for $name" >&2; exit 2; }
  slugs="$slugs $s"
  for p in $pads; do
    pre=""; mid=""
    if [ "$p" -gt 0 ]; then
      if [ "$mode" = before ]; then pre="$out/pads/pad$p.o"; else mid="$out/pads/pad$p.o"; fi
    fi
    rtargs=""; rest=""
    while IFS= read -r a; do
      case "$a" in RT/*) rtargs="$rtargs $out/rt/${a#RT/}" ;; *) rest="$rest $a" ;; esac
    done < "$out/save/$s/args"
    # shellcheck disable=SC2086
    timeout -k 5 120 /usr/bin/cc $pre "$out/save/$s/prog.o" $mid $rtargs $rest -o "$out/bins/$s.$p" < /dev/null \
      || { echo "placement-probe: link failed: $s pad $p" >&2; exit 2; }
  done
done

# 3. Rounds.
tsv="$out/probe.tsv"
echo "# mode=$mode pads=$pads rounds=$rounds pin=$pin compiler=$comp governor=$gov date=$(date -Is)" > "$tsv"
npads=$(echo $pads | wc -w)
r=1; red=0
while [ "$r" -le "$rounds" ]; do
  for s in $slugs; do
    exp=$(sed -n 's/^official_checksum: //p' "$out/la/raw/$s.txt")
    case "$s" in matmul_t*) pre="";; *) pre="taskset -c $pin";; esac
    set -- $pads $pads
    i=0; while [ "$i" -lt $(( (r - 1) % npads )) ]; do shift; i=$((i + 1)); done
    k=0
    for p in "$@"; do
      [ "$k" -ge "$npads" ] && break; k=$((k + 1))
      $pre timeout -k 5 300 perf stat -x, -e cycles:u -o "$out/bins/stat" "$out/bins/$s.$p" < /dev/null > "$out/bins/stdout" 2> "$out/bins/stderr"
      got=$(head -n 1 "$out/bins/stdout" | tr -d ' \r'); ok=ok
      [ "$got" = "$exp" ] || { ok=BAD; red=1; }
      cyc=$(awk -F, '/cycles/ {print $1}' "$out/bins/stat")
      printf '%s\t%s\t%s\t%s\t%s\n' "$s" "$p" "$r" "$cyc" "$ok" >> "$tsv"
    done
  done
  r=$((r + 1))
done
rm -rf "$out/bins"

# 4. Spread and noise.
awk -F'\t' -v bar="$bar" '
  function median(s,   n, a, i, j, t) {
    n = split(s, a, " ")
    for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j + 1] = a[j]; a[j + 1] = t }
    return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
  }
  function minmax(s,   n, a, i) { n = split(s, a, " "); lo = hi = a[1]; for (i = 2; i <= n; i++) { if (a[i] < lo) lo = a[i]; if (a[i] > hi) hi = a[i] } }
  /^#/ { next }
  $5 == "ok" { v[$1 SUBSEP $2] = v[$1 SUBSEP $2] " " $4; if (!($1 in seen)) { seen[$1] = 1; order[++nt] = $1 } pads[$1] = pads[$1] " " $2 }
  END {
    over = 0
    for (k = 1; k <= nt; k++) {
      t = order[k]; np = split(pads[t], pl, " "); delete done; lo_m = 0; hi_m = 0; ns = ""
      for (i = 1; i <= np; i++) {
        p = pl[i]; if (p in done) continue; done[p] = 1
        m = median(v[t SUBSEP p]); if (lo_m == 0 || m < lo_m) lo_m = m; if (m > hi_m) hi_m = m
        minmax(v[t SUBSEP p]); ns = ns " " (hi - lo) / m
      }
      spread = (hi_m / lo_m - 1) * 100; noise = median(ns) * 100
      verdict = (spread > bar && spread > noise) ? "OVER" : "ok"
      if (verdict == "OVER") over++
      printf "%s\t%.1f\t%.1f\t%.3f\t%s\n", t, spread, noise, lo_m / 1e9, verdict
    }
    exit over ? 1 : 0
  }' "$tsv" | sort -t "$(printf '\t')" -k2,2 -rn > "$out/spread.body"
st=$(awk -F'\t' '$5 == "OVER"' "$out/spread.body" | wc -l)
{ printf 'task\tspread%%\tnoise%%\tfastest_Gcycles\tverdict (bar %s%%)\n' "$bar"; cat "$out/spread.body"; } > "$out/spread.txt"
rm -f "$out/spread.body"
cat "$out/spread.txt"
[ "$red" -eq 0 ] || { echo "placement-probe: a checksum differed (OUT/probe.tsv rows marked BAD)" >&2; exit 2; }
if [ "$st" -gt 0 ]; then
  echo "placement-probe: $st task(s) over the bar and their noise:" >&2
  awk -F'\t' '$5 == "OVER" {print "  " $1 " " $2 "%"}' "$out/spread.txt" >&2
  exit 1
fi
exit 0
