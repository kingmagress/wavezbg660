# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

**wavezbg** is a PSP (PlayStation Portable) VSH/XMB plugin that lets users customize the background wave colours on the PSP's XMB menu. Upstream targets firmware **6.61 only (ARK CFW)**. This fork **also supports 6.60 (PRO-C CFW)** — implemented and confirmed working on 6.60 PRO-C Fix 3 (see "6.60 support" below). There is also a companion web editor ([index.html](index.html)) hosted on GitHub Pages.

## Build

Requires the PSP toolchain (`psp-cmake`, `pspdev`). Build on the PSP/WSL side, not Windows.

```bash
./build.sh
```

This script:

1. Converts [wave.txt](wave.txt) into [src/default_wave.h](src/default_wave.h) (a C header with the default colour config embedded as a string literal)
2. Runs `psp-cmake -DBUILD_PRX=1 -DENC_PRX=1 ..` in `build/`
3. Runs `make`

The output is an encrypted PRX plugin, installed via ARK's (or PRO's) Custom Launcher as a VSH plugin.

## Architecture

### PSP plugin ([src/main.c](src/main.c))

The plugin runs as a kernel-mode PRX on PSP firmware 6.60/6.61. Execution flow:

1. **`module_start`** — accepts firmware 6.61 (`0x06060110`, ARK) and 6.60 (`0x06060010`, PRO-C); rejects anything else. Spawns `wavezbg_thread` (32 KB stack)
2. **`read_config`** — loads `wave.txt` from `ef0:/seplugins/` or `ms0:/seplugins/` (tries ef0 first for PSP Go), falls back to defaults embedded in `default_wave.h`. The parser is bounds-safe (`valid_hex6` validates each `#RRGGBB` before reading it). On total parse failure it writes `wavez_ERROR.txt`
3. **`generate_wave_bmp`** — pre-generates two BMP files in a `wavez_cache/` directory: `w1.bmp` from config slots 1–12, `w2.bmp` from slots 13–34. BMPs are 60×34px, 24-bit, with vertical colour gradients (1–3 stop). Skips generation entirely if `w1.bmp` already exists (cache hit — **see caveat below**)
4. **`patch_wave_strings`** — iterates loaded kernel modules looking for `system_plugin_bg`, `sysconf_plugin`, or `vsh`; scans their live memory for the stock flash0 BMP paths and patches them in-place with the cache paths, then invalidates the dcache. Idempotent (a patched string no longer starts with `flash`). Returns 1 when anything was patched
5. **`main_thread`** — bounded poll: calls `patch_wave_strings` every 100 ms for up to ~60 s (600 attempts), then exits and self-deletes the thread **regardless of outcome**. The poll exists because the target VSH modules aren't loaded yet when `module_start` runs

**Colour config format** (`wave.txt`): lines of `<index 1–34> = #RRGGBB [#RRGGBB [#RRGGBB]]`. A trailing `# name` comment per line is allowed (the parser skips to end-of-line after the colours; the web editor's import regex ignores non-hex text). Up to 3 colours per slot for a 3-stop vertical gradient (colour 1 = bottom of the wave, colour 3 = top).

The 35-element `month_colours[]` array is 1-indexed in the config but 0-indexed internally; index 34 in the config fills slot `[33]`, and `[34]` is a guard slot (`if (m >= 35) m = 34`).

### `wave.txt` → BMP slot mapping (non-obvious)

`generate_wave_bmp` splits the 34 slots across the PSP's two wave resource files:

| Generated file | Replaces (flash0)                   | `wave.txt` slots |
| -------------- | ----------------------------------- | ---------------- |
| `w1.bmp`       | `01-12.bmp` **and** `01-12_03g.bmp` | **1–12**         |
| `w2.bmp`       | `13-27.bmp`                         | **13–34**        |

- The colour set the XMB actually cycles through for the wave is the **12 colours in `w1.bmp` (slots 1–12)**. So the "2nd-to-last selectable colour" is slot 11, not slot 33 — this is expected, not a bug.
- `01-12_03g.bmp` is the slim/03g colour profile (also used on a PSP-1000 when "use slim colours" is enabled). Both it and `01-12.bmp` are patched to `w1.bmp`, so slots 1–12 apply either way.

### Cache invalidation gotcha

`generate_wave_bmp` returns early if `wavez_cache/w1.bmp` already exists. So after editing `wave.txt`, the **`wavez_cache/` folder must be deleted** (on `ms0:/` or `ef0:/`, wherever `wave.txt` lives) and the PSP restarted, or the old rendered BMPs keep showing.

### Robustness & memory invariants (don't regress these)

- **No heap allocations anywhere** — every buffer is stack or static, so there are no memory leaks by construction. Keep it that way.
- Every `sceIoOpen` has a matching `sceIoClose` on all paths.
- The config parser must stay bounds-safe: `valid_hex6` short-circuits on the first non-hex char (the NUL terminator is not hex), so it never reads past a truncated buffer. Don't reintroduce unchecked `parse_hex_colour` + `p += 7`.
- The module-id list is clamped to the `ids[]` capacity. The string scanner walks each loaded segment **separately** via the module's segment table (`nsegment` / `segmentaddr[]` / `segmentsize[]`), with a text-segment fallback when that table is empty. **Do not** revert to scanning `[text_addr, text_addr + text_size + data_size + bss_size)` as one block — a module's segments are page-aligned and not necessarily adjacent, so that range crosses unmapped gaps and a stray read there is a TLB-miss exception (the random-crash bug fixed in this fork).
- BMP header fields are written byte-by-byte (`put_u32` / `put_u16`), not via `*(unsigned int *)&buf[off]`. The fields sit at offsets ≡ 2 (mod 4), so pointer-cast stores are unaligned word accesses — illegal on the Allegrex MIPS core and UB in C. Keep the byte writes.
- `info.name` is force-terminated before `strstr` (it is a fixed 28-byte field with no guaranteed NUL).
- The worker thread always reaches `sceKernelExitDeleteThread`, so it never lingers even if patching never succeeds.

### Web editor ([index.html](index.html))

Single-file vanilla HTML/CSS/JS app. Persists the 34-palette state in `localStorage`. Exports/imports `wave.txt` files. No build step — open directly in a browser or serve statically.

### Default wave config ([wave.txt](wave.txt))

Source of truth for the default colours baked into the PRX. Editing this file and re-running `./build.sh` updates the embedded default. Note that editing `wave.txt` only changes the _embedded fallback_; an end user's runtime colours come from the `wave.txt` on their memory stick.

## 6.60 support

This fork supports **6.60 PRO-C** in addition to upstream's 6.61 ARK. Reference notes:

### Firmware version gate

`module_start` accepts both devkit versions:

```c
unsigned int ver = sceKernelDevkitVersion();
if (ver != 0x06060110 && ver != 0x06060010) {
    return 1;
}
```

`sceKernelDevkitVersion()` returns `0x0MMmmrr10`:

| Firmware | Devkit version |
| -------- | -------------- |
| 6.60     | `0x06060010`   |
| 6.61     | `0x06060110`   |

### CFW differences (ARK vs PRO-C)

- Both ARK and PRO(-C) load VSH plugins via the same `seplugins/` mechanism, and `patch_wave_strings` scans live VSH module memory rather than using hardcoded addresses — so it is largely CFW-agnostic and ported without address changes.
- Confirmed loading and patching on 6.60 PRO-C Fix 3. The upstream README still disclaims PRO/LE support (it is written for ARK only) — treat that disclaimer as stale for this fork.

# Claude Code Rules (PSP Project)

## Core Workflow (mandatory)

Before writing code:

1. Search external/pspsdk/src and docs/pspsdk-api for relevant APIs
2. Identify exact headers and functions
3. Produce a plan with risks and alternatives
4. Wait for approval before modifying files

## Implementation Rules

- Never guess PSP SDK function signatures
- If unsure, search headers or samples first

## PSP Constraints

- Target firmware: 6.60, 6.61
- Avoid flash modifications
- Keep PRX lightweight and stable
- Avoid blocking VSH/main threads
- Validate all sce\* APIs in PSPSDK headers

## Verification

After changes:

- Fix errors before finishing
- Summarize all modifications

## General rule

Never proceed directly to implementation.

Always:

- retrieve
- reason
- propose
- wait
- implement
- verify
