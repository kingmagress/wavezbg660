> Generated from [src/main.c](src/main.c) + external/pspsdk headers + project [CLAUDE.md](CLAUDE.md). Verify signatures against the header before use.

# PSP VSH/XMB Plugin Hooking

## Overview

The **VSH** (VSHell) is the PSP's system software shell; the **XMB** (XrossMediaBar) is the
cross-shaped menu it renders. Both are implemented as a set of signed Sony PRX modules loaded
from `flash0:` at boot (e.g. `vsh.prx`, `system_plugin_bg`, `sysconf_plugin`). They read their
art assets — including the animated background "wave" — from BMP files under
`flash0:/vsh/resource/`.

Custom firmware loads third-party VSH plugins through the `seplugins/` mechanism:

| Firmware | CFW    | Devkit version (`sceKernelDevkitVersion`) |
| -------- | ------ | ----------------------------------------- |
| 6.61     | ARK    | `0x06060110`                              |
| 6.60     | PRO-C  | `0x06060010`                              |

Both ARK and PRO(-C) register VSH plugins listed in `seplugins/` (via the Custom Launcher /
`VSH.TXT`-style configuration) and start them as kernel PRXes in the VSH context. This plugin
takes advantage of that to **patch live VSH module memory** rather than relying on hardcoded
firmware addresses: it enumerates the loaded modules, finds the VSH background modules by name,
scans their mapped memory for the stock `flash0:` BMP path strings, and rewrites those strings
in place to point at pre-rendered BMPs in a memory-stick cache. Because the patch is driven by
**string content found at runtime** (not absolute addresses), it is largely **CFW-agnostic** and
ports between 6.60 PRO-C and 6.61 ARK with no address changes.

The one timing subtlety: when the plugin's `module_start` runs, **the target VSH modules are not
loaded yet** — their resource strings aren't in memory. So patching cannot happen synchronously in
`module_start`. Instead the plugin spawns a worker thread that **polls** for the target modules,
patches once they appear, and then exits. See [Correct usage patterns](#correct-usage-patterns).

## Key APIs

All signatures below are copied verbatim from the referenced headers. **Do not paraphrase a
signature into code — re-verify against the header**, because some of these have user-mode and
kernel-mode variants with subtly different prototypes.

### Module enumeration (the path actually used by [src/main.c](src/main.c))

From [external/pspsdk/src/user/pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h):

```c
/* Get a list of loaded module UIDs. */
int sceKernelGetModuleIdList(SceUID *readbuf, int readbufsize, int *idcount);

/* Query info about a loaded module from its UID. */
int sceKernelQueryModuleInfo(SceUID modid, SceKernelModuleInfo *info);
```

The struct filled by `sceKernelQueryModuleInfo` (note: this is **`SceKernelModuleInfo`**, the
querying struct — distinct from `SceModule` below):

```c
typedef struct SceKernelModuleInfo {
	SceSize 		size;          /* must be set to sizeof(*info) before the call */
	char 			nsegment;      /* number of loaded segments */
	char 			reserved[3];
	int 			segmentaddr[4];/* start address of each segment */
	int 			segmentsize[4];/* size of each segment */
	unsigned int 	entry_addr;
	unsigned int 	gp_value;
	unsigned int 	text_addr;     /* TEXT segment start (fallback scan target) */
	unsigned int 	text_size;     /* TEXT segment size */
	unsigned int 	data_size;
	unsigned int 	bss_size;
	unsigned short  attribute;
	unsigned char   version[2];
	char            name[28];      /* fixed 28-byte field, NOT guaranteed NUL-terminated */
} SceKernelModuleInfo;
```

The plugin scans `segmentaddr[i]` / `segmentsize[i]` for `i < nsegment`, falling back to
`text_addr` / `text_size` when `nsegment == 0`. The fixed-width `name[28]` is force-terminated
before any `strstr` (see [Common mistakes](#common-mistakes)).

> `info.size` **must** be initialized to `sizeof(SceKernelModuleInfo)` before the call —
> `main.c` does `memset(&info, 0, sizeof(info)); info.size = sizeof(info);`.

### Module discovery (alternative path — from LoadCore)

From [external/pspsdk/src/kernel/psploadcore.h](external/pspsdk/src/kernel/psploadcore.h):

```c
/* Find a module by name; NULL if not found. */
SceModule * sceKernelFindModuleByName(const char *modname);

/* Find a module from any address inside it; NULL if not found. */
SceModule * sceKernelFindModuleByAddress(unsigned int addr);

/* Other LoadCore enumerators in the same header. */
SceModule * sceKernelFindModuleByUID(SceUID modid);
int         sceKernelModuleCount(void);
```

These return a `SceModule *` (a different, richer struct than `SceKernelModuleInfo`). `main.c`
does **not** use this path — it uses the `Get...IdList` + `QueryModuleInfo` pair above — but
`sceKernelFindModuleByName("vsh")` is the natural alternative if you want to target a single known
module without iterating every UID. The fields relevant to a memory scan are the same conceptually
(`nsegment`, `segmentaddr[4]`, `segmentsize[4]`, `text_addr`, `text_size`, `data_size`, `bss_size`,
`next` linked-list pointer):

```c
typedef struct SceModule {
	struct SceModule *next;     /* modules are a linked list */
	u16   attribute;            /* SceModulePrivilegeLevel: SCE_MODULE_VSH = 0x0800, etc. */
	u8    version[2];
	char  modname[27];          /* note: 27 bytes + separate `char terminal` = always-'\0' */
	char  terminal;
	/* ... */
	u32   text_addr;            /* TEXT segment start */
	u32   text_size;            /* TEXT segment size */
	u32   data_size;            /* DATA segment size */
	u32   bss_size;             /* BSS segment size */
	u8    nsegment;             /* number of segments (<= SCE_KERNEL_MAX_MODULE_SEGMENT = 4) */
	u8    padding2[3];
	u32   segmentaddr[4];       /* per-segment start addresses */
	SceSize segmentsize[4];     /* per-segment sizes */
	u32   segmentalign[4];
	/* ... */
} SceModule;
```

> Naming note: `SceModule.modname` is a 27-byte field followed by a dedicated `char terminal`
> that is always `'\0'`, so it is reliably terminated. `SceKernelModuleInfo.name` is a flat
> 28-byte array with **no** guaranteed terminator — those are two different conventions, and the
> plugin uses the second (so it terminates manually).

### Cache writeback / invalidation (used after patching)

The string patches modify **data** in another module's mapped pages. After writing, the data
cache must be flushed back to RAM and invalidated so the live module sees the new bytes. `main.c`
calls, after each `memcpy`:

```c
/* From external/pspsdk/src/user/psputils.h:
 * Write back AND invalidate a range of the data cache. */
void sceKernelDcacheWritebackInvalidateRange(const void *p, unsigned int size);
```

Related cache primitives available in the same header
([external/pspsdk/src/user/psputils.h](external/pspsdk/src/user/psputils.h)) and the kernel
variant ([external/pspsdk/src/kernel/psputilsforkernel.h](external/pspsdk/src/kernel/psputilsforkernel.h)):

```c
/* psputils.h (user) */
void sceKernelDcacheWritebackAll(void);
void sceKernelDcacheWritebackInvalidateAll(void);
void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
void sceKernelDcacheInvalidateRange(const void *p, unsigned int size);
void sceKernelIcacheInvalidateAll(void);
void sceKernelIcacheInvalidateRange(const void *p, unsigned int size);

/* psputilsforkernel.h (kernel) */
void sceKernelDcacheInvalidateAll(void);
int  sceKernelDcacheProbe(void *addr);
void sceKernelIcacheInvalidateAll(void);
void sceKernelIcacheInvalidateRange(const void *addr, unsigned int size);
int  sceKernelIcacheProbe(const void *addr);
```

> **Instruction cache:** the wavezbg patch only rewrites read-only **rodata** path strings (data),
> so a D-cache writeback/invalidate is sufficient. If you ever patch **code** (e.g. NOP-ing a
> branch, hooking a function prologue), you must additionally invalidate the I-cache for that
> range — `sceKernelIcacheInvalidateRange(addr, size)` (or `sceKernelIcacheClearAll()` from
> [psploadcore.h](external/pspsdk/src/kernel/psploadcore.h)) — or the CPU keeps executing the
> stale instructions it already prefetched.

### PRX / module-info macros and the kernel-mode flag

From [external/pspsdk/src/user/pspmoduleinfo.h](external/pspsdk/src/user/pspmoduleinfo.h):

```c
/* Declare the module. The `attributes` argument carries the privilege flags. */
#define PSP_MODULE_INFO(name, attributes, major_version, minor_version) /* ... */

/* Privilege attributes (PspModuleInfoAttr): */
enum PspModuleInfoAttr {
	PSP_MODULE_USER         = 0,
	PSP_MODULE_NO_STOP      = 0x0001,
	PSP_MODULE_SINGLE_LOAD  = 0x0002,
	PSP_MODULE_SINGLE_START = 0x0004,
	PSP_MODULE_KERNEL       = 0x1000,   /* <-- load in kernel mode */
};
```

In [src/main.c](src/main.c) the declaration is:

```c
PSP_MODULE_INFO("wavezbg", 0x1000, 1, 0);   /* 0x1000 == PSP_MODULE_KERNEL */
PSP_MAIN_THREAD_ATTR(0);
```

The `0x1000` attribute is `PSP_MODULE_KERNEL` — **required**, because reading and writing another
module's mapped memory, enumerating kernel modules, and the cache ops are all kernel-mode
operations. A user-mode plugin would fault on the cross-module memory access.

### Firmware version gate

From [external/pspsdk/src/user/pspsysmem.h](external/pspsdk/src/user/pspsysmem.h):

```c
/* Returns 0x0MMmmrr10 — e.g. 0x06060110 for 6.61, 0x06060010 for 6.60. */
int sceKernelDevkitVersion(void);
```

### VSH bridge stub libraries (overview)

`external/pspsdk/src/vsh/` ships three VSH-context **stub libraries** (`.S` import stubs compiled
into `.a` archives) that let an ordinary PRX call into VSH-only resident libraries. They are not
headers with inline code — they resolve NIDs to the firmware's VSH libs at link time. wavezbg does
**not** link any of these (it patches strings directly), but they are the standard entry points
for richer VSH integration:

- **`sceVshBridge`** ([external/pspsdk/src/vsh/sceVshBridge.S](external/pspsdk/src/vsh/sceVshBridge.S) → `libpspvshbridge.a`)
  — bridge to `vshbridge`/VSH-side functionality (launching apps, eject/insert events, registry-ish
  helpers). The broadest "talk to the VSH" stub.
- **`scePaf`** ([external/pspsdk/src/vsh/scePaf.S](external/pspsdk/src/vsh/scePaf.S) → bundled in the vsh archives)
  — stubs for **PAF** (PlayStation Application Framework), the UI/widget/resource framework the XMB
  itself is built on. Used by plugins that draw native-looking XMB UI.
- **`sceChnnlsv`** ([external/pspsdk/src/vsh/sceChnnlsv.S](external/pspsdk/src/vsh/sceChnnlsv.S) → `libpspchnnlsv.a`)
  — stubs for the **chnnlsv** crypto library used for savedata encryption/integrity.

The chnnlsv header [external/pspsdk/src/vsh/pspchnnlsv.h](external/pspsdk/src/vsh/pspchnnlsv.h)
declares (with backwards-compat `sceChnnlsv_*` NID aliases) the savedata cipher/hash API:

```c
int sceSdSetIndex(SceSdContext1 *ctx, int mode);                                  /* init cipher context */
int sceSdRemoveValue(SceSdContext1 *ctx, unsigned char *data, int len);           /* process data (0x10-aligned) */
int sceSdGetLastIndex(SceSdContext1 *ctx, unsigned char *hash, unsigned char *cryptkey); /* finalize hash */
int sceSdCreateList(SceSdContext2 *ctx, int mode1, int mode2,
                    unsigned char *hashkey, unsigned char *cipherkey);            /* prepare key + integrity check */
int sceSdSetMember(SceSdContext2 *ctx, unsigned char *data, int len);             /* process data for integrity */
int sceSdCleanList(SceSdContext2 *ctx);                                           /* check integrity */
```

(Contexts `SceSdContext1`/`SceSdContext2`, a.k.a. `pspChnnlsvContext1/2`, are defined in the same
header.) Not used by wavezbg — listed because it is one of the VSH bridge libs present in-tree.

## Correct usage patterns

The canonical VSH-patch recipe, mirrored from [src/main.c](src/main.c). Function names below
are the real ones in that file.

**1. Version-gate in `module_start`.** Reject any firmware you have not validated, then spawn the
worker thread. Do *not* do the patching work here — VSH modules aren't loaded yet.

```c
int module_start(SceSize args, void *argp) {
    unsigned int ver = sceKernelDevkitVersion();
    if (ver != 0x06060110 && ver != 0x06060010) return 1;   /* 6.61 ARK / 6.60 PRO-C only */

    SceUID thid = sceKernelCreateThread("wavezbg_thread", main_thread,
                                        0x18 /*prio*/, 0x8000 /*32KB stack*/, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, args, argp);
    return 0;
}
```

**2. Do setup, then poll, in the worker thread (`main_thread`).** It first runs `read_config()`
(loads `wave.txt` from `ef0:/`/`ms0:/seplugins/`, falls back to the embedded default) and
`generate_wave_bmp()` (pre-renders `w1.bmp` / `w2.bmp` into `wavez_cache/`), then enters the
bounded poll because the targets load late:

```c
int main_thread(SceSize args, void *argp) {
    read_config();
    generate_wave_bmp();
    for (int attempts = 0; attempts < 600; attempts++) {   /* ~60s @ 100ms */
        if (patch_wave_strings()) break;                   /* success: stop early */
        sceKernelDelayThread(100000);                      /* 100 ms */
    }
    sceKernelExitDeleteThread(0);                          /* always self-delete */
    return 0;
}
```

**3. Find the target modules by name (`patch_wave_strings`).** Enumerate all module UIDs, query
each, force-terminate the name field, and skip anything that isn't a VSH background module:

```c
SceUID ids[100]; int count = 0;
if (sceKernelGetModuleIdList(ids, sizeof(ids), &count) >= 0) {
    int max_ids = (int)(sizeof(ids) / sizeof(ids[0]));
    if (count > max_ids) count = max_ids;                  /* clamp to capacity */
    for (int i = 0; i < count; i++) {
        SceKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelQueryModuleInfo(ids[i], &info) < 0) continue;
        info.name[sizeof(info.name) - 1] = '\0';           /* fixed 28-byte field */
        if (strstr(info.name, "system_plugin_bg") == NULL &&
            strstr(info.name, "sysconf_plugin")   == NULL &&
            strstr(info.name, "vsh")              == NULL) continue;
        /* ... walk segments ... */
    }
}
```

**4. Walk each segment separately**, with a text-segment fallback when the table is empty. **Never
treat the module as one contiguous block** — see [Common mistakes](#common-mistakes):

```c
int nseg = info.nsegment;
if (nseg > 4) nseg = 4;                                    /* SCE_KERNEL_MAX_MODULE_SEGMENT */
if (nseg > 0) {
    for (int s = 0; s < nseg; s++) {
        char *start = (char *)(unsigned int)info.segmentaddr[s];
        unsigned int sz = (unsigned int)info.segmentsize[s];
        if (patch_range(start, start + sz, replace1, replace2)) patched = 1;
    }
} else if (info.text_addr != 0) {                          /* fallback: text segment only */
    char *start = (char *)info.text_addr;
    if (patch_range(start, start + info.text_size, replace1, replace2)) patched = 1;
}
```

**5. Scan the segment, patch in place with byte-safe ops, and invalidate the cache (`patch_range`).**
A cheap first-byte gate avoids `strncmp` on most bytes; matched strings are overwritten with
`memcpy`, then the written range is flushed:

```c
while (addr + 34 <= end_addr) {                            /* keep 34 readable bytes ahead */
    if (addr[0]=='f' && addr[1]=='l' && addr[2]=='a' && addr[3]=='s' && addr[4]=='h') {
        if (strncmp(addr, "flash0:/vsh/resource/01-12.bmp", 30) == 0) {
            memcpy(addr, replace1, 30);
            sceKernelDcacheWritebackInvalidateRange(addr, 30);   /* flush the 30 bytes we wrote */
            patched = 1;
        }
        /* ...also 01-12_03g.bmp (34 bytes) -> replace1, and 13-27.bmp (30 bytes) -> replace2... */
    }
    addr++;
}
```

**6. Idempotency** is structural: once a path is rewritten it no longer begins with `"flash"`, so
the first-byte gate skips it on every later poll/pass. No "already patched" bookkeeping is needed.

**7. Cache invalidation** happens per write (`sceKernelDcacheWritebackInvalidateRange` over exactly
the bytes touched). Because only data strings change, no I-cache invalidation is required here.

**8. Bound the poll and self-delete.** The loop caps at 600 attempts (~60 s) so an unexpected setup
(renamed modules, CXMB, etc.) can't spin forever, and `sceKernelExitDeleteThread(0)` always runs so
the worker thread never lingers — whether or not patching succeeded.

## Common mistakes

Each of these is a real invariant in this project (see [CLAUDE.md](CLAUDE.md) "Robustness &
memory invariants"). Tie-in API/struct noted per item.

- **Scanning the module as one `[text_addr, text_addr + text_size + data_size + bss_size)` block.**
  A module's segments (`SceKernelModuleInfo.segmentaddr[]` / `SceModule.segmentaddr[]`) are
  page-aligned and **need not be adjacent** — the gaps between them are **unmapped**. A scan that
  runs across a gap dereferences an unmapped address and raises a **TLB-miss exception**, crashing
  the PSP. Whether a given module has a gap depends on its layout, which is why the crash was
  *random*. **Fix:** walk each `segmentaddr[s]`/`segmentsize[s]` separately (clamp `nsegment` to 4 =
  `SCE_KERNEL_MAX_MODULE_SEGMENT`), with a `text_addr`/`text_size`-only fallback. This was the
  random-crash bug fixed in this fork.

- **Forgetting cache invalidation after patching.** You wrote new bytes via the data cache; until
  you `sceKernelDcacheWritebackInvalidateRange(addr, n)` (from
  [psputils.h](external/pspsdk/src/user/psputils.h)) the target module may still read the stale
  cached copy. If you ever patch **code**, also `sceKernelIcacheInvalidateRange` — the CPU executes
  prefetched stale instructions otherwise.

- **Unaligned word stores on Allegrex MIPS.** Writing multi-byte fields with
  `*(unsigned int *)&buf[off] = x` at an offset that isn't 4-byte aligned (e.g. BMP header fields
  that sit at offsets ≡ 2 mod 4 because of the 2-byte `"BM"` magic) is an **Address Error
  exception** on the MIPS core and undefined behaviour in C. **Fix:** write byte-by-byte
  (`put_u32` / `put_u16` in `write_bmp`). The string `memcpy`s in `patch_range` are byte-granular
  and therefore already safe.

- **Assuming `SceKernelModuleInfo.name` is NUL-terminated.** It is a fixed **28-byte** field with
  no guaranteed terminator (unlike `SceModule.modname`, which has a dedicated trailing `terminal`
  byte). A maxed-out name lets `strstr` run off the end of the struct. **Fix:**
  `info.name[sizeof(info.name) - 1] = '\0';` before any `strstr`.

- **Not bounding the poll / leaking the worker thread.** An unbounded retry loop hangs the thread
  forever on setups where the patch never lands. **Fix:** cap attempts (600 × 100 ms here) and
  always reach `sceKernelExitDeleteThread(0)` so the thread is reclaimed regardless of outcome.

- **Running before the VSH modules exist.** `module_start` fires before `system_plugin_bg` / `vsh`
  are loaded, so `sceKernelGetModuleIdList` won't list them yet and a one-shot patch silently does
  nothing. **Fix:** poll from a worker thread until the modules appear (the success path usually
  exits within the first second or two).

- **Not making the patch idempotent (double-patching).** The poll re-scans every 100 ms and a
  module may be scanned across multiple passes; re-copying over an already-rewritten string
  (especially with a different length) corrupts it. **Fix:** make "already patched" unrepresentable
  — after rewriting, the string no longer starts with `"flash"`, so the gate skips it.

- **Forgetting `info.size` before `sceKernelQueryModuleInfo`.** The kernel uses `size` to decide
  how much of the struct it may fill; leaving it zero/garbage yields a failed or partial query.
  **Fix:** `info.size = sizeof(info);` after zeroing.

## Sample references

- [src/main.c](src/main.c) — **primary example.** The full VSH-patch implementation:
  version gate, worker thread, module enumeration, per-segment scan, in-place string patch, cache
  flush, bounded poll.
- [CLAUDE.md](CLAUDE.md) — project architecture and the robustness/memory invariants that the
  [Common mistakes](#common-mistakes) section codifies (segment-walk, cache, alignment, idempotency).
- [external/pspsdk/src/kernel/psploadcore.h](external/pspsdk/src/kernel/psploadcore.h) —
  `SceModule` struct and `sceKernelFindModuleByName` / `sceKernelFindModuleByAddress` (the
  alternative discovery path); `SCE_KERNEL_MAX_MODULE_SEGMENT`, `SCE_MODULE_VSH`.
- [external/pspsdk/src/user/pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h) —
  `SceKernelModuleInfo`, `sceKernelGetModuleIdList`, `sceKernelQueryModuleInfo` (the path `main.c`
  uses).
- [external/pspsdk/src/user/psputils.h](external/pspsdk/src/user/psputils.h) — data/instruction
  cache writeback & invalidate functions (`sceKernelDcacheWritebackInvalidateRange` used after each
  patch).
- [external/pspsdk/src/kernel/psputilsforkernel.h](external/pspsdk/src/kernel/psputilsforkernel.h)
  — kernel cache primitives (`sceKernelIcacheInvalidateRange`, etc.).
- [external/pspsdk/src/user/pspmoduleinfo.h](external/pspsdk/src/user/pspmoduleinfo.h) —
  `PSP_MODULE_INFO` macro and the `PSP_MODULE_KERNEL` (`0x1000`) flag.
- [external/pspsdk/src/user/pspsysmem.h](external/pspsdk/src/user/pspsysmem.h) —
  `sceKernelDevkitVersion` prototype and its return-value encoding.
- [external/pspsdk/src/vsh/pspchnnlsv.h](external/pspsdk/src/vsh/pspchnnlsv.h) — chnnlsv
  (`sceSd*`) savedata crypto API; one of the VSH bridge libs present in-tree.
- [external/pspsdk/src/samples/prx/testprx/main.c](external/pspsdk/src/samples/prx/testprx/main.c)
  — minimal PRX skeleton (`PSP_MODULE_INFO` + entry); a barebones reference for PRX structure (it
  does **not** demonstrate VSH hooking).
- [external/pspsdk/src/samples/prx/prx_loader/main.c](external/pspsdk/src/samples/prx/prx_loader/main.c)
  — PRX loader sample using `sceKernelLoadModule`/`sceKernelStartModule`; shows module-management
  APIs but, again, not the live-memory string patch technique.
