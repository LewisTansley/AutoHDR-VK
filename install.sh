#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
JOBS="${JOBS:-$(nproc)}"

cmake -S "$ROOT" -B "$ROOT/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

cmake --build "$ROOT/build" -j"$JOBS"
cmake --install "$ROOT/build"

mkdir -p "$HOME/.config/autohdr-vk"
if [[ ! -f "$HOME/.config/autohdr-vk/conf.toml" ]]; then
  cp "$ROOT/conf.example.toml" "$HOME/.config/autohdr-vk/conf.toml"
  echo "Wrote $HOME/.config/autohdr-vk/conf.toml"
fi

echo
echo "Installed AutoHDR-VK to $PREFIX"
echo "Enable with: ENABLE_AUTOHDR=1 <command>"
echo "Verify:      ENABLE_AUTOHDR=1 $PREFIX/bin/autohdr-vk-cli"
