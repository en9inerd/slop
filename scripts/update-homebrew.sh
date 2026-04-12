#!/bin/bash
# Updates the Homebrew tap formula with a new version and SHA256 checksums.
# Reads binaries from a local dist directory — works in CI and locally.
#
# Usage: update-homebrew.sh <version> <dist_dir> <tap_repo_url>
#
# Example (CI):
#   bash scripts/update-homebrew.sh 1.2.3 dist \
#     "https://x-access-token:${TAP_REPO_TOKEN}@github.com/en9inerd/homebrew-tap.git"
#
# Example (local, after downloading release assets to ./dist):
#   bash scripts/update-homebrew.sh 1.2.3 dist \
#     "https://x-access-token:${TAP_REPO_TOKEN}@github.com/en9inerd/homebrew-tap.git"
set -euo pipefail

VERSION="${1:?version required}"
DIST="${2:?dist dir required}"
TAP_REPO="${3:?tap repo URL required}"

sha_of() { shasum -a 256 "${DIST}/$1" | awk '{print $1}'; }

SHA_LINUX_X86_64=$(sha_of slop_x86_64-linux-musl)
SHA_LINUX_ARM64=$(sha_of slop_aarch64-linux-musl)
SHA_MACOS_X86_64=$(sha_of slop_x86_64-macos)
SHA_MACOS_ARM64=$(sha_of slop_aarch64-macos)

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

git clone "$TAP_REPO" "$TMPDIR/tap"
cp packaging/homebrew/slop.rb "$TMPDIR/tap/Formula/slop.rb"

sed -i.bak \
  -e "s/VERSION_PLACEHOLDER/${VERSION}/g" \
  -e "s/SHA256_LINUX_X86_64/${SHA_LINUX_X86_64}/g" \
  -e "s/SHA256_LINUX_ARM64/${SHA_LINUX_ARM64}/g" \
  -e "s/SHA256_MACOS_X86_64/${SHA_MACOS_X86_64}/g" \
  -e "s/SHA256_MACOS_ARM64/${SHA_MACOS_ARM64}/g" \
  "$TMPDIR/tap/Formula/slop.rb"
rm -f "$TMPDIR/tap/Formula/slop.rb.bak"

cd "$TMPDIR/tap"
git config user.name "github-actions[bot]"
git config user.email "github-actions[bot]@users.noreply.github.com"
git add Formula/slop.rb
git commit -m "slop ${VERSION}" || true
git push

echo "Updated homebrew-tap Formula/slop.rb to v${VERSION}"
