# pr-assets

Images for pull request descriptions: SVG diagrams and before/after screenshots. One folder per PR branch,
for example `feat-gltfCompleteness/`.

This branch is never merged and has no history in common with `main`. PR bodies link the files by commit hash
(`https://raw.githubusercontent.com/adel-elmala/rasm/<sha>/<folder>/<file>`), so the images keep working after a PR
branch is deleted. Add files with `.claude/skills/resolve-goal/scripts/pr_assets.py`, not by hand.
