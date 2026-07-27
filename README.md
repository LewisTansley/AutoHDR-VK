# AutoHDR-VK

DE-agnostic Vulkan implicit layer that applies AutoHDR’s classical SDR→HDR tone map at `vkQueuePresentKHR`. Works with native Vulkan apps and DXVK/vkd3d (Proton) under any compositor that can present HDR.

This does **not** replace the Plasma KWin effect. It only processes **Vulkan clients**.

Related: [PlasmaAutoHDR](https://github.com/LewisTansley/PlasmaAutoHDR) (KWin desktop effect for Plasma 6).

## Requirements

- Linux, Vulkan 1.2+ ICD (Mesa / NVIDIA)
- `glslangValidator` to build
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

## Configuration

Copy the example config:

```bash
mkdir -p ~/.config/autohdr-vk
cp conf.example.toml ~/.config/autohdr-vk/conf.toml
# or after install:
# cp ~/.local/share/autohdr-vk/conf.example.toml ~/.config/autohdr-vk/conf.toml
```

Important keys under `[global]`:

- `reference_nits` / `peak_nits` — match your display calibration when possible
- `tone_curve_preset` — `linear`, `balanced`, `lifted_shadows`, `soft_shadows`, `vivid_highlights`, `high_contrast`, `exponential`
- `encoding` — `auto` (prefer PQ on HDR10 swapchains, else SDR preview), `pq`, `scrgb`, `sdr_preview`
- `prefer_hdr_swapchain` — try to select an HDR10 surface format/colorspace at swapchain create
- `set_hdr_metadata` — call `vkSetHdrMetadataEXT` when available
- `perceptual_color` / `color_intensity` / `gamut_expansion` — color path controls (see source / Plasma AutoHDR docs)

Per-exe overrides:

```toml
[[profile]]
exe = "game.exe"
peak_nits = 800
tone_curve_preset = "vivid_highlights"
```

Inspect resolved settings:

```bash
ENABLE_AUTOHDR=1 autohdr-vk-cli
```

## How it works

```
App / DXVK → Vulkan loader → VK_LAYER_AUTOHDR_tonemap → ICD → WSI → compositor
```

On present, the layer copies the swapchain image, runs a fullscreen tone-map pass (black point, tone-curve LUT, vibrance, gamut expansion, highlight limit), optionally encodes PQ / scRGB, then presents.

## Limitations

- Vulkan only (OpenGL needs Zink or a separate hook)
- No AI guidance (Plasma AutoHDR optional AI path is not ported)
- No in-game calibration GUI yet (edit TOML / use `autohdr-vk-cli`)
- Some anti-cheat systems dislike Vulkan layers
- Flatpak/Steam Runtime may need the layer visible inside the sandbox (`VK_LAYER_PATH` / Flatpak extension)

## Relation to Plasma AutoHDR

| | KWin effect ([PlasmaAutoHDR](https://github.com/LewisTansley/PlasmaAutoHDR)) | AutoHDR-VK |
|---|---|---|
| Scope | Any window | Vulkan clients only |
| DE | Plasma 6 | Any |
| AI guidance | Optional | Not in MVP |
| Calibration UI | Yes | Config file |

## License

MIT — see [LICENSE](LICENSE). Tone-curve and shader math originated in PlasmaAutoHDR; see [NOTICE](NOTICE).
