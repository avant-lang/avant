#!/bin/bash
# Runner for Valk's general benchmark suite, tier 1, as Valk ships it
# (bench/valk). New code, not a modification of Valk's examples/bench/run.sh.
# README.md beside it has the method; equivalence.md has the notes.
#
# Valk's, Go's and Rust's programs are NOT in this tree: they are read from
# a checkout of Valk at the pinned tag ($VALK_SRC), with the matching
# compiler linked as $VALK_SRC/valk (what Valk's own run.sh wants).
#
#   run.sh versions   toolchains, machine, governor, the checkout's commit
#   run.sh build      every row, with the commands of Valk's run.sh for the
#                     shipped rows (into $VALK_SRC/debug/benchmarks, the
#                     directory Valk's run.sh builds into) and the labelled
#                     variants (into $VALK_SRC/debug/benchmarks-variants)
#   run.sh shipped [RUNS]   Valk's own run.sh, unchanged, for the seven
#                     tier-1 programs it knows (default RUNS=5); timed
#   run.sh check      every row once, untimed; output against expected/
#   run.sh counts     no clock: perf stat instructions:u per row,
#                     VALK_BENCH_COUNT_RUNS runs (default 3) every thread
#                     and one of the main thread (measure.py)
#   run.sh time [ROUNDS]    one unmeasured warm-up, then ROUNDS (default 7)
#                     fresh processes per row, rows in rotating order;
#                     wall time by a nanosecond clock, peak RSS, the output
#                     checked on every run, one raw file per run (the loop
#                     of tools/rounds.py)
#
# Tables: python3 tables.py $VALK_BENCH_OUT (regenerates every table from
# the raw files).
#
# Environment:
#   VALK_SRC         the checkout of valk-lang/valk at the pinned tag (required)
#   VALK_BENCH_OUT   raw files (default $VALK_SRC/debug/avant-bench)
#   VALK_BENCH_SETS  sets that check/counts/time run (default: all;
#                    shipped switch levelled v3 lto)
#   VALK_BENCH_ROWS  restrict to these row ids (default: all)
#   VALK_BENCH_TIMEOUT  seconds per run (default 120)
#   VALK_BENCH_COUNT_RUNS  counts: runs per row, every thread (default 3)
#   VALK_BENCH_ALLOW_GOVERNOR=1  let `time` and `shipped` run without the
#                    performance governor or beside other work (a dry run;
#                    the session is marked "disturbed, not evidence")
#   GOEXPERIMENT     exported as jsonv2 when the installed Go is older than
#                    1.27 (json and json-serde import encoding/json/v2);
#                    set it yourself to override
#
# Timed runs (shipped, time) must be alone on the machine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
: "${VALK_SRC:?set VALK_SRC to a checkout of valk at the pinned tag}"
VALK_SRC=$(CDPATH='' cd -- "$VALK_SRC" && pwd)
BENCH=$VALK_SRC/examples/bench
SHIP=$VALK_SRC/debug/benchmarks
VAR=$VALK_SRC/debug/benchmarks-variants
OUT=${VALK_BENCH_OUT:-$VALK_SRC/debug/avant-bench}
LIMIT=${VALK_BENCH_TIMEOUT:-120}
PINNED_TAG=0.8.5
PINNED_COMMIT=332243001c505373a8203cc60abaafcf188c4701

# The programs and their arguments: Valk's run.sh (benchmark_args) for the
# seven it lists. fannkuch-redux is not in Valk's run.sh; 11 is the default
# of Valk's program.
PROGRAMS="binary-tree merkletrees lru json json-serde nsieve spectral-norm fannkuch-redux"
prog_args() {
  case "$1" in
    binary-tree) echo "19" ;;
    merkletrees) echo "18" ;;
    lru) echo "1000 11000000" ;;
    json) echo "2000000" ;;
    json-serde) echo "sample 50000" ;;
    nsieve) echo "13" ;;
    spectral-norm) echo "5500" ;;
    fannkuch-redux) echo "11" ;;
  esac
}
# Languages a program ships in (fannkuch-redux has no Go version).
prog_langs() {
  case "$1" in
    fannkuch-redux) echo "valk rust" ;;
    *) echo "valk go rust" ;;
  esac
}

say() { printf '%s\n' "$*"; }
die() { printf 'run.sh: %s\n' "$*" >&2; exit 1; }

go_experiment() {
  # encoding/json/v2 is behind GOEXPERIMENT=jsonv2 before Go 1.27.
  if [ -z "${GOEXPERIMENT+x}" ]; then
    case "$(go env GOVERSION)" in
      go1.2[0-6]|go1.2[0-6].*) export GOEXPERIMENT=jsonv2 ;;
    esac
  fi
}

governors() {
  cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | awk '{printf "%s%s x%s", (NR>1?", ":""), $2, $1} END {print ""}'
}

cmd_versions() {
  mkdir -p "$OUT"
  go_experiment
  {
    say "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    say "kernel: $(uname -srm)"
    say "cpu: $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //') ($(nproc) threads)"
    say "governor: $(governors)"
    say "memory: $(free -g | awk 'NR==2 {print $2 " GB total, " $7 " GB available"}')"
    say "valk checkout: $(git -C "$VALK_SRC" describe --tags --always 2>/dev/null) $(git -C "$VALK_SRC" rev-parse HEAD 2>/dev/null) (pinned: $PINNED_TAG $PINNED_COMMIT)"
    say "valk: $("$VALK_SRC/valk" version 2>&1 | head -1); $(readlink -f "$VALK_SRC/valk"); sha256 $(sha256sum "$(readlink -f "$VALK_SRC/valk")" | cut -c1-64)"
    say "go: $(go version); GOAMD64=$(go env GOAMD64); GOEXPERIMENT=${GOEXPERIMENT:-}"
    say "rustc: $(rustc --version); LLVM $(rustc --version --verbose | sed -n 's/^LLVM version: //p'); default target features: $(rustc --print cfg | sed -n 's/^target_feature="\(.*\)"/\1/p' | tr '\n' ' ')"
    say "cargo: $(cargo --version)"
    say "perf: $(perf --version 2>&1)"
    say "time: $(/usr/bin/time --version 2>&1 | head -1)"
    say "python3: $(python3 --version 2>&1)"
  } | tee "$OUT/versions.txt"
}

# ---- build -----------------------------------------------------------------

ROWS_TMP=
add_row() { # id program row set binary
  local cwd="$BENCH/$2"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$cwd" "$(prog_args "$2")" >> "$ROWS_TMP"
}
logrun() { # write the command (with the build variables set for it) into the build log, then run it
  local v pre=""
  for v in GOAMD64 RUSTFLAGS CARGO_PROFILE_RELEASE_LTO CARGO_PROFILE_RELEASE_CODEGEN_UNITS; do
    [ -n "${!v:-}" ] && pre="$pre$v='${!v}' "
  done
  printf '%s%s\n' "$pre" "$*" >> "$OUT/build/commands.txt"
  "$@" > "$OUT/build/last.log" 2>&1 < /dev/null || { cat "$OUT/build/last.log" >&2; die "build failed: $*"; }
}

build_valk() { # source out flags...
  local src=$1 out=$2; shift 2
  logrun timeout -k 10 600 "$VALK_SRC/valk" build "$src" --release "$@" -o "$out"
}
build_go() { # program out
  logrun timeout -k 10 600 go build -o "$2" "$BENCH/$1/main.go"
}
build_rust() { # program out target-dir-suffix [extra rustc flags for the single-file form...]
  local p=$1 out=$2 suffix=$3; shift 3
  if [ -f "$BENCH/$p/Cargo.toml" ]; then
    local td
    if [ -z "$suffix" ]; then td="$SHIP/cargo-$p"; else td="$VAR/cargo-$p-$suffix"; fi
    logrun timeout -k 10 900 cargo build --quiet --release --manifest-path "$BENCH/$p/Cargo.toml" --target-dir "$td"
    cp "$td/release/$p" "$out"
  else
    logrun timeout -k 10 600 rustc -C opt-level=3 -C debuginfo=0 "$@" -o "$out" "$BENCH/$p/main.rs"
  fi
}

cmd_build() {
  [ -x "$VALK_SRC/valk" ] || die "no compiler at $VALK_SRC/valk"
  [ "$(git -C "$VALK_SRC" rev-parse HEAD)" = "$PINNED_COMMIT" ] || die "$VALK_SRC is not at $PINNED_TAG ($PINNED_COMMIT)"
  go_experiment
  mkdir -p "$SHIP" "$VAR" "$OUT/build"
  : > "$OUT/build/commands.txt"
  ROWS_TMP="$OUT/build/rows.tmp"; : > "$ROWS_TMP"
  local p l
  say "GOEXPERIMENT=${GOEXPERIMENT:-} (exported for every go build)" >> "$OUT/build/commands.txt"
  for p in $PROGRAMS; do
    # Shipped rows: the three commands of Valk's run.sh (build_benchmark).
    for l in $(prog_langs "$p"); do
      say "build $p $l"
      case "$l" in
        valk) build_valk "$BENCH/$p/main.valk" "$SHIP/$p-valk" ;;
        go)   build_go "$p" "$SHIP/$p-go" ;;
        rust) build_rust "$p" "$SHIP/$p-rust" "" ;;
      esac
      add_row "$p:$l" "$p" "$l" shipped "$SHIP/$p-$l"
    done
    # Valk's switch: no regions (every object on the collector's heap).
    case "$p" in binary-tree|merkletrees)
      say "build $p valk-noregions"
      build_valk "$BENCH/$p/main.valk" "$VAR/$p-valk-noregions" --no-regions
      add_row "$p:valk-noregions" "$p" valk-noregions switch "$VAR/$p-valk-noregions" ;;
    esac
    # The levelled twins of a few lines (variants/, with their headers).
    case "$p" in
      lru)
        say "build $p valk-plain"
        build_valk "$HERE/variants/lru-plain/main.valk" "$VAR/$p-valk-plain"
        add_row "$p:valk-plain" "$p" valk-plain levelled "$VAR/$p-valk-plain" ;;
      nsieve)
        say "build $p valk-checked"
        build_valk "$HERE/variants/nsieve-checked/main.valk" "$VAR/$p-valk-checked"
        add_row "$p:valk-checked" "$p" valk-checked levelled "$VAR/$p-valk-checked" ;;
    esac
    # Every compiler told to target x86-64-v3 (the defaults are the baseline).
    for l in $(prog_langs "$p"); do
      say "build $p $l-v3"
      case "$l" in
        valk) build_valk "$BENCH/$p/main.valk" "$VAR/$p-valk-v3" --cpu x86-64-v3 ;;
        go)   GOAMD64=v3 build_go "$p" "$VAR/$p-go-v3" ;;
        rust) RUSTFLAGS="-C target-cpu=x86-64-v3" build_rust "$p" "$VAR/$p-rust-v3" v3 -C target-cpu=x86-64-v3 ;;
      esac
      add_row "$p:$l-v3" "$p" "$l-v3" v3 "$VAR/$p-$l-v3"
    done
    # Rust with one code-generation unit and fat LTO (Valk's --release is one
    # whole-program module by default; Rust's default is neither).
    say "build $p rust-lto"
    CARGO_PROFILE_RELEASE_LTO=fat CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 build_rust "$p" "$VAR/$p-rust-lto" lto -C lto=fat -C codegen-units=1
    add_row "$p:rust-lto" "$p" rust-lto lto "$VAR/$p-rust-lto"
  done
  mv "$ROWS_TMP" "$OUT/rows.tsv"
  rm -f "$OUT/build/last.log"
  cut -f5 "$OUT/rows.tsv" | xargs sha256sum | sed "s|  $VALK_SRC/|  |" > "$OUT/build/sha256.txt"
  for p in $PROGRAMS; do
    [ -f "$BENCH/$p/Cargo.lock" ] && cp "$BENCH/$p/Cargo.lock" "$OUT/build/Cargo.lock.$p"
  done
  say "built $(wc -l < "$OUT/rows.tsv") rows; rows in $OUT/rows.tsv"
}

# ---- Valk's own run.sh, unchanged ------------------------------------------

cmd_shipped() {
  local runs=${1:-5}
  go_experiment
  mkdir -p "$OUT/shipped"
  local gov; gov=$(governors)
  {
    say "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    say "governor: $gov"
    say "loadavg before: $(cut -d' ' -f1-3 /proc/loadavg)"
    say "command: GOEXPERIMENT=${GOEXPERIMENT:-} $BENCH/run.sh --runs $runs binary-tree json json-serde lru merkletrees nsieve spectral-norm"
    say "run.sh sha256: $(sha256sum "$BENCH/run.sh" | cut -c1-64) (git status of the checkout: $(git -C "$VALK_SRC" status --porcelain -- examples/bench/run.sh | wc -l) changes)"
    [ "$gov" = "performance x$(nproc)" ] || say "DISTURBED, NOT EVIDENCE: governor is '$gov', not performance on every CPU"
    [ -n "${VALK_BENCH_ALLOW_GOVERNOR:-}" ] && say "DISTURBED, NOT EVIDENCE: dry run (VALK_BENCH_ALLOW_GOVERNOR)"
  } > "$OUT/shipped/session.txt"
  if grep -q DISTURBED "$OUT/shipped/session.txt" && [ -z "${VALK_BENCH_ALLOW_GOVERNOR:-}" ]; then
    die "governor is '$gov', not performance: no timed work (VALK_BENCH_ALLOW_GOVERNOR=1 for a dry run)"
  fi
  timeout -k 10 3600 "$BENCH/run.sh" --runs "$runs" binary-tree json json-serde lru merkletrees nsieve spectral-norm \
    > "$OUT/shipped/run-sh.stdout" 2> "$OUT/shipped/run-sh.stderr" < /dev/null
  local rc=$?
  say "exit: $rc" >> "$OUT/shipped/session.txt"
  say "loadavg after: $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/shipped/session.txt"
  cat "$OUT/shipped/run-sh.stdout"
  grep -i 'warning\|differs\|unstable' "$OUT/shipped/run-sh.stderr"
  [ $rc -eq 0 ] || die "Valk's run.sh failed ($rc): $OUT/shipped/run-sh.stderr"
}

# ---- check, counts, time: measure.py ---------------------------------------

measure() {
  [ -f "$OUT/rows.tsv" ] || die "no $OUT/rows.tsv: run.sh build first"
  VALK_BENCH_OUT="$OUT" VALK_BENCH_TIMEOUT="$LIMIT" python3 "$HERE/measure.py" "$@"
}

case "${1:-}" in
  versions) cmd_versions ;;
  build) cmd_build ;;
  shipped) shift; cmd_shipped "$@" ;;
  check) measure check ;;
  counts) measure counts ;;
  time) shift; measure time "${1:-7}" ;;
  *) sed -n '2,40p' "$0"; exit 2 ;;
esac
