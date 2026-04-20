#!/bin/bash
set -euo pipefail

DIST_DIR="dist"
VERSION=${VERSION:-dev}
DESCRIPTION="AI slop detector - finds AI-typical code defects using information theory"
MAINTAINER="en9inerd"
URL="https://github.com/en9inerd/slop"

mkdir -p "$DIST_DIR"

targets=(
  "x86_64-linux-musl"
  "aarch64-linux-musl"
  "x86_64-macos"
  "aarch64-macos"
)

deb_arch_map() {
  case "$1" in
    x86_64-linux-musl)  echo "amd64" ;;
    aarch64-linux-musl) echo "arm64" ;;
    *) echo "" ;;
  esac
}

for target in "${targets[@]}"; do
  echo "Building slop for $target (v$VERSION)"
  zig build -Doptimize=ReleaseFast -Dtarget="$target" -Dversion="$VERSION"

  cp zig-out/bin/slop "$DIST_DIR/slop_${target}"

  deb_arch=$(deb_arch_map "$target")
  if [[ -n "$deb_arch" ]]; then
    pkg_dir=$(mktemp -d)
    mkdir -p "$pkg_dir/usr/local/bin"
    mkdir -p "$pkg_dir/DEBIAN"

    cp zig-out/bin/slop "$pkg_dir/usr/local/bin/slop"
    chmod 755 "$pkg_dir/usr/local/bin/slop"

    cat > "$pkg_dir/DEBIAN/control" <<EOF
Package: slop
Version: ${VERSION}
Architecture: ${deb_arch}
Maintainer: ${MAINTAINER}
Description: ${DESCRIPTION}
Homepage: ${URL}
Section: devel
Priority: optional
EOF

    deb_name="slop_${VERSION}_${deb_arch}.deb"
    dpkg-deb --build --root-owner-group "$pkg_dir" "$DIST_DIR/$deb_name"
    rm -rf "$pkg_dir"
  fi
done

echo ""
echo "Built artifacts:"
ls -lh "$DIST_DIR/"
