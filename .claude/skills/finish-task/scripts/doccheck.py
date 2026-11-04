"""Find docs that no longer match the code, for the RASM finish-task skill.

  python doccheck.py [--base main]

Reports:
  1. files changed since the merge-base with --base (committed, uncommitted and untracked)
  2. code links in docs ([x](../src/a.cpp#L10-L20)) to missing files or lines past the end
  2b. live code links with line numbers into files the change edited, whose lines may have shifted
  3. backticked repo paths (`src/...`, `shaders/...`) that do not exist
  4. names removed by the change that the docs still mention
  5. review IDs (B1, D4, ...) named by the branch's commits or diff, with their status in
     code_review.md and development_plan.md
Mentions inside a code_review.md entry marked Fixed/Resolved, or a done `- [x]` plan item, are labelled
"historical": they describe the code as it was, so they may stay as they are.
"""

import glob
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
CODE_DIRS = ("src", "include", "shaders", "tests", "examples", "cmake", "tools")
DOCS = sorted(glob.glob(os.path.join(REPO, "docs", "*.md"))) + [os.path.join(REPO, "CLAUDE.md")] + \
    sorted(glob.glob(os.path.join(REPO, ".claude", "skills", "**", "*.md"), recursive=True))
ID_RE = re.compile(r"\b([BDPC][0-9]{1,3})\b")
LINK_RE = re.compile(r"\]\(((?:\.\./|\./)?[^)#\s]+\.(?:h|hpp|cpp|slang|txt|cmake|md|json|in|bat))(?:#L(\d+)(?:-L(\d+))?)?\)")
DONE_RE = re.compile(r"\*(Fixed|Resolved|Partly fixed|Partly resolved|Mostly fixed|Mostly resolved|Worked around|Extended)")


def git(*args):
    return subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True, text=True, errors="replace").stdout


def rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def line_count(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return sum(1 for _ in f)


def code_text():
    chunks = []
    for d in CODE_DIRS:
        for path in glob.glob(os.path.join(REPO, d, "**", "*"), recursive=True):
            if os.path.isfile(path) and os.path.splitext(path)[1] in (".h", ".hpp", ".cpp", ".slang", ".txt", ".cmake", ".in", ".bat"):
                with open(path, encoding="utf-8", errors="replace") as f:
                    chunks.append(f.read())
    return "\n".join(chunks)


def doc_lines(path):
    """(line number, text, historical) per line. In code_review.md a line is historical when it, or the
    entry header it belongs to (the last line starting with a bold ID), carries a Fixed/Resolved marker."""
    out = []
    header_done = False
    review = path.endswith("code_review.md")
    plan = path.endswith("development_plan.md")
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if plan:
                out.append((n, line, line.startswith("- [x]")))
                continue
            if re.match(r"^(- )?\*\*[BDPC][0-9]+", line):
                header_done = bool(DONE_RE.search(line))
                historical = header_done
            elif line.startswith("#"):
                header_done = False
                historical = False
            else:
                historical = header_done or bool(DONE_RE.search(line))
            out.append((n, line, review and historical))
    return out


def check_links():
    problems = []
    for doc in DOCS:
        base = os.path.dirname(doc)
        for n, line, historical in doc_lines(doc):
            tag = " (historical)" if historical else ""
            for m in LINK_RE.finditer(re.sub(r"`[^`]*`", "", line)):
                target = os.path.normpath(os.path.join(base, m.group(1)))
                if not os.path.exists(target):
                    problems.append("%s:%d  missing file %s%s" % (rel(doc), n, m.group(1), tag))
                elif m.group(2):
                    last = int(m.group(3) or m.group(2))
                    count = line_count(target)
                    if last > count:
                        problems.append("%s:%d  %s#L%s past end (%d lines)%s" % (rel(doc), n, m.group(1), m.group(3) or m.group(2), count, tag))
    return problems


def links_into(changed):
    """Live links whose target file this change edited: line numbers may have shifted without going past the end."""
    out = []
    for doc in DOCS:
        base = os.path.dirname(doc)
        for n, line, historical in doc_lines(doc):
            if historical:
                continue
            for m in LINK_RE.finditer(re.sub(r"`[^`]*`", "", line)):
                if not m.group(2):
                    continue
                target = rel(os.path.normpath(os.path.join(base, m.group(1))))
                if target in changed and os.path.exists(os.path.join(REPO, target)):
                    out.append("%s:%d  %s#L%s%s" % (rel(doc), n, target, m.group(2), "-L" + m.group(3) if m.group(3) else ""))
    return out


def check_paths():
    problems = []
    pat = re.compile(r"`((?:%s)/[A-Za-z0-9_./\-]+)`" % "|".join(CODE_DIRS))
    for doc in DOCS:
        for n, line, historical in doc_lines(doc):
            for m in pat.finditer(line):
                p = m.group(1).rstrip(".")
                if p.endswith("/") or "NN_" in p:
                    continue
                if not os.path.exists(os.path.join(REPO, p)):
                    problems.append("%s:%d  `%s` does not exist%s" % (rel(doc), n, p, " (historical)" if historical else ""))
    return problems


def removed_names(diff, code):
    names = set()
    for line in diff.splitlines():
        if not line.startswith("-") or line.startswith("---"):
            continue
        line = re.sub(r"//.*|/\*.*?\*/|\"(?:[^\"\\]|\\.)*\"", "", line)
        if line.lstrip("- \t").startswith("*"):
            continue
        for m in re.finditer(r"\b(?:struct|class|enum class|enum)\s+([A-Za-z_]\w+)", line):
            names.add(m.group(1))
        for m in re.finditer(r"\b([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\(", line):
            names.add(m.group(1).split("::")[-1])
    keywords = {"if", "for", "while", "switch", "return", "sizeof", "static_assert", "assert", "offsetof", "static_cast", "reinterpret_cast", "const_cast", "defined"}
    gone = []
    for n in sorted(names):
        if len(n) < 4 or n in keywords:
            continue
        if not re.search(r"\b%s\b" % re.escape(n), code):
            gone.append(n)
    return gone


def stale_mentions(gone):
    hits = []
    for doc in DOCS:
        for i, line, historical in doc_lines(doc):
            for n in gone:
                if re.search(r"`(?:[\w:]*::)?%s(?:\(\))?`" % re.escape(n), line):
                    hits.append("%s:%d  mentions removed `%s`%s" % (rel(doc), i, n, " (historical)" if historical else ""))
    return hits


def id_status(ids):
    review = open(os.path.join(REPO, "docs", "code_review.md"), encoding="utf-8").read().splitlines()
    plan = open(os.path.join(REPO, "docs", "development_plan.md"), encoding="utf-8").read().splitlines()
    out = []
    for i in sorted(ids, key=lambda s: (s[0], int(s[1:]))):
        entry = next((l for l in review if re.match(r"^(- )?\*\*%s\b" % i, l)), None)
        if entry is None:
            out.append("%-4s  not found in code_review.md" % i)
            continue
        status = DONE_RE.search(entry)
        state = entry[status.start():entry.find(":*", status.start()) + 1].strip("*") if status else "open"
        plan_lines = [l.strip() for l in plan if re.search(r"`%s`" % i, l)]
        boxes = ", ".join("[x]" if l.startswith("- [x]") else "[ ]" if l.startswith("- [ ]") else "text" for l in plan_lines) or "not in plan"
        out.append("%-4s  review: %-38s plan: %s" % (i, state[:38], boxes))
    return out


def report(title, items):
    live = [p for p in items if not p.endswith("(historical)")]
    print("\n%s: %d (+%d historical)" % (title, len(live), len(items) - len(live)))
    for p in live + [p for p in items if p.endswith("(historical)")]:
        print("   " + p)


def main(args):
    base_ref = args[args.index("--base") + 1] if "--base" in args else "main"
    base = git("merge-base", "HEAD", base_ref).strip() or "HEAD"
    if base == git("rev-parse", "HEAD").strip():
        base = "HEAD"
    diff = git("diff", base) + git("diff", "--cached")
    changed = sorted(set(git("diff", "--name-only", base).split() + git("diff", "--name-only", "--cached").split() +
                         git("ls-files", "--others", "--exclude-standard").split()))
    log = git("log", "--format=%s%n%b", "%s..HEAD" % base) if base != "HEAD" else ""
    print("base: %s (%s)" % (base_ref, base[:10]))
    print("\n1. changed files (%d)" % len(changed))
    for f in changed:
        print("   " + f)
    code = code_text()
    report("2. broken code links", check_links())
    shifted = links_into(set(changed) - {rel(d) for d in DOCS})
    print("\n2b. links into files this change edited (check the lines still match): %d" % len(shifted))
    for s in shifted:
        print("   " + s)
    report("3. missing paths", check_paths())
    report("4. docs mentioning removed names", stale_mentions(removed_names(diff, code)))
    ids = set(ID_RE.findall(log))
    for line in diff.splitlines():
        if line.startswith(("+", "-")) and not line.startswith(("+++", "---")):
            ids.update(m for m in ID_RE.findall(line) if "`%s`" % m in line or "**%s" % m in line)
    print("\n5. review IDs in this change: %d" % len(ids))
    for l in id_status(ids):
        print("   " + l)


if __name__ == "__main__":
    if "-h" in sys.argv or "--help" in sys.argv:
        print(__doc__)
        sys.exit(0)
    main(sys.argv[1:])
