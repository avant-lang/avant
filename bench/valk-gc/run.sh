#!/bin/bash
# Runner for Valk's GC benchmark suite as ported to Avant (bench/valk-gc).
# New code, not a modification of upstream/run-gc.sh. README.md beside it
# has the method and the notes; this header is the short form.
#
#   run.sh build [ROW...]   build every program into $BUILD; with rows, only
#                           what those rows use, binaries.sha256
#                           updated for them; avant-compiler.txt names the
#                           Avant compiler of the directory's Avant binaries
#   run.sh check            run every row once, untimed; verification only
#   run.sh time [ROUNDS]    one unmeasured warm-up, then ROUNDS (default 7)
#                           fresh processes per row, rows in rotating order
#   run.sh counts           no clock: AVANT_GC_STATS and instruction counts
#                           per scenario (scenario-isolated builds), and the
#                           collection counts the other runtimes print; each
#                           process in an emptied environment without
#                           address-space randomization (stable counts)
#   run.sh cg [N...]        no clock: exact instructions per function of the
#                           Avant scenario-isolated build under callgrind,
#                           exclusive and inclusive, one process per
#                           scenario (default every scenario; `4` runs 3
#                           then 4)
#   run.sh rounds [ROUNDS]  the loop of tools/rounds.py: as `time`,
#                           each run started by its spawner (wall and peak
#                           RSS of the program's own process), the sides
#                           of VALK_GC_SIDES in one session
#   run.sh trees-counts     no clock: the tree ports (set trees), each
#                           in its own process: AVANT_GC_STATS and
#                           instructions, in counts' fixed environment
#   run.sh versions         the toolchains, the machine, the governor
#
# Tables: python3 tables.py $OUT (regenerates every table from raw files).
#
# Environment:
#   VALK_GC_BUILD   build directory            (default /tmp/valk-gc-build)
#   VALK_GC_OUT     raw per-run files          (default $BUILD/out)
#   AVANT_BENCH_COMPILER  the Avant compiler   (default <root>/bin/avant-c-llvm)
#   VALK            the Valk compiler          (default valk on PATH, else ~/.vman/bin/valk)
#   VALK_GC_SETS    which sets `check` and `time` run (default "headline
#                   variants iso"; `trees` is the tree ports' set)
#   VALK_GC_ROWS    restrict to these row ids  (default: all)
#   VALK_GC_SIDES   `rounds`: "NAME=BUILD_DIR ..."  (default "this=$BUILD")
#   VALK_GC_UNITS   `rounds`: iso scenarios to run (default: all; 4 is 3 4)
#   VALK_GC_ALLOW_GOVERNOR=1  let `time` run without the performance
#                   governor (a dry run; the session file says so)
#   VALK_GC_CG_TIMEOUT  seconds one `cg` process may take (default 3600)
#   VALK_GC_CG_ROWS   how many functions each `cg` listing keeps (default 70)
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
# Trees: two LangArena tasks, each as LangArena builds it (with
# std.gc's hints) and without the hints (trees/, README.md). Not in the
# default sets or the full build (check, counts, cg and time read what
# they read before the set was added): name the
# set (VALK_GC_SETS=trees) and build it (run.sh build trees).
ROWS_trees="binarytrees-obj binarytrees-obj-unhinted calc-ast calc-ast-unhinted"
# The checksum each prints (LangArena's run.js, prod configuration).
tree_checksum() {
  case "$1" in
    binarytrees-obj*) say 75497436 ;;
    calc-ast*) say 1670614456 ;;
    *) die "unknown tree row $1" ;;
  esac
}

say() { printf '%s\n' "$*"; }
die() { printf 'run.sh: %s\n' "$*" >&2; exit 1; }

want_row() {
  [ -z "$ONLY_ROWS" ] && return 0
  case " $ONLY_ROWS " in *" $1 "*) return 0 ;; esac
  return 1
}

# want_unit N: VALK_GC_UNITS (scenario numbers, `4` the unit `3 4`)
# restricts the iso entries; unset, every entry. Only `rounds` reads it;
# `time` and `check` are as they were before it was added.
want_unit() {
  [ -z "${UNITS_FILTER:-}" ] && return 0
  case " $UNITS_FILTER " in *" $1 "*) return 0 ;; esac
  return 1
}

# cmd_for SET ROW: sets CMD (the command) and ENVV (its environment).
cmd_for() {
  ENVV=()
  local b=$BUILD
  if [ "$1" = trees ]; then
    tree_checksum "$2" >/dev/null
    CMD=("$b/trees/$2")
    return
  fi
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
  # the source directory, and no build server left running after it: by
  # default MSBuild's worker nodes and the Roslyn compiler server stay
  # about ten minutes after a publish: under `ledger run` they held the
  # machine lock that long (529 s after a full build), and a server that
  # inherited build_row's descriptor 3 kept `build ROW` waiting for it.
  DOTNET_CLI_HOME="$BUILD/dotnet-home" DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_USE_MSBUILD_SERVER=0 \
    dotnet publish "$1" --configuration Release --output "$2" --nologo -nodeReuse:false -p:UseSharedCompilation=false \
    -p:BaseIntermediateOutputPath="$BUILD/$3-obj/" -p:BaseOutputPath="$BUILD/$3-bin/" \
    -p:GcTargetFramework="$fw" -p:DisableTransitiveFrameworkReferenceDownloads=true \
    -p:RestoreIgnoreFailedSources=true -p:NuGetAudit=false </dev/null >"$BUILD/$3-build.log" 2>&1 3>&- || die "C# build failed: see $BUILD/$3-build.log"
}

emit() { printf '%s\n' "$*" >&3; }

# build_row ROW: builds every binary ROW uses in any set (the headline,
# variants and iso rows of one id share their build: `avant` is gc-avant
# and gc-avant-iso). Writes the built paths, relative to $BUILD, to file
# descriptor 3; the build tools' own output goes to standard error.
build_row() {
  local v=$HERE/variants u=$HERE/upstream
  case "$1" in
    # Avant: --release (the compiler's default level, x86-64-v2), and
    # --release --cpu=v3; one --cpu=baseline build of the headline program.
    avant)
      build_avant "$HERE/main.av" "$BUILD/gc-avant" --release
      build_avant "$v/iso/main.av" "$BUILD/gc-avant-iso" --release
      emit gc-avant gc-avant-iso ;;
    avant-v3)
      build_avant "$HERE/main.av" "$BUILD/gc-avant-v3" --release --cpu=v3
      build_avant "$v/iso/main.av" "$BUILD/gc-avant-iso-v3" --release --cpu=v3
      emit gc-avant-v3 gc-avant-iso-v3 ;;
    avant-baseline) build_avant "$HERE/main.av" "$BUILD/gc-avant-baseline" --release --cpu=baseline; emit gc-avant-baseline ;;
    avant-young) build_avant "$v/young/main.av" "$BUILD/gc-avant-young" --release; emit gc-avant-young ;;
    avant-young-v3) build_avant "$v/young/main.av" "$BUILD/gc-avant-young-v3" --release --cpu=v3; emit gc-avant-young-v3 ;;
    avant-hints) build_avant "$v/hints/main.av" "$BUILD/gc-avant-hints" --release; emit gc-avant-hints ;;
    avant-hints-v3) build_avant "$v/hints/main.av" "$BUILD/gc-avant-hints-v3" --release --cpu=v3; emit gc-avant-hints-v3 ;;
    # Valk: upstream's flags (run-gc.sh: build --release), and --no-regions.
    valk)
      "$VALK" build "$u/main.valk" --release -o "$BUILD/gc-valk" </dev/null >/dev/null || die "valk build failed"
      "$VALK" build "$v/iso/main.valk" --release -o "$BUILD/gc-valk-iso" </dev/null >/dev/null || die "valk iso build failed"
      emit gc-valk gc-valk-iso ;;
    valk-noregions)
      "$VALK" build "$u/main.valk" --release --no-regions -o "$BUILD/gc-valk-noregions" </dev/null >/dev/null || die "valk --no-regions build failed"
      emit gc-valk-noregions ;;
    # Go: upstream's command (go build).
    go | go-gogc-off)
      go build -o "$BUILD/gc-go" "$u/main.go" </dev/null || die "go build failed"
      if [ "$1" = go ]; then
        go build -o "$BUILD/gc-go-iso" "$v/iso/main.go" </dev/null || die "go build failed"
        emit gc-go gc-go-iso
      else
        emit gc-go
      fi ;;
    go-plain-sink) go build -o "$BUILD/gc-go-plain-sink" "$v/go-plain-sink/main.go" </dev/null || die "go build failed"; emit gc-go-plain-sink ;;
    go-free-outside) go build -o "$BUILD/gc-go-free-outside" "$v/go-free-outside/main.go" </dev/null || die "go build failed"; emit gc-go-free-outside ;;
    # D: upstream's flags for LDC.
    d)
      ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d" "$u/main.d" </dev/null || die "ldc2 build failed"
      ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d-iso" "$v/iso/main.d" </dev/null || die "ldc2 build failed"
      emit gc-d gc-d-iso ;;
    d-minimize-outside) ldc2 -O3 -release -boundscheck=off -of="$BUILD/gc-d-minimize-outside" "$v/d-minimize-outside/main.d" </dev/null || die "ldc2 build failed"; emit gc-d-minimize-outside ;;
    # C#: upstream's publish command.
    csharp | csharp-notiered)
      build_csharp "$u/gc-csharp.csproj" "$BUILD/csharp" csharp
      if [ "$1" = csharp ]; then
        build_csharp "$v/iso/gc-csharp.csproj" "$BUILD/csharp-iso" csharp-iso
        emit csharp/gc-csharp.dll csharp-iso/gc-csharp.dll
      else
        emit csharp/gc-csharp.dll
      fi ;;
    # Crystal and Java: the new ports.
    crystal)
      CRYSTAL_CACHE_DIR="$BUILD/crystal-cache" crystal build --release -o "$BUILD/gc-cr" "$HERE/main.cr" </dev/null || die "crystal build failed"
      CRYSTAL_CACHE_DIR="$BUILD/crystal-cache" crystal build --release -o "$BUILD/gc-cr-iso" "$v/iso/main.cr" </dev/null || die "crystal build failed"
      emit gc-cr gc-cr-iso ;;
    java | java-parallel)
      mkdir -p "$BUILD/java"
      javac -d "$BUILD/java" "$HERE/Main.java" </dev/null || die "javac failed"
      (cd "$BUILD" && ls java/*.class) >&3
      if [ "$1" = java ]; then
        mkdir -p "$BUILD/java-iso"
        javac -d "$BUILD/java-iso" "$v/iso/Main.java" </dev/null || die "javac failed"
        (cd "$BUILD" && ls java-iso/*.class) >&3
      fi ;;
    # Trees: stitched as LangArena's harness stitches a task (the
    # helper, the task's library, its configuration, the task; each file
    # after a "// file:" line), built with the harness's flags (--release
    # --cpu=v3).
    trees)
      local t
      for t in $ROWS_trees; do build_row "$t"; done ;;
    binarytrees-obj | binarytrees-obj-unhinted | calc-ast | calc-ast-unhinted)
      local src=${1//-/_} parts=(helper.av) f
      case "$1" in
        binarytrees-obj*) parts+=(cfg_binarytrees_obj.av) ;;
        calc-ast*) parts+=(calc_lib.av cfg_calc_ast.av) ;;
      esac
      parts+=("$src.av")
      mkdir -p "$BUILD/trees"
      for f in "${parts[@]}"; do
        printf '// file: bench/valk-gc/trees/%s\n' "$f"
        cat "$HERE/trees/$f"
        printf '\n'
      done >"$BUILD/trees/$1.av"
      build_avant "$BUILD/trees/$1.av" "$BUILD/trees/$1" --release --cpu=v3
      emit "trees/$1" ;;
    *) die "build: unknown row $1" ;;
  esac
}

# every_row: the row ids of every set, each once, in the sets' order.
every_row() {
  local set row
  for set in headline variants iso; do
    local rows_var=ROWS_$set
    for row in ${!rows_var}; do say "$row"; done
  done | awk '!seen[$0]++'
}

# note_compiler: the Avant compiler a build directory's Avant binaries
# came from, so two build directories (a base and a final
# compiler) each say which they hold.
note_compiler() {
  {
    say "compiler: $COMPILER"
    say "sha256: $(sha256sum "$COMPILER" | cut -d' ' -f1)"
    say "checkout: $AVANT_ROOT HEAD $(git -C "$AVANT_ROOT" rev-parse HEAD 2>/dev/null) ($(git -C "$AVANT_ROOT" status --porcelain 2>/dev/null | wc -l) changed paths)"
    [ -f "$COMPILER.versions.txt" ] && sed 's/^/versions: /' "$COMPILER.versions.txt"
    say "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >"$BUILD/avant-compiler.txt"
}

# do_build [ROW...]: with no row, every program of the headline, variants
# and iso sets, and binaries.sha256 rewritten; with rows, only what
# those rows use, their lines of binaries.sha256 replaced and the others
# kept, so a base and a final build directory can hold two rows each.
do_build() {
  mkdir -p "$BUILD"
  local rows=("$@") all=0 row
  if [ ${#rows[@]} = 0 ]; then
    all=1
    mapfile -t rows < <(every_row)
  fi
  say "building into $BUILD"
  case " ${rows[*]} " in *" avant"* | *" trees"* | *" binarytrees-obj"* | *" calc-ast"*) note_compiler ;; esac
  local built=()
  for row in "${rows[@]}"; do
    local files
    files=$(build_row "$row" 3>&1 1>&2) || exit 1
    # shellcheck disable=SC2206
    built+=($files)
  done
  if [ "$all" = 1 ]; then
    (cd "$BUILD" && sha256sum gc-avant* gc-valk* gc-go* gc-d gc-d-minimize-outside gc-d-iso gc-cr gc-cr-iso csharp/gc-csharp.dll csharp-iso/gc-csharp.dll java/*.class java-iso/*.class >"$BUILD/binaries.sha256")
    say "built: $(wc -l <"$BUILD/binaries.sha256") files hashed in $BUILD/binaries.sha256"
    return
  fi
  local new
  new=$(cd "$BUILD" && printf '%s\n' "${built[@]}" | awk '!seen[$0]++' | xargs sha256sum) || die "build: cannot hash ${built[*]}"
  touch "$BUILD/binaries.sha256"
  # Replace the lines of the paths just built (in place), append new ones.
  awk -v new="$new" '
    BEGIN { n = split(new, l, "\n"); for (i = 1; i <= n; i++) { p = substr(l[i], 67); line[p] = l[i]; order[i] = p } }
    { p = substr($0, 67); if (p in line) { print line[p]; done[p] = 1 } else print }
    END { for (i = 1; i <= n; i++) if (!(order[i] in done)) print line[order[i]] }
  ' "$BUILD/binaries.sha256" >"$BUILD/binaries.sha256.new" && mv "$BUILD/binaries.sha256.new" "$BUILD/binaries.sha256"
  say "built: ${#built[@]} files for rows ${rows[*]}; their lines of $BUILD/binaries.sha256 replaced ($(wc -l <"$BUILD/binaries.sha256") lines)"
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
  case "$want" in
    checksum=*)
      # A tree port prints its checksum and nothing else.
      if [ "$(cat "$prefix.out")" != "${want#checksum=}" ]; then
        printf 'output %s, expected the checksum %s\n' "$(head -c 200 "$prefix.out")" "${want#checksum=}" | tee "$prefix.verify" >&2
        die "verification failed: ${ENVV[*]} ${CMD[*]} $* (see $prefix.out)"
      fi
      echo ok >"$prefix.verify"
      return ;;
  esac
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
        for u in "${ISO_UNITS[@]}"; do
          want_unit "${u##* }" || continue
          say "$set $row ${u// /+}"
        done
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
  if [ "$set" = trees ]; then
    mkdir -p "$dir/trees/$row"
    run_one "$dir/trees/$row/$round" "checksum=$(tree_checksum "$row")"
    return
  fi
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

# all_performance: true when every CPU has a governor file and each reads
# performance.
all_performance() {
  [ "$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u)" = performance ]
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
  # Every CPU's governor file, counted: not against nproc, which
  # inside a timed slot (numactl --cpunodebind) is one node's CPUs.
  if ! all_performance; then
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

# rounds: the timing loop of tools/rounds.py, through measure.py,
# over the units `time` runs (VALK_GC_SETS, VALK_GC_ROWS, and
# VALK_GC_UNITS for the iso entries), for every side of VALK_GC_SIDES
# ("NAME=BUILD_DIR ...", default "this=$BUILD"): a base and a final build
# directory are timed in one session, the side that goes first rotating
# inside each unit. Writes $OUT/rounds (removed first): session.txt,
# raw/round-R/SIDE--SET--ROW--UNIT.run and .out, sides/NAME.sha256 and
# NAME.compiler.txt. `time` stays as it was before `rounds` was added.
do_rounds() {
  local rounds=${1:-7} dir=$OUT/rounds sides=${VALK_GC_SIDES:-this=$BUILD}
  case "$rounds" in '' | *[!0-9]* | 0) die "rounds: ROUNDS must be a positive number" ;; esac
  local side name bdir line set row unit want saved=$BUILD
  # The sides first: a bad one stops before the last session is removed.
  for side in $sides; do
    name=${side%%=*}
    case "$name" in '' | *[!A-Za-z0-9_-]*) die "rounds: side name '$name': letters, digits, - and _ only" ;; esac
    [ "$name" != "$side" ] && [ -d "${side#*=}" ] || die "rounds: side '$side' is not NAME=BUILD_DIR with a directory"
  done
  mkdir -p "$OUT"
  rm -rf "$dir" "$OUT/rounds.rows.tsv" "$OUT/rounds.head.txt"
  mapfile -t all < <(units)
  [ ${#all[@]} -gt 0 ] || die "rounds: no unit to run (VALK_GC_SETS, VALK_GC_ROWS, VALK_GC_UNITS)"
  {
    do_versions
    say "sets: $SETS"
    [ -n "$ONLY_ROWS" ] && say "rows: $ONLY_ROWS"
    [ -n "${VALK_GC_UNITS:-}" ] && say "iso units: $VALK_GC_UNITS"
    [ -n "${VALK_GC_NOTE:-}" ] && say "note: $VALK_GC_NOTE"
    say "sides: $sides"
  } >"$OUT/rounds.head.txt"
  for side in $sides; do
    name=${side%%=*}
    bdir=${side#*=}
    bdir=$(CDPATH='' cd -- "$bdir" && pwd)
    say "side $name: $bdir" >>"$OUT/rounds.head.txt"
    [ -f "$bdir/avant-compiler.txt" ] && sed "s/^/side $name avant-/" "$bdir/avant-compiler.txt" >>"$OUT/rounds.head.txt"
    BUILD=$bdir
    for line in "${all[@]}"; do
      read -r set row unit <<<"$line"
      cmd_for "$set" "$row"
      local args=()
      case "$set" in
        trees) want="checksum=$(tree_checksum "$row")"; unit=- ;;
        iso) want="scenarios ${unit//+/ }"; read -ra args <<<"${unit//+/ }" ;;
        *) want="scenarios 1 2 3 4 5 6 7" ;;
      esac
      local envs=${ENVV[*]:-}
      printf '%s\t%s\t%s\t%s\t%s\t%s' "$name" "$set" "$row" "$unit" "$want" "${envs:--}"
      printf '\t%s' "${CMD[@]}" "${args[@]}"
      printf '\n'
    done >>"$OUT/rounds.rows.tsv"
  done
  BUILD=$saved
  say "commands:" >>"$OUT/rounds.head.txt"
  sed 's/\t/ /g; s/^/  /' "$OUT/rounds.rows.tsv" >>"$OUT/rounds.head.txt"
  python3 -I "$HERE/measure.py" rounds "$rounds" "$OUT/rounds.rows.tsv" "$OUT/rounds.head.txt" "$dir" </dev/null || die "rounds: the session stopped (see above)"
  mkdir -p "$dir/sides"
  for side in $sides; do
    name=${side%%=*}
    bdir=${side#*=}
    [ -f "$bdir/binaries.sha256" ] && cp "$bdir/binaries.sha256" "$dir/sides/$name.sha256"
    [ -f "$bdir/avant-compiler.txt" ] && cp "$bdir/avant-compiler.txt" "$dir/sides/$name.compiler.txt"
  done
  mv "$OUT/rounds.rows.tsv" "$OUT/rounds.head.txt" "$dir/"
  say "rounds in $dir; tables: python3 $HERE/tables.py $OUT"
}

# counts: nothing here reads a clock. For each scenario, in its own
# process: Avant's AVANT_GC_STATS line and user-mode instructions (perf
# stat) for the three rows; user-mode instructions for the other
# ahead-of-time programs; and the collection counts Go, D, Crystal and
# Java print themselves.
#
# Every counted process runs in a fixed environment:
# emptied (env -i, PATH and HOME kept, then the variables a row needs)
# and without address-space randomization (setarch -R). Measured on the
# Avant iso program, 10 runs each: with the
# caller's environment and randomization a count spread over about 2,900
# instructions (2,884 at unit 0, 2,875 at scenario 5, 2,541 at scenario
# 1), and it read about 14,500 higher than in an empty environment (the
# runtime's getenv calls walk every variable: about 300 instructions per
# variable, nothing per byte of a value); with both fixed the spread is
# 2, 10 and 31. The randomization was the spread, the environment the
# level; the executable's path moves the level too (by up to about 1,700
# on scenario 5, the stack's layout): so two counts compare only when both
# ran here, from one $BUILD.
fixed_env() { # VAR=VALUE... CMD...: CMD in the fixed environment
  env -i PATH="$PATH" HOME="$HOME" "$@"
}
do_counts() {
  command -v setarch >/dev/null || die "counts needs setarch (util-linux) to turn off address-space randomization"
  local dir=$OUT/counts
  rm -rf "$dir"
  mkdir -p "$dir"
  local u nums tag mode
  perf_ins() { # PREFIX CMD...: writes PREFIX.ins (instructions:u)
    local prefix=$1
    shift
    fixed_env setarch -R timeout -k 10 "$LIMIT" perf stat -x, -e instructions:u -o "$prefix.perf" "$@" </dev/null >"$prefix.out" 2>"$prefix.err" || die "perf stat failed: $*"
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
      fixed_env AVANT_GC_STATS=1 setarch -R timeout -k 10 "$LIMIT" "$BUILD/gc-avant-iso" "${margs[@]}" $nums </dev/null >"$dir/avant-$mode/$tag.out" 2>"$dir/avant-$mode/$tag.stats" || die "avant iso $mode $nums failed"
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
    fixed_env GODEBUG=gctrace=1 setarch -R timeout -k 10 "$LIMIT" "$BUILD/gc-go-iso" $nums </dev/null >"$dir/go-gctrace/$tag.out" 2>"$dir/go-gctrace/$tag.err" || die "go gctrace $nums"
    # shellcheck disable=SC2086
    fixed_env setarch -R timeout -k 10 "$LIMIT" "$BUILD/gc-d-iso" "--DRT-gcopt=profile:1" $nums </dev/null >"$dir/d-profile/$tag.out" 2>"$dir/d-profile/$tag.err" || die "d profile $nums"
    # shellcheck disable=SC2086
    fixed_env GC_PRINT_STATS=1 setarch -R timeout -k 10 "$LIMIT" "$BUILD/gc-cr-iso" $nums </dev/null >"$dir/crystal-gcstats/$tag.out" 2>"$dir/crystal-gcstats/$tag.err" || die "crystal stats $nums"
    # shellcheck disable=SC2086
    timeout -k 10 "$LIMIT" java -Xlog:gc -cp "$BUILD/java-iso" Main $nums </dev/null >"$dir/java-xlog/$tag.out" 2>"$dir/java-xlog/$tag.err" || die "java xlog $nums"
    # shellcheck disable=SC2086
    timeout -k 10 "$LIMIT" java -XX:+UseParallelGC -Xlog:gc -cp "$BUILD/java-iso" Main $nums </dev/null >"$dir/java-parallel-xlog/$tag.out" 2>"$dir/java-parallel-xlog/$tag.err" || die "java parallel xlog $nums"
    say "counts: scenario unit '$u' done"
  done
  do_versions >"$dir/session.txt"
  say "counts in $dir; tables: python3 $HERE/tables.py $OUT"
}

# trees-counts: no clock. Each tree row in its own process, in
# counts' fixed environment (fixed_env, setarch -R): its AVANT_GC_STATS=1
# line, its checksum checked, then its user-mode instructions (perf stat
# -e instructions:u) in a second process. Written under $OUT/trees-counts:
# ROW.out, ROW.stats, ROW.ins (and ROW.perf, ROW.err), session.txt.
do_trees_counts() {
  command -v setarch >/dev/null || die "trees-counts needs setarch (util-linux) to turn off address-space randomization"
  local dir=$OUT/trees-counts row
  rm -rf "$dir"
  mkdir -p "$dir"
  for row in $ROWS_trees; do
    want_row "$row" || continue
    [ -x "$BUILD/trees/$row" ] || die "no $BUILD/trees/$row (run.sh build trees)"
    fixed_env AVANT_GC_STATS=1 setarch -R timeout -k 10 "$LIMIT" "$BUILD/trees/$row" </dev/null >"$dir/$row.out" 2>"$dir/$row.stats" || die "trees-counts: $row failed"
    [ "$(cat "$dir/$row.out")" = "$(tree_checksum "$row")" ] || die "trees-counts: $row printed $(head -c 200 "$dir/$row.out"), not its checksum"
    fixed_env setarch -R timeout -k 10 "$LIMIT" perf stat -x, -e instructions:u -o "$dir/$row.perf" "$BUILD/trees/$row" </dev/null >"$dir/$row.perf-out" 2>"$dir/$row.err" || die "perf stat failed: $row"
    cmp -s "$dir/$row.out" "$dir/$row.perf-out" || die "trees-counts: $row printed another output under perf"
    rm -f "$dir/$row.perf-out"
    awk -F, '/instructions/ {print $1}' "$dir/$row.perf" >"$dir/$row.ins"
    say "trees-counts: $row: $(cat "$dir/$row.ins") instructions; $(grep '^avant gc:' "$dir/$row.stats" | head -1)"
  done
  { do_versions; say "binaries:"; (cd "$BUILD" && find trees -maxdepth 1 -type f ! -name '*.av' | sort | xargs sha256sum | sed 's/^/  /'); } >"$dir/session.txt"
  say "trees-counts in $dir; tables: python3 $HERE/tables.py $OUT"
}

# cg: nothing here reads a clock either. Each scenario asked for (default
# all eight; 4 runs 3 then 4) runs once, in its own process, under
# `valgrind --tool=callgrind`, which counts every instruction the process
# executes (event Ir) and attributes it to its function exactly: a sampled
# profile attributes a sample to the instruction that waits, so it
# over-counts the functions that take cache misses. Written per scenario
# under $OUT/cg: sN.excl.txt (each function's own instructions), sN.incl.txt
# (with its callees'), sN.total (the process), sN.out (the program's
# output, verified). The raw callgrind file is removed after the two
# listings are made. AVANT_HEAP_RESERVE_GB=2 keeps the address range the
# runtime reserves small enough for valgrind; it changes no count. The
# process runs in counts' emptied environment (fixed_env): callgrind
# repeats exactly from run to run, but its count moves with the number of
# variables the runtime's getenv calls walk.
do_cg() {
  command -v valgrind >/dev/null && command -v callgrind_annotate >/dev/null || die "cg needs valgrind and callgrind_annotate"
  [ -x "$BUILD/gc-avant-iso" ] || die "no $BUILD/gc-avant-iso (run.sh build)"
  local dir=$OUT/cg limit=${VALK_GC_CG_TIMEOUT:-3600} rows=${VALK_GC_CG_ROWS:-70}
  mkdir -p "$dir"
  local wanted=("$@") n nums tag
  [ ${#wanted[@]} -gt 0 ] || wanted=(1 2 3 4 5 6 7 8)
  for n in "${wanted[@]}"; do
    case "$n" in
      [1235678]) nums=$n ;;
      4) nums="3 4" ;;
      *) die "cg: unknown scenario $n (1 to 8)" ;;
    esac
    tag=s$n
    rm -f "$dir/$tag".*
    # shellcheck disable=SC2086
    fixed_env AVANT_HEAP_RESERVE_GB=2 timeout -k 10 "$limit" valgrind --tool=callgrind --callgrind-out-file="$dir/$tag.callgrind" "$BUILD/gc-avant-iso" $nums </dev/null 2>"$dir/$tag.err" | head -c 50000000 >"$dir/$tag.out"
    [ "${PIPESTATUS[0]}" = 0 ] || die "cg: scenario $n failed or timed out (see $dir/$tag.err)"
    # shellcheck disable=SC2086
    verify_output "$dir/$tag.out" $nums || die "cg: scenario $n: verification"
    callgrind_annotate --threshold=99.9 "$dir/$tag.callgrind" 2>/dev/null | sed -n '/PROGRAM TOTALS/,$p' | sed 's# \[/[^]]*\]##; s#???:##' | head -n "$rows" >"$dir/$tag.excl.txt"
    callgrind_annotate --threshold=99.9 --inclusive=yes "$dir/$tag.callgrind" 2>/dev/null | sed -n '/PROGRAM TOTALS/,$p' | sed 's# \[/[^]]*\]##; s#???:##' | head -n "$rows" >"$dir/$tag.incl.txt"
    awk '/PROGRAM TOTALS/ {gsub(",", "", $1); print $1}' "$dir/$tag.excl.txt" >"$dir/$tag.total"
    [ -s "$dir/$tag.total" ] || die "cg: scenario $n: no total (see $dir/$tag.err)"
    rm -f "$dir/$tag.callgrind"
    say "cg: scenario $n: $(cat "$dir/$tag.total") instructions ($dir/$tag.excl.txt, $tag.incl.txt)"
  done
  { do_versions; say "valgrind: $(valgrind --version)"; say "binary: $(sha256sum "$BUILD/gc-avant-iso")"; } >"$dir/session.txt"
}

case "${1:-}" in
  build) shift; do_build "$@" ;;
  check) do_check ;;
  time) do_time "${2:-7}" ;;
  rounds) UNITS_FILTER=${VALK_GC_UNITS:-} do_rounds "${2:-7}" ;;
  counts) do_counts ;;
  cg) shift; do_cg "$@" ;;
  trees-counts) do_trees_counts ;;
  versions) do_versions ;;
  *) awk 'NR > 1 && /^set -u/ { exit } NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
