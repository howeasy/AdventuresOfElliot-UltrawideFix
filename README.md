# HUDFix — 16:9 HUD constraint for *The Adventures of Elliot: The Millennium Tales*

An ASI plugin that constrains the game's HUD/UI to a centered **16:9** band on ultrawide displays,
while the 3D world keeps filling the full ultrawide screen.

It is a **companion to [SUWSF](https://github.com/PhantomGamers/SUWSF)** (Somewhat Universal
Widescreen Fix), which provides the ultrawide world rendering. SUWSF alone makes the HUD stretch out
to the screen edges; HUDFix pulls it back into a proper 16:9 region.

Engine: Unreal Engine **5.6.1**.

## Features

- Gameplay HUD, menus, dialogs, and the objective marker are pillarboxed to centered 16:9.
- The world/3D scene is untouched (stays ultrawide via SUWSF).
- The in-world **damage vignette stays full-screen** (not constrained).
- The **world-tracked interact prompt** (the floating button icon next to NPCs/items) is realigned so
  it sits on its target instead of being pushed sideways by the 16:9 constraint.
- A small INI-driven toolset to **hide / reposition** a stray screen-edge decoration and to **shrink**
  a chosen HUD widget (used by default to tidy the dragged-in corner border and the support-fairy
  portrait).
- Toggle on/off and tune everything via `HUDFix.ini`.

### Known limitation

Menu **background dim/blur** overlays (e.g. behind the weapon wheel and menus) remain 16:9, so the
ultrawide world is visible un-dimmed at the sides behind menus. These are `UBackgroundBlur` widgets
that sample their layout geometry, which the 16:9 viewport slot bounds — they can't be extended with
the techniques this plugin uses. Everything else is constrained correctly.

## Configuration (`HUDFix.ini`)

All options live under `[Fix HUD]`. Defaults are shown.

| Option | Default | Description |
| --- | --- | --- |
| `Enabled` | `true` | Master on/off for the 16:9 HUD constraint. |
| `Diagnostic` | `false` | Verbose widget logging to `HUDFix.log`. Only needed for troubleshooting/tuning. |
| `MarkerShiftPx` | `275` | Pixels to shift the world-tracked interact prompt back onto its target. `-1` = auto (full pillarbox offset, which overshoots). |
| `BorderWidget` | `Decoration_L` | Widget-name **substring** the border tool acts on (blank = off). |
| `BorderMode` | `hide` | `hide` (make invisible), `shiftL` (move toward the screen edge by `BorderShiftPx`), or `scale` (counter-scale to full screen). |
| `BorderShiftPx` | `325` | Pixels for `shiftL` mode. `-1` = auto (full pillarbox offset). |
| `ShrinkWidget` | `WBP_SupportCharacter` | Widget-name **substring** to render-scale (blank = off). |
| `ShrinkScale` | `0.7` | Scale factor; `<1` shrinks, `1` = no change. |
| `ShrinkPivotX` / `ShrinkPivotY` | `0.5` / `0.5` | Point the scale pulls toward (`0.5,0.5` = centre). |
| `ShrinkOffsetX` / `ShrinkOffsetY` | `-60` / `-30` | Pixel nudge applied after scaling (`-X` = left, `-Y` = up). |

By default the border tool **hides** the dragged-in bottom-left corner decoration; set
`BorderMode=shiftL` instead if you'd rather move it out to the true screen edge. The shrink tool
shrinks the support-fairy portrait to 70% and re-seats it in the corner.

### Per-resolution tuning

The 16:9 constraint adapts to any resolution automatically. However, the **pixel-based offsets**
(`MarkerShiftPx`, `BorderShiftPx`, and the `ShrinkOffset*` values) were dialed in at **5120×2160**.
The HUD art is a fixed pixel size, so the shrink offsets carry over well, but `MarkerShiftPx` (and
`BorderShiftPx` if you switch the border to `shiftL`) scale with the pillarbox width and may need
adjusting on other resolutions. To re-tune, set `Diagnostic=true`, launch, and watch the `MARK …`
lines in `HUDFix.log` while standing at an interactable.

## Installation

1. Install **SUWSF** for this game first (it provides the ultrawide world **and** the Ultimate ASI
   Loader, `winmm.dll`). Follow its instructions; play in windowed/borderless fullscreen.
2. Copy **`HUDFix.asi`** and **`HUDFix.ini`** into the same folder as `SUWSF.asi`:
   ```
   <Steam>\steamapps\common\The Adventures of Elliot_The Millennium Tales\Elliot\Binaries\Win64\
   ```
3. Launch the game. A `HUDFix.log` is written next to the .asi.

To disable without removing files, set `Enabled=false` in `HUDFix.ini`.

> Note: the function offsets are specific to the current `Elliot-Win64-Shipping.exe` build. If the
> game updates, the offsets/signatures in `src/dllmain.cpp` may need to be re-derived.

## Building from source

Requirements: **Visual Studio 2022 Build Tools** (Desktop C++ workload, v143 toolset).

```sh
git clone --recursive https://github.com/<you>/HUDFix
# from a "x64 Native Tools Command Prompt for VS 2022":
msbuild HUDFix.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\HUDFix.asi`. Dependencies (safetyhook, spdlog, inipp) are vendored under
`external/`. The Unreal SDK under `src/SDK/` was generated with [Dumper-7](https://github.com/Encryqed/Dumper-7)
for this game build.

The `tools/` folder has the read-only reverse-engineering helpers used to find offsets
(`aobscan.py`, `disasm.py` — require Python + `capstone`).

## Changelog

### v1.0.1

- **Interact prompt realigned.** The floating button prompt next to NPCs/items is world-positioned
  every frame; the 16:9 constraint offset it sideways. It's now nudged back onto its target via a
  render-translation on the prompt's inner content, re-applied continuously so it survives the prompt
  showing/hiding. Tunable with `MarkerShiftPx`.
- **Border tool (new).** INI-driven tool to act on a named screen-edge decoration. By default it
  **hides** the dragged-in bottom-left corner decoration (`Decoration_L`); it can instead move it to
  the screen edge (`shiftL`) or counter-scale it (`scale`).
- **Shrink tool (new).** INI-driven tool to render-scale a named HUD widget. By default the
  support-fairy portrait (`WBP_SupportCharacter`) is scaled to 70% and re-seated in the bottom-left
  corner.
- **Crash hardening.** The viewport-widget hook paths are now wrapped in C++ exception guards — the
  pre-existing structured-exception (`__try`) guards do not catch C++ exceptions under `/EHsc`, so a
  malformed/transient widget name during level load could escape the hook and crash the game. The
  anchor rewrite is also now restricted to actual `UserWidget`s. This targets the rare crashes
  reported on some setups.
- `Diagnostic` logging now defaults **off**.
- New pixel-offset options are tuned for 5120×2160 — see **Per-resolution tuning** above.

### v1.0.0

- Initial release: pillarbox the HUD/menus/dialogs/objective marker to centered 16:9 while the world
  stays ultrawide; damage vignette kept full-screen.

## Credits & license

- Built from **[Lyall/DQ3Fix](https://github.com/Lyall/DQ3Fix)** as a template (MIT licensed).
- **[PhantomGamers/SUWSF](https://github.com/PhantomGamers/SUWSF)** — the ultrawide fix this companions.
- **[Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7)** — generated the Unreal SDK.
- **[cursey/safetyhook](https://github.com/cursey/safetyhook)**, **spdlog**, **inipp** — vendored libs.

Licensed under the **MIT License** (see `LICENSE`); preserves the upstream DQ3Fix copyright notice.
