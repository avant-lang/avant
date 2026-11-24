#!/bin/sh
# bench/http/run.sh with stub servers and a stub `wrk`: its build step and its
# base-against-final mode. Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/http-bench.sh WORK
#
# WORK is a new scratch directory. The harness runs from a copy (its own
# files, so `HERE` is the copy) on other ports than the real ones. Two build
# directories hold a stub `avant` row: a server script that answers `hello`
# on the port and `/rate` with the build's own number (1000 in the base, 1100
# in the final), and a `wrk` first on PATH that prints that number as the
# load's requests per second. A stub `crystal` counts its builds.
#   http-no-build: HTTP_BENCH_NO_BUILD=1 builds nothing (the stub Crystal is
#     never run) and the run uses the directory's server; a row with no
#     server there stops the run (status 2), named;
#   http-build-named: `run.sh build avant` builds only that row: the stub
#     Crystal is not run;
#   http-crystal-flags: the Crystal servers are rebuilt when their flags
#     differ (a flags file of another content), and not when only the
#     script is newer;
#   http-versus-cells: `METHOD=B run.sh versus BASE FINAL avant` with three
#     rounds keeps three cells for each side under base/B and final/B, every
#     one status ok;
#   http-versus-order: the base is first in the odd rounds and the final in
#     the even ones;
#   http-versus-ratio: the last lines give each build's median and the
#     ratio (1100 over 1000);
#   http-versus-usage: without METHOD, with a missing directory or a row
#     without its server: status 2.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/http-bench.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w/avant/bench/http" "$w/bin" "$w/base" "$w/final" || exit 2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
cp "$root/bench/http/run.sh" "$w/avant/bench/http/run.sh"
: > "$w/avant/bench/http/crystal_server.cr"; : > "$w/avant/bench/http/crystal_ws.cr"
# The port: one above the real ones, a number the caller's other runs do not use.
port=$((19000 + $$ % 900))
wsport=$((port + 1000))
# A stub server for the row `avant` of a build whose rate is RATE.
mkserver() { # DIR RATE
  cat > "$1/avant_server" <<EOF
#!/bin/sh
exec python3 -c '
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"hello"
        if self.path == "/rate":
            body = b"$2"
        if self.path == "/quit":
            body = b"bye"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        if self.path == "/quit":
            self.wfile.flush()
            import threading
            threading.Thread(target=self.server.shutdown).start()
    def log_message(self, *a):
        pass
http.server.ThreadingHTTPServer(("127.0.0.1", $port), H).serve_forever()
'
EOF
  chmod +x "$1/avant_server"
}
mkserver "$w/base" 1000
mkserver "$w/final" 1100
# The stub wrk: the server's own /rate as requests per second.
cat > "$w/bin/wrk" <<EOF
#!/bin/sh
case "\$1" in -v) echo "wrk stub"; exit 0 ;; esac
n=\$(curl -s --max-time 5 http://127.0.0.1:$port/rate)
echo "  Requests/sec:  \$n.00"
echo "  \$n requests in 1.00s, 1.00KB read"
EOF
chmod +x "$w/bin/wrk"
# The stub crystal: counts its builds, writes the binary.
cat > "$w/bin/crystal" <<EOF
#!/bin/sh
echo "\$*" >> "$w/crystal.calls"
while [ \$# -gt 0 ]; do [ "\$1" = -o ] && out=\$2; shift; done
printf '#!/bin/sh\n' > "\$out"; chmod +x "\$out"
EOF
chmod +x "$w/bin/crystal"
# The stub compiler of the Avant row: `compile SRC OUT ...` writes OUT.
printf '#!/bin/sh\n[ "$1" = compile ] || exit 2\nprintf "#!/bin/sh\\n" > "$3"; chmod +x "$3"\n' > "$w/bin/avantc"
chmod +x "$w/bin/avantc"
hb() { # NAME [VAR=value...] -- ARGS...: run.sh from the copy, output in $w/NAME.log, its status in $rc
  n=$1; shift
  envs=""; while [ $# -gt 0 ] && [ "$1" != -- ]; do envs="$envs $1"; shift; done; shift
  # shellcheck disable=SC2086
  (cd "$w/avant" && env -i PATH="$w/bin:$PATH" HOME="${HOME:-/}" HTTP_BENCH_PORT=$port HTTP_BENCH_WS_PORT=$wsport STUB_PORT=$port HTTP_BENCH_WARMUP=0 HTTP_BENCH_DURATION=1 $envs timeout -k 10 300 sh bench/http/run.sh "$@" < /dev/null > "$w/$n.log" 2>&1); rc=$?
}

# No build: the directory's server is used; a Crystal build is never started.
: > "$w/crystal.calls"
hb nobuild HTTP_BENCH_NO_BUILD=1 HTTP_BENCH_BUILD="$w/base" METHOD=A HTTP_BENCH_RUNS=1 HTTP_BENCH_OUT="$w/nb.out" -- avant
ok=0; [ $rc = 0 ] && grep -q '^HTTP_BENCH_NO_BUILD=1: nothing is built' "$w/nobuild.log" && grep -q '^A r1 avant status=ok req/s=1000' "$w/nobuild.log" && [ ! -s "$w/crystal.calls" ] || ok=1
hb nobuild2 HTTP_BENCH_NO_BUILD=1 HTTP_BENCH_BUILD="$w/base" METHOD=A HTTP_BENCH_RUNS=1 HTTP_BENCH_OUT="$w/nb2.out" -- go
[ $ok = 0 ] && [ $rc = 2 ] && grep -q "^HTTP_BENCH_NO_BUILD=1: the row go has no server in $w/base" "$w/nobuild2.log"
say http-no-build $? "rc=$rc; $(tr '\n' ';' < "$w/nobuild.log" | cut -c1-200); $(tr '\n' ';' < "$w/nobuild2.log" | cut -c1-200)"

# `build ROW` builds that row only.
rm -rf "$w/b1"; mkdir -p "$w/b1"; : > "$w/crystal.calls"
hb buildrow HTTP_BENCH_BUILD="$w/b1" -- build crystal
n1=$(wc -l < "$w/crystal.calls")
: > "$w/crystal.calls"
mkdir -p "$w/b2"
hb buildavant HTTP_BENCH_BUILD="$w/b2" AVANT_BENCH_COMPILER="$w/bin/avantc" -- build avant
[ "$n1" = 2 ] && [ ! -e "$w/b2/crystal_server" ] && grep -q '^building avant into ' "$w/buildavant.log"
say http-build-named $? "crystal builds for 'build crystal': $n1; $(tr '\n' ';' < "$w/buildavant.log" | cut -c1-200)"

# Crystal's flags: by content.
: > "$w/crystal.calls"
hb crflags1 HTTP_BENCH_BUILD="$w/b1" -- build crystal
c1=$(wc -l < "$w/crystal.calls")
touch "$w/avant/bench/http/run.sh"
hb crflags2 HTTP_BENCH_BUILD="$w/b1" -- build crystal
c2=$(wc -l < "$w/crystal.calls")
echo "--release -Dother" > "$w/b1/crystal_server.flags"
hb crflags3 HTTP_BENCH_BUILD="$w/b1" -- build crystal
c3=$(wc -l < "$w/crystal.calls")
[ "$c1" = 0 ] && [ "$c2" = 0 ] && [ "$c3" = 1 ] && [ "$(cat "$w/b1/crystal_server.flags")" = "--release -Dpreview_mt" ]
say http-crystal-flags $? "builds after the flags written $c1, after the script touched $c2, after other flags $c3"

# Versus: three rounds.
hb versus HTTP_BENCH_RUNS=3 HTTP_BENCH_OUT="$w/vs.out" METHOD=B -- versus "$w/base" "$w/final" avant
nb=$(ls "$w/vs.out/base/B"/r*.avant.wrk 2>/dev/null | wc -l); nf=$(ls "$w/vs.out/final/B"/r*.avant.wrk 2>/dev/null | wc -l)
okc=1; for f in "$w/vs.out/base/B"/r*.avant.meta "$w/vs.out/final/B"/r*.avant.meta; do grep -q '^status=ok$' "$f" || okc=0; done
[ $rc = 0 ] && [ "$nb" = 3 ] && [ "$nf" = 3 ] && [ $okc = 1 ]
say http-versus-cells $? "rc=$rc base cells $nb, final cells $nf; $(tail -n 5 "$w/versus.log" | cut -c1-200 | tr '\n' ';')"
order=$(grep -o '^\(base\|final\): B r[0-9]' "$w/versus.log" | sed 's/: B r/ /' | tr '\n' ',')
[ "$order" = "base 1,final 1,final 2,base 2,base 3,final 3," ]
say http-versus-order $? "$order"
grep -q '^versus B avant: base 1000.* req/s (3 runs, 1000.*-1000.*), final 1100.* req/s (3 runs, 1100.*-1100.*), final/base 1.1000$' "$w/versus.log"
say http-versus-ratio $? "$(grep '^versus' "$w/versus.log")"

# Usage.
hb vs1 -- versus "$w/base" "$w/final" avant
u1=$rc
hb vs2 METHOD=B -- versus "$w/base" "$w/missing" avant
u2=$rc
hb vs3 METHOD=B -- versus "$w/base" "$w/final" rust
u3=$rc
[ "$u1$u2$u3" = 222 ]
say http-versus-usage $? "$u1 $u2 $u3"
echo done
