#!/usr/bin/env bash
#
# release.bash — cut a GitHub Release for the current Voilà toolchain version.
#
# The version is whatever the compiler reports (`voila version`), so the tag,
# the release title, and the shipped binary can never disagree. The release
# carries the source tarball (buildable with only a C compiler) and every
# native binary build_voila.bash could produce on this machine.
#
#     ./release.bash             build, tag v<version>, publish the release
#     ./release.bash --dry-run   do everything except push the tag or publish
#
# Requirements: the GitHub CLI `gh`, an `origin` remote, and `gh auth login`.
set -euo pipefail
cd "$(dirname "$0")"

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '\033[32m  ✓ %s\033[0m\n' "$1"; }
die()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$1" >&2; exit 1; }

# ---- preconditions -----------------------------------------------------------
command -v gh >/dev/null 2>&1 \
    || die "the GitHub CLI 'gh' is required — https://cli.github.com"
git rev-parse --git-dir >/dev/null 2>&1 \
    || die "not a git repository"
git remote get-url origin >/dev/null 2>&1 \
    || die "no 'origin' remote — add one: git remote add origin <url>"
if [ "$DRY" -eq 0 ]; then
    gh auth status >/dev/null 2>&1 \
        || die "gh is not authenticated — run: gh auth login"
fi
if [ -n "$(git status --porcelain)" ]; then
    die "the working tree is dirty — commit or stash before releasing (a tag must mean something)"
fi

# ---- 1. build, and read the version from the binary itself -------------------
bold "==> building the toolchain"
./build.sh >/dev/null
VER="$(bin/voila version | awk '{print $2}')"
[ -n "$VER" ] || die "could not read the version from 'voila version'"
TAG="v$VER"
ok "version $VER  (tag $TAG)"

if gh release view "$TAG" >/dev/null 2>&1; then
    die "release $TAG already exists — bump the version in voilac/main.voi first"
fi

# ---- 2. package the artifacts ------------------------------------------------
bold "==> packaging (source tarball + native binaries)"
SKIP_TESTS=1 ./build_voila.bash >/dev/null
FILES=()
for f in dist/*; do
    [ -f "$f" ] && FILES+=("$f")
done
[ "${#FILES[@]}" -gt 0 ] || die "no artifacts were produced in dist/"
for f in "${FILES[@]}"; do ok "$(basename "$f")"; done

# ---- 3. release notes: the commits since the previous tag --------------------
PREV="$(git describe --tags --abbrev=0 2>/dev/null || true)"
NOTES="$(mktemp)"
{
    echo "Voilà $VER — a self-hosted, statically typed language with a C backend."
    echo
    echo "**Build from source with only a C compiler:**"
    echo '```'
    echo "tar xzf voila-src.tar.gz && cd voila-$VER && ./build.sh"
    echo '```'
    echo
    if [ -n "$PREV" ]; then
        echo "### Changes since $PREV"
        git log --no-merges --pretty='- %s' "$PREV"..HEAD
    else
        echo "### Highlights"
        git log --no-merges --pretty='- %s' -n 20
    fi
} > "$NOTES"

# ---- 4. tag and publish ------------------------------------------------------
if [ "$DRY" -eq 1 ]; then
    bold "==> dry run — the following would run:"
    echo "  git tag -a $TAG -m \"Voilà $VER\""
    echo "  git push origin $TAG"
    echo "  gh release create $TAG ${FILES[*]} --title \"Voilà $VER\" --notes-file <notes>"
    echo
    echo "  ---- notes ----"
    sed 's/^/  /' "$NOTES"
    rm -f "$NOTES"
    exit 0
fi

bold "==> tagging and publishing"
git rev-parse "$TAG" >/dev/null 2>&1 || git tag -a "$TAG" -m "Voilà $VER"
git push origin "$TAG"
gh release create "$TAG" "${FILES[@]}" --title "Voilà $VER" --notes-file "$NOTES"
rm -f "$NOTES"
ok "released $TAG"
