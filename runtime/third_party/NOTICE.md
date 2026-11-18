# Third-party C libraries

Avant wraps these; it does not reimplement JSON or HTTP.

| Tree | Role | License | Pin |
| --- | --- | --- | --- |
| `yyjson/` | JSON | MIT, copyright YaoYuan | v0.10.0 (`yyjson.h` / `yyjson.c`), with the local patch listed under "yyjson: local patches" |
| `uSockets/` | HTTP evented sockets (the C library only) | Apache-2.0 | `7a7c820` (`src/` only), with the local patches listed under "uSockets: local patches" |
| `base64/` | Base64 codec (aklomp/base64) | BSD-style, copyright Nick Galbreath, Wojciech Muła, Matthieu Darbois, and Alfred Klomp (see `base64/LICENSE`) | `e4cc438b7a8f2780b15f88aed24a89927f6bd6c6` (`base64/PIN`) |
| `pcre2/` | Regular expressions for `std.regex` computed patterns; literal patterns take re2c | BSD-3-Clause with the PCRE2 exception, copyright University of Cambridge and the PCRE2 authors (see `pcre2/LICENCE.md`, `pcre2/AUTHORS.md`); the bundled sljit JIT compiler is BSD-2-Clause, copyright Zoltan Herczeg (`pcre2/deps/sljit/LICENSE`) | 10.49 (`pcre2/PIN`: tarball sha256 and the release signature checked) |
| `zlib/` | Deflate for `std.zlib` | zlib license, copyright Jean-loup Gailly and Mark Adler (see `zlib/LICENSE`) | 1.3.2 (`zlib/PIN`: tarball sha256 and the release signature checked) |
| `crc32-chromium/` | CRC-32 by carry-less multiply for `std.digest` (Chromium's zlib, `crc32_sse42_simd_` only: the file's copyright notice and that function, byte for byte) | BSD-3-Clause, copyright The Chromium Authors (see `crc32-chromium/LICENSE`) | `401d1853956d408bfd327ee5e54c1c7b67086f87` (`crc32-chromium/PIN`: the lines taken and the source file's sha256) |
| `sha-intrinsics/` | SHA-256 with the SHA instructions for `std.digest` (noloader/SHA-Intrinsics, `sha256-x86.c` unmodified) | public domain, by Jeffrey Walton, based on code from Intel and by Sean Gulley for miTLS (the file's header; `sha-intrinsics/README.md`) | `d03795497f3e4576083fc2cd8fe0b924f24d0bb2` (`sha-intrinsics/PIN`) |
| `picohttpparser/` | HTTP/1 request/response parser (h2o/picohttpparser) | MIT (dual-licensed MIT/Perl; the MIT side is taken), copyright Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, and Shigeo Mitsunari (see the license header in `picohttpparser.h`) | `dd573d8` |

Attribution: PCRE2 and sljit keep their license files in place (`pcre2/LICENCE.md`, `pcre2/deps/sljit/LICENSE`), as their terms require; a binary that links `std.regex` carries PCRE2, so its distributor reproduces that notice. uSockets is licensed under the Apache License, Version 2.0 (`uSockets/LICENSE`); this notice satisfies its redistribution terms for the vendored `src/` tree. The base64 tree keeps its own `LICENSE` file in place, as its terms require. The Chromium excerpt keeps its copyright notice in the file and the BSD-3-Clause text in `crc32-chromium/LICENSE`, which a binary that links `std.digest` carries; its distributor reproduces that notice.

Do not tidy these trees except to update the pin.

## CVE and update policy

Each vendored tree has exactly one pin of record: the table above for `yyjson/` and `uSockets/`, `crc32-chromium/PIN` for `crc32-chromium/`, `sha-intrinsics/PIN` for `sha-intrinsics/`, `base64/PIN` for `base64/`, `pcre2/PIN` for `pcre2/`, and `zlib/PIN` for `zlib/`. A security fix or a needed upstream fix updates the work the same way every time: replace the tree with the fixed upstream revision, update the pin in this file (and the tree's `PIN` file where it has one) in the same change, and record the reason in the commit message. There is no floating "latest" anywhere: a build always names the revision it vendored. If upstream has no fixed revision yet, the pin stays and the exposure is recorded here rather than patched silently in-tree, so the vendored tree never diverges from its named pin without this file saying so.

## yyjson: local patches

The tree is upstream v0.10.0 plus the change below, marked "Avant patch" in the source. An update of the pin re-applies it or drops it with the upstream change that makes it unnecessary, and says which in the commit message.

- `yyjson/yyjson.c`, `read_root_minify` and `read_root_pretty`, and `yyjson/yyjson.h`: a nesting limit. yyjson's reader takes any depth (it keeps no stack of its own), so a document of a million `[` is read, and every reader that walks it after by recursion (`std.json`'s `to_value`, a typed decode, a program's own walk) runs out of stack on it. The reader now counts the arrays and objects it is inside, the root's being 1, and refuses a container past the reading thread's `yyjson_avant_max_depth` with `YYJSON_READ_ERROR_AVANT_DEPTH` at its bracket; 0 is no limit, yyjson's own behaviour. `std.json`'s `Json.parse` sets it from its `max_depth` (`runtime/avant_json.c`). Cost: an increment and a compare per container read.

## uSockets: local patches

The tree is upstream `7a7c820` plus the changes below, each marked "Avant patch" in the source. An update of the pin re-applies each one or drops it with the upstream change that makes it unnecessary, and says which in the commit message.

- `uSockets/src/bsd.c`, `bsd_write2`: `sendmsg` with `MSG_NOSIGNAL` in place of `writev`. `writev` cannot carry the flag, so a write to a connection the peer had reset raised SIGPIPE and ended the process (a client that closed with two pipelined answers unread did it); the call now fails with `EPIPE`, as `bsd_send` in the same file already does. Every socket write of the runtime goes through `bsd_send` or `bsd_write2`.
- `uSockets/src/socket.c`, `us_socket_write2`: a short write sets the loop's `last_write_failed`, as `us_socket_write` does. Without it a short `us_socket_write2` made from inside `on_writable` lost its writable poll when the handler returned (`loop.c` stops polling for writable unless that flag is set), and the rest was never sent. `std.http` answers the requests that waited behind a queued answer from `on_writable`.
- `uSockets/src/loop.c`, `us_internal_dispatch_ready_poll`: when `accept` fails for want of descriptors or memory (`EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`) the listening socket is no longer polled and the program's hook `us_internal_accept_paused` (a weak symbol: without it nothing changes) is told; `std.http` polls the socket again at its loop's next sweep, at most a second later. Upstream left a "Todo: start timer here": the connection stayed in the backlog and the socket readable, so the loop polled it again at once, a busy loop for as long as the shortage lasted.
- `uSockets/src/loop.c`, `us_internal_dispatch_ready_poll`: after a read that filled the whole receive buffer the socket is read again only if it is still polled for readable. `std.http` stops polling a connection for readable while an answer's tail is queued, so that a client that does not read is held back by TCP; reading on regardless grew that connection's input without bound.

## PCRE2

Merit: matching speed on a computed pattern is PCRE2's and its JIT's, reached through `std.regex`; a benchmark row that leans on it says so. Avant's own share is the literal path, which re2c compiles ahead of time. Configuration: `pcre2/src/config.h` is upstream's `config.h.generic` with one block appended at its end; that block reproduces the Ubuntu `libpcre2-8` that `std.regex` linked before vendoring, so an update reviews `config.h.generic` against the new release (upstream's own advice) and keeps the block. The tree is compiled by `runtime/build.sh` into `avant_re.o` with its symbols made local. PCRE2's security reports go through its GitHub repository (`pcre2/SECURITY.md` upstream); a fixed release is taken per the policy above, its signature checked against the key upstream's `SECURITY.md` names. Condition for a pure replacement: a regex engine written in Avant replaces PCRE2 for computed patterns when it implements the same pattern language the suite and the documentation promise, passes `test_std_regex` unchanged, and matches or beats PCRE2's JIT on LangArena's regex rows (`template_regex`, `log_parser`).

## zlib

Merit: compression speed and ratio are zlib's, reached through `std.zlib`. Only the files `compress` and `uncompress` need are vendored (`zlib/PIN` lists them) and built as zlib's own `configure` builds them (`-O3 -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN`), folded into `avant_zlib.o` with only the `avant_*` symbols global. zlib's license asks that its notice stay with the source, which `zlib/LICENSE` does. zlib 1.4 appeared on the day 1.3.2 was pinned (2026-11-05) and was not taken on its first day; moving to it follows the policy above. Condition for a pure replacement: a deflate written in Avant replaces zlib when it produces streams zlib reads (and reads zlib's), passes `test_std_zlib` unchanged, and matches zlib's speed and ratio on a stated benchmark.

## Bundled tools

The release archive also ships `re2c` beside `avant`: re2c **4.6**, public domain (`re2c-4.6/LICENSE` upstream: "re2c is in the public domain"), built fully static by `tools/build-re2c.sh` from `https://github.com/skvadrik/re2c/releases/download/4.6/re2c-4.6.tar.xz`, sha256 `75bf2696445e831d0d44e0d9f2909eeffc18c09757f222b2fb025f7e59fe130b` (upstream publishes no signature, so the pin is the hash, checked on every build). It is a build-time generator: it runs on the developer's machine when a literal pattern is compiled and is not linked into programs; the scanners it writes are. The same pin and update policy applies: `tools/build-re2c.sh` holds the one version and hash of record.
