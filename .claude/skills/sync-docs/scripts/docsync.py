"""Keep the RASM HTML docs (docs/index.html, docs/guides/, docs/api/) in sync with the code.

  python docsync.py status [--target main]   what changed in the code since the docs were last synced (in the
                                             working tree, or in --target), and which pages describe those files
  python docsync.py check                    problems in the pages: unknown names in <code>, missing paths,
                                             broken links and anchors, pages missing from site.js, public
                                             API names no API page mentions
  python docsync.py stamp [--rev main]       record that the docs now match the code at --rev, a commit on main
                                             (writes docs/assets/sync.js)

Each page lists the files it describes in <meta name="rasm-sources" content="a.cpp b.h">. The stamp in
docs/assets/sync.js is the full hash of the commit on main whose code the docs describe. Only main commits are
stamped: a branch commit leaves the history after a squash merge.
"""

import argparse
import glob
import html
import os
import re
import subprocess
import sys
from html.parser import HTMLParser

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
DOCS = os.path.join(REPO, "docs")
SYNC_JS = os.path.join(DOCS, "assets", "sync.js")
SITE_JS = os.path.join(DOCS, "assets", "site.js")
MAIN = "main"
CODE_DIRS = ("src", "include", "shaders", "examples", "tests", "cmake", "tools")
CODE_EXT = (".h", ".hpp", ".cpp", ".slang", ".txt", ".cmake", ".in", ".bat", ".json")
# Changes to these never make a page stale on their own, unless a page lists them in rasm-sources.
IGNORED = ("docs/", ".claude/", ".github/", "assets/", "ports/", "README.md", "CLAUDE.md", "LICENSE", ".gitignore", ".clang-format")


def git(*args):
    result = subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout.strip()


def rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def pages():
    files = [os.path.join(DOCS, "index.html")]
    files += sorted(glob.glob(os.path.join(DOCS, "guides", "*.html")))
    files += sorted(glob.glob(os.path.join(DOCS, "api", "*.html")))
    return [f for f in files if os.path.isfile(f)]


class Page(HTMLParser):
    """Collects the sources meta, ids, links and the text of <code> outside <pre>."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sources = None
        self.ids = set()
        self.links = []
        self.codes = []
        self.sigs = []
        self.text = []
        self._code = None
        self._pre = 0
        self._sig = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "meta" and a.get("name") == "rasm-sources":
            self.sources = (a.get("content") or "").split()
        if "id" in a:
            self.ids.add(a["id"])
        if tag == "a" and a.get("href"):
            self.links.append((a["href"], self.getpos()[0]))
        if tag == "pre":
            self._pre += 1
            if "sig" in (a.get("class") or "").split():
                self._sig = []
        if tag == "code" and self._pre == 0:
            self._code = []

    def handle_endtag(self, tag):
        if tag == "code" and self._code is not None:
            self.codes.append(("".join(self._code).strip(), self.getpos()[0]))
            self._code = None
        if tag == "pre":
            self._pre -= 1
            if self._sig is not None:
                self.sigs.append("".join(self._sig))
                self._sig = None

    def handle_data(self, data):
        self.text.append(data)
        if self._code is not None:
            self._code.append(data)
        if self._sig is not None:
            self._sig.append(data)


def parse(path):
    page = Page()
    with open(path, encoding="utf-8") as f:
        page.feed(f.read())
    return page


def read_stamp():
    if not os.path.isfile(SYNC_JS):
        return None
    with open(SYNC_JS, encoding="utf-8") as f:
        m = re.search(r'commit:\s*"([0-9a-f]+)"', f.read())
    return m.group(1) if m else None


def resolve(rev):
    code, full = git("rev-parse", "--verify", "--quiet", rev + "^{commit}")
    return full if code == 0 else None


def on_main(commit):
    code, _ = git("merge-base", "--is-ancestor", commit, MAIN)
    return code == 0


def covers(source, changed):
    source = source.rstrip("/")
    return changed == source or changed.startswith(source + "/")


# ---------------------------------------------------------------- status

def cmd_status(args):
    stamp = read_stamp()
    worktree = args.target is None
    rev = "HEAD" if worktree else args.target
    target = resolve(rev)
    if target is None:
        sys.exit(f"unknown target {rev}")
    label = "working tree" if worktree else rev
    print(f"docs stamp   : {stamp[:10] if stamp else '(none)'}")
    print(f"{label:<13}: {target[:10]}{' + uncommitted changes' if worktree else ''}")
    if not stamp:
        print(f"\nNo stamp: review every page against the code, then run `stamp --rev {MAIN}`.")
        return
    stamp = resolve(stamp)
    if stamp is None:
        print(f"\nThe stamped commit is not in this clone. Review every page, then run `stamp --rev {MAIN}`.")
        return

    _, log = git("log", "--oneline", f"{stamp}..{target}")
    if worktree:
        _, diff = git("diff", "--name-status", stamp)
        _, untracked = git("ls-files", "--others", "--exclude-standard")
        diff += "".join(f"\nA\t{p}" for p in untracked.splitlines())
    else:
        _, diff = git("diff", "--name-status", stamp, target)
    changed = []
    for line in diff.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        changed.append((parts[0][0], parts[-1]))
        if parts[0][0] == "R":
            changed.append(("D", parts[1]))

    parsed = {rel(p): parse(p) for p in pages()}
    hits = {}
    for _, path in changed:
        hits[path] = [name for name, page in parsed.items() if any(covers(s, path) for s in (page.sources or []))]
    changed = [(status, path) for status, path in changed if hits[path] or not path.startswith(IGNORED)]

    if worktree or not on_main(target):
        next_step = f"commit, merge, then run `status --target {MAIN}` and `stamp --rev {MAIN}` on {MAIN} (stamp only main commits)."
    else:
        next_step = f"run: python {rel(__file__)} stamp --rev {rev}"
    if not changed:
        print("\nUp to date: no code changed since the stamp.")
        if stamp != target:
            print(f"To move the stamp, {next_step}")
        return

    print(f"\nCommits since the stamp ({len(log.splitlines()) if log else 0}):")
    for line in log.splitlines()[:40]:
        print("  " + line)

    print("\nChanged code -> pages that describe it:")
    uncovered = []
    stale = {}
    for status, path in changed:
        for h in hits[path]:
            stale.setdefault(h, []).append(path)
        print(f"  {status} {path}")
        for h in hits[path]:
            print(f"      -> {h}")
        if not hits[path]:
            uncovered.append((status, path))

    print("\nPages to review:")
    for name in sorted(stale):
        print(f"  {name}  ({len(stale[name])} changed sources)")
    if uncovered:
        print("\nChanged files no page lists in rasm-sources (new topic? add it to a page's sources, or say why not):")
        for status, path in uncovered:
            print(f"  {status} {path}")
    print(f"\nWhen every page matches the code, {next_step}")


# ---------------------------------------------------------------- check

def code_corpus():
    words = set()
    files = set()
    for d in CODE_DIRS:
        for path in glob.glob(os.path.join(REPO, d, "**", "*"), recursive=True):
            if not os.path.isfile(path):
                continue
            files.add(rel(path))
            if os.path.splitext(path)[1] in CODE_EXT:
                with open(path, encoding="utf-8", errors="replace") as f:
                    words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", f.read()))
    for pattern in ("*", "docs/**/*", ".claude/**/*"):
        for path in glob.glob(os.path.join(REPO, pattern), recursive=True):
            if os.path.isfile(path):
                files.add(rel(path))
                if pattern == "*" and os.path.splitext(path)[1] in CODE_EXT:
                    with open(path, encoding="utf-8", errors="replace") as f:
                        words.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", f.read()))
    return words, files


IDENT = re.compile(r"^(?:[A-Za-z_][A-Za-z0-9_]*::)*~?[A-Za-z_][A-Za-z0-9_]*(?:\(\))?$")
PATHLIKE = re.compile(r"^[\w.\-]+(/[\w.\-]+)+/?$|^[\w\-]+\.(h|hpp|cpp|slang|txt|cmake|in|bat|json|md|py)$")


def check_code_names(name, page, words, files, problems):
    for text, line in page.codes:
        t = html.unescape(text)
        if PATHLIKE.match(t):
            if "/" in t:
                candidates = [t.rstrip("/"), "src/" + t, "include/" + t, "shaders/" + t, "include/rasm/" + t]
                if not any(c in files or os.path.isdir(os.path.join(REPO, c)) for c in candidates):
                    problems.append(f"{name}:{line}: path `{t}` does not exist")
            elif not any(f.endswith("/" + t) or f == t for f in files):
                problems.append(f"{name}:{line}: file `{t}` does not exist")
            continue
        if not IDENT.match(t) or re.match(r"^[BDPC][0-9]{1,3}$", t):
            continue
        for part in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", t):
            if len(part) < 3 or part in words:
                continue
            problems.append(f"{name}:{line}: `{t}`: `{part}` is not in the code")


def check_links(path, name, page, parsed, problems):
    for href, line in page.links:
        if re.match(r"^[a-z]+:", href) or href.startswith("//"):
            continue
        target, _, anchor = href.partition("#")
        if target:
            full = os.path.normpath(os.path.join(os.path.dirname(path), target))
            if not os.path.exists(full):
                problems.append(f"{name}:{line}: link `{href}`: file does not exist")
                continue
            target_page = parsed.get(rel(full))
        else:
            target_page = page
        if anchor and target_page is not None and anchor not in target_page.ids:
            problems.append(f"{name}:{line}: link `{href}`: no id `{anchor}`")


SNIPPET = re.compile(r'<pre><code(?P<attr>[^>]*)>(?P<body>.*?)</code></pre>', re.S)


def snippet_language(attr, text):
    """The same choice docs/assets/site.js makes for highlighting."""
    m = re.search(r"lang-(\w+)", attr)
    if m:
        return m.group(1)
    if re.search(r"^\s*(cmake_minimum_required|find_package|target_link_libraries|add_executable|add_subdirectory)\s*\(", text, re.M):
        return "cmake"
    if re.search(r"^\s*(cmake|ctest|git|python|pip|vcpkg|\.\\|\./)\s", text, re.M) and not re.search(r";\s*$", text, re.M):
        return "shell"
    return "cpp"


def check_snippet_format(path, name, problems):
    """C++ snippets must look like RASM .cpp code: unchanged by clang-format with the repo's .clang-format."""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    for m in SNIPPET.finditer(src):
        text = html.unescape(m.group("body"))
        if snippet_language(m.group("attr"), text) not in ("cpp", "slang"):
            continue
        try:
            result = subprocess.run(["clang-format", "--style=file", "--assume-filename=" + os.path.join(REPO, "snippet.cpp")],
                                    input=text, capture_output=True, text=True, encoding="utf-8", cwd=REPO)
        except FileNotFoundError:
            problems.append("clang-format not found: snippet formatting was not checked")
            return False
        if result.returncode != 0:
            error = (result.stderr.strip().splitlines() or ["no message"])[0]
            problems.append(f"clang-format failed ({error}): snippet formatting was not checked")
            return False
        if result.stdout.rstrip("\n") != text.rstrip("\n"):
            line = src.count("\n", 0, m.start()) + 1
            first = text.strip().splitlines()[0][:60] if text.strip() else ""
            problems.append(f"{name}:{line}: code snippet is not clang-formatted (`{first}`)")
    return True


def public_api_names():
    """Public function, type and constant names declared in the public headers. Protected and private
    sections (up to the class's closing brace at namespace indentation) and forward declarations are skipped."""
    names = {}
    skip = {"if", "for", "while", "return", "switch", "sizeof", "operator", "static_cast", "decltype"}
    for path in sorted(glob.glob(os.path.join(REPO, "include", "rasm", "**", "*.h"), recursive=True)):
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        src = re.sub(r"//[^\n]*", "", src)
        src = re.sub(r"\b(?:protected|private)\s*:.*?^    \};", "", src, flags=re.S | re.M)
        found = set(re.findall(r"\b(?:struct|class|enum class|enum|using)\s+([A-Za-z_]\w*)\b(?!\s*;)", src))
        found |= set(re.findall(r"^\s*(?:\[\[nodiscard\]\]\s*)?(?:(?:static|virtual|inline|explicit|constexpr|const)\s+)*"
                                r"[\w:<>]+(?:<[^>]*>)?\s*[&*]?\s+[&*]?([A-Za-z_]\w*)\s*\(", src, flags=re.M))
        found |= set(re.findall(r"\bconstexpr\s+[\w:]+\s+([A-Z][A-Z0-9_]{3,})\s*=", src))
        for n in found:
            if n not in skip and not n.startswith("_"):
                names.setdefault(n, rel(path))
    return names


def cmd_check(args):
    words, files = code_corpus()
    paths = pages()
    parsed = {rel(p): parse(p) for p in paths}
    problems = []
    formatter = True

    with open(SITE_JS, encoding="utf-8") as f:
        listed = set(re.findall(r'path:\s*"([^"]+)"', f.read()))
    for p in paths:
        name = rel(p)
        page = parsed[name]
        short = os.path.relpath(p, DOCS).replace("\\", "/")
        if short not in listed:
            problems.append(f"{name}: not listed in docs/assets/site.js SECTIONS")
        if page.sources is None:
            problems.append(f"{name}: no <meta name=\"rasm-sources\">")
        for s in page.sources or []:
            if not os.path.exists(os.path.join(REPO, s)):
                problems.append(f"{name}: rasm-sources `{s}` does not exist")
        check_code_names(name, page, words, files, problems)
        check_links(p, name, page, parsed, problems)
        if formatter:
            formatter = check_snippet_format(p, name, problems)
    for short in sorted(listed):
        if not os.path.isfile(os.path.join(DOCS, short.partition("#")[0])):
            problems.append(f"docs/assets/site.js: lists `{short}`, which does not exist")

    api_text = " ".join(" ".join(pg.text) + " " + " ".join(pg.ids) for n, pg in parsed.items() if n.startswith("docs/api/"))
    api_words = set(re.findall(r"[A-Za-z_]\w*", api_text))
    missing = sorted((h, n) for n, h in public_api_names().items() if n not in api_words)
    if missing:
        problems.append("public API names no docs/api page mentions:")
        for header, n in missing:
            problems.append(f"    {n}  ({header})")

    if problems:
        print("\n".join(problems))
        print(f"\n{len([p for p in problems if not p.startswith('    ')])} problem(s).")
        sys.exit(1)
    print(f"OK: {len(paths)} pages checked.")


# ---------------------------------------------------------------- stamp

def cmd_stamp(args):
    commit = resolve(args.rev)
    if commit is None:
        sys.exit(f"unknown revision {args.rev}")
    if not on_main(commit):
        sys.exit(f"{args.rev} is not on {MAIN}: stamp only main commits (a branch commit is lost in a squash merge)")
    _, date = git("log", "-1", "--format=%cs", commit)
    with open(SYNC_JS, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Written by .claude/skills/sync-docs/scripts/docsync.py. Do not edit by hand.\n")
        f.write(f'window.RASM_DOCS_SYNC = {{ commit: "{commit}", date: "{date}" }};\n')
    print(f"stamped {commit[:10]} ({date})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("--target", default=None, help="commit or branch to compare with (default: the working tree)")
    sub.add_parser("check")
    st = sub.add_parser("stamp")
    st.add_argument("--rev", default=MAIN)
    args = parser.parse_args()
    {"status": cmd_status, "check": cmd_check, "stamp": cmd_stamp}[args.cmd](args)


if __name__ == "__main__":
    main()
