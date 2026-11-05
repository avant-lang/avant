# Third-party C libraries

Avant wraps these; it does not reimplement JSON or HTTP.

| Tree | Role | License | Pin |
| --- | --- | --- | --- |
| `yyjson/` | JSON (D8) | MIT, copyright YaoYuan | v0.10.0 (`yyjson.h` / `yyjson.c`) |
| `uSockets/` | HTTP evented sockets (D8, D37: C only) | Apache-2.0 | `7a7c820` (`src/` only) |
| `base64/` | Base64 codec (aklomp/base64) | BSD-style, copyright Nick Galbreath, Wojciech Muła, Matthieu Darbois, and Alfred Klomp (see `base64/LICENSE`) | `e4cc438b7a8f2780b15f88aed24a89927f6bd6c6` (`base64/PIN`) |
| `pcre2/` | Regular expressions for `std.regex` computed patterns (D116, D146); literal patterns take re2c | BSD-3-Clause with the PCRE2 exception, copyright University of Cambridge and the PCRE2 authors (see `pcre2/LICENCE.md`, `pcre2/AUTHORS.md`); the bundled sljit JIT compiler is BSD-2-Clause, copyright Zoltan Herczeg (`pcre2/deps/sljit/LICENSE`) | 10.49 (`pcre2/PIN`: tarball sha256 and the release signature checked) |
| `picohttpparser/` | HTTP/1 request/response parser (h2o/picohttpparser) | MIT (dual-licensed MIT/Perl; the MIT side is taken), copyright Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, and Shigeo Mitsunari (see the license header in `picohttpparser.h`) | `dd573d8` |

Attribution: PCRE2 and sljit keep their license files in place (`pcre2/LICENCE.md`, `pcre2/deps/sljit/LICENSE`), as their terms require; a binary that links `std.regex` carries PCRE2, so its distributor reproduces that notice. uSockets is licensed under the Apache License, Version 2.0 (`uSockets/LICENSE`); this notice satisfies its redistribution terms for the vendored `src/` tree. The base64 tree keeps its own `LICENSE` file in place, as its terms require.

Do not tidy these trees except to update the pin.

## CVE and update policy

Each vendored tree has exactly one pin of record: the table above for `yyjson/` and `uSockets/`, `base64/PIN` for `base64/`, and `pcre2/PIN` for `pcre2/`. A security fix or a needed upstream fix updates the work the same way every time: replace the tree with the fixed upstream revision, update the pin in this file (and `base64/PIN` or `pcre2/PIN` where it applies) in the same change, and record the reason in the commit message. There is no floating "latest" anywhere: a build always names the revision it vendored. If upstream has no fixed revision yet, the pin stays and the exposure is recorded here rather than patched silently in-tree, so the vendored tree never diverges from its named pin without this file saying so.

## PCRE2

Merit: matching speed on a computed pattern is PCRE2's and its JIT's, reached through `std.regex`; a benchmark row that leans on it says so. Avant's own share is the literal path, which re2c compiles ahead of time. Configuration: `pcre2/src/config.h` is upstream's `config.h.generic` with one block appended at its end; that block reproduces the Ubuntu `libpcre2-8` that `std.regex` linked before vendoring, so an update reviews `config.h.generic` against the new release (upstream's own advice) and keeps the block. The tree is compiled by `runtime/build.sh` into `avant_re.o` with its symbols made local. PCRE2's security reports go through its GitHub repository (`pcre2/SECURITY.md` upstream); a fixed release is taken per the policy above, its signature checked against the key upstream's `SECURITY.md` names. Condition for a pure replacement: a regex engine written in Avant replaces PCRE2 for computed patterns when it implements the same pattern language the suite and the documentation promise, passes `test_std_regex` unchanged, and matches or beats PCRE2's JIT on LangArena's regex rows (`template_regex`, `log_parser`).
