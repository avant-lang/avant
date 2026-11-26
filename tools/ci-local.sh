#!/bin/sh
# ci-local.sh — the CI workflows' jobs rehearsed on this machine, before a push.
#
#   sh tools/ci-local.sh OUT [WORKFLOW[:JOB,JOB...]]...     (from the checkout root)
#
# Each job named (default: every job of `seed`, the pull request's gate) runs
# in a clean container of its runner's image (ubuntu-26.04 → ubuntu:26.04,
# ubuntu-24.04 → ubuntu:24.04), as an unprivileged user `runner` with
# passwordless sudo and none of this machine's tools (no perf, no LLVM, no
# seed in bin/), on `git archive` of the commit (CI_LOCAL_REF, default HEAD)
# at the runner's workspace path. tools/ci-local.py turns the job into a
# script of its `run:` steps, its expressions and the few actions the
# workflows take (checkout, cache, upload and download of artifacts;
# Codecov's uploads are skipped); a step or an expression it cannot rehearse
# stops the rehearsal before it starts. Jobs run in their `needs:` order and
# pass artifacts on as on GitHub. The `publish` job of `release` never runs
# (it writes to GitHub).
#
# The container gets CI_LOCAL_CPUS CPUs (default 4, a hosted runner's), the
# first of those this process may use (under `ledger run --cpus N`, give
# CI_LOCAL_CPUS=N), and the host network (apt, LLVM's release, the seed's
# download). The image is the runner's base with what a hosted runner has
# and the base image lacks (sudo, ca-certificates, curl, git, xz-utils,
# gnupg, python3, file, bc): built once as avant-ci-local:VERSION-HASH.
# LLVM's 630 MB cache is kept across rehearsals in CI_LOCAL_CACHE (default
# ~/.cache/avant-ci-local). DOCKER names the client (default docker; `sudo
# docker` where the socket is root's).
#
# What it cannot show: a change of GitHub's runner image itself (its
# preinstalled packages, apt's architecture variants), the network, and
# GitHub's own services. What it shows: a step that fails for this
# repository's reasons on a clean ubuntu-26.04 without this machine's tools.
#
#   OUT/JOB.log          the job's output (each step between ::group:: lines)
#   OUT/summary.txt      one line per job and per failed step
#   OUT/jobs/JOB/        the job's script and steps; OUT/artifacts/ the artifacts
#
# Environment: CI_LOCAL_REF, CI_LOCAL_CPUS, CI_LOCAL_CACHE, CI_LOCAL_TAG
# (release: the tag, default v and avant.toml's version), CI_LOCAL_TIMEOUT
# (seconds a job may take, default 5400), DOCKER, CI_LOCAL_DRY=1 (write every
# job's script and the plan, run nothing: its test, and a check that a
# workflow holds nothing the rehearsal cannot do).
# Status 0 when every job is green, 1 otherwise, 2 on a usage error.
set -u
[ $# -ge 1 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
cd "$root" || exit 2
out=$(realpath -m "$1"); shift
[ -e "$out" ] && { echo "ci-local.sh: $out exists" >&2; exit 2; }
docker=${DOCKER:-docker}
ref=${CI_LOCAL_REF:-HEAD}
ncpu=${CI_LOCAL_CPUS:-4}
cache=${CI_LOCAL_CACHE:-$HOME/.cache/avant-ci-local}
limit=${CI_LOCAL_TIMEOUT:-5400}
commit=$(git rev-parse --verify "$ref^{commit}" 2> /dev/null) || { echo "ci-local.sh: no commit $ref" >&2; exit 2; }
version=$(git show "$commit:avant.toml" | sed -n 's/^version *= *"\([^"]*\)".*/\1/p' | head -n 1)
tag=${CI_LOCAL_TAG:-v$version}
[ $# -gt 0 ] || set -- seed
mkdir -p "$out/jobs" "$out/artifacts" "$cache" || exit 2
: > "$out/summary.txt"
[ "${CI_LOCAL_DRY:-0}" = 1 ] || git archive --format=tar -o "$out/src.tar" "$commit" || exit 2
echo "ci-local: commit $commit (tree $(git rev-parse "$commit^{tree}")), $ncpu CPUs" >> "$out/summary.txt"

# The container's CPUs: the first $ncpu of those this process may use.
allowed=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
cpuset=$(printf '%s\n' "$allowed" | tr ',' '\n' | awk -F- -v n="$ncpu" '
  { lo = $1; hi = ($2 == "" ? $1 : $2); for (c = lo; c <= hi && k < n; c++) { s = s (k ? "," : "") c; k++ } }
  END { print s }')

# image RUNS_ON: the rehearsal's image for a runner label, built when missing.
image() {
  case "$1" in
    ubuntu-26.04) base=ubuntu:26.04 ;;
    ubuntu-24.04) base=ubuntu:24.04 ;;
    *) echo "ci-local.sh: no image for runs-on $1" >&2; return 1 ;;
  esac
  d="$out/image-$1"; mkdir -p "$d"
  cat > "$d/Dockerfile" << EOF
FROM $base
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \\
      sudo ca-certificates curl git xz-utils gnupg python3 file bc \\
    && rm -rf /var/lib/apt/lists/*
RUN (getent passwd $(id -u) | cut -d: -f1 | xargs -r userdel -r 2> /dev/null || true) \\
    && useradd -m -u $(id -u) -s /bin/bash runner \\
    && echo 'runner ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/runner \\
    && mkdir -p /home/runner/work/avant/avant && chown -R runner /home/runner/work
USER runner
EOF
  tagname="avant-ci-local:$1-$(sha256sum "$d/Dockerfile" | cut -c1-12)"
  if ! $docker image inspect "$tagname" > /dev/null 2>&1; then
    echo "ci-local: building $tagname from $base" | tee -a "$out/summary.txt"
    $docker build -q -t "$tagname" "$d" > "$d/build.log" 2>&1 || { echo "ci-local.sh: the image $tagname did not build ($d/build.log)" >&2; return 1; }
  fi
  echo "$tagname"
}

# The jobs to run, in order: each argument's jobs (default all but publish), each after the jobs it needs.
plan=""
for arg in "$@"; do
  wf=${arg%%:*}; f=.github/workflows/$wf.yml
  [ -f "$f" ] || { echo "ci-local.sh: no workflow $f" >&2; exit 2; }
  case "$arg" in
    *:*) jobs=$(printf '%s' "${arg#*:}" | tr ',' ' ') ;;
    *) jobs=$(python3 -I -c 'import sys, yaml; print(" ".join(j for j in yaml.safe_load(open(sys.argv[1]))["jobs"] if j != "publish"))' "$f") || exit 2 ;;
  esac
  for j in $jobs; do plan="$plan $wf:$j"; done
done

bad=0
for wj in $plan; do
  wf=${wj%%:*}; j=${wj#*:}; jd="$out/jobs/$wf-$j"; mkdir -p "$jd" "$out/out-$wf-$j"
  meta=$(python3 -I tools/ci-local.py ".github/workflows/$wf.yml" "$j" "$jd" "github.ref=refs/tags/$tag" "github.ref_name=$tag") \
    || { echo "$wf:$j: cannot rehearse (see above)" >> "$out/summary.txt"; bad=$((bad + 1)); continue; }
  for n in $(printf '%s\n' "$meta" | sed -n 's/^needs //p'); do
    grep -q "^$wf:$n: green" "$out/summary.txt" || { echo "$wf:$j: skipped: it needs $n, which is not green" >> "$out/summary.txt"; bad=$((bad + 1)); continue 2; }
  done
  if [ "${CI_LOCAL_DRY:-0}" = 1 ]; then
    echo "$wf:$j: green (dry: written, not run; runs-on $(printf '%s\n' "$meta" | sed -n 's/^runs-on //p'))" >> "$out/summary.txt"; continue
  fi
  img=$(image "$(printf '%s\n' "$meta" | sed -n 's/^runs-on //p')") || { bad=$((bad + 1)); continue; }
  src="$out/src-$wf-$j"; mkdir -p "$src" && tar -C "$src" -xf "$out/src.tar" || exit 2
  echo "ci-local: $wf:$j in $img" >> "$out/summary.txt"
  echo "ci-local: $wf:$j in $img (log $out/$wf-$j.log)"
  t0=$(date +%s)
  timeout -k 30 "$limit" $docker run --rm --network host --cpuset-cpus "$cpuset" \
    -v "$jd:/ci/job:ro" -v "$out/artifacts:/ci/artifacts" -v "$cache:/ci/cache" -v "$out/out-$wf-$j:/ci/out" \
    -v "$src:/home/runner/work/avant/avant" -w /home/runner/work/avant/avant "$img" bash /ci/job/job.sh \
    > "$out/$wf-$j.log" 2>&1 < /dev/null
  rc=$?
  t1=$(date +%s)
  if [ $rc -eq 0 ]; then echo "$wf:$j: green ($((t1 - t0)) s)" >> "$out/summary.txt"
  else
    echo "$wf:$j: RED, status $rc ($((t1 - t0)) s; $out/$wf-$j.log)" >> "$out/summary.txt"
    grep '^ci-local: step [0-9]* FAILED' "$out/$wf-$j.log" | sed 's/^/  /' >> "$out/summary.txt"
    bad=$((bad + 1))
  fi
done
cat "$out/summary.txt"
if [ $bad -eq 0 ]; then echo "ci-local: every job green"; exit 0; fi
echo "ci-local: red"; exit 1
