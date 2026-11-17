#!/usr/bin/env python3
"""readme-facts.py OLD.md NEW.md: the facts a prose edit must keep.

  python3 tools/readme-facts.py OLD.md NEW.md

A rewrite of a Markdown document's prose (the README's above all) should
change words, never facts. This lists, between two versions, every fact
one has and the other lacks:

- inline code spans (`x.sum()`, ``a `b` c``): names, flags, messages;
- numbers in the prose outside code spans (3, 1.25, 10,438);
- link targets ((#anchor), (path), (https://...));
- the fenced code blocks, compared in order, byte for byte.

A span, number or link is "lost" when OLD has it and NEW has none of it,
"new" the other way (counts are not compared: a fact said twice and then
once is kept). Prints one section per kind and a last line `FACTS SAME`
or `FACTS DIFFER: N lost, M new, code blocks ...`; status 0 when nothing
differs, 1 when something does, 2 on a usage error. The lists are what a
reviewer reads by hand: a lost span may be a moved sentence's, a new
number a correction.
"""
import re
import sys
from collections import Counter

SPAN = re.compile(r"``(.+?)``|`([^`]+)`")
NUMBER = re.compile(r"(?<![\w.])\d[\d,.]*\d|(?<![\w.])\d")
LINK = re.compile(r"\]\(([^)]+)\)")


def facts(path):
    """(spans, numbers, links, code blocks) of one Markdown file."""
    spans, nums, links, code, cur, fenced = Counter(), Counter(), Counter(), [], [], False
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    for line in lines:
        if line.lstrip().startswith("```"):
            if fenced:
                code.append("\n".join(cur))
                cur = []
            fenced = not fenced
            continue
        if fenced:
            cur.append(line)
            continue
        for m in SPAN.finditer(line):
            spans[(m.group(1) or m.group(2)).strip()] += 1
        nums.update(NUMBER.findall(SPAN.sub("", line)))
        links.update(LINK.findall(line))
    if fenced:
        code.append("\n".join(cur))  # an unclosed fence: its text is still a block
    return spans, nums, links, code


def main(argv):
    if len(argv) != 3 or argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(argv) == 2 and argv[1] in ("-h", "--help") else 2
    try:
        old, new = facts(argv[1]), facts(argv[2])
    except OSError as e:
        print("readme-facts.py: %s" % e, file=sys.stderr)
        return 2
    lost = gained = 0
    for name, i in (("spans", 0), ("numbers", 1), ("links", 2)):
        a = sorted(k for k in old[i] if k not in new[i])
        b = sorted(k for k in new[i] if k not in old[i])
        lost += len(a)
        gained += len(b)
        print("%s lost (%d):" % (name, len(a)))
        for k in a:
            print("  %s" % k)
        print("%s new (%d):" % (name, len(b)))
        for k in b:
            print("  %s" % k)
    oc, nc = old[3], new[3]
    changed = [i + 1 for i in range(min(len(oc), len(nc))) if oc[i] != nc[i]]
    same_code = oc == nc
    print("code blocks: %d and %d; %s" % (len(oc), len(nc), "identical" if same_code else
                                          "differ (changed in order: %s%s)" % (", ".join(map(str, changed[:20])) or "none",
                                                                              "; the counts differ" if len(oc) != len(nc) else "")))
    if not lost and not gained and same_code:
        print("FACTS SAME")
        return 0
    print("FACTS DIFFER: %d lost, %d new, code blocks %s" % (lost, gained, "identical" if same_code else "differ"))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
