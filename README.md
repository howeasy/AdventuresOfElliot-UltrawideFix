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
- Toggle on/off via `HUDFix.ini`.

### Known limitation

Menu **background dim/blur** overlays (e.g. behind the weapon wheel and menus) remain 16:9, so the
ultrawide world is visible un-dimmed at the sides behind menus. These are `UBackgroundBlur` widgets
that sample their layout geometry, which the 16:9 viewport slot bounds — they can't be extended with
the techniques this plugin uses. Everything else is constrained correctly.

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

## Credits & license

- Built from **[Lyall/DQ3Fix](https://github.com/Lyall/DQ3Fix)** as a template (MIT licensed).
- **[PhantomGamers/SUWSF](https://github.com/PhantomGamers/SUWSF)** — the ultrawide fix this companions.
- **[Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7)** — generated the Unreal SDK.
- **[cursey/safetyhook](https://github.com/cursey/safetyhook)**, **spdlog**, **inipp** — vendored libs.

Licensed under the **MIT License** (see `LICENSE`); preserves the upstream DQ3Fix copyright notice.
