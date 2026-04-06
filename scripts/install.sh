#!/bin/bash
set -euo pipefail

REPO="en9inerd/slop"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$OS" in
  linux)
    case "$ARCH" in
      x86_64|amd64) TARGET="x86_64-linux-musl" ;;
      aarch64|arm64) TARGET="aarch64-linux-musl" ;;
      *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
    esac
    ;;
  darwin)
    case "$ARCH" in
      x86_64|amd64) TARGET="x86_64-macos" ;;
      aarch64|arm64) TARGET="aarch64-macos" ;;
      *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
    esac
    ;;
  *) echo "Unsupported OS: $OS"; exit 1 ;;
esac

LATEST=$(curl -sL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | sed -E 's/.*"v([^"]+)".*/\1/')

if [ -z "$LATEST" ]; then
  echo "Failed to fetch latest version"
  exit 1
fi

URL="https://github.com/$REPO/releases/download/v${LATEST}/slop_${TARGET}"

echo "Installing slop v${LATEST} (${TARGET}) to ${INSTALL_DIR}..."

curl -sL "$URL" -o /tmp/slop
chmod +x /tmp/slop

if [ -w "$INSTALL_DIR" ]; then
  mv /tmp/slop "$INSTALL_DIR/slop"
else
  sudo mv /tmp/slop "$INSTALL_DIR/slop"
fi

echo "slop v${LATEST} installed to ${INSTALL_DIR}/slop"
slop --help 2>/dev/null | head -1 || true
