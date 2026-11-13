#!/bin/sh
# lr-table.sh — LangArena per-task multiplier table (Stage 28 step LR).
#
# One measurement method for every language: each task is its own process,
# timed as process wall clock (date +%s%N around the process) with peak RSS
# from /usr/bin/time %M (getrusage ru_maxrss). Start-up, data preparation and
# warmup are inside the number for everyone. LangArena's in-program time
# ("Name: OK in X s") is kept as a side column for reference languages only.
#
# Usage:
#   lr-table.sh ref LABEL DIR OUT.tsv RUN_JS CMD...
#       Run CMD RUN_JS <Task> from DIR for every task in RUN_JS (a LangArena
#       reference binary). Writes OUT.tsv.
#   lr-table.sh avant BIN_DIR RUN_JS OUT.tsv
#       Re-measure the harness-built Avant binaries (BIN_DIR/<slug>, as left by
#       `measure.cr -- prod` in langarena/tmp). Put OUT.tsv in the harness
#       results directory as lr-avant.tsv.
#   lr-table.sh table REF_DIR AVANT [PUBLISHED.js]
#       Markdown table on stdout. REF_DIR holds one <lang>.tsv per reference
#       language, and optionally notes.txt ("Task<TAB>note" lines, shown in
#       the Notes column). AVANT is a harness results directory (uses lr-avant.tsv if
#       present, else the harness's runtime.tsv, labelled as such) or a tsv.
#       PUBLISHED.js (optional) is a LangArena results file; it adds an
#       "estimated best" column for prod runs of languages not measured here,
#       scaled per task by C/Clang's local (c_clang.tsv) / published ratio. Estimates never enter the measured best.
#   lr-table.sh rounds N RUN_JS OUT_DIR [OPTION...] [TASK...]
#       Median-of-N with rotation (tools/rounds.py langarena; its --help has
#       the method): one warm-up, then N rounds of every task's binary,
#       started directly (no wrapper: the RSS is the task's own), the order
#       rotated by round, every output checked against RUN_JS, one raw file
#       per run, the median with its minimum and maximum, the versions and
#       the governor written by the script. OPTIONs: --side NAME=BIN_DIR
#       (repeatable; default the checkout's langarena/tmp), --pin CPU,
#       --short SECONDS, --timeout SECONDS, --allow-governor. For milestones
#       and publication, never per gate. OUT_DIR/SIDE.lr-avant.tsv is a
#       `table` input of the medians.
#
# After a wave (from avant/, results kept, references not re-run):
#   AVANT_COMPILER=./bin/avant-c-llvm AVANT_CODEGEN=release \
#     AVANT_RESULTS_DIR=$R crystal run langarena/harness/measure.cr -- prod
#   tools/lr-table.sh avant langarena/tmp ../LangArena/run.js $R/lr-avant.tsv
#   tools/lr-table.sh table results/2026-11-07-s28-lr/reference $R > table.md
#
# TSV columns: task status wall_s rss_kb inprog_s exit  (status ok|err|fail)
# Each tsv starts with "# key: value" lines (label, governor, date).
#
# Every input is checked before anything is written (O71, Stage 28 W15a2):
# a RUN_JS that does not exist or lists no task, a BIN_DIR or DIR that is
# not a directory, a REF_DIR without a reference tsv, an AVANT that is
# neither a tsv nor a results directory holding lr-avant.tsv or
# runtime.tsv, or a PUBLISHED.js that does not exist stops the script
# with "lr-table.sh: ..." on stderr and status 1, and a table whose Avant
# input has no task row is refused the same way. Before, a relative path
# that did not resolve (../LangArena from a scratch worktree) wrote an
# empty table and exited 0.
set -u

die() {
  echo "lr-table.sh: $*" >&2
  exit 1
}

need_runjs() { # RUN_JS
  [ -f "$1" ] || die "RUN_JS $1: no such file"
  [ -n "$(tasks "$1" | head -n 1)" ] || die "RUN_JS $1: lists no task"
}

need_dir() { # WHAT DIR
  [ -d "$2" ] || die "$1 $2: not a directory"
}

rows() { # TSV -> the number of task rows
  awk -F'\t' '!/^#/ && $1 != "task" && NF >= 2 { n++ } END { print n + 0 }' "$1"
}

tasks() { # RUN_JS -> "name<TAB>checksum" lines
  awk -F'"' '/"name":/ { n = $4 } /"checksum":/ { c = $3; gsub(/[^0-9]/, "", c); print n "\t" c }' "$1"
}

header() { # LABEL
  echo "# label: $1"
  echo "# governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# method: process wall (date +%s%N) + /usr/bin/time %M"
  printf 'task\tstatus\twall_s\trss_kb\tinprog_s\texit\n'
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case "$cmd" in
ref)
  [ $# -ge 5 ] || die "ref needs LABEL DIR OUT.tsv RUN_JS CMD..."
  label=$1 dir=$2 out=$3 runjs=$4; shift 4
  need_dir DIR "$dir"; need_runjs "$runjs"
  raw="$out.raw"; mkdir -p "$raw"
  header "$label" >"$out"
  tasks "$runjs" | while IFS="$(printf '\t')" read -r name sum; do
    slug=$(echo "$name" | tr 'A-Z' 'a-z' | sed 's/::/_/g')
    t0=$(date +%s%N)
    (cd "$dir" && timeout -k 10 900 /usr/bin/time -f '%M' -o "$raw/$slug.time" "$@" "$runjs" "$name" </dev/null >"$raw/$slug.out" 2>"$raw/$slug.err")
    rc=$?; t1=$(date +%s%N)
    w=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.4f", (b - a) / 1e9 }')
    rss=$(tail -n 1 "$raw/$slug.time" 2>/dev/null | tr -cd '0-9'); [ -n "$rss" ] || rss=0
    line=$(grep -F "$name: " "$raw/$slug.out" | head -n 1)
    ip=$(echo "$line" | sed -n 's/.* in \([0-9.]*\)s$/\1/p'); [ -n "$ip" ] || ip=-
    case "$line" in *": OK "*) st=ok ;; *ERR*) st=err ;; *) st=fail ;; esac
    [ $rc -ne 0 ] && st=fail
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$st" "$w" "$rss" "$ip" "$rc" >>"$out"
    echo "$label $name $st $w s $rss KB" >&2
  done ;;
avant)
  [ $# -eq 3 ] || die "avant needs BIN_DIR RUN_JS OUT.tsv"
  bindir=$1 runjs=$2 out=$3
  need_dir BIN_DIR "$bindir"; need_runjs "$runjs"
  raw="$out.raw"; mkdir -p "$raw"
  header "Avant" >"$out"
  tasks "$runjs" | while IFS="$(printf '\t')" read -r name sum; do
    slug=$(echo "$name" | tr 'A-Z' 'a-z' | sed 's/::/_/g')
    if [ ! -x "$bindir/$slug" ]; then
      printf '%s\tfail\t-\t-\t-\t-\n' "$name" >>"$out"; continue
    fi
    t0=$(date +%s%N)
    (cd "$bindir" && timeout -k 10 900 /usr/bin/time -f '%M' -o "$raw/$slug.time" "./$slug" </dev/null >"$raw/$slug.out" 2>"$raw/$slug.err")
    rc=$?; t1=$(date +%s%N)
    w=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.4f", (b - a) / 1e9 }')
    rss=$(tail -n 1 "$raw/$slug.time" 2>/dev/null | tr -cd '0-9'); [ -n "$rss" ] || rss=0
    got=$(head -n 1 "$raw/$slug.out" | tr -d ' \r')
    if [ $rc -ne 0 ]; then st=fail; elif [ "$got" = "$sum" ]; then st=ok; else st=err; fi
    printf '%s\t%s\t%s\t%s\t-\t%s\n' "$name" "$st" "$w" "$rss" "$rc" >>"$out"
    echo "Avant $name $st $w s $rss KB" >&2
  done ;;
table)
  [ $# -ge 2 ] && [ $# -le 3 ] || die "table needs REF_DIR AVANT [PUBLISHED.js]"
  refdir=$1 av=$2 pub=${3:-}
  need_dir REF_DIR "$refdir"
  ls "$refdir"/*.tsv >/dev/null 2>&1 || die "REF_DIR $refdir: no reference tsv"
  if [ -d "$av" ]; then
    [ -f "$av/lr-avant.tsv" ] || [ -f "$av/runtime.tsv" ] || die "AVANT $av: neither lr-avant.tsv nor runtime.tsv"
  else
    [ -f "$av" ] || die "AVANT $av: no such file or directory"
  fi
  [ -z "$pub" ] || [ -f "$pub" ] || die "PUBLISHED.js $pub: no such file"
  avsrc=harness
  wd=$(mktemp -d); trap 'rm -rf "$wd"' EXIT
  if [ -d "$av" ]; then
    if [ -f "$av/lr-avant.tsv" ]; then av="$av/lr-avant.tsv"; avsrc=lr
    else
      # harness runtime.tsv: name ... checksum_ok ... wall_s rss_mb (by header)
      tmp=$wd/avant.tsv; awk -F'\t' 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next }
        { st = ($h["checksum_ok"] == "true") ? "ok" : "err"
          printf "%s\t%s\t%s\t%d\t-\t%s\n", $h["name"], st, $h["wall_s"], $h["rss_mb"] * 1024, $h["exit_code"] }' "$av/runtime.tsv" >"$tmp"
      av=$tmp
    fi
  else
    avsrc=lr
  fi
  [ "$(rows "$av")" -gt 0 ] || die "AVANT $av: no task row"
  pubtsv=/dev/null
  notes=/dev/null; [ -f "$refdir/notes.txt" ] && notes=$refdir/notes.txt
  if [ -n "$pub" ]; then
    # published runtime: lines '"<Task>-runtime": {' then '"<Run>": <num>,'
    pubtsv=$wd/pub.tsv
    awk -F'"' '/-runtime": \{/ { t = $2; sub(/-runtime$/, "", t); inr = 1; next }
      /-mem-mb": \{/ { t = $2; sub(/-mem-mb$/, "", t); inm = 1; next }
      /^ *\}/ { inr = 0; inm = 0; next }
      (inr || inm) && NF >= 3 { v = $3; gsub(/[:, ]/, "", v); if (v ~ /^[0-9.]+$/) print (inr ? "t" : "m") "\t" t "\t" $2 "\t" v }' "$pub" >"$pubtsv"
    runsfile=$wd/runs.txt
    awk '/"runs": \{/ { r = 1; next } r && /\}/ { exit } r { gsub(/[",:]/, " "); print $1, $NF }' "$pub" >"$runsfile"
  fi
  awk -v avsrc="$avsrc" -v pubtsv="$pubtsv" -v runsfile="${runsfile:-/dev/null}" -v notesfile="$notes" -F'\t' '
  function fmt(x) { return (x == "") ? "—" : sprintf("%.2f", x) }
  function mb(k) { return sprintf("%.1f", k / 1024) }
  FNR == 1 { file++; fname = FILENAME; sub(/.*\//, "", fname); sub(/\.tsv$/, "", fname) }
  /^#/ { if ($0 ~ /^# label: /) { l = $0; sub(/^# label: /, "", l); label[fname] = l } ; next }
  $1 == "task" { next }
  FILENAME == pubtsv { if ($1 == "t") pt[$2, $3] = $4; else pm[$2, $3] = $4; next }
  FILENAME == notesfile { if ($1 != "" && $1 !~ /^#/) note[$1] = $2; next }
  FILENAME == runsfile { if ($0 ~ / prod$/) { sub(/ prod$/, ""); prod[$0] = 1 }; next }
  file == 1 { order[++n] = $1; ast[$1] = $2; aw[$1] = $3; ar[$1] = $4; next }
  { lang = fname; langs[lang] = 1; st[lang, $1] = $2; w[lang, $1] = $3; r[lang, $1] = $4 }
  END {
    for (i = 1; i <= n; i++) {
      t = order[i]; bt[t] = ""; br[t] = ""; miss[t] = ""
      for (L in langs) {
        if (!((L, t) in st)) { miss[t] = miss[t] " " label[L] ": not run;"; continue }
        if (st[L, t] != "ok") { miss[t] = miss[t] " " label[L] ": " st[L, t] ";"; continue }
        if (bt[t] == "" || w[L, t] + 0 < bt[t] + 0) { bt[t] = w[L, t]; btl[t] = L }
        if (br[t] == "" || r[L, t] + 0 < br[t] + 0) { br[t] = r[L, t]; brl[t] = L }
      }
      ok = (ast[t] == "ok" && bt[t] != "")
      tm[t] = ok ? aw[t] / bt[t] : ""; mm[t] = ok ? ar[t] / br[t] : ""
      worst[t] = ok ? (tm[t] > mm[t] ? tm[t] : mm[t]) : -1
      cm[t] = (ok && st["crystal", t] == "ok") ? sprintf("%.2f / %.2f", aw[t] / w["crystal", t], ar[t] / r["crystal", t]) : "—"
      gm[t] = (ok && st["go", t] == "ok") ? sprintf("%.2f / %.2f", aw[t] / w["go", t], ar[t] / r["go", t]) : "—"
      est[t] = "—"
      if (pubtsv != "/dev/null" && st["c_clang", t] == "ok" && (t, "C/Clang") in pt && pt[t, "C/Clang"] > 0) {
        k = w["c_clang", t] / pt[t, "C/Clang"]; e = ""; el = ""
        for (key in pt) { split(key, kk, SUBSEP); if (kk[1] != t) continue
          rn = kk[2]; if (rn ~ /^(C\/|C\+\+\/|Rust|Go$|Crystal|JS\/Node)/) continue
          if (!(rn in prod)) continue
          if (e == "" || pt[key] < e) { e = pt[key]; el = rn } }
        if (e != "") est[t] = sprintf("%.2f %s (×%.2f)", e * k, el, ok ? aw[t] / (e * k) : 0)
      }
      if (ast[t] != "ok") miss[t] = miss[t] " Avant: " ast[t] ";"
      if (t in note) miss[t] = miss[t] " " note[t]
    }
    printf "Avant source: %s. Multiplier = Avant / best measured here; best excludes failed runs. Crystal and Go columns are Avant/Crystal and Avant/Go as time / memory.\n\n", (avsrc == "lr" ? "lr-avant.tsv (one wrapper for all)" : "harness runtime.tsv (ps sampling; not the same method as the references)")
    sec("Combined (worse of the two multipliers)", "both")
    sec("By time multiplier", "time")
    sec("By memory multiplier", "mem")
  }
  function sec(title, key,    i, j, x, idx, m, t, flag) {
    m = 0
    for (i = 1; i <= n; i++) idx[++m] = order[i]
    for (i = 2; i <= m; i++) { x = idx[i]; j = i - 1
      while (j >= 1 && val(idx[j], key) < val(x, key)) { idx[j + 1] = idx[j]; j-- }
      idx[j + 1] = x }
    printf "## %s\n\n", title
    printf "| # | Task | Avant s | Best s | Best (time) | Time × | Avant MB | Best MB | Best (mem) | Mem × | vs Crystal t/m | vs Go t/m | <0.2 s | Estimated best (published, scaled by C) | Notes |\n"
    printf "| ---: | --- | ---: | ---: | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | :---: | --- | --- |\n"
    for (i = 1; i <= m; i++) { t = idx[i]
      flag = (aw[t] != "" && (aw[t] < 0.2 || (bt[t] != "" && bt[t] < 0.2))) ? "yes" : ""
      printf "| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", i, t,
        (aw[t] ~ /^[0-9]/ ? sprintf("%.3f", aw[t]) : "—"), (bt[t] == "" ? "—" : sprintf("%.3f", bt[t])), (bt[t] == "" ? "—" : label[btl[t]]),
        fmt(tm[t]), (ar[t] ~ /^[0-9]/ ? mb(ar[t]) : "—"), (br[t] == "" ? "—" : mb(br[t])), (br[t] == "" ? "—" : label[brl[t]]),
        fmt(mm[t]), cm[t], gm[t], flag, est[t], miss[t] }
    printf "\n"
  }
  function val(t, key) { return key == "both" ? worst[t] : (key == "time" ? (tm[t] == "" ? -1 : tm[t]) : (mm[t] == "" ? -1 : mm[t])) }
  ' "$av" "$refdir"/*.tsv "$pubtsv" "${runsfile:-/dev/null}" "$notes"
  ;;
rounds)
  [ $# -ge 3 ] || die "rounds needs N RUN_JS OUT_DIR [OPTION...] [TASK...]"
  n=$1 runjs=$2 out=$3; shift 3
  need_runjs "$runjs"
  exec python3 -I "$(dirname "$0")/rounds.py" langarena --rounds "$n" --run-js "$runjs" --out "$out" "$@" ;;
*) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac
