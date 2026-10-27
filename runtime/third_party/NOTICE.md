# Third-party C libraries

Avant wraps these; it does not reimplement JSON or HTTP.

| Tree | Role | License | Pin |
| --- | --- | --- | --- |
| `yyjson/` | JSON (D8) | MIT, copyright YaoYuan | v0.10.0 (`yyjson.h` / `yyjson.c`) |
| `uSockets/` | HTTP evented sockets (D8, D37: C only) | Apache-2.0 | `7a7c820` (`src/` only) |
| `base64/` | Base64 codec (aklomp/base64) | BSD-style, copyright Nick Galbreath, Wojciech Muła, Matthieu Darbois, and Alfred Klomp (see `base64/LICENSE`) | `e4cc438b7a8f2780b15f88aed24a89927f6bd6c6` (`base64/PIN`) |

Attribution: uSockets is licensed under the Apache License, Version 2.0 (`uSockets/LICENSE`); this notice satisfies its redistribution terms for the vendored `src/` tree. The base64 tree keeps its own `LICENSE` file in place, as its terms require.

Do not tidy these trees except to update the pin.

## CVE and update policy

Each vendored tree has exactly one pin of record: the table above for `yyjson/` and `uSockets/`, and `base64/PIN` for `base64/`. A security fix or a needed upstream fix updates the work the same way every time: replace the tree with the fixed upstream revision, update the pin in this file (and `base64/PIN` where it applies) in the same change, and record the reason in the commit message. There is no floating "latest" anywhere: a build always names the revision it vendored. If upstream has no fixed revision yet, the pin stays and the exposure is recorded here rather than patched silently in-tree, so the vendored tree never diverges from its named pin without this file saying so.
