#!/usr/bin/env python3
"""The job scripts of a CI rehearsal: one GitHub workflow job as a shell script.

    python3 -I tools/ci-local.py WORKFLOW.yml JOB OUT_DIR [NAME=VALUE ...]

Writes OUT_DIR/job.sh, which runs the job's steps in order inside the
rehearsal's container (tools/ci-local.sh starts it): each `run:` step as
GitHub runs it (`bash --noprofile --norc -eo pipefail`, the step's own
`env:` over the job's and the workflow's), with the GitHub expressions the
workflows use replaced (`${{ env.X }}`, `${{ github.workspace }}`,
`${{ hashFiles('PATH') }}`, `A || B`, `secrets.*` and `github.token` empty).
The `uses:` steps the workflows take are done locally:

  actions/checkout          nothing: the workspace already holds `git archive`
                            of the commit (tools/ci-local.sh)
  actions/cache             the path restored from, and at the job's end saved
                            to, /ci/cache/KEY (a directory the rehearsals keep);
                            the step's `cache-hit` output says which
  actions/upload-artifact   the paths copied to /ci/artifacts/NAME
  actions/download-artifact /ci/artifacts/NAME copied to the step's `path`
  codecov/*                 skipped, said in the log: an upload to an outside
                            service

A step's `if:` may be `always()`, `success()`, `failure()` or a comparison
of a step's output with a quoted string (`steps.ID.outputs.KEY != 'true'`).
Anything else (another action, another expression) is refused, status 2,
so a rehearsal never skips a step silently. NAME=VALUE pairs set GitHub's
context (github.ref, github.ref_name, ...). Prints the job's runs-on and
needs, one per line: `runs-on IMAGE`, `needs JOB`.

The job script prints one line per step, `ci-local: step N ok|FAILED|skipped
SECONDS NAME`, and exits 1 when a step failed.
"""
import hashlib
import os
import re
import shlex
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write("ci-local.py: needs PyYAML (python3-yaml)\n")
    sys.exit(2)

WORKSPACE = "/home/runner/work/avant/avant"
EXPR = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")


class Unsupported(Exception):
    pass


def lookup(name, ctx, env):
    if name.startswith("env."):
        return env.get(name[4:], "")
    if name.startswith("secrets.") or name == "github.token":
        return ""
    if name in ctx:
        return ctx[name]
    if name.startswith("github."):
        return ""
    raise Unsupported("the context %r" % name)


def evaluate(expr, ctx, env, root):
    """The value of one ${{ }} expression: a name, a quoted string, hashFiles('P'), or A || B (the first that is not
    empty)."""
    parts = [p.strip() for p in expr.split("||")]
    for p in parts:
        m = re.fullmatch(r"hashFiles\('([^']+)'\)", p)
        if m:
            path = os.path.join(root, m.group(1))
            if not os.path.isfile(path):
                v = ""
            else:
                with open(path, "rb") as f:
                    v = hashlib.sha256(f.read()).hexdigest()
        elif re.fullmatch(r"'[^']*'", p):
            v = p[1:-1]
        elif re.fullmatch(r"[A-Za-z_][\w.\-]*", p):
            v = lookup(p, ctx, env)
        else:
            raise Unsupported("the expression %r" % expr)
        if v:
            return v
    return ""


def subst(text, ctx, env, root):
    return EXPR.sub(lambda m: evaluate(m.group(1), ctx, env, root), str(text))


def condition(expr):
    """A step's `if:` as a shell test, given the job's status in $ci_failed (0 or 1)."""
    e = EXPR.sub(lambda m: m.group(1), str(expr)).strip()
    if e == "always()":
        return "true"
    if e == "success()":
        return '[ "$ci_failed" = 0 ]'
    if e == "failure()":
        return '[ "$ci_failed" = 1 ]'
    m = re.fullmatch(r"steps\.([\w-]+)\.outputs\.([\w-]+)\s*(==|!=)\s*'([^']*)'", e)
    if m:
        var = "ci_out_%s_%s" % (m.group(1).replace("-", "_"), m.group(2).replace("-", "_"))
        test = "=" if m.group(3) == "==" else "!="
        return '[ "$ci_failed" = 0 ] && [ "${%s:-}" %s %s ]' % (var, test, shlex.quote(m.group(4)))
    raise Unsupported("the condition %r" % expr)


def main(argv):
    if len(argv) < 3 or argv[0] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 2
    wf_path, job_name, out = argv[0], argv[1], argv[2]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(wf_path))))
    ctx = {"github.workspace": WORKSPACE, "github.workflow": os.path.splitext(os.path.basename(wf_path))[0],
           "github.ref": "refs/heads/rehearsal", "github.ref_name": "rehearsal"}
    for kv in argv[3:]:
        k, _, v = kv.partition("=")
        ctx[k] = v
    with open(wf_path, encoding="utf-8") as f:
        wf = yaml.safe_load(f)
    jobs = wf.get("jobs") or {}
    if job_name not in jobs:
        sys.stderr.write("ci-local.py: %s has no job %r (it has: %s)\n" % (wf_path, job_name, ", ".join(jobs)))
        return 2
    job = jobs[job_name]
    env = {}
    try:
        for k, v in (wf.get("env") or {}).items():
            env[k] = subst(v, ctx, env, root)
        for k, v in (job.get("env") or {}).items():
            env[k] = subst(v, ctx, env, root)
        lines = ["#!/usr/bin/env bash", "# job %s of %s, written by tools/ci-local.py" % (job_name, os.path.basename(wf_path)),
                 "set -u", "cd %s || exit 2" % WORKSPACE, "ci_failed=0", "ci_saves=()", "export CI=true GITHUB_ACTIONS=true",
                 "export GITHUB_WORKSPACE=%s RUNNER_TEMP=/home/runner/work/_temp" % WORKSPACE,
                 "export GITHUB_STEP_SUMMARY=/ci/out/step-summary.md GITHUB_ENV=/ci/out/github-env GITHUB_OUTPUT=/ci/out/github-output",
                 "mkdir -p \"$RUNNER_TEMP\"; : > \"$GITHUB_ENV\"",
                 'ci_step() { local n=$1 name=$2 rc t0 t1; shift 2; t0=$(date +%s); echo "::group::$name"; "$@"; rc=$?; t1=$(date +%s);',
                 '  echo "::endgroup::"; if [ $rc -eq 0 ]; then echo "ci-local: step $n ok $((t1 - t0)) $name"; else ci_failed=1; echo "ci-local: step $n FAILED $((t1 - t0)) $name (status $rc)"; fi; }']
        for k, v in env.items():
            lines.append("export %s=%s" % (k, shlex.quote(v)))
        steps = job.get("steps") or []
        for n, st in enumerate(steps, 1):
            name = st.get("name") or st.get("uses") or "step %d" % n
            name = subst(name, ctx, env, root)
            senv = {k: subst(v, ctx, env, root) for k, v in (st.get("env") or {}).items()}
            cond = condition(st["if"]) if "if" in st else '[ "$ci_failed" = 0 ]'
            with_ = {k: subst(v, ctx, env, root) for k, v in (st.get("with") or {}).items()}
            sid = (st.get("id") or "").replace("-", "_")
            body = None
            if "run" in st:
                script = os.path.join(out, "step-%02d.sh" % n)
                with open(script, "w", encoding="utf-8") as f:
                    f.write(subst(st["run"], ctx, env, root))
                shell = st.get("shell", "bash")
                if shell != "bash":
                    raise Unsupported("the shell %r of step %r" % (shell, name))
                exports = " ".join("%s=%s" % (k, shlex.quote(v)) for k, v in senv.items())
                body = "env %s bash --noprofile --norc -eo pipefail /ci/job/step-%02d.sh" % (exports, n) if exports else \
                    "bash --noprofile --norc -eo pipefail /ci/job/step-%02d.sh" % n
            else:
                uses = st.get("uses", "")
                act = uses.split("@")[0]
                if act == "actions/checkout":
                    body = "true"
                elif act == "actions/cache":
                    key = hashlib.sha256(with_["key"].encode()).hexdigest()[:16]
                    path = with_["path"]
                    var = "ci_out_%s_cache_hit" % sid if sid else "ci_cache_unused"
                    body = "ci_cache_step %s %s %s" % (shlex.quote(path), key, var)
                elif act == "actions/upload-artifact":
                    paths = " ".join(shlex.quote(p) for p in str(with_.get("path", "")).split() if p)
                    body = "ci_upload %s %s" % (shlex.quote(with_["name"]), paths)
                elif act == "actions/download-artifact":
                    body = "ci_download %s %s" % (shlex.quote(with_["name"]), shlex.quote(with_.get("path", ".")))
                elif act.startswith("codecov/"):
                    body = "echo 'ci-local: skipped: %s uploads to an outside service'" % act
                else:
                    raise Unsupported("the action %r (step %r)" % (uses, name))
            lines.append("if %s; then ci_step %d %s %s; else echo %s; fi"
                         % (cond, n, shlex.quote(name), body, shlex.quote("ci-local: step %d skipped 0 %s" % (n, name))))
            if "run" in st:
                # a step's GITHUB_ENV lines reach the steps after it, as on GitHub
                lines.append('while IFS= read -r l; do case "$l" in *=*) export "$l" ;; esac; done < "$GITHUB_ENV"; : > "$GITHUB_ENV"')
        lines.append('for s in "${ci_saves[@]}"; do eval "$s"; done')
        lines.append('exit $ci_failed')
    except (Unsupported, KeyError) as e:
        sys.stderr.write("ci-local.py: %s: job %s: cannot rehearse %s\n" % (wf_path, job_name, e))
        return 2
    helpers = [
        '# actions/cache: restore PATH from /ci/cache/KEY, and save it there when the job ends green',
        'ci_cache_step() { local p=$1 k=$2 v=$3; if [ -d "/ci/cache/$k" ] && [ -n "$(ls -A "/ci/cache/$k" 2> /dev/null)" ]; then',
        '  sudo mkdir -p "$p" && sudo chown "$(id -u):$(id -g)" "$p" && cp -a "/ci/cache/$k/." "$p/" && eval "$v=true" && echo "ci-local: cache $k restored to $p";',
        '  else eval "$v=false"; echo "ci-local: cache $k missed"; ci_saves+=("[ \\"\\$ci_failed\\" = 0 ] && mkdir -p /ci/cache/$k && cp -a $p/. /ci/cache/$k/ && echo ci-local: cache $k saved"); fi; }',
        'ci_upload() { local n=$1 p; shift; mkdir -p "/ci/artifacts/$n"; for p in "$@"; do [ -e "$p" ] || { echo "ci-local: no $p to upload" >&2; return 1; }; cp -a --parents "$p" "/ci/artifacts/$n/"; done; }',
        'ci_download() { local n=$1 p=$2 f; [ -d "/ci/artifacts/$n" ] || { echo "ci-local: no artifact $n" >&2; return 1; }; mkdir -p "$p";',
        '  find "/ci/artifacts/$n" -type f | while IFS= read -r f; do cp -a "$f" "$p/"; done; }',
    ]
    # the helpers go after ci_step, before the job's environment and its first step
    first = next(i for i, l in enumerate(lines) if l.startswith("  echo \"::endgroup::\"")) + 1
    lines[first:first] = helpers
    with open(os.path.join(out, "job.sh"), "w", encoding="utf-8") as f:
        f.write("\n".join(l for l in lines if l != "") + "\n")
    print("runs-on %s" % subst(job.get("runs-on", ""), ctx, env, root))
    needs = job.get("needs") or []
    for n in ([needs] if isinstance(needs, str) else needs):
        print("needs %s" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
