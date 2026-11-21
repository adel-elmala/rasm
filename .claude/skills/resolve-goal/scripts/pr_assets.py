"""Publish images for a PR description to the orphan `pr-assets` branch and print <img> tags for them.

    python .claude/skills/resolve-goal/scripts/pr_assets.py <folder> <file> [<file> ...] [--width 860]

<folder> is the PR branch with "/" replaced by "-" (feat/gltfCompleteness -> feat-gltfCompleteness). The files are
copied into <folder>/ on pr-assets, committed and pushed from a temporary worktree, so the current checkout and its
branch are not touched. The printed URLs name the commit hash, so they keep working after the PR branch is deleted.
Publishing a file with the same name again replaces it in a new commit; use the new tags.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

BRANCH = "pr-assets"
ALLOWED = {".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp"}
MAX_BYTES = 5 * 1024 * 1024


def git(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def repo_slug() -> str:
    url = git("remote", "get-url", "origin")
    slug = url.removesuffix(".git").replace("git@github.com:", "").split("github.com/")[-1]
    if slug.count("/") != 1:
        sys.exit(f"origin is not a GitHub repo URL: {url}")
    return slug


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--width", type=int, default=860, help="width attribute of the printed <img> tags")
    args = parser.parse_args()

    folder = args.folder.replace("/", "-")
    for f in args.files:
        if not f.is_file():
            sys.exit(f"no such file: {f}")
        if f.suffix.lower() not in ALLOWED:
            sys.exit(f"{f}: only {', '.join(sorted(ALLOWED))} files")
        if f.stat().st_size > MAX_BYTES:
            sys.exit(f"{f}: larger than 5 MB; downscale it or save it as .jpg")
        if f.suffix.lower() == ".svg" and "<script" in f.read_text(encoding="utf-8", errors="ignore").lower():
            sys.exit(f"{f}: GitHub blocks scripts in SVG images; remove the <script>")

    git("fetch", "-q", "origin", BRANCH)
    work = Path(tempfile.mkdtemp(prefix="rasm-pr-assets-"))
    try:
        git("worktree", "add", "-q", "--detach", str(work), f"origin/{BRANCH}")
        target = work / folder
        target.mkdir(exist_ok=True)
        for f in args.files:
            shutil.copyfile(f, target / f.name)
        git("add", folder, cwd=work)
        if git("status", "--porcelain", cwd=work):
            git("commit", "-q", "-m", f"{folder}: {', '.join(f.name for f in args.files)}", cwd=work)
            pushed = subprocess.run(["git", "push", "-q", "origin", f"HEAD:{BRANCH}"], cwd=work, capture_output=True, text=True)
            if pushed.returncode != 0:
                git("pull", "-q", "--rebase", "origin", BRANCH, cwd=work)
                git("push", "-q", "origin", f"HEAD:{BRANCH}", cwd=work)
        sha = git("rev-parse", "HEAD", cwd=work)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(work)], capture_output=True)
        shutil.rmtree(work, ignore_errors=True)

    slug = repo_slug()
    for f in args.files:
        url = f"https://raw.githubusercontent.com/{slug}/{sha}/{folder}/{f.name}"
        print(f'<img src="{url}" width="{args.width}" alt="{f.stem}">')


if __name__ == "__main__":
    main()
