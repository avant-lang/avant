#!/bin/sh
# lr-table.sh — LangArena per-task multiplier table.
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
#       reference binary). Writes OUT.tsv, and machine.txt beside it when
#       there is none (the CPU and thread count wave-final.sh checks).
#   lr-table.sh avant BIN_DIR RUN_JS OUT.tsv
#       Re-measure the harness-built Avant binaries (BIN_DIR/<slug>, as left by
#       `measure.cr -- prod` in langarena/tmp). Put OUT.tsv in the harness
#       results directory as lr-avant.tsv. When BIN_DIR holds library.tsv
#       (the harness's list of the library-backed tasks and of every
#       pure-Avant twin: task, library or "-", twin, twin_slug, kind,
#       compared), each twin's binary (BIN_DIR/<twin_slug>) is measured the
#       same way, checked against its task's checksum in RUN_JS, into
#       lr-avant-pure.tsv beside OUT.tsv (the same columns, the twin's name
#       in the first), and library.tsv is copied beside OUT.tsv.
#   lr-table.sh table REF_DIR AVANT [PUBLISHED.js]
#       Markdown table on stdout. REF_DIR holds one <lang>.tsv per reference
#       language, and optionally notes.txt ("Task<TAB>note" lines, shown in
#       the Notes column). AVANT is a harness results directory (uses lr-avant.tsv if
#       present, else the harness's runtime.tsv, labelled as such) or a tsv.
#       PUBLISHED.js (optional) is a LangArena results file; it adds an
#       "estimated best" column for prod runs of languages not measured here,
#       scaled per task by C/Clang's local (c_clang.tsv) / published ratio. Estimates never enter the measured best.
#       When a library.tsv is beside the Avant input (in the results
#       directory, or beside the tsv given), each library-backed row's Notes
#       gain "library: <what>;", the first paragraph says how many of the
#       rows rely on a library, and a line gives the "Avant as written" total
#       (the wall sum of Avant's ok rows and their average MB). The Task
#       cells are unchanged. Without library.tsv the output is as before.
#   lr-table.sh pure REF_DIR AVANT [PUBLISHED.js]
#       The same table for pure Avant: each task that has a common twin
#       (library.tsv's kind "common" or "common+clever", or no kind: the
#       majority's algorithm; its numbers in lr-avant-pure.tsv beside
#       lr-avant.tsv, or runtime-pure.tsv beside the harness's runtime.tsv)
#       is shown with its twin's numbers under its own task name, its Notes
#       naming the twin, its kind and the references whose algorithm it
#       writes; the other library-backed rows are noted "still on a
#       library: <what>;". The totals come first: "Avant as written",
#       "pure Avant", and pure Avant with the clever twins (kind "clever"
#       or "common+clever": the approach of the reference ranked first on
#       the task), each task with a clever twin counted by it and the
#       others as in pure Avant. Refused without library.tsv or without the
#       twins' tsv.
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
#   tools/lr-table.sh table REFERENCE_DIR $R > table.md
#   tools/lr-table.sh pure REFERENCE_DIR $R > table-pure.md
#
# TSV columns: task status wall_s rss_kb inprog_s exit  (status ok|err|fail),
# then (since the counters) user_s sys_s nvcsw nivcsw outside_cpus attempts
# undisturbed: `ref` and `avant` start each process under tools/rounds.py's
# spawner, inside the same timeout and GNU time (the wall and the peak RSS
# are read as before; the spawner adds about 0.3 ms and 1.4 MB, under every
# task's own), for the process's user and system CPU time, its voluntary
# and involuntary context switches, and the CPUs' worth of work outside it
# on the CPUs it may run on. A run the undisturbed rule (`python3
# tools/rounds.py rule`) calls disturbed is run again at once, at most
# LR_RETRIES times (default 3), every attempt kept in OUT.tsv.raw as
# SLUG.N.*; the row is the first undisturbed attempt, else the last one
# with `undisturbed` "no: REASON", which `table` names in the Notes column.
# Each tsv starts with "# key: value" lines (label, governor, date, method,
# rule).
#
# Every input is checked before anything is written:
# a RUN_JS that does not exist or lists no task, a BIN_DIR or DIR that is
# not a directory, a REF_DIR without a reference tsv, an AVANT that is
# neither a tsv nor a results directory holding lr-avant.tsv or
# runtime.tsv, or a PUBLISHED.js that does not exist stops the script
# with "lr-table.sh: ..." on stderr and status 1, and a table whose Avant
# input has no task row is refused the same way, as is a pure table
# without library.tsv or without its twins' tsv (or with no row in it). Before, a relative path
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
  echo "# method: process wall (date +%s%N) + /usr/bin/time %M; the task under tools/rounds.py's spawner for its counters"
  echo "# rule: $(python3 -I "$here/rounds.py" rule)"
  printf 'task\tstatus\twall_s\trss_kb\tinprog_s\texit\tuser_s\tsys_s\tnvcsw\tnivcsw\toutside_cpus\tattempts\tundisturbed\n'
}

# timed DIR RAW SLUG CMD...: one measured run of CMD from DIR, run again at
# once while the undisturbed rule calls it disturbed (tools/rounds.py
# judge; at most RETRIES more times). Each attempt's files are kept as
# RAW/SLUG.N.{out,err,time,res}; RAW/SLUG.{out,err,time} are the counted
# attempt's (the first undisturbed one, else the last). Sets rc, w (wall
# seconds), rss (KB), counters (the judge's fields: user_s sys_s nvcsw
# nivcsw outside_cpus), attempts and und (yes, or no: REASON).
timed() {
  t_dir=$1 t_raw=$2 t_slug=$3; shift 3
  attempts=0
  while :; do
    attempts=$((attempts + 1)); t_b=$t_raw/$t_slug.$attempts
    t0=$(date +%s%N)
    (cd "$t_dir" && timeout -k 10 900 /usr/bin/time -f '%M' -o "$t_b.time" "$spawn" "$t_b.res" "$@" </dev/null >"$t_b.out" 2>"$t_b.err")
    rc=$?; t1=$(date +%s%N)
    w=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.4f", (b - a) / 1e9 }')
    rss=$(tail -n 1 "$t_b.time" 2>/dev/null | tr -cd '0-9'); [ -n "$rss" ] || rss=0
    if [ -s "$t_b.res" ]; then
      j=$(python3 -I "$here/rounds.py" judge "$t_b.res")
      # the spawner's own status is 0; the task's is in its result
      [ $rc -eq 0 ] && rc=$(awk '{ s = $5; if (s % 256 == 0) print int(s / 256) % 256; else print 128 + s % 128 }' "$t_b.res")
    else
      j=$(printf -- '-\t-\t-\t-\t-\tno: no counters (the spawner did not finish)')
    fi
    counters=$(printf '%s' "$j" | cut -f1-5)
    und=$(printf '%s' "$j" | cut -f6)
    if [ "$und" = yes ] || [ $rc -ne 0 ] || [ $attempts -gt "$retries" ]; then break; fi
    echo "  disturbed ($und): run again" >&2
  done
  for t_x in out err time; do cp "$t_b.$t_x" "$t_raw/$t_slug.$t_x" 2>/dev/null; done
}

here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
retries=${LR_RETRIES:-3}
cmd=${1:-}; [ $# -gt 0 ] && shift
spawner() { # RAW: compiles the spawner into RAW once
  spawn=$(python3 -I "$here/rounds.py" spawner "$1") || die "cannot build the spawner (tools/rounds.py spawner)"
}
case "$cmd" in
ref)
  [ $# -ge 5 ] || die "ref needs LABEL DIR OUT.tsv RUN_JS CMD..."
  label=$1 dir=$2 out=$3 runjs=$4; shift 4
  need_dir DIR "$dir"; need_runjs "$runjs"
  raw="$out.raw"; mkdir -p "$raw"
  # the machine a reference is measured on, beside it once (tools/wave-final.sh
  # refuses a reference whose machine.txt names another CPU or thread count)
  m=$(dirname "$out")/machine.txt
  [ -f "$m" ] || {
    echo "date: $(date -u +%Y-%m-%d)"
    echo "cpu: $(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)"
    echo "threads: $(getconf _NPROCESSORS_ONLN)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)"
    echo "kernel: $(uname -r)"
  } > "$m"
  spawner "$raw"
  # the command's path as the spawner execs it (no PATH search)
  c=$1; shift
  case "$c" in /*) ;; */*) c=$(cd "$dir" && realpath "$c") ;; *) c=$(command -v "$c") || die "CMD $c: not found" ;; esac
  set -- "$c" "$@"
  header "$label" >"$out"
  tasks "$runjs" | while IFS="$(printf '\t')" read -r name sum; do
    slug=$(echo "$name" | tr 'A-Z' 'a-z' | sed 's/::/_/g')
    timed "$dir" "$raw" "$slug" "$@" "$runjs" "$name"
    line=$(grep -F "$name: " "$raw/$slug.out" | head -n 1)
    ip=$(echo "$line" | sed -n 's/.* in \([0-9.]*\)s$/\1/p'); [ -n "$ip" ] || ip=-
    case "$line" in *": OK "*) st=ok ;; *ERR*) st=err ;; *) st=fail ;; esac
    [ $rc -ne 0 ] && st=fail
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$st" "$w" "$rss" "$ip" "$rc" "$counters" "$attempts" "$und" >>"$out"
    echo "$label $name $st $w s $rss KB ($und)" >&2
  done ;;
avant)
  [ $# -eq 3 ] || die "avant needs BIN_DIR RUN_JS OUT.tsv"
  bindir=$1 runjs=$2 out=$3
  need_dir BIN_DIR "$bindir"; need_runjs "$runjs"
  raw="$out.raw"; mkdir -p "$raw"
  spawner "$raw"
  header "Avant" >"$out"
  tasks "$runjs" | while IFS="$(printf '\t')" read -r name sum; do
    slug=$(echo "$name" | tr 'A-Z' 'a-z' | sed 's/::/_/g')
    if [ ! -x "$bindir/$slug" ]; then
      printf '%s\tfail\t-\t-\t-\t-\n' "$name" >>"$out"; continue
    fi
    timed "$bindir" "$raw" "$slug" "./$slug"
    got=$(head -n 1 "$raw/$slug.out" | tr -d ' \r')
    if [ $rc -ne 0 ]; then st=fail; elif [ "$got" = "$sum" ]; then st=ok; else st=err; fi
    printf '%s\t%s\t%s\t%s\t-\t%s\t%s\t%s\t%s\n' "$name" "$st" "$w" "$rss" "$rc" "$counters" "$attempts" "$und" >>"$out"
    echo "Avant $name $st $w s $rss KB ($und)" >&2
  done
  # the pure-Avant twins the harness listed, each against its task's checksum
  lib=$bindir/library.tsv
  if [ -f "$lib" ]; then
    od=$(dirname "$out"); pout=$od/lr-avant-pure.tsv
    praw="$pout.raw"; mkdir -p "$praw"
    [ "$(realpath "$lib")" = "$(realpath -m "$od/library.tsv")" ] || cp "$lib" "$od/library.tsv"
    header "Avant pure" >"$pout"
    awk -F'\t' '$1 != "task" && $3 != "-" && $3 != "" { print $1 "\t" $3 "\t" $4 }' "$lib" |
    while IFS="$(printf '\t')" read -r task name slug; do
      sum=$(tasks "$runjs" | awk -F'\t' -v t="$task" '$1 == t { print $2; exit }')
      if [ ! -x "$bindir/$slug" ] || [ -z "$sum" ]; then
        printf '%s\tfail\t-\t-\t-\t-\n' "$name" >>"$pout"; continue
      fi
      timed "$bindir" "$praw" "$slug" "./$slug"
      got=$(head -n 1 "$praw/$slug.out" | tr -d ' \r')
      if [ $rc -ne 0 ]; then st=fail; elif [ "$got" = "$sum" ]; then st=ok; else st=err; fi
      printf '%s\t%s\t%s\t%s\t-\t%s\t%s\t%s\t%s\n' "$name" "$st" "$w" "$rss" "$rc" "$counters" "$attempts" "$und" >>"$pout"
      echo "Avant $name $st $w s $rss KB ($und)" >&2
    done
  fi ;;
table|pure)
  [ $# -ge 2 ] && [ $# -le 3 ] || die "$cmd needs REF_DIR AVANT [PUBLISHED.js]"
  refdir=$1 av=$2 pub=${3:-}
  need_dir REF_DIR "$refdir"
  ls "$refdir"/*.tsv >/dev/null 2>&1 || die "REF_DIR $refdir: no reference tsv"
  if [ -d "$av" ]; then
    [ -f "$av/lr-avant.tsv" ] || [ -f "$av/runtime.tsv" ] || die "AVANT $av: neither lr-avant.tsv nor runtime.tsv"
  else
    [ -f "$av" ] || die "AVANT $av: no such file or directory"
  fi
  [ -z "$pub" ] || [ -f "$pub" ] || die "PUBLISHED.js $pub: no such file"
  # the harness's library.tsv (library-backed tasks, their twins) beside the input
  if [ -d "$av" ]; then libf=$av/library.tsv; else libf=$(dirname "$av")/library.tsv; fi
  [ -f "$libf" ] || libf=/dev/null
  puref=/dev/null
  if [ "$cmd" = pure ]; then
    [ "$libf" != /dev/null ] || die "AVANT $av: no library.tsv beside it (the pure table needs the harness's list of twins)"
    if [ -d "$av" ]; then
      if [ -f "$av/lr-avant.tsv" ]; then puref=$av/lr-avant-pure.tsv; else puref=$av/runtime-pure.tsv; fi
    elif [ "$(basename "$av")" = runtime.tsv ]; then puref=$(dirname "$av")/runtime-pure.tsv
    else puref=$(dirname "$av")/lr-avant-pure.tsv
    fi
    [ -f "$puref" ] || die "AVANT $av: no twins' tsv $puref"
  fi
  avsrc=harness
  wd=$(mktemp -d); trap 'rm -rf "$wd"' EXIT
  # harness runtime.tsv: name ... checksum_ok ... wall_s rss_mb (by header)
  harness_tsv() { # RUNTIME.tsv OUT
    awk -F'\t' 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next }
      { st = ($h["checksum_ok"] == "true") ? "ok" : "err"
        printf "%s\t%s\t%s\t%d\t-\t%s\n", $h["name"], st, $h["wall_s"], $h["rss_mb"] * 1024, $h["exit_code"] }' "$1" >"$2"
  }
  if [ -d "$av" ]; then
    if [ -f "$av/lr-avant.tsv" ]; then av="$av/lr-avant.tsv"; avsrc=lr
    else
      tmp=$wd/avant.tsv; harness_tsv "$av/runtime.tsv" "$tmp"
      av=$tmp
    fi
  elif [ "$cmd" = pure ] && [ "$(basename "$av")" = runtime.tsv ]; then
    tmp=$wd/avant.tsv; harness_tsv "$av" "$tmp"; av=$tmp
  else
    avsrc=lr
  fi
  case "$puref" in */runtime-pure.tsv) harness_tsv "$puref" "$wd/pure.tsv"; puref=$wd/pure.tsv ;; esac
  [ "$(rows "$av")" -gt 0 ] || die "AVANT $av: no task row"
  [ "$puref" = /dev/null ] || [ "$(rows "$puref")" -gt 0 ] || die "AVANT: the twins' tsv has no row"
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
  awk -v avsrc="$avsrc" -v pubtsv="$pubtsv" -v runsfile="${runsfile:-/dev/null}" -v notesfile="$notes" -v mode="$cmd" -v libfile="$libf" -v purefile="$puref" -F'\t' '
  function fmt(x) { return (x == "") ? "—" : sprintf("%.2f", x) }
  function mb(k) { return sprintf("%.1f", k / 1024) }
  FNR == 1 { file++; fname = FILENAME; sub(/.*\//, "", fname); sub(/\.tsv$/, "", fname) }
  /^#/ { if ($0 ~ /^# label: /) { l = $0; sub(/^# label: /, "", l); label[fname] = l } ; next }
  $1 == "task" { next }
  FILENAME == pubtsv { if ($1 == "t") pt[$2, $3] = $4; else pm[$2, $3] = $4; next }
  FILENAME == notesfile { if ($1 != "" && $1 !~ /^#/) note[$1] = $2; next }
  FILENAME == runsfile { if ($0 ~ / prod$/) { sub(/ prod$/, ""); prod[$0] = 1 }; next }
  FILENAME == libfile { if ($1 != "") { if ($2 != "-" && $2 != "") lib[$1] = $2
      if ($3 != "-" && $3 != "") { kind[$3] = ($5 == "" ? "common" : $5); cmpd[$3] = $6
        if (kind[$3] ~ /^common/ && !($1 in twin)) twin[$1] = $3
        if (kind[$3] ~ /clever$/ && !($1 in clever)) clever[$1] = $3 } }; next }
  FILENAME == purefile { pst[$1] = $2; pw[$1] = $3; pr[$1] = $4; if ($13 ~ /^no/) pdist[$1] = " Avant: disturbed (" $12 " attempts);"; next }
  file == 1 { order[++n] = $1; ast[$1] = $2; aw[$1] = $3; ar[$1] = $4; if ($13 ~ /^no/) { adist[$1] = " Avant: disturbed (" $12 " attempts);"; dist[$1] = dist[$1] adist[$1] }; next }
  { lang = fname; langs[lang] = 1; st[lang, $1] = $2; w[lang, $1] = $3; r[lang, $1] = $4; if ($13 ~ /^no/) dist[$1] = dist[$1] " " lang ": disturbed;" }
  END {
    haslib = (libfile != "/dev/null")
    # the totals of Avant as written: its ok rows, then (pure) each task with
    # a twin replaced by the twin; nl counts the rows on a library
    nl = 0; ko = 0; ws = 0; ms = 0
    for (i = 1; i <= n; i++) { t = order[i]; if (t in lib) nl++; if (ast[t] == "ok") { ko++; ws += aw[t]; ms += ar[t] } }
    if (mode == "pure") {
      # the clever line first, from the numbers of the tasks: each task with a
      # clever twin that ran counts it, else its common twin, else itself
      cko = 0; cws = 0; cms = 0; nc = 0
      for (i = 1; i <= n; i++) { t = order[i]; ct = (t in clever) ? clever[t] : ((t in twin) ? twin[t] : "")
        if (ct != "" && (ct in pst)) { cs = pst[ct]; cw = pw[ct]; cr = pr[ct]; if (t in clever) nc++ } else { cs = ast[t]; cw = aw[t]; cr = ar[t] }
        if (cs == "ok") { cko++; cws += cw; cms += cr } }
      np = 0; npl = 0; pko = 0; pws = 0; pms = 0
      for (i = 1; i <= n; i++) { t = order[i]
        if ((t in twin) && (twin[t] in pst)) { tt = twin[t]; ast[t] = pst[tt]; aw[t] = pw[tt]; ar[t] = pr[tt]
          dist[t] = pdist[tt] substr(dist[t], length(adist[t]) + 1); rep[t] = 1; np++; if (t in lib) npl++ }
        if (ast[t] == "ok") { pko++; pws += aw[t]; pms += ar[t] } }
    }
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
      if (t in dist) miss[t] = miss[t] dist[t]
      if (t in rep) miss[t] = miss[t] " " kind[twin[t]] " twin " twin[t] ((t in lib) ? " of " lib[t] : "") ((cmpd[twin[t]] != "") ? ", the algorithm of " cmpd[twin[t]] : "") ";"
      if (mode == "pure" && (t in rep) && (t in clever) && clever[t] != twin[t]) miss[t] = miss[t] " clever twin " clever[t] ((cmpd[clever[t]] != "") ? ", the algorithm of " cmpd[clever[t]] : "") ": " ((clever[t] in pst) ? sprintf("%s %.3f s", pst[clever[t]], pw[clever[t]]) : "not run") ";"
      else if (mode == "pure" && !(t in rep) && (t in clever)) miss[t] = miss[t] " clever twin " clever[t] ((cmpd[clever[t]] != "") ? ", the algorithm of " cmpd[clever[t]] : "") ": " ((clever[t] in pst) ? sprintf("%s %.3f s", pst[clever[t]], pw[clever[t]]) : "not run") ";"
      if (!(t in rep) && (t in lib)) miss[t] = miss[t] (mode == "pure" ? " still on a library: " : " library: ") lib[t] ";"
    }
    src = (avsrc == "lr" ? "lr-avant.tsv (one wrapper for all)" : "harness runtime.tsv (its own wrapper; peak RSS by GNU time; not the same method as the references)")
    if (mode == "pure") {
      printf "Pure Avant: each task that has a common twin (%d of them) is shown with its twin'"'"'s numbers under its own name (Notes: the twin, its kind, what the task calls and the references whose algorithm the twin writes, and a clever twin'"'"'s time where the task has one); the other library-backed rows are still on a library.\n\n", np
      printf "Avant as written: wall sum %.3f s over %d ok rows, average %.1f MB. Pure Avant: wall sum %.3f s over %d ok rows, average %.1f MB. Pure Avant with the clever twins (%d of them, the approach of the reference ranked first where the references differ): wall sum %.3f s over %d ok rows, average %.1f MB.\n\n", ws, ko, (ko ? ms / ko / 1024 : 0), pws, pko, (pko ? pms / pko / 1024 : 0), nc, cws, cko, (cko ? cms / cko / 1024 : 0)
      printf "Avant source: %s, the twins from %s. %d of the %d rows still rely on a library (their Notes say which). Multiplier = Avant / best measured here; best excludes failed runs. Crystal and Go columns are Avant/Crystal and Avant/Go as time / memory.\n\n", src, (avsrc == "lr" ? "lr-avant-pure.tsv" : "harness runtime-pure.tsv"), nl - npl, n
    } else if (haslib) {
      printf "Avant source: %s. %d of the %d rows rely on a library (their Notes say which). Multiplier = Avant / best measured here; best excludes failed runs. Crystal and Go columns are Avant/Crystal and Avant/Go as time / memory.\n\n", src, nl, n
      printf "Avant as written: wall sum %.3f s over %d ok rows, average %.1f MB.\n\n", ws, ko, (ko ? ms / ko / 1024 : 0)
    } else
      printf "Avant source: %s. Multiplier = Avant / best measured here; best excludes failed runs. Crystal and Go columns are Avant/Crystal and Avant/Go as time / memory.\n\n", src
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
  ' "$av" "$refdir"/*.tsv "$pubtsv" "${runsfile:-/dev/null}" "$notes" "$libf" "$puref"
  ;;
rounds)
  [ $# -ge 3 ] || die "rounds needs N RUN_JS OUT_DIR [OPTION...] [TASK...]"
  n=$1 runjs=$2 out=$3; shift 3
  need_runjs "$runjs"
  exec python3 -I "$(dirname "$0")/rounds.py" langarena --rounds "$n" --run-js "$runjs" --out "$out" "$@" ;;
*) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac
