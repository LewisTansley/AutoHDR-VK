#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
cmake --build "$ROOT/build" -j"$(nproc)"
# Refresh stage
STAGE="$ROOT/build/install-stage"
mkdir -p "$STAGE/lib" "$STAGE/bin" "$STAGE/share/vulkan/implicit_layer.d"
cp -f "$ROOT/build/libVkLayer_AUTOHDR_tonemap.so" "$STAGE/lib/"
cp -f "$ROOT/build/autohdr-vk-cli" "$STAGE/bin/" 2>/dev/null || true
JSON_SRC=""
for c in "$ROOT/build/VkLayer_AUTOHDR_tonemap.json" "$ROOT/build/implicit_layer.d/VkLayer_AUTOHDR_tonemap.json"; do
  [[ -f "$c" ]] && JSON_SRC="$c" && break
done
if [[ -n "$JSON_SRC" ]]; then
  python3 - "$JSON_SRC" "$STAGE/share/vulkan/implicit_layer.d/VkLayer_AUTOHDR_tonemap.json" "$PREFIX/lib/libVkLayer_AUTOHDR_tonemap.so" <<'PY'
import json,sys
src,dst,lib=sys.argv[1],sys.argv[2],sys.argv[3]
with open(src) as f: data=json.load(f)
layer=data.get("layer") or data["layers"][0]
layer["library_path"]=lib
if "layer" in data: data["layer"]=layer
else: data["layers"][0]=layer
with open(dst,"w") as f:
    json.dump(data,f,indent=4); f.write("\n")
PY
fi
# Take ownership if needed, then install
if [[ ! -w "$PREFIX/lib/libVkLayer_AUTOHDR_tonemap.so" ]] 2>/dev/null; then
  echo "Taking ownership of root-owned install (sudo)..."
  sudo chown "$USER:$USER" \
    "$PREFIX/lib/libVkLayer_AUTOHDR_tonemap.so" \
    "$PREFIX/share/vulkan/implicit_layer.d/VkLayer_AUTOHDR_tonemap.json" \
    "$PREFIX/bin/autohdr-vk-cli" || true
fi
bash "$ROOT/build/overwrite-install.sh"
