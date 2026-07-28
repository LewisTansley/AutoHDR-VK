# AutoHDR-VK

DE-agnostic Vulkan implicit layer that applies **compute-based** AutoHDR-style SDR→HDR at `vkQueuePresentKHR` (histogram adaptation, UI mask, BT.2446 + ICtCp). Works with native Vulkan apps and DXVK/vkd3d (Proton) under any compositor that can present HDR.

This does **not** replace the Plasma KWin effect. It only processes **Vulkan clients**.

Related: [PlasmaAutoHDR](https://github.com/LewisTansley/PlasmaAutoHDR) (KWin desktop effect for Plasma 6).

## Requirements

- Linux, Vulkan 1.2+ ICD (Mesa / NVIDIA)
- `glslangValidator` to build
- Build deps: `libX11`, `wayland-client`, `libxkbcommon` (for in-game overlay hotkeys)
- An HDR-capable presentation path for correct results (KWin HDR, gamescope HDR, Hyprland HDR, etc.)

### Compositor notes

| Compositor | Notes |
|---|---|
| KDE Plasma / KWin | Works when display HDR is enabled |
| gamescope | Good target for Steam Deck / nested HDR |
| Hyprland | Works if HDR/color management is enabled |
| **niri** | Layer loads, but **niri has no HDR yet** (Smithay color management pending). Output will look wrong until niri gains HDR |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
cmake --install build
```

Or:

```bash
./install.sh
```

This installs:

- `~/.local/lib/libVkLayer_AUTOHDR_tonemap.so` (or `lib64` on some distros)
- `~/.local/share/vulkan/implicit_layer.d/VkLayer_AUTOHDR_tonemap.json`
- `~/.local/bin/autohdr-vk-cli`

Ensure `~/.local/share` is on your XDG data path (default).

### Test without installing

```bash
export VK_ADD_IMPLICIT_LAYER_PATH="$PWD/build/implicit_layer.d"
export ENABLE_AUTOHDR=1
export AUTOHDR_LOG=1
vulkaninfo 2>&1 | head
# Then run a Vulkan game / vkcube
```

## Usage

```bash
# Steam launch options
ENABLE_AUTOHDR=1 %command%

# Or one-shot
ENABLE_AUTOHDR=1 ./my-vulkan-game
```

Force off: `DISABLE_AUTOHDR=1`.

### In-game overlay (hotkeys)

| Hotkey | Action |
|---|---|
| **Super+H** (Shift optional) | Toggle calibrator overlay (Intensity + Shape + Color sliders) |

While the overlay is open:

- **Up/Down** or **Tab** — cycle Intensity / Shape (`expansion_shape`) / Color (`color_intensity`)
- **Left/Right** — adjust ±0.05 (hold **Shift** for ±0.01)
- **Super+H** again — close and save to `conf.toml`
- **Esc** — close and discard changes
- Mouse drag on tracks works on X11/XWayland when the pointer is visible

Hotkeys work on **X11**, **XWayland**, and **native Wayland** (including Proton with `PROTON_ENABLE_WAYLAND=1`) via the app’s Wayland seat + xkbcommon.

## Configuration

Copy the example config:

```bash
mkdir -p ~/.config/autohdr-vk
cp conf.example.toml ~/.config/autohdr-vk/conf.toml
# or after install:
# cp ~/.local/share/autohdr-vk/conf.example.toml ~/.config/autohdr-vk/conf.toml
```

Important keys under `[global]`:

- **`intensity`** — primary look control (Windows AutoHDR-style): `0` ≈ SDR, `1` = full peak headroom. Linear blend toward the tonemapped result.
- **`color_intensity`** — saturation / chroma strength (`0` = luma-only + no gamut expand, `1` = full chroma restore and full `gamut_expansion`). Overlay **Color** slider.
- **`expansion_shape`** — shadow→highlight curve shape (`0` = linear / brighter mids, `1` = exponential / darker mids). Overlay **Shape** slider.
- `encoding` — `auto` (prefer PQ on HDR10 swapchains, else SDR preview), `pq`, `scrgb`, `sdr_preview`
- `prefer_hdr_swapchain` — try to select an HDR10 surface format/colorspace at swapchain create
- `set_hdr_metadata` — call `vkSetHdrMetadataEXT` when available (maxCLL/maxFALL from scene stats)

Advanced overrides (optional / back-compat): `reference_nits`, `peak_nits`, `tone_curve_preset`, `gamut_expansion` (max expand scale when Color = 1), `dither` / `dither_strength` (IGN bit-depth dither before swapchain quantize; on by default), etc.

Per-exe overrides:

```toml
[[profile]]
exe = "game.exe"
intensity = 0.7
```

Inspect resolved settings:

```bash
ENABLE_AUTOHDR=1 autohdr-vk-cli
```

## How it works

```
App / DXVK → Vulkan loader → VK_LAYER_AUTOHDR_tonemap → ICD → WSI → compositor
```

On present, the layer copies the swapchain image and runs a compute pipeline:

1. **Histogram** — scene luminance stats (geo-mean, p10/p90, adaptive peak)
2. **UI cluster** — 8×8 tile mask to lock flat UI/text to SDR white
3. **Base blur** — half-res linear base layer for detail preservation
4. **Tonemap** — BT.2446 + ICtCp spatial tonemap (base/detail split), UI mask blend, PQ/scRGB/SDR encode
5. **Overlay** (optional) — in-swapchain intensity/color calibrator HUD

Then the result is blitted back to the swapchain for present.

## Limitations

- Vulkan only (OpenGL needs Zink or a separate hook)
- No AI guidance (Plasma AutoHDR optional AI path is not ported)
- Overlay mouse drag is X11/XWayland only; keyboard works on all backends
- Some anti-cheat systems dislike Vulkan layers
- Flatpak/Steam Runtime may need the layer visible inside the sandbox (`VK_LAYER_PATH` / Flatpak extension)

## Relation to Plasma AutoHDR

| | KWin effect ([PlasmaAutoHDR](https://github.com/LewisTansley/PlasmaAutoHDR)) | AutoHDR-VK |
|---|---|---|
| Scope | Any window | Vulkan clients only |
| DE | Plasma 6 | Any |
| AI guidance | Optional | Not in MVP |
| Calibration UI | Qt overlay | In-swapchain Super+H overlay |

## License

MIT — see [LICENSE](LICENSE). Tone-curve and shader math originated in PlasmaAutoHDR; see [NOTICE](NOTICE).
