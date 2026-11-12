#!/bin/bash
# Runner for Valk's GC benchmark suite as ported to Avant (bench/valk-gc).
# New code, not a modification of upstream/run-gc.sh. README.md beside it
# has the method and the notes; this header is the short form.
#
#   run.sh build            build every program into $BUILD
#   run.sh check            run every row once, untimed; verification only
#   run.sh time [ROUNDS]    one unmeasured warm-up, then ROUNDS (default 7)
#                           fresh processes per row, rows in rotating order
#   run.sh counts           no clock: AVANT_GC_STATS and instruction counts
#                           per scenario (scenario-isolated builds), and the
#                           collection counts the other runtimes print
#   run.sh versions         the toolchains, the machine, the governor
#
# Tables: python3 tables.py $OUT (regenerates every table from raw files).
#
# Environment:
#   VALK_GC_BUILD   build directory            (default /tmp/valk-gc-build)
#   VALK_GC_OUT     raw per-run files          (default $BUILD/out)
#   AVANT_BENCH_COMPILER  the Avant compiler   (default <root>/bin/avant-c-llvm)
#   VALK            the Valk compiler          (default valk on PATH, else ~/.vman/bin/valk)
#   VALK_GC_SETS    which sets `time` runs     (default "headline variants iso")
#   VALK_GC_ROWS    restrict to these row ids  (default: all)
#   VALK_GC_ALLOW_GOVERNOR=1  let `time` run without the performance
#                   governor (a dry run; the session file says so)
#
# Every run: stdin from /dev/null, a timeout, the output capped, and the
# verification values of each scenario checked (a wrong value, a missing
# scenario, a non-zero exit or a timeout stops the session: such a run is
# not a result). Timed runs must be alone on the machine: nothing else
# running, no build, no test suite.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
AVANT_ROOT=$(CDPATH='' cd -- "$HERE/../.." && pwd)
BUILD=${VALK_GC_BUILD:-/tmp/valk-gc-build}
OUT=${VALK_GC_OUT:-$BUILD/out}
COMPILER=${AVANT_BENCH_COMPILER:-$AVANT_ROOT/bin/avant-c-llvm}
VALK=${VALK:-$(command -v valk || echo "$HOME/.vman/bin/valk")}
LIMIT=${VALK_GC_TIMEOUT:-180}
SETS=${VALK_GC_SETS:-headline variants iso}
ONLY_ROWS=${VALK_GC_ROWS:-}

# The rows. Headline: the seven scenarios in one process, as upstream ships
# them (Valk, Go, D, C#) or as ported (Avant, Crystal, Java). Variants: the
# labelled rows (README.md says what each differs in). Iso: the
# scenario-isolated builds, one scenario per process, plus scenario 8.
ROWS_headline="avant avant-v3 valk go d csharp crystal java"
ROWS_variants="avant-baseline avant-young avant-young-v3 avant-hints avant-hints-v3 valk-noregions go-plain-sink go-free-outside go-gogc-off d-minimize-outside csharp-notiered java-parallel"
ROWS_iso="avant avant-v3 valk go d csharp crystal java"
# One process per entry: the scenario numbers passed to an iso build.
# `3 4` is scenario 4 (it needs the chain scenario 3 builds).
ISO_UNITS=("1" "2" "3" "3 4" "5" "6" "7" "8")

say() { printf '%s\n' "$*"; }
die() { printf 'run.sh: %s\n' "$*" >&2; exit 1; }

want_row() {
  [ -z "$ONLY_ROWS" ] && return 0
  case " $ONLY_ROWS " in *" $1 "*) return 0 ;; esac
  return 1
}

# cmd_for SET ROW: sets CMD (the command) and ENVV (its environment).
cmd_for() {
  ENVV=()
  local b=$BUILD
  if [ "$1" = iso ]; then
    case "$2" in
      avant) CMD=("$b/gc-avant-iso") ;;
      avant-v3) CMD=("$b/gc-avant-iso-v3") ;;
      valk) CMD=("$b/gc-valk-iso") ;;
      go) CMD=("$b/gc-go-iso") ;;
      d) CMD=("$b/gc-d-iso") ;;
      csharp) CMD=(dotnet "$b/csharp-iso/gc-csharp.dll") ;;
      crystal) CMD=("$b/gc-cr-iso") ;;
      java) CMD=(java -cp "$b/java-iso" Main) ;;
      *) die "unknown iso row $2" ;;
    esac
    return
  fi
  case "$2" in
    avant) CMD=("$b/gc-avant") ;;
    avant-v3) CMD=("$b/gc-avant-v3") ;;
    avant-baseline) CMD=("$b/gc-avant-baseline") ;;
    avant-young) CMD=("$b/gc-avant-young") ;;
    avant-young-v3) CMD=("$b/gc-avant-young-v3") ;;
    avant-hints) CMD=("$b/gc-avant-hints") ;;
    avant-hints-v3) CMD=("$b/gc-avant-hints-v3") ;;
    valk) CMD=("$b/gc-valk") ;;
    valk-noregions) CMD=("$b/gc-valk-noregions") ;;
    go) CMD=("$b/gc-go") ;;
    go-plain-sink) CMD=("$b/gc-go-plain-sink") ;;
    go-free-outside) CMD=("$b/gc-go-free-outside") ;;
    go-gogc-off) ENVV=(GOGC=off); CMD=("$b/gc-go") ;;
    d) CMD=("$b/gc-d") ;;
    d-minimize-outside) CMD=("$b/gc-d-minimize-outside") ;;
    csharp) CMD=(dotnet "$b/csharp/gc-csharp.dll") ;;
    csharp-notiered) ENVV=(DOTNET_TieredCompilation=0); CMD=(dotnet "$b/csharp/gc-csharp.dll") ;;
    crystal) CMD=("$b/gc-cr") ;;
    java) CMD=(java -cp "$b/java" Main) ;;
    java-parallel) CMD=(java -XX:+UseParallelGC -cp "$b/java" Main) ;;
    *) die "unknown row $2" ;;
  esac
}

# ---------------------------------------------------------------- build

build_avant() { # SOURCE OUT FLAGS...
  local src=$1 out=$2
  shift 2
  (cd "$AVANT_ROOT" && "$COMPILER" compile "$src" "$out" "$@" </dev/null >/dev/null) || die "avant build failed: $src $*"
}

build_csharp() { # PROJECT OUTDIR TAG
  local fw
  fw="net$(dotnet --version | cut -d. -f1).0"
  # Upstream's command (run-gc.sh), with the build directories kept out of
  # the source directory.
  DOTNET_CLI_HOME="$BUILD/dotnet-home" DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    dotnet publish "$1" --configuration Release --output "$2" --nologo \
    -p:BaseIntermediateOutputPath="$BUILD/$3-obj/" -p:BaseOutputPath="$BUILD/$3-bin/" \
    -p:GcTargetFramework="$fw" -p:DisableTransitiveFrameworkReferenceDownloads=true \
    -p:RestoreIgnoreFailedSources=true -p:NuGetAudit=false </dev/null >"$BUILD/$3-build.log" 2>&1 || die "C# build failed: see $BUILD/$3-build.log"
}

do_build() {
  mkdir -p "$BUILD"
  local v=$HERE/variants u=$HERE/upstream
  say "building into $BUILD"
  # Avant: --release (the compiler's default level, x86-64-v2), and
  # --release --cpu=v3; one --cpu=baseline build of the headline program.
  build_avant "$HERE/main.av" "$BUILD/gc-avant" --release
  build_avant "$HERE/main.av" "$BUILD/gc-avant-v3" --release --cpu=v3
  build_avant "$HERE/main.av" "$BUILD/gc-avant-baseline" --release --cpu=baseline
  build_avant "$v/young/main.av" "$BUILD/gc-avant-young" --release
  build_avant "$v/young/main.av" "$BUILD/gc-avant-young-v3" --release --cpu=v3
  build_avant "$v/hints/main.av" "$BUILD/gc-avant-hints" --release
  build_avant "$v/hints/main.av" "$BUILD/gc-avant-hints-v3" --release --cpu=v3
  build_avant "$v/iso/main.av" "$BUILD/gc-avant-iso" --release
  build_avant "$v/iso/main.av" "$BUILD/gc-avant-iso-v3" --release --cpu=v3
  # Valk: upstream's flags (run-gc.sh: build --release), and --no-regions.
  "$VALK" build "$u/main.valk" --release -o "$BUILD/gc-valk" </dev/null >/dev/null || die "valk build failed"
  "$VALK" build "$u/main.valk" --release --no-regions -o "$BUILD/gc-valk-noregions" </dev/null >/dev/null || die "valk --no-regions build failed"
  "$VALK" build "$v/iso/main.valk" --release -o "$BUILD/gc-valk-iso" </dev/null >/dev/null || die "valk iso build failed"
  # Go: upstream's command (go build).
  go build -o "$BUILD/gc-go" "$u/main.go" </dev/null || die "go build failed"
  go build -o "$BUILD/gc-go-plain-sink" "$v/go-plain-sink/main.go" </dev/null || die "go build failed"
  go build -o "$BUILD/gc-go-free-outside" "$v/go-free-outside/main.go" </dev/null || die "go build failed"
  go build -o "$BUILD/gc-go-iso" "$v/iso/main.go" </dev/null || die "go build failed"
  # D: upstream's flags for LDC.
  ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d" "$u/main.d" </dev/null || die "ldc2 build failed"
  ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d-minimize-outside" "$v/d-minimize-outside/main.d" </dev/null || die "ldc2 build failed"
  ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d-iso" "$v/iso/main.d" </dev/null || die "ldc2 build failed"
  # C#: upstream's publish command.
  build_csharp "$u/gc-csharp.csproj" "$BUILD/csharp" csharp
  build_csharp "$v/iso/gc-csharp.csproj" "$BUILD/csharp-iso" csharp-iso
  # Crystal and Java: the new ports.
  CRYSTAL_CACHE_DIR="$BUILD/crystal-cache" crystal build --release -o "$BUILD/gc-cr" "$HERE/main.cr" </dev/null || die "crystal build failed"
  CRYSTAL_CACHE_DIR="$BUILD/crystal-cache" crystal build --release -o "$BUILD/gc-cr-iso" "$v/iso/main.cr" </dev/null || die "crystal build failed"
  mkdir -p "$BUILD/java" "$BUILD/java-iso"
  javac -d "$BUILD/java" "$HERE/Main.java" </dev/null || die "javac failed"
  javac -d "$BUILD/java-iso" "$v/iso/Main.java" </dev/null || die "javac failed"
  (cd "$BUILD" && sha256sum gc-avant* gc-valk* gc-go* gc-d gc-d-minimize-outside gc-d-iso gc-cr gc-cr-iso csharp/gc-csharp.dll csharp-iso/gc-csharp.dll java/*.class java-iso/*.class >"$BUILD/binaries.sha256")
  say "built: $(wc -l <"$BUILD/binaries.sha256") files hashed in $BUILD/binaries.sha256"
}

# ------------------------------------------------------------ one run

# verify_output FILE NUMBERS...: every scenario asked for is present once,
# with the verification value of gc.md section 6.2 (and OK for scenario 8).
verify_output() {
  local f=$1
  shift
  awk -v want="$*" '
    BEGIN {
      name[1] = "short-lived alloc"; expect[1] = "2"
      name[2] = "stable-heap forced collects"; expect[2] = "OK"
      name[3] = "build long-lived chain"; expect[3] = "OK"
      name[4] = "free long-lived chain"; expect[4] = "5000000"
      name[5] = "mutate live links"; expect[5] = "OK"
      name[6] = "short-lived churn with large live set"; expect[6] = "OK"
      name[7] = "tree churn"; expect[7] = "13106800"
      name[8] = "reverse live links"; expect[8] = "OK"
      for (i = 1; i <= 8; i++) num[name[i]] = i
      n = split(want, w, " ")
      for (i = 1; i <= n; i++) need[w[i]] = 1
      bad = 0
    }
    /^=== .* ===$/ {
      cur = substr($0, 5, length($0) - 8)
      if (cur == "done") { done = 1; cur = ""; next }
      if (!(cur in num)) { print "unknown scenario: " cur; bad = 1; cur = ""; next }
      seen[num[cur]]++
      next
    }
    /^time_us: / { if (cur != "") timed[num[cur]]++ }
    /^verify: / {
      if (cur == "") { print "verify outside a scenario"; bad = 1; next }
      k = num[cur]
      if ($2 != expect[k]) { print "scenario " k " (" cur "): verify " $2 ", expected " expect[k]; bad = 1 }
      verified[k]++
    }
    END {
      for (k in need) if (seen[k] != 1 || verified[k] != 1 || timed[k] != 1) { print "scenario " k ": missing or repeated"; bad = 1 }
      for (k in seen) if (!(k in need)) { print "scenario " k ": not asked for"; bad = 1 }
      if (!done) { print "no === done === line"; bad = 1 }
      exit bad
    }' "$f"
}

# run_one PREFIX WANT ARGS...: runs CMD (with ENVV) once. Writes
# PREFIX.out (stdout), PREFIX.err (stderr), PREFIX.time ("elapsed_s
# peak_rss_kb" from GNU time), PREFIX.wall_us (the launch to the exit, by
# the shell's microsecond clock), PREFIX.rc. Fails the session on a bad run.
run_one() {
  local prefix=$1 want=$2
  shift 2
  local t0 t1 rc
  t0=${EPOCHREALTIME/./}
  env "${ENVV[@]}" timeout -k 10 "$LIMIT" /usr/bin/time -f '%e %M' -o "$prefix.time" "${CMD[@]}" "$@" </dev/null 2>"$prefix.err" | head -c 50000000 >"$prefix.out"
  rc=${PIPESTATUS[0]}
  t1=${EPOCHREALTIME/./}
  echo "$rc" >"$prefix.rc"
  echo $((t1 - t0)) >"$prefix.wall_us"
  if [ "$rc" != 0 ]; then
    die "exit status $rc (124 is a timeout): ${ENVV[*]} ${CMD[*]} $* (see $prefix.err)"
  fi
  # shellcheck disable=SC2086
  if ! verify_output "$prefix.out" $want >"$prefix.verify" 2>&1; then
    cat "$prefix.verify" >&2
    die "verification failed: ${ENVV[*]} ${CMD[*]} $* (see $prefix.out)"
  fi
}

# units: prints "SET ROW UNIT" lines for the sets asked for; UNIT is `all`
# or an iso entry with its blank as `+` (`3+4`).
units() {
  local set row u
  for set in $SETS; do
    local rows_var=ROWS_$set
    for row in ${!rows_var}; do
      want_row "$row" || continue
      if [ "$set" = iso ]; then
        for u in "${ISO_UNITS[@]}"; do say "$set $row ${u// /+}"; done
      else
        say "$set $row all"
      fi
    done
  done
}

# run_unit DIR ROUND SET ROW UNIT
run_unit() {
  local dir=$1 round=$2 set=$3 row=$4 unit=$5
  cmd_for "$set" "$row"
  if [ "$set" = iso ]; then
    local nums=${unit//+/ } last=${unit##*+}
    mkdir -p "$dir/iso/$row/s$last"
    # The scenario of the unit is its last number; `3 4` also prints 3.
    # shellcheck disable=SC2086
    run_one "$dir/iso/$row/s$last/$round" "$nums" $nums
  else
    mkdir -p "$dir/$set/$row"
    run_one "$dir/$set/$row/$round" "1 2 3 4 5 6 7"
  fi
}

# ---------------------------------------------------------------- modes

governors() {
  cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s%s x%s", sep, $2, $1; sep = ", "} END {print ""}'
}

do_versions() {
  say "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  say "kernel: $(uname -sr)"
  say "cpu: $(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo | head -1) ($(nproc) threads)"
  say "governor: $(governors)"
  say "loadavg: $(cut -d' ' -f1-3 /proc/loadavg)"
  say "memory: $(free -m | awk '/^Mem:/ {print $2 " MB total, " $7 " MB available"}')"
  say "avant: $COMPILER sha256 $(sha256sum "$COMPILER" | cut -c1-16) (tree $(git -C "$AVANT_ROOT" rev-parse --short HEAD 2>/dev/null))"
  say "valk: $("$VALK" version 2>&1 | grep -v '^ *$' | grep -v -- '---' | head -1 | sed 's/^ *//') ($VALK)"
  say "go: $(go version) GOAMD64=$(go env GOAMD64)"
  say "ldc2: $(ldc2 --version | head -2 | tr '\n' ' ')"
  say "dotnet: SDK $(dotnet --version), runtime $(dotnet --list-runtimes | sed -n 's/^Microsoft.NETCore.App \([^ ]*\).*/\1/p' | tail -1)"
  say "crystal: $(crystal --version | head -1), $(crystal --version | sed -n 's/^LLVM: /LLVM /p')"
  say "java: $(java -version 2>&1 | head -1)"
  say "perf: $(perf --version 2>/dev/null)"
}

do_check() {
  local dir=$OUT/check
  rm -rf "$dir"
  mkdir -p "$dir"
  local set row unit
  while read -r set row unit; do
    run_unit "$dir" once "$set" "$row" "$unit"
    say "ok $set $row $unit"
  done < <(units)
  say "check: every row verified ($dir)"
}

do_time() {
  local rounds=${1:-7}
  local gov
  gov=$(governors)
  local dir=$OUT/raw
  if [ "$gov" != "performance x$(nproc)" ]; then
    [ "${VALK_GC_ALLOW_GOVERNOR:-0}" = 1 ] || die "the governor is not performance on every CPU ($gov): no timed run"
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  mapfile -t all < <(units)
  local n=${#all[@]}
  {
    do_versions
    say "rounds: $rounds (after one unmeasured warm-up round), ${n} processes a round, rotating order"
    say "sets: $SETS"
    [ -n "$ONLY_ROWS" ] && say "rows: $ONLY_ROWS"
    [ -n "${VALK_GC_NOTE:-}" ] && say "note: $VALK_GC_NOTE"
    say "commands:"
    local line set row unit
    for line in "${all[@]}"; do
      read -r set row unit <<<"$line"
      cmd_for "$set" "$row"
      if [ "$set" = iso ]; then
        say "  $set $row $unit: ${ENVV[*]} ${CMD[*]} ${unit//+/ }"
      else
        say "  $set $row: ${ENVV[*]} ${CMD[*]}"
      fi
    done
    say "binaries:"
    sed 's/^/  /' "$BUILD/binaries.sha256"
  } >"$dir/session.txt"
  local r i k line set row unit
  for ((r = 0; r <= rounds; r++)); do
    # Round 0 is the warm-up (kept under warmup/, never read by the
    # tables). Each round starts further into the list, and every other
    # round walks it backwards, so a row meets different positions and
    # both of its neighbours as predecessor.
    for ((i = 0; i < n; i++)); do
      k=$(((i + r * (n / (rounds + 1) + 1)) % n))
      [ $((r % 2)) = 0 ] || k=$((n - 1 - k))
      line=${all[$k]}
      read -r set row unit <<<"$line"
      if [ "$r" = 0 ]; then
        run_unit "$dir/warmup" "r0" "$set" "$row" "$unit"
      else
        run_unit "$dir" "r$r" "$set" "$row" "$unit"
      fi
    done
    say "round $r of $rounds done ($(date -u +%H:%M:%SZ))"
  done
  {
    say "end: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    say "governor at end: $(governors)"
    say "loadavg at end: $(cut -d' ' -f1-3 /proc/loadavg)"
  } >>"$dir/session.txt"
  say "raw files in $dir; tables: python3 $HERE/tables.py $OUT"
}

# counts: nothing here reads a clock. For each scenario, in its own
# process: Avant's AVANT_GC_STATS line and user-mode instructions (perf
# stat) for the three rows; user-mode instructions for the other
# ahead-of-time programs; and the collection counts Go, D, Crystal and
# Java print themselves.
do_counts() {
  local dir=$OUT/counts
  rm -rf "$dir"
  mkdir -p "$dir"
  local u nums tag mode
  perf_ins() { # PREFIX CMD...: writes PREFIX.ins (instructions:u)
    local prefix=$1
    shift
    timeout -k 10 "$LIMIT" perf stat -x, -e instructions:u -o "$prefix.perf" "$@" </dev/null >"$prefix.out" 2>"$prefix.err" || die "perf stat failed: $*"
    awk -F, '/instructions/ {print $1}' "$prefix.perf" >"$prefix.ins"
  }
  for u in "0" "${ISO_UNITS[@]}"; do
    nums=$u
    tag=s${u##* }
    for mode in full young hints; do
      mkdir -p "$dir/avant-$mode"
      local margs=()
      [ "$mode" = full ] || margs=("$mode")
      # shellcheck disable=SC2086
      AVANT_GC_STATS=1 timeout -k 10 "$LIMIT" "$BUILD/gc-avant-iso" "${margs[@]}" $nums </dev/null >"$dir/avant-$mode/$tag.out" 2>"$dir/avant-$mode/$tag.stats" || die "avant iso $mode $nums failed"
      # shellcheck disable=SC2086
      [ "$u" = 0 ] || verify_output "$dir/avant-$mode/$tag.out" $nums || die "avant iso $mode $nums: verification"
      # shellcheck disable=SC2086
      perf_ins "$dir/avant-$mode/$tag" "$BUILD/gc-avant-iso" "${margs[@]}" $nums
    done
    local row
    for row in avant-v3 valk go d crystal; do
      cmd_for iso "$row"
      mkdir -p "$dir/$row"
      # shellcheck disable=SC2086
      perf_ins "$dir/$row/$tag" "${CMD[@]}" $nums
    done
    # Collection counts printed by the other runtimes (stderr or stdout).
    mkdir -p "$dir/go-gctrace" "$dir/d-profile" "$dir/crystal-gcstats" "$dir/java-xlog" "$dir/java-parallel-xlog"
    # shellcheck disable=SC2086
    GODEBUG=gctrace=1 timeout -k 10 "$LIMIT" "$BUILD/gc-go-iso" $nums </dev/null >"$dir/go-gctrace/$tag.out" 2>"$dir/go-gctrace/$tag.err" || die "go gctrace $nums"
    # shellcheck disable=SC2086
    timeout -k 10 "$LIMIT" "$BUILD/gc-d-iso" "--DRT-gcopt=profile:1" $nums </dev/null >"$dir/d-profile/$tag.out" 2>"$dir/d-profile/$tag.err" || die "d profile $nums"
    # shellcheck disable=SC2086
    GC_PRINT_STATS=1 timeout -k 10 "$LIMIT" "$BUILD/gc-cr-iso" $nums </dev/null >"$dir/crystal-gcstats/$tag.out" 2>"$dir/crystal-gcstats/$tag.err" || die "crystal stats $nums"
    # shellcheck disable=SC2086
    timeout -k 10 "$LIMIT" java -Xlog:gc -cp "$BUILD/java-iso" Main $nums </dev/null >"$dir/java-xlog/$tag.out" 2>"$dir/java-xlog/$tag.err" || die "java xlog $nums"
    # shellcheck disable=SC2086
    timeout -k 10 "$LIMIT" java -XX:+UseParallelGC -Xlog:gc -cp "$BUILD/java-iso" Main $nums </dev/null >"$dir/java-parallel-xlog/$tag.out" 2>"$dir/java-parallel-xlog/$tag.err" || die "java parallel xlog $nums"
    say "counts: scenario unit '$u' done"
  done
  do_versions >"$dir/session.txt"
  say "counts in $dir; tables: python3 $HERE/tables.py $OUT"
}

case "${1:-}" in
  build) do_build ;;
  check) do_check ;;
  time) do_time "${2:-7}" ;;
  counts) do_counts ;;
  versions) do_versions ;;
  *) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
