# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

**wavezbg** is a PSP (PlayStation Portable) VSH/XMB plugin that lets users customize the background wave colours on the PSP's XMB menu. Upstream targets firmware **6.61 only (ARK CFW)**. This fork is being ported to also support **6.60 (PRO-C CFW)** — see "6.60 compatibility" below. There is also a companion web editor ([index.html](index.html)) hosted on GitHub Pages.

## Build

Requires the PSP toolchain (`psp-cmake`, `pspdev`).

```bash
./build.sh
```

This script:
1. Converts [wave.txt](wave.txt) into [src/default_wave.h](src/default_wave.h) (a C header with the default colour config embedded as a string literal)
2. Runs `psp-cmake -DBUILD_PRX=1 -DENC_PRX=1 ..` in `build/`
3. Runs `make`

The output is an encrypted PRX plugin ready to be installed via ARK's Custom Launcher as a VSH plugin.

## Architecture

### PSP plugin ([src/main.c](src/main.c))

The plugin runs as a kernel-mode PRX on PSP firmware 6.61. Execution flow:

1. **`module_start`** — checks firmware version (`0x06060110` = 6.61), then spawns `wavezbg_thread`. **This gate must be widened for 6.60 support** (see below)
2. **`read_config`** — loads `wave.txt` from `ef0:/seplugins/` or `ms0:/seplugins/` (tries ef0 first for PSP Go), falls back to defaults embedded in `default_wave.h`. On parse failure writes `wavez_ERROR.txt`
3. **`generate_wave_bmp`** — pre-generates two BMP files (`w1.bmp`, `w2.bmp`) in a `wavez_cache/` directory. BMPs are 60×34px, 24-bit, with vertical colour gradients (1–3 stop) matching each `MonthColour` entry. Skips generation if `w1.bmp` already exists (cache hit)
4. **`patch_wave_strings`** — iterates loaded kernel modules looking for `system_plugin_bg`, `sysconf_plugin`, or `vsh`; scans their memory for the stock flash0 BMP paths (`flash0:/vsh/resource/01-12.bmp`, `01-12_03g.bmp`, `13-27.bmp`) and patches them in-place with the cache paths, then invalidates the dcache. Returns 1 when patched
5. **`main_thread`** — polls `patch_wave_strings` every 100ms until patching succeeds, then exits

**Colour config format** (`wave.txt`): lines of `<index 1–34> = #RRGGBB [#RRGGBB [#RRGGBB]]`. Indices map to months/themes (34 slots total). Up to 3 colours per slot for a 3-stop vertical gradient in the BMP.

The 35-element `month_colours[]` array is 1-indexed in the config but 0-indexed internally; index 34 in the config fills slot `[33]`, and `[34]` is a duplicate of `[33]` (bounds guard at `if (m >= 35) m = 34`).

### Web editor ([index.html](index.html))

Single-file vanilla HTML/CSS/JS app. Persists the 34-palette state in `localStorage`. Exports/imports `wave.txt` files. No build step — open directly in a browser or serve statically.

### Default wave config ([wave.txt](wave.txt))

Source of truth for the default colours baked into the PRX. Editing this file and re-running `./build.sh` updates the embedded default.

## 6.60 compatibility (porting goal)

Upstream supports only 6.61 on ARK. This fork targets **6.60 PRO-C** as well. Notes for the port:

### Firmware version gate (required change)

[src/main.c:312](src/main.c#L312) rejects anything that isn't 6.61:

```c
if (sceKernelDevkitVersion() != 0x06060110) {  // 0x06060110 = 6.61
    return 1;
}
```

`sceKernelDevkitVersion()` returns `0x0MMmmrr10`. The relevant values:

| Firmware | Devkit version |
|----------|----------------|
| 6.60     | `0x06060010`   |
| 6.61     | `0x06060110`   |

To support both, accept either value, e.g.:

```c
unsigned int ver = sceKernelDevkitVersion();
if (ver != 0x06060110 && ver != 0x06060010) {
    return 1;
}
```

### CFW differences (ARK vs PRO-C)

- Both ARK and PRO(-C) load VSH plugins via the same `seplugins/` mechanism, and the in-memory string patching in `patch_wave_strings` scans live VSH module memory rather than using hardcoded addresses — so it is largely CFW-agnostic and should port without address changes.
- This has **not** been tested on PRO-C. The README explicitly disclaims PRO/LE support, so any 6.60/PRO-C behaviour is new ground for this fork.

### To verify on 6.60 (not yet confirmed)

- The stock VSH BMP resource paths (`flash0:/vsh/resource/01-12.bmp`, `01-12_03g.bmp`, `13-27.bmp`) are believed identical on 6.60, but confirm the exact strings exist in the 6.60 VSH modules before assuming the patch lands.
- The target module names (`system_plugin_bg`, `sysconf_plugin`, `vsh`) should match on 6.60; confirm if patching never succeeds (the `main_thread` poll loop would spin forever).
