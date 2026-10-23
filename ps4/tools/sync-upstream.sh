#!/usr/bin/env bash
# Merges upstream TriAevum (coccofresco/TriAevum main) into this PS4 fork.
# Run from a clone of the fork; it stops before committing when a real
# conflict needs a hand. Paths listed in ps4/sync/removed-paths.txt are
# deleted again, so upstream changes to other platforms never conflict.
#   ps4/tools/sync-upstream.sh
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
cd "$root"
git remote get-url upstream >/dev/null 2>&1 ||
    git remote add upstream https://github.com/coccofresco/TriAevum
git fetch -q upstream
behind="$(git rev-list --count HEAD..upstream/main)"
[ "$behind" -gt 0 ] || { echo "already up to date with upstream"; exit 0; }
echo "merging $behind upstream commits"
git merge --no-commit --no-ff upstream/main || true
grep -v '^\s*#' ps4/sync/removed-paths.txt | sed '/^\s*$/d' | while read -r path; do
    git rm -r -q --ignore-unmatch -- "$path" >/dev/null
done
left="$(git diff --name-only --diff-filter=U)"
if [ -n "$left" ]; then
    echo "resolve these conflicts, then git commit:" >&2
    echo "$left" >&2
    exit 1
fi
git commit -q -m "Merge upstream TriAevum ($behind commits) into the PS4 fork"
echo "merged; rebuild the closure (docs/FASE3-PRODUTO.md) and the product before pushing"
