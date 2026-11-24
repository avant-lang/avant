#!/bin/sh
# The runner's speed index for a CI job: its CPU model and count, and the
# wall time of one fixed reference step, the make of the pinned re2c
# sources (tools/build-re2c.sh, the same sources and compiler every run).
# A step's time divided by the reference reads in units of runner speed,
# so two runs on runners of different speed can be compared
# (tools/ci-steps.py reads a job's step times that way).
#
#   sh .github/runner-speed.sh OUT_FILE      builds re2c into OUT_FILE, timed
#
# Prints the lines and appends them to $GITHUB_STEP_SUMMARY when set.
set -eu
[ "$#" -eq 1 ] || { echo "usage: sh .github/runner-speed.sh OUT_FILE" >&2; exit 2; }
times=$(mktemp "${TMPDIR:-/tmp}/runner-speed-XXXXXX")
trap 'rm -f "$times"' EXIT
RE2C_TIMES=$times sh tools/build-re2c.sh "$1"
cpu=$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)
n=$(nproc)
make_s=$(awk '$1 == "make" { print $2 }' "$times")
{
  echo "runner: ${cpu:-unknown CPU}, $n CPUs"
  echo "runner speed reference: re2c make ${make_s:-?} s at -j$n (download $(awk '$1 == "download" { print $2 }' "$times") s, configure $(awk '$1 == "configure" { print $2 }' "$times") s)"
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
