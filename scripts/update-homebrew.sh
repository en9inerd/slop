#!/bin/bash
set -euo pipefail

VERSION=${1:?Usage: update-homebrew.sh <version>}
REPO="en9inerd/slop"
FORMULA="packaging/homebrew/slop.rb"

targets=("slop_x86_64-linux-musl" "slop_aarch64-linux-musl" "slop_x86_64-macos" "slop_aarch64-macos")
declare -A sha_map

for target in "${targets[@]}"; do
  url="https://github.com/$REPO/releases/download/v${VERSION}/${target}"
  echo "Downloading $target..."
  sha=$(curl -sL "$url" | shasum -a 256 | awk '{print $1}')
  sha_map[$target]="$sha"
  echo "  $target: $sha"
done

sed -i.bak \
  -e "s/VERSION_PLACEHOLDER/${VERSION}/g" \
  -e "s/SHA256_LINUX_X86_64/${sha_map[slop_x86_64-linux-musl]}/g" \
  -e "s/SHA256_LINUX_ARM64/${sha_map[slop_aarch64-linux-musl]}/g" \
  -e "s/SHA256_MACOS_X86_64/${sha_map[slop_x86_64-macos]}/g" \
  -e "s/SHA256_MACOS_ARM64/${sha_map[slop_aarch64-macos]}/g" \
  "$FORMULA"

rm -f "${FORMULA}.bak"
echo ""
echo "Updated $FORMULA for v$VERSION"
