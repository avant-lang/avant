#!/bin/sh
# The README's examples, checked (Stage 29, the README rewrite).
#
#   sh tools/readme-check.sh [FILE.md]        (default: README.md)
#   sh tools/readme-check.sh --plan [FILE.md] (extract and plan only: no compiler)
#
# Every ```avant block of the document is compiled, and run, with the
# compiler under test, and what the document says a program prints is
# compared with what it prints. The last line is `README CHECK PASS` or
# `README CHECK FAIL`; the exit status is 0 only on PASS (2 on usage).
#
# How a block is marked. A block with no mark must be a complete program
# (it has `fn main` or `fn run`): it is compiled and run, and must exit 0.
# Any other block carries a mark, an HTML comment on the line before its
# fence (blank lines between are allowed; a rendered page does not show it):
#
#   <!-- check: MODE [SHAPE] [KEY=VALUE ...] -->
#
# MODE
#   run        compile and run; exit status 0 (or status=N)
#   compile    compile only (a server that never returns, a program that
#              needs something the checker does not have)
#   error      the compile must fail (see `stderr` below for its text)
#   trap       compile and run; exit status 1 (or status=N)
#   file       not a program: only written into a project (see project=)
#   fragment   shown only, not compiled; REASON follows the word
#   skip       a complete program that is not compiled; REASON follows
#   output     on a ```text block: the exact stdout of the last program
#              that ran (`output contains`: these lines, contiguous, are
#              somewhere in it)
#   stderr     on a ```text block: these lines, contiguous, are somewhere
#              in the last program's stderr (the compiler's for `error`,
#              the compiler's and then the program's otherwise)
#   hidden     not a block: a comment that holds source the page does not
#              show, `<!-- check: hidden id=NAME`, the source, then `-->`
#              on its own line; other blocks take it with pre= or post=
# SHAPE (how the shown text becomes a program)
#   body       the text is statements: it is wrapped in `fn main { }`
#   decls      the text is declarations: an empty `fn main { }` is added
# KEY=VALUE
#   id=NAME          keep this block's text under NAME
#   pre=A,B          put the texts A and B (ids, hidden or shown) before it
#   post=A,B         put them after it
#   project=P        the block belongs to project P (a directory kept for
#                    the whole document); file=PATH is its file there
#                    (default main.av). A project block with a mode that
#                    compiles is the root, compiled in a copy of the
#                    project as it stands at that block
#   file=PATH        the root file's name (default main.av); a diagnostic
#                    names it
#   flags=A,B        extra `avant compile` arguments (flags=--lib,z)
#   args=A,B         the program's arguments
#   status=N         the expected exit status
#
# Inside a block, a trailing comment `// => TEXT` says that the program
# prints the line TEXT there: when a block has any, its stdout must be
# exactly those lines, in source order. A program that prints in a loop
# states its output in an `output` block instead.
#
# Environment:
#   AVANT_COMPILER   the compiler (default bin/avant-c-llvm, then bin/avant,
#                    then avant on PATH)
#   README_JOBS      how many programs are built and run at once (default:
#                    the online processors)
#   README_KEEP=1    keep the work directory and print its path
#   README_MODE      the codegen flag (default --debug)
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

if [ "${1:-}" = "--one" ]; then
  # Internal: build and run one planned program. $2 work, $3 number.
  work=$2
  n=$3
  d=$work/c/$n
  res=$work/r/$n
  # shellcheck disable=SC1090 # the plan's own file
  . "$d/meta"
  cd "$run_dir" || { printf 'FAIL\ncannot enter %s\n' "$run_dir" > "$res"; exit 0; }
  set -- compile "$root_file" "$d/prog" "$README_MODE"
  if [ -n "$flags" ]; then
    oldifs=$IFS
    IFS=,
    # shellcheck disable=SC2086 # a comma list of words
    set -- "$@" $flags
    IFS=$oldifs
  fi
  timeout -k 5 180 "$AVANT_COMPILER" "$@" > "$d/compile.out" 2> "$d/compile.err" < /dev/null
  crc=$?
  cat "$d/compile.err" > "$d/all.err"
  : > "$d/out"
  if [ "$mode" = error ]; then
    if [ "$crc" -eq 0 ]; then
      printf 'FAIL\nexpected a compile error, but it compiled\n' > "$res"
    elif [ "$crc" -ge 124 ]; then
      printf 'FAIL\nthe compiler ended with status %s (a timeout or a crash, not a diagnostic)\n' "$crc" > "$res"
    else
      echo PASS > "$res"
    fi
    exit 0
  fi
  if [ "$crc" -ne 0 ]; then
    { echo FAIL; echo "does not compile (status $crc):"; head -n 12 "$d/compile.err"; } > "$res"
    exit 0
  fi
  if [ "$mode" = compile ]; then
    echo PASS > "$res"
    exit 0
  fi
  set --
  if [ -n "$args" ]; then
    oldifs=$IFS
    IFS=,
    # shellcheck disable=SC2086 # a comma list of words
    set -- $args
    IFS=$oldifs
  fi
  timeout -k 5 60 "$d/prog" "$@" > "$d/out" 2> "$d/run.err" < /dev/null
  rrc=$?
  cat "$d/run.err" >> "$d/all.err"
  if [ "$rrc" -ne "$status" ]; then
    { echo FAIL; echo "exit status $rrc, expected $status"; head -n 6 "$d/run.err"; } > "$res"
    exit 0
  fi
  if [ -f "$d/expect.inline" ] && ! cmp -s "$d/expect.inline" "$d/out"; then
    { echo FAIL; echo "stdout differs from the block's // => lines:"; diff "$d/expect.inline" "$d/out" | head -n 12; } > "$res"
    exit 0
  fi
  echo PASS > "$res"
  exit 0
fi

plan_only=0
if [ "${1:-}" = "--plan" ]; then
  plan_only=1
  shift
fi
doc=${1:-$root/README.md}
[ -f "$doc" ] || { echo "readme-check: no file $doc" >&2; exit 2; }
doc=$(realpath "$doc")

if [ "$plan_only" -eq 0 ]; then
  if [ -z "${AVANT_COMPILER:-}" ]; then
    if [ -x "$root/bin/avant-c-llvm" ]; then
      AVANT_COMPILER=$root/bin/avant-c-llvm
    elif [ -x "$root/bin/avant" ]; then
      AVANT_COMPILER=$root/bin/avant
    else
      AVANT_COMPILER=$(command -v avant || true)
    fi
  fi
  [ -n "$AVANT_COMPILER" ] && [ -x "$AVANT_COMPILER" ] || { echo "readme-check: no compiler (set AVANT_COMPILER)" >&2; exit 2; }
  AVANT_COMPILER=$(realpath "$AVANT_COMPILER")
  export AVANT_COMPILER
fi
README_MODE=${README_MODE:---debug}
export README_MODE

work=$(mktemp -d "${TMPDIR:-/tmp}/readme-check-XXXXXX") || exit 2
if [ "${README_KEEP:-0}" = 1 ]; then
  echo "readme-check: work directory $work"
else
  trap 'rm -rf "$work"' EXIT
  trap 'exit 1' INT TERM
fi
mkdir -p "$work/b" "$work/ids" "$work/c" "$work/p" "$work/r"

# 1. Extract: every fenced block to b/N, its line, language and mark to
#    blocks.tsv; every hidden comment to ids/NAME.
awk -v W="$work" '
function flush_pending() { pending = "" }
BEGIN { n = 0; infence = 0; inhidden = 0; pending = "" }
{
  line = $0
  if (inhidden) {
    if (line == "-->") { inhidden = 0; close(hfile); next }
    print line > hfile
    next
  }
  if (infence) {
    if (line ~ /^```[ \t]*$/) { infence = 0; close(bfile); next }
    print line > bfile
    next
  }
  if (line ~ /^<!-- check: hidden id=[A-Za-z0-9_-]+[ \t]*$/) {
    name = line
    sub(/^<!-- check: hidden id=/, "", name)
    sub(/[ \t]*$/, "", name)
    hfile = W "/ids/" name
    printf "" > hfile
    inhidden = 1
    next
  }
  if (line ~ /^<!-- check: .* -->[ \t]*$/) {
    pending = line
    sub(/^<!-- check: /, "", pending)
    sub(/ -->[ \t]*$/, "", pending)
    next
  }
  if (line ~ /^```/) {
    n++
    lang = line
    sub(/^```/, "", lang)
    sub(/[ \t].*$/, "", lang)
    bfile = W "/b/" n
    printf "" > bfile
    printf "%d\t%d\t%s\t%s\n", n, NR, (lang == "" ? "-" : lang), (pending == "" ? "-" : pending) > (W "/blocks.tsv")
    pending = ""
    infence = 1
    next
  }
  if (line !~ /^[ \t]*$/) pending = ""
}
END {
  if (infence) { print "readme-check: unclosed fence at the end of the file" > "/dev/stderr"; exit 1 }
  if (inhidden) { print "readme-check: unclosed hidden comment at the end of the file" > "/dev/stderr"; exit 1 }
}
' "$doc" || exit 1
[ -f "$work/blocks.tsv" ] || { echo "readme-check: no fenced block in $doc" >&2; exit 2; }

# 2. Plan.
problems=0
n_avant=0
n_run=0
n_compile=0
n_error=0
n_trap=0
n_fragment=0
n_skip=0
n_shaped=0
n_file=0
n_inline=0
: > "$work/todo"
: > "$work/expect.tsv"
: > "$work/notes"
last_prog=""

problem() {
  problems=$((problems + 1))
  echo "readme-check: $doc:$1: $2"
}

# Appends the texts named in the comma list $1 to file $2 (line $3 for a message).
add_ids() {
  [ -n "$1" ] || return 0
  oldifs=$IFS
  IFS=,
  for name in $1; do
    IFS=$oldifs
    if [ -f "$work/ids/$name" ]; then
      cat "$work/ids/$name" >> "$2"
      echo >> "$2"
    else
      problem "$3" "no block or hidden comment has id=$name"
    fi
  done
  IFS=$oldifs
}

tab=$(printf '\t')
while IFS=$tab read -r n line lang mark; do
  src=$work/b/$n
  mode=""
  shape=""
  id=""
  pre=""
  post=""
  project=""
  file=""
  flags=""
  args=""
  status=""
  reason=""
  contains=0
  [ "$mark" = - ] && mark=""
  if [ -n "$mark" ]; then
    # shellcheck disable=SC2086 # the mark is words
    set -- $mark
    mode=$1
    shift
    case $mode in
      fragment|skip)
        reason=$*
        set --
        ;;
    esac
    for w in "$@"; do
      case $w in
        body|decls) shape=$w ;;
        contains) contains=1 ;;
        id=*) id=${w#id=} ;;
        pre=*) pre=${w#pre=} ;;
        post=*) post=${w#post=} ;;
        project=*) project=${w#project=} ;;
        file=*) file=${w#file=} ;;
        flags=*) flags=${w#flags=} ;;
        args=*) args=${w#args=} ;;
        status=*) status=${w#status=} ;;
        *) problem "$line" "unknown word '$w' in the mark '$mark'" ;;
      esac
    done
  fi
  [ -n "$id" ] && cp "$src" "$work/ids/$id"

  case $mode in
    output|stderr)
      if [ -z "$last_prog" ]; then
        problem "$line" "an '$mode' block with no program before it"
      else
        printf '%s\t%s\t%s\t%s\t%s\n' "$n" "$line" "$mode" "$last_prog" "$contains" >> "$work/expect.tsv"
      fi
      continue
      ;;
    file)
      [ -n "$project" ] || { problem "$line" "a 'file' block needs project=NAME"; continue; }
      [ -n "$file" ] || { problem "$line" "a 'file' block needs file=PATH"; continue; }
      mkdir -p "$(dirname "$work/p/$project/$file")"
      cp "$src" "$work/p/$project/$file"
      n_file=$((n_file + 1))
      [ "$lang" = avant ] && n_avant=$((n_avant + 1))
      continue
      ;;
  esac

  if [ "$lang" != avant ]; then
    [ -z "$mode" ] || problem "$line" "the mark '$mark' is on a '$lang' block (only output, stderr and file apply there)"
    continue
  fi
  n_avant=$((n_avant + 1))

  if [ -z "$mode" ]; then
    if grep -Eq '^fn (main|run)([ :{]|$)' "$src"; then
      mode=run
    else
      problem "$line" "an avant block without fn main needs a mark (run body, compile decls, fragment REASON, ...)"
      continue
    fi
  fi
  case $mode in
    fragment)
      [ -n "$reason" ] || problem "$line" "a fragment needs its reason"
      n_fragment=$((n_fragment + 1))
      printf 'fragment  line %s: %s\n' "$line" "$reason" >> "$work/notes"
      continue
      ;;
    skip)
      [ -n "$reason" ] || problem "$line" "a skip needs its reason"
      n_skip=$((n_skip + 1))
      printf 'skip      line %s: %s\n' "$line" "$reason" >> "$work/notes"
      continue
      ;;
    run|compile|error|trap) ;;
    *)
      problem "$line" "unknown mode '$mode'"
      continue
      ;;
  esac

  d=$work/c/$n
  mkdir -p "$d"
  [ -n "$file" ] || file=main.av
  if [ -z "$status" ]; then
    status=0
    [ "$mode" = trap ] && status=1
  fi
  if [ -n "$project" ]; then
    mkdir -p "$work/p/$project"
    cp -R "$work/p/$project" "$d/tree"
    run_dir=$d/tree
  else
    mkdir -p "$d/tree"
    run_dir=$d/tree
  fi
  mkdir -p "$(dirname "$run_dir/$file")"
  out=$run_dir/$file
  : > "$out"
  add_ids "$pre" "$out" "$line"
  case $shape in
    body)
      { echo "fn main {"; cat "$src"; echo "}"; } >> "$out"
      n_shaped=$((n_shaped + 1))
      ;;
    decls)
      { cat "$src"; echo; echo "fn main {"; echo "}"; } >> "$out"
      n_shaped=$((n_shaped + 1))
      ;;
    *)
      cat "$src" >> "$out"
      [ -n "$pre$post" ] && n_shaped=$((n_shaped + 1))
      ;;
  esac
  if [ -n "$post" ]; then
    echo >> "$out"
    add_ids "$post" "$out" "$line"
  fi
  # A project keeps its root file too, so a later block may import it.
  if [ -n "$project" ]; then
    mkdir -p "$(dirname "$work/p/$project/$file")"
    cp "$out" "$work/p/$project/$file"
  fi
  if grep -q '// =>' "$src"; then
    sed -n -e 's|^.*// =>$||p' -e 's|^.*// => \(.*\)$|\1|p' "$src" > "$d/expect.inline"
    n_inline=$((n_inline + 1))
    case $mode in
      run|trap) ;;
      *) problem "$line" "a '$mode' block cannot state output with // =>" ;;
    esac
  fi
  {
    echo "mode=$mode"
    echo "status=$status"
    echo "run_dir='$run_dir'"
    echo "root_file='$file'"
    echo "flags='$flags'"
    echo "args='$args'"
  } > "$d/meta"
  echo "$n" >> "$work/todo"
  printf '%s\t%s\t%s\n' "$n" "$line" "$mode" >> "$work/progs.tsv"
  last_prog=$n
  case $mode in
    run) n_run=$((n_run + 1)) ;;
    compile) n_compile=$((n_compile + 1)) ;;
    error) n_error=$((n_error + 1)) ;;
    trap) n_trap=$((n_trap + 1)) ;;
  esac
done < "$work/blocks.tsv"

n_expect=$(wc -l < "$work/expect.tsv")
summary="avant blocks $n_avant: run $n_run, compile only $n_compile, expected compile errors $n_error, expected traps $n_trap, project files $n_file, fragments shown only $n_fragment, skipped $n_skip; of the compiled, $n_shaped are fragments completed by a shape or by pre=/post=; outputs stated: $n_inline inline, $n_expect blocks"

if [ "$plan_only" -eq 1 ]; then
  cat "$work/notes"
  echo "readme-check: $summary"
  if [ "$problems" -gt 0 ]; then
    echo "README CHECK FAIL: $problems problems in the marks"
    exit 1
  fi
  echo "README PLAN OK (nothing compiled)"
  exit 0
fi

# 3. Build and run, README_JOBS at once.
jobs=${README_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
if [ -s "$work/todo" ]; then
  xargs -P "$jobs" -n 1 sh "$0" --one "$work" < "$work/todo"
fi

fails=0
if [ -f "$work/progs.tsv" ]; then
  while IFS=$tab read -r n line mode; do
    res=$work/r/$n
    if [ ! -f "$res" ]; then
      fails=$((fails + 1))
      echo "FAIL $doc:$line ($mode): no result"
      continue
    fi
    if [ "$(head -n 1 "$res")" != PASS ]; then
      fails=$((fails + 1))
      echo "FAIL $doc:$line ($mode)"
      sed -e 1d -e 's/^/    /' "$res"
    fi
  done < "$work/progs.tsv"
fi

# 4. The stated outputs and diagnostics.
while IFS=$tab read -r n line kind target contains; do
  want=$work/b/$n
  if [ "$kind" = output ]; then
    got=$work/c/$target/out
  else
    got=$work/c/$target/all.err
  fi
  if [ ! -f "$got" ]; then
    fails=$((fails + 1))
    echo "FAIL $doc:$line ($kind): its program produced nothing to compare"
    continue
  fi
  if [ "$kind" = output ] && [ "$contains" -eq 0 ]; then
    if ! cmp -s "$want" "$got"; then
      fails=$((fails + 1))
      echo "FAIL $doc:$line (output): the program prints something else:"
      diff "$want" "$got" | head -n 12 | sed 's/^/    /'
    fi
    continue
  fi
  # The block's lines, contiguous, somewhere in the text.
  if ! awk '
    NR == FNR { want[++w] = $0; next }
    { got[++g] = $0 }
    END {
      if (w == 0) exit 0
      for (s = 1; s + w - 1 <= g; s++) {
        ok = 1
        for (i = 1; i <= w; i++) if (got[s + i - 1] != want[i]) { ok = 0; break }
        if (ok) exit 0
      }
      exit 1
    }' "$want" "$got"; then
    fails=$((fails + 1))
    echo "FAIL $doc:$line ($kind): these lines are not in the program's $kind:"
    sed 's/^/    | /' "$want" | head -n 8
    echo "    the program's $kind:"
    head -n 12 "$got" | sed 's/^/    | /'
  fi
done < "$work/expect.tsv"

cat "$work/notes"
echo "readme-check: $summary"
if [ "$problems" -gt 0 ] || [ "$fails" -gt 0 ]; then
  echo "README CHECK FAIL: $fails failed, $problems problems in the marks"
  exit 1
fi
echo "README CHECK PASS"
exit 0
