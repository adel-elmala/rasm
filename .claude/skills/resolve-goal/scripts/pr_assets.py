"""Pictures for a PR description: bake guide-style SVGs into standalone files, preview them, publish them.

    python .claude/skills/resolve-goal/scripts/pr_assets.py preview <file> [<file> ...]
    python .claude/skills/resolve-goal/scripts/pr_assets.py publish <branch> <file> [<file> ...] [--width 860]

SVGs are drawn like the figures of docs/guides: classes from docs/assets/style.css, arrowheads #ah, #ah-gpu, ...
(see .claude/skills/sync-docs/visuals.md). A figure can be copied from a guide as it is. GitHub shows an SVG as an
image without the page's stylesheet, so each SVG is baked first: the theme tokens (light, and dark under
prefers-color-scheme), the `svg ...` rules of style.css, the arrowhead markers, a margin and a background card go into
the file. An SVG that already has its own <style> is published as it is. PNG and JPG files are published as they are.

preview  bakes into build/pr-assets/ and renders each SVG in both themes with headless Chrome. Read the PNGs.
publish  bakes, then commits the files to <branch with / as ->/ on the orphan `pr-assets` branch from a temporary
         worktree (the current checkout is not touched), pushes, and prints one <img> tag per file. The URLs name
         the commit hash, so they keep working after the PR branch is deleted. Publish again to replace a picture.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

BRANCH = "pr-assets"
ALLOWED = {".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp"}
MAX_BYTES = 5 * 1024 * 1024
MARGIN = 16
ROOT = Path(__file__).resolve().parents[4]
STYLE = ROOT / "docs" / "assets" / "style.css"
OUT = ROOT / "build" / "pr-assets"
ARROWS = ["", "cpu", "gpu", "mem", "shader", "hot", "ok", "bad", "light"]
CHROMES = [
    os.environ.get("CHROME", ""),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    shutil.which("google-chrome") or "",
    shutil.which("chromium") or "",
]


def git(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def block(css: str, opener: str) -> str:
    start = css.index(opener) + len(opener)
    return css[start:css.index("}", start)].strip()


def guide_css(theme: str) -> str:
    css = STYLE.read_text(encoding="utf-8")
    light = block(css, ":root {")
    dark = block(css, ':root:not([data-theme="light"]) {')
    rules = "\n".join(line for line in css.splitlines() if re.match(r"(svg[\s.:]|@keyframes flowing)", line))
    if theme == "light":
        tokens = f":root {{ {light} }}"
    elif theme == "dark":
        tokens = f":root {{ {light} }}\n:root {{ {dark} }}"
    else:
        tokens = f":root {{ {light} }}\n@media (prefers-color-scheme: dark) {{ :root {{ {dark} }} }}"
    return f"{tokens}\nsvg {{ font-family: var(--sans); }}\n{rules}\n.card {{ fill: var(--bg-raised); stroke: var(--line); stroke-width: 1; }}"


def bake(svg: str, theme: str) -> str:
    if re.search(r"<style[\s>]", svg):
        return svg
    open_tag = re.search(r"<svg\b[^>]*>", svg)
    box = re.search(r'viewBox="\s*([-\d.]+)[\s,]+([-\d.]+)[\s,]+([-\d.]+)[\s,]+([-\d.]+)\s*"', open_tag.group(0)) if open_tag else None
    if not box:
        sys.exit("an SVG needs an <svg ... viewBox=\"x y w h\"> tag")
    x, y, w, h = (float(v) for v in box.groups())
    x, y, w, h = x - MARGIN, y - MARGIN, w + 2 * MARGIN, h + 2 * MARGIN
    attrs = re.sub(r'\s(viewBox|width|height|xmlns|class|style)="[^"]*"', "", open_tag.group(0)[4:-1])
    head = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x:g} {y:g} {w:g} {h:g}" width="{w:g}" height="{h:g}"{attrs}>'
    markers = "".join(
        f'<marker id="ah{"-" + c if c else ""}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
        f'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" class="head{" " + c if c else ""}"/></marker>'
        for c in ARROWS
    )
    card = f'<rect class="card" x="{x + 0.5:g}" y="{y + 0.5:g}" width="{w - 1:g}" height="{h - 1:g}" rx="12"/>'
    body = svg[open_tag.end():]
    return f'{head}<style>{guide_css(theme)}</style><defs>{markers}</defs>{card}{body}'


def check_files(files: list[Path]) -> None:
    for f in files:
        if not f.is_file():
            sys.exit(f"no such file: {f}")
        if f.suffix.lower() not in ALLOWED:
            sys.exit(f"{f}: only {', '.join(sorted(ALLOWED))} files")
        if f.stat().st_size > MAX_BYTES:
            sys.exit(f"{f}: larger than 5 MB; downscale it or save it as .jpg")
        if f.suffix.lower() == ".svg" and re.search(r"<script|<foreignObject|xlink:href=\"http|href=\"http", f.read_text(encoding="utf-8"), re.I):
            sys.exit(f"{f}: GitHub blocks scripts, foreignObject and external links in SVG images")


def baked_copies(files: list[Path], folder: Path) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    out = []
    for f in files:
        target = folder / f.name
        if f.suffix.lower() == ".svg":
            target.write_text(bake(f.read_text(encoding="utf-8"), "auto"), encoding="utf-8")
        else:
            shutil.copyfile(f, target)
        out.append(target)
    return out


def preview(files: list[Path]) -> None:
    chrome = next((c for c in CHROMES if c and Path(c).is_file()), None)
    folder = OUT / "preview"
    for f in files:
        if f.suffix.lower() != ".svg":
            continue
        svg = f.read_text(encoding="utf-8")
        for theme in ("light", "dark"):
            baked = folder / f"{f.stem}.{theme}.svg"
            folder.mkdir(parents=True, exist_ok=True)
            baked.write_text(bake(svg, theme), encoding="utf-8")
            size = re.search(r'width="([\d.]+)" height="([\d.]+)"', baked.read_text(encoding="utf-8"))
            png = baked.with_suffix(".png")
            if not chrome:
                print(f"{baked}  (no Chrome found: set CHROME to render a PNG)")
                continue
            w, h = (round(float(v)) for v in size.groups()) if size else (960, 600)
            subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--window-size={w},{h}",
                            f"--screenshot={png}", baked.as_uri()], capture_output=True)
            print(png if png.is_file() else f"{baked}  (Chrome did not write a PNG)")


def publish(branch: str, files: list[Path], width: int) -> None:
    folder = branch.replace("/", "-")
    staged = baked_copies(files, OUT / folder)
    git("fetch", "-q", "origin", BRANCH)
    work = Path(tempfile.mkdtemp(prefix="rasm-pr-assets-"))
    try:
        git("worktree", "add", "-q", "--detach", str(work), f"origin/{BRANCH}")
        (work / folder).mkdir(exist_ok=True)
        for f in staged:
            shutil.copyfile(f, work / folder / f.name)
        git("add", folder, cwd=work)
        if git("status", "--porcelain", cwd=work):
            git("commit", "-q", "-m", f"{folder}: {', '.join(f.name for f in staged)}", cwd=work)
            if subprocess.run(["git", "push", "-q", "origin", f"HEAD:{BRANCH}"], cwd=work, capture_output=True).returncode != 0:
                git("pull", "-q", "--rebase", "origin", BRANCH, cwd=work)
                git("push", "-q", "origin", f"HEAD:{BRANCH}", cwd=work)
        sha = git("rev-parse", "HEAD", cwd=work)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(work)], capture_output=True)
        shutil.rmtree(work, ignore_errors=True)

    url = git("remote", "get-url", "origin")
    slug = url.removesuffix(".git").replace("git@github.com:", "").split("github.com/")[-1]
    for f in staged:
        print(f'<img src="https://raw.githubusercontent.com/{slug}/{sha}/{folder}/{f.name}" width="{width}" alt="{f.stem}">')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("preview")
    p.add_argument("files", nargs="+", type=Path)
    p = sub.add_parser("publish")
    p.add_argument("branch")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("--width", type=int, default=860, help="width attribute of the printed <img> tags")
    args = parser.parse_args()

    check_files(args.files)
    if args.command == "preview":
        preview(args.files)
    else:
        publish(args.branch, args.files, args.width)


if __name__ == "__main__":
    main()
