# AutoHDR-VK

DE-agnostic Vulkan implicit layer that applies **compute-based** AutoHDR-style SDR→HDR at `vkQueuePresentKHR` (histogram range adaptation, UI mask, tone LUT + perceptual color). Works with native Vulkan apps and DXVK/vkd3d (Proton) under any compositor that can present HDR.

This does **not** replace the Plasma KWin effect. It only processes **Vulkan clients**.

Related: [PlasmaAutoHDR](https://github.com/LewisTansley/PlasmaAutoHDR) (KWin desktop effect for Plasma 6).

## Requirements

- Linux, Vulkan 1.2+ ICD (Mesa / NVIDIA)
- `glslangValidator` to build
- Build deps: `libX11`, `wayland-client`, `libxkbcommon`, `wayland-protocols`, `wayland-scanner` (for in-game overlay hotkeys)
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
| **Super+H** (Shift optional) | Toggle calibrator overlay (Curve + Knee + Stretch + White + Floor + Color + Blowout; Shape + Scurve instead of Curve when `split_curve_sliders = true`; Intensity only with `AUTOHDR_SHOW_INTENSITY=1`; Soften + Smooth only with `AUTOHDR_SHOW_FINISHING=1`) |

While the overlay is open:

- **Up/Down** or **Tab** — cycle Curve (`expansion_shape` / `expansion_scurve`) / Knee (`expansion_mid`) / Stretch (`highlight_stretch`) / White (`stretch_white_ref`) / Floor (`black_floor`) / Color (`color_intensity`) / Blowout (`highlight_blowout`). With `split_curve_sliders = true`, Curve splits into Shape then Scurve. Intensity is included at the front when `AUTOHDR_SHOW_INTENSITY=1`. Soften (`highlight_softness`) and Smooth (`deband_strength`) are included at the end when `AUTOHDR_SHOW_FINISHING=1`; otherwise the effect uses `1.0` for both and closing the overlay does not overwrite the stored values.
- **Left/Right** — adjust ±0.005 (hold **Shift** for ±0.05)
- **Super+H** again — close and save to `conf.toml`
- **Esc** — close and discard changes
- Mouse drag on slider tracks (X11/XWayland absolute pointer, or Wayland/Proton Wayland via a virtual HUD cursor when the game locks the mouse)

The overlay header shows five diagnostic bars (left→right): **nearMax** (red), **p90** (green), **contentWhite** (yellow), **stretchOut** (blue), **highlightGap** (orange, ×2 scale). Red is scene relative highlight peak (SDR white = 100%), not panel nits — when it hits full and Intensity is high, MaxCLL/`peak_nits` can advertise full punch. contentWhite tracks the White slider; a large gap means sparse peaks sit above the chosen anchor.

Hotkeys work on **X11**, **XWayland**, and **native Wayland** (including Proton with `PROTON_ENABLE_WAYLAND=1`) via the app’s Wayland seat + xkbcommon. On Wayland, opening the overlay seeds a virtual cursor on the panel and drives it from `zwp_relative_pointer` so locked-mouse games (FPS look) can still drag sliders.

## Configuration

Copy the example config:

```bash
mkdir -p ~/.config/autohdr-vk
cp conf.example.toml ~/.config/autohdr-vk/conf.toml
# or after install:
# cp ~/.local/share/autohdr-vk/conf.example.toml ~/.config/autohdr-vk/conf.toml
```

Important keys under `[global]`:

- **`intensity`** — primary look control (Windows AutoHDR-style): `0` ≈ SDR, `1` = full peak headroom. Linear blend toward the tonemapped result across the full slider. Unless `AUTOHDR_SHOW_INTENSITY=1`, the overlay hides this slider and the effect uses `1.0`. Closing the overlay does not overwrite the stored value.
- **`color_intensity`** — saturation / chroma strength (`0` = luma-only + no gamut expand, `1` = full ICtCp chroma restore + moderate vibrance/gamut — the former ~0.4 ceiling stretched across the slider). Overlay **Color** slider.
- **`expansion_shape`** / **`expansion_scurve`** — shadow→highlight curve. With `split_curve_sliders = false` (default) the overlay shows one **Curve** slider: `0` is full exponential (`shape = 1`, `scurve = 0`) and `1` is full S (`shape = 0`, `scurve = 1`), with `shape = 1 - t` and `scurve = t` in between. Off-axis pairs project onto that line with `t = clamp((1 - shape + scurve) / 2, 0, 1)` when settings are resolved, so the previous `0.55` / `0` default becomes `0.775` / `0.225`. Saving the overlay writes the coupled pair and leaves the flag unchanged.
- **`split_curve_sliders`** — when `true`, Shape and Scurve are independent overlay sliders (`expansion_shape`: `0` = linear / brighter mids, `1` = exponential / darker mids; `expansion_scurve`: `0` = off, `1` = darker lows / mid contrast with a soft highlight shoulder). No projection.
- **`expansion_mid`** — tone-curve mid/knee height on the Shape+Scurve LUT (`0.5` = current mid, lower darkens mids, higher lifts mids). Overlay **Knee** slider.
- **`black_floor`** — raised-floor black crush (`0` = off, `1` = max). Overlay **Floor** slider. Use when a game's blacks look lifted/foggy.
- **`highlight_stretch`** — histogram highlight-tail pre-stretch (`0` = off, `0.45` = default, `1` = legacy max). Overlay **Stretch** slider (range `0–1`; values above `1` can still be set in TOML). Precomputed in histogram pass2 together with White. Lifts only the top of the content-white band; midtones below the knee stay put. Dark-scene auto-raise pulls back when near-max highlights are already present, and mid-luminance frames without speculars stay gain-capped (they do not auto-fill to peak).
- **`stretch_white_ref`** — which histogram percentile defines the scene highlight anchor (`0` = p100 near-max default, `1` = p98). Overlay **White** slider. Precomputed in histogram pass2; tonemap applies a lift-only remap from the resulting `contentWhite` / `stretchOut` stats (never compresses above white).
- **`separation`** — Pixel / Ultra HDR–inspired highlight punch (`0` = off, `1` = full). Config only (no overlay slider). Keeps lows/mids relatively flat while applying a multiplicative log boost above a mid→contentWhite recovery knee. Unlike Stretch (mild highlight-tail lift) or White (percentile remapper that can blow out a whole band), it scales punch without dragging the white anchor. Overlay save leaves this value unchanged.
- **`highlight_blowout`** — RenoDX-style highlight desaturation for overexposed vivid lights (`0` = off, `1` = strong). Pulls hot speculars toward neutral white while leaving mid-bright colored halos intact. Overlay **Blowout** slider. Useful on older titles that clip saturated primaries instead of blowing out to white.
- `encoding` — `auto` (prefer PQ on HDR10 swapchains, else SDR preview), `pq`, `scrgb`, `sdr_preview`
- `prefer_hdr_swapchain` — try to select an HDR10 surface format/colorspace at swapchain create
- `set_hdr_metadata` — call `vkSetHdrMetadataEXT` when available (maxCLL/maxFALL from scene stats; updates are debounced to avoid compositor hitching)

If you run **OptiScaler** (or another upstream Vulkan layer) alongside AutoHDR and see pink/magenta corruption, set `AUTOHDR_NO_SWAPCHAIN_REMAP=1` so the layer tonemaps at present without changing the swapchain format the game/layer negotiated. You can also set `prefer_hdr_swapchain = false` in `conf.toml`.

Advanced overrides (optional / back-compat): `reference_nits`, `peak_nits`, `tone_curve_preset`, `gamut_expansion` (max expand scale when Color = 1; default `1.5`), `dither` / `dither_strength` (IGN bit-depth dither before swapchain quantize; on by default), etc.

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

1. **Histogram** — scene luminance stats (geo-mean, p05/p25/p90–p98 ladder/near-max); White + Stretch precompute `contentWhite` / `stretchOut`; maxCLL = `nearMax × peak` (sparse speculars keep full metadata punch; `highlightGap` is overlay-only)
2. **UI cluster** — 8×8 tile mask to lock flat UI/text to SDR white (currently gated off)
3. **Tonemap** — highlight-tail remap from precomputed stats (mids below knee stay put), optional Separation log-boost (flat mids + punchy highs), then tone LUT + ICtCp perceptual color + CIE-Y peak limit; PQ (BT.2020)/scRGB/SDR encode
4. **Overlay** (optional) — in-swapchain calibrator HUD

Then the result is blitted back to the swapchain for present.

Working color space is linear Rec.709 nits. HDR10 (`encoding = pq` / auto on ST2084 swapchains) converts Rec.709 → BT.2020 immediately before ST.2084 encode so compositors (e.g. KWin) see correct primaries.

## Limitations

- Vulkan only (OpenGL needs Zink or a separate hook)
- No AI guidance (Plasma AutoHDR optional AI path is not ported)
- Overlay mouse: X11/XWayland use absolute pointer when visible; Wayland uses a relative-pointer virtual cursor while the overlay is open (falls back to keyboard if the compositor lacks relative-pointer)
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
