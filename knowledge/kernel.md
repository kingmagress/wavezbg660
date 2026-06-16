> Generated from `external/pspsdk` headers + `src/main.c`. Verify signatures against the header before use.

# PSP Kernel / System APIs (firmware 6.60 / 6.61)

A working reference for kernel-mode VSH/XMB plugin (PRX) development, grounded in the
real PSPSDK headers shipped in this repo and the patterns used by
[src/main.c](src/main.c). Every signature below was copied verbatim from a header;
file links point at the exact source.

---

## Overview

### Kernel vs user mode

The Allegrex MIPS core runs PSP code in one of two privilege levels:

- **User mode** — what games and most homebrew run in. The kernel mediates access to
  hardware, kernel memory, and privileged APIs through syscall stubs. Many `sce*`
  functions have a user-mode stub; the kernel-only ones do not and will fault (or be
  rejected at link time) if called from user mode.
- **Kernel mode** — full access to the kernel memory partition, the loaded-module
  list, raw physical memory, cache control, and interrupt control. This is what a VSH
  plugin PRX needs in order to scan and patch another module's live memory.

A PRX declares itself a kernel module via the attribute flag in `PSP_MODULE_INFO`.
This repo uses `0x1000` (`PSP_MODULE_KERNEL`) — see
[src/main.c](src/main.c) line 18:

```c
PSP_MODULE_INFO("wavezbg", 0x1000, 1, 0);
PSP_MAIN_THREAD_ATTR(0);
```

The flag values come from
[pspmoduleinfo.h](external/pspsdk/src/user/pspmoduleinfo.h):

```c
enum PspModuleInfoAttr
{
    PSP_MODULE_USER         = 0,
    PSP_MODULE_NO_STOP      = 0x0001,
    PSP_MODULE_SINGLE_LOAD  = 0x0002,
    PSP_MODULE_SINGLE_START = 0x0004,
    PSP_MODULE_KERNEL       = 0x1000,
};
```

### What a kernel PRX can do

- Enumerate every loaded module (not just user modules) and read its `SceModule` /
  `SceKernelModuleInfo`.
- Read and **write** another module's mapped TEXT/DATA segments (e.g. patch a
  hard-coded resource path string), then flush the caches so the CPU sees the change.
- Allocate from kernel memory partitions, control interrupts, and call the kernel-only
  cache/decompress APIs.

### Firmware version gate

`sceKernelDevkitVersion()` returns the firmware version packed as `0x0MMmmrr10`. This
repo gates `module_start` on the exact versions it supports (see
[src/main.c](src/main.c) line 404):

```c
unsigned int ver = sceKernelDevkitVersion();
if (ver != 0x06060110 && ver != 0x06060010) {
    return 1;  // refuse to start on unsupported firmware
}
```

| Firmware       | Devkit version | CFW (this repo) |
| -------------- | -------------- | --------------- |
| 6.60           | `0x06060010`   | PRO-C           |
| 6.61           | `0x06060110`   | ARK             |

Returning non-zero from `module_start` aborts the module load, so an unsupported
firmware simply gets no plugin rather than a crash.

---

## Key APIs

> Headers ending in `_kernel.h` expose kernel-only functions. The plain header is the
> user/general interface; some functions appear in both. Always include the header the
> signature came from.

### Memory — system memory manager

From [pspsysmem.h](external/pspsdk/src/user/pspsysmem.h):

```c
/** Specifies the type of allocation used for memory blocks. */
enum PspSysMemBlockTypes {
    PSP_SMEM_Low = 0,   /* Allocate from the lowest available address.  */
    PSP_SMEM_High,      /* Allocate from the highest available address. */
    PSP_SMEM_Addr       /* Allocate from the specified address.         */
};

SceUID  sceKernelAllocPartitionMemory(SceUID partitionid, const char *name,
                                      int type, SceSize size, void *addr);
int     sceKernelFreePartitionMemory(SceUID blockid);
void   *sceKernelGetBlockHeadAddr(SceUID blockid);

SceSize sceKernelTotalFreeMemSize(void);   /* kernel-partition free memory */
SceSize sceKernelMaxFreeMemSize(void);     /* largest free kernel block    */
```

`sceKernelAllocPartitionMemory` returns a block **UID**, not a pointer — resolve the
address with `sceKernelGetBlockHeadAddr(blockid)`. The `addr` argument is only used
when `type == PSP_SMEM_Addr`.

**Partition IDs** — defined in
[pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h):

```c
#define PSP_MEMORY_PARTITION_KERNEL 1
#define PSP_MEMORY_PARTITION_USER   2
```

(The `loadmodule` sample hard-codes `1`/`2` for these same partitions —
[samples/kernel/loadmodule/main.c](external/pspsdk/src/samples/kernel/loadmodule/main.c)
lines 104-108.)

Kernel-only partition queries and heap APIs from
[pspsysmem_kernel.h](external/pspsdk/src/kernel/pspsysmem_kernel.h):

```c
int     sceKernelQueryMemoryPartitionInfo(int pid, PspSysmemPartitionInfo *info);
SceSize sceKernelPartitionTotalFreeMemSize(int pid);
SceSize sceKernelPartitionMaxFreeMemSize(int pid);

SceUID  sceKernelCreateHeap(SceUID partitionid, SceSize size, int unk, const char *name);
void   *sceKernelAllocHeapMemory(SceUID heapid, SceSize size);
int     sceKernelFreeHeapMemory(SceUID heapid, void *block);
int     sceKernelDeleteHeap(SceUID heapid);

int     sceKernelGetModel(void);   /* <= 0 original, 1 slim */
```

> **This repo allocates no heap or partition memory at all** — every buffer is on the
> worker-thread stack or static. That is a deliberate invariant (no leaks by
> construction). The allocation APIs are documented here for completeness; prefer the
> stack/static approach for a small resident plugin.

### Modules / loadcore — `SceModule` and lookups

From [psploadcore.h](external/pspsdk/src/kernel/psploadcore.h). The full struct is
large; the fields relevant to **scanning a loaded module's memory** are:

```c
#define SCE_KERNEL_MAX_MODULE_SEGMENT (4)

typedef struct SceModule {
    struct SceModule *next;     /* linked list of loaded modules           */
    u16   attribute;
    u8    version[2];
    char  modname[27];          /* module name                             */
    char  terminal;             /* always '\0' (name terminator)           */
    /* ... */
    u32   text_addr;            /* start address of the TEXT segment       */
    u32   text_size;            /* size of the TEXT segment                */
    u32   data_size;            /* size of the DATA segment                */
    u32   bss_size;             /* size of the BSS segment                 */
    u8    nsegment;             /* number of segments (<= 4)               */
    u8    padding2[3];
    u32   segmentaddr[4];       /* start address of each segment           */
    SceSize segmentsize[4];     /* size of each segment                    */
    u32   segmentalign[4];      /* alignment of each segment               */
    /* ... many more fields ... */
} SceModule;

SceModule *sceKernelFindModuleByName(const char *modname);
SceModule *sceKernelFindModuleByAddress(unsigned int addr);
SceModule *sceKernelFindModuleByUID(SceUID modid);
int        sceKernelModuleCount(void);
void       sceKernelIcacheClearAll(void);
```

> **Critical:** `segmentaddr[i]` / `segmentsize[i]` describe the **separately mapped**
> segments. `nsegment` says how many of the 4 slots are valid. `text_addr` /
> `text_size` / `data_size` / `bss_size` describe the segments individually too — they
> are **not** a single contiguous range (see Common Mistakes).

This repo, however, enumerates modules through the **module-manager** view, not by
walking the `SceModule` linked list — see the next section for the
`SceKernelModuleInfo` struct it actually uses, which carries the same
`nsegment` / `segmentaddr[]` / `segmentsize[]` / `text_addr` fields.

### Module manager — info, listing, load/start/stop

From [pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h):

```c
typedef struct SceKernelModuleInfo {
    SceSize      size;
    char         nsegment;
    char         reserved[3];
    int          segmentaddr[4];
    int          segmentsize[4];
    unsigned int entry_addr;
    unsigned int gp_value;
    unsigned int text_addr;
    unsigned int text_size;
    unsigned int data_size;
    unsigned int bss_size;
    unsigned short attribute;       /* v1.5+ */
    unsigned char  version[2];
    char           name[28];        /* fixed 28 bytes, NOT guaranteed NUL-terminated */
} SceKernelModuleInfo;

int sceKernelQueryModuleInfo(SceUID modid, SceKernelModuleInfo *info);
int sceKernelGetModuleIdList(SceUID *readbuf, int readbufsize, int *idcount);
int sceKernelGetModuleIdByAddress(const void *moduleAddr);
```

> Before calling `sceKernelQueryModuleInfo`, **set `info.size = sizeof(info)`** (the
> kernel uses it to know which struct layout you expect). This repo does exactly that —
> [src/main.c](src/main.c) lines 330-333.

Load / start / stop / unload (also from
[pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h)):

```c
SceUID sceKernelLoadModule(const char *path, int flags, SceKernelLMOption *option);
SceUID sceKernelLoadModuleByID(SceUID fid, int flags, SceKernelLMOption *option);
int    sceKernelStartModule(SceUID modid, SceSize argsize, void *argp,
                            int *status, SceKernelSMOption *option);
int    sceKernelStopModule(SceUID modid, SceSize argsize, void *argp,
                           int *status, SceKernelSMOption *option);
int    sceKernelUnloadModule(SceUID modid);
int    sceKernelStopUnloadSelfModule(SceSize argsize, void *argp,
                                     int *status, SceKernelSMOption *option);
```

`flags` is unused — always pass `0`. `option` may be `NULL`. From
[pspmodulemgr_kernel.h](external/pspsdk/src/kernel/pspmodulemgr_kernel.h):

```c
SceUID sceKernelLoadModuleBuffer(void *buf, SceSize bufsize, int flags,
                                 SceKernelLMOption *option);
int    sceKernelGetModuleList(int readbufsize, SceUID *readbuf);
int    sceKernelModuleCount(void);
```

> Note the **argument order differs** between `sceKernelGetModuleIdList(buf, size,
> &count)` (user header) and `sceKernelGetModuleList(size, buf)` (kernel header). This
> repo uses `sceKernelGetModuleIdList`.

### Cache — why a memory-patching plugin must flush

The Allegrex has separate data (D) and instruction (I) caches. When you write to
another module's memory by storing through the D-cache, the change can sit in the
D-cache and never reach physical RAM, and the I-cache may still hold the old
instructions/data. After patching live memory you must **write the D-cache back to RAM
and invalidate** the affected range (and invalidate the I-cache if you patched code).

User/general cache ops from [psputils.h](external/pspsdk/src/user/psputils.h):

```c
void sceKernelDcacheWritebackAll(void);
void sceKernelDcacheWritebackInvalidateAll(void);
void sceKernelDcacheWritebackRange(const void *p, unsigned int size);
void sceKernelDcacheWritebackInvalidateRange(const void *p, unsigned int size);
void sceKernelDcacheInvalidateRange(const void *p, unsigned int size);
void sceKernelIcacheInvalidateAll(void);
void sceKernelIcacheInvalidateRange(const void *p, unsigned int size);
```

Kernel-only cache ops from
[psputilsforkernel.h](external/pspsdk/src/kernel/psputilsforkernel.h):

```c
void sceKernelDcacheInvalidateAll(void);
int  sceKernelDcacheProbe(void *addr);
void sceKernelIcacheInvalidateAll(void);
void sceKernelIcacheInvalidateRange(const void *addr, unsigned int size);
int  sceKernelIcacheProbe(const void *addr);

/* gzip/deflate decompression (kernel mode) */
int  sceKernelGzipDecompress(u8 *dest, u32 destSize, const u8 *src, u8 **src_out);
int  sceKernelDeflateDecompress(u8 *dest, u32 destSize, const u8 *src, u8 **src_out);
```

This repo writes-back-and-invalidates each patched range immediately after the
`memcpy` — [src/main.c](src/main.c) lines 290, 295, 299:

```c
memcpy(addr, replace1, 30);
sceKernelDcacheWritebackInvalidateRange(addr, 30);
```

`...WritebackInvalidateRange` is the right call here: it flushes the bytes you just
wrote out to RAM **and** drops the stale cache line, in one call, over only the bytes
that changed (cheaper than the `...All` variants).

### File I/O

Functions from [pspiofilemgr.h](external/pspsdk/src/user/pspiofilemgr.h):

```c
SceUID sceIoOpen(const char *file, int flags, SceMode mode);
int    sceIoClose(SceUID fd);
int    sceIoRead(SceUID fd, void *data, SceSize size);
int    sceIoWrite(SceUID fd, const void *data, SceSize size);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
int    sceIoLseek32(SceUID fd, int offset, int whence);
int    sceIoRemove(const char *file);
int    sceIoMkdir(const char *dir, SceMode mode);
int    sceIoRmdir(const char *path);
int    sceIoRename(const char *oldname, const char *newname);
int    sceIoGetstat(const char *file, SceIoStat *stat);

SceUID sceIoDopen(const char *dirname);
int    sceIoDread(SceUID fd, SceIoDirent *dir);
int    sceIoDclose(SceUID fd);
```

`sceIoOpen` returns a non-negative `SceUID` on success, **negative on error** — always
test `fd >= 0` (this repo tests `fd < 0`). `sceIoRead` / `sceIoWrite` return the byte
count (or `< 0`).

Open flags from
[pspiofilemgr_fcntl.h](external/pspsdk/src/user/pspiofilemgr_fcntl.h):

```c
#define PSP_O_RDONLY  0x0001
#define PSP_O_WRONLY  0x0002
#define PSP_O_RDWR    (PSP_O_RDONLY | PSP_O_WRONLY)
#define PSP_O_NBLOCK  0x0004
#define PSP_O_DIR     0x0008
#define PSP_O_APPEND  0x0100
#define PSP_O_CREAT   0x0200
#define PSP_O_TRUNC   0x0400
#define PSP_O_EXCL    0x0800
#define PSP_O_NOWAIT  0x8000

#define PSP_SEEK_SET  0
#define PSP_SEEK_CUR  1
#define PSP_SEEK_END  2
```

Device prefixes used by this repo: `ms0:/` (Memory Stick) and `ef0:/` (PSP Go internal
flash). It tries `ef0:` first, falls back to `ms0:` — [src/main.c](src/main.c)
lines 106-114.

### Version / init macros and version query

`sceKernelDevkitVersion` is declared in
[pspsysmem.h](external/pspsdk/src/user/pspsysmem.h):

```c
int sceKernelDevkitVersion(void);
```

Module-declaration and thread macros from
[pspmoduleinfo.h](external/pspsdk/src/user/pspmoduleinfo.h):

```c
PSP_MODULE_INFO(name, attributes, major_version, minor_version)  /* required */
PSP_MAIN_THREAD_ATTR(attr)
PSP_MAIN_THREAD_PRIORITY(priority)
PSP_MAIN_THREAD_STACK_SIZE_KB(size_kb)
PSP_HEAP_SIZE_KB(size_kb)
PSP_HEAP_THRESHOLD_SIZE_KB(size_kb)
PSP_NO_CREATE_MAIN_THREAD()
```

### Interrupt control (use sparingly)

If you must make a memory edit atomic w.r.t. interrupts, the CPU-level suspend/resume
pair from [pspintrman.h](external/pspsdk/src/user/pspintrman.h):

```c
unsigned int sceKernelCpuSuspendIntr(void);             /* returns prior state */
void         sceKernelCpuResumeIntr(unsigned int flags);
void         sceKernelCpuResumeIntrWithSync(unsigned int flags);
int          sceKernelIsCpuIntrEnable(void);
```

Per-IRQ enable/disable from
[pspintrman_kernel.h](external/pspsdk/src/kernel/pspintrman_kernel.h):

```c
int sceKernelEnableIntr(int intno);
int sceKernelDisableIntr(int intno);
int sceKernelIsIntrContext(void);
```

> Do **not** hold interrupts disabled for long — the watchdog will reset the system.
> This repo does **not** disable interrupts; its patches are small `memcpy`s of
> read-only path strings, where a torn write would at worst be re-patched on the next
> poll. Reach for interrupt control only when correctness genuinely requires atomicity.

---

## Correct usage patterns

### Module enumeration + per-segment scan (the core of this plugin)

Get the module ID list, query each module, force-terminate the name, filter by name,
then scan **each segment separately**. Condensed from
[src/main.c](src/main.c) lines 322-374:

```c
SceUID ids[100];
int count = 0;
if (sceKernelGetModuleIdList(ids, sizeof(ids), &count) >= 0) {
    /* count may exceed what fit in ids[]; clamp to capacity */
    int max_ids = (int)(sizeof(ids) / sizeof(ids[0]));
    if (count > max_ids) count = max_ids;

    for (int i = 0; i < count; i++) {
        SceKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);                 /* required before query   */

        if (sceKernelQueryModuleInfo(ids[i], &info) < 0) continue;

        /* name[] is a fixed 28-byte field; force a terminator before strstr */
        info.name[sizeof(info.name) - 1] = '\0';

        if (strstr(info.name, "system_plugin_bg") == NULL &&
            strstr(info.name, "sysconf_plugin")   == NULL &&
            strstr(info.name, "vsh")              == NULL) {
            continue;
        }

        int nseg = info.nsegment;
        if (nseg > 4) nseg = 4;                    /* clamp to array capacity */

        if (nseg > 0) {
            for (int s = 0; s < nseg; s++) {
                char *start = (char *)(unsigned int)info.segmentaddr[s];
                unsigned int seg_size = (unsigned int)info.segmentsize[s];
                patch_range(start, start + seg_size, /* ... */);   /* ONE segment */
            }
        } else if (info.text_addr != 0) {
            /* fallback: segment table empty -> scan just the text segment */
            char *start = (char *)info.text_addr;
            patch_range(start, start + info.text_size, /* ... */);
        }
    }
}
```

Key points the plugin relies on:

- `info.size = sizeof(info)` is set before the query, and `info.name` is
  NUL-terminated before any `strstr`.
- Each `patch_range(start, start + seg_size, ...)` call covers exactly one mapped
  segment, never a span of two.
- The fallback path (empty segment table) scans only the TEXT segment, which is also a
  single mapped region — still gap-safe.

### Bounded poll instead of blocking the VSH

The target VSH modules are not loaded when `module_start` runs, so the worker thread
polls and always exits (never spins forever) — [src/main.c](src/main.c) lines
393-401:

```c
for (int attempts = 0; attempts < 600; attempts++) {   /* ~60s at 100ms */
    if (patch_wave_strings()) break;
    sceKernelDelayThread(100000);                       /* 100 ms          */
}
sceKernelExitDeleteThread(0);   /* always reached -> thread never lingers */
```

### Safe `sceIoOpen` → read/write → `sceIoClose` with error checks

Every open is matched by a close on **every** path. Read pattern (
[src/main.c](src/main.c) lines 106-153):

```c
SceUID fd = sceIoOpen("ef0:/seplugins/wave.txt", PSP_O_RDONLY, 0777);
if (fd >= 0) {
    char buf[4096];
    int bytes = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);                 /* close before using the data */
    if (bytes > 0) {
        buf[bytes] = '\0';          /* NUL-terminate what we read  */
        /* ... parse buf ... */
    }
}
```

Write pattern with create+truncate ([src/main.c](src/main.c) lines 180-251):

```c
SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
if (fd < 0) return;                 /* bail early, nothing to close */
/* ... build buffer ... */
sceIoWrite(fd, bmp_data, 6176);
sceIoClose(fd);
```

Directories are created with `sceIoMkdir(path, 0777)` before writing into them
([src/main.c](src/main.c) lines 123, 259).

### Patch memory, then write back + invalidate

```c
memcpy(addr, replacement, n);                       /* edit live module memory */
sceKernelDcacheWritebackInvalidateRange(addr, n);   /* push to RAM, drop stale */
```

Do this for **each** patched range, over only the bytes you changed. If you patched
executable code (not the case here — these are rodata path strings), also call
`sceKernelIcacheInvalidateRange(addr, n)` so the CPU refetches the new instructions.

---

## Common mistakes

- **Scanning `[text_addr, text_addr + text_size + data_size + bss_size)` as one
  contiguous block.** A module's segments are page-aligned and need **not** be
  adjacent; the gaps between them are unmapped. A single scan across that span steps
  into an unmapped page and raises a **TLB-miss exception → instant crash**. Because
  whether a gap exists depends on the module's layout, the crash is *random*. **This
  repo fixed exactly this bug** — it now walks `nsegment` / `segmentaddr[i]` /
  `segmentsize[i]` separately (`sceKernelQueryModuleInfo` + the loop above). Never
  revert to the contiguous-span scan.

- **Forgetting cache invalidation after patching memory.** A write that stays in the
  D-cache never reaches RAM, so the target module keeps reading the old bytes; stale
  I-cache means the CPU keeps executing old code. Always
  `sceKernelDcacheWritebackInvalidateRange` (and, for code,
  `sceKernelIcacheInvalidateRange`) over the patched range.

- **Unaligned word stores on Allegrex.** Writing a 32-bit value with
  `*(unsigned int *)&buf[off] = v` when `off` is not 4-byte aligned is an **Address
  Error exception** on MIPS (and UB in C). This repo writes multi-byte header fields
  one byte at a time via `put_u32` / `put_u16` ([src/main.c](src/main.c) lines
  162-172) precisely because the BMP fields land at offsets ≡ 2 (mod 4). Keep
  byte-wise writes for any field whose alignment you can't guarantee.

- **Not closing file handles on every path.** `sceIoOpen` returns a `SceUID`; a leaked
  fd is a kernel resource leak. Match every successful open with a close on success
  **and** error paths (this repo closes the check-file handle even on the cache-hit
  early-return — [src/main.c](src/main.c) lines 264-268).

- **Calling kernel-only APIs from user mode.** Functions declared only in a `_kernel.h`
  header (e.g. `sceKernelGzipDecompress`, `sceKernelDcacheInvalidateAll`,
  `sceKernelEnableIntr`) require the module to be built as a kernel module
  (`PSP_MODULE_KERNEL` / `0x1000`). From a user-mode module they will fault or fail to
  link.

- **Assuming `info.name` is NUL-terminated.** `SceKernelModuleInfo.name` is a fixed
  `char name[28]` with no guaranteed terminator. A maxed-out name makes `strstr` run
  off the end of the struct. Force `info.name[sizeof(info.name) - 1] = '\0'` first
  ([src/main.c](src/main.c) line 338). The same applies to `SceModule.modname[27]`,
  though that struct does carry a separate `terminal` byte.

- **Forgetting `info.size = sizeof(info)` before `sceKernelQueryModuleInfo`.** The
  kernel keys the returned layout off this field; leave it zero and the query may fail
  or fill the wrong fields.

- **Blocking the VSH/main thread.** The XMB must stay responsive. Do work on a spawned
  thread that delays (`sceKernelDelayThread`) between attempts and **always** exits
  (`sceKernelExitDeleteThread`); never busy-spin or block indefinitely on the main
  thread.

---

## Sample references

Headers used (paths relative to repo root):

- [external/pspsdk/src/user/pspsysmem.h](external/pspsdk/src/user/pspsysmem.h) —
  `sceKernelAllocPartitionMemory` family, `PSP_SMEM_*`, `sceKernelDevkitVersion`.
- [external/pspsdk/src/kernel/pspsysmem_kernel.h](external/pspsdk/src/kernel/pspsysmem_kernel.h) —
  kernel heap / partition-query APIs, `sceKernelGetModel`.
- [external/pspsdk/src/kernel/psploadcore.h](external/pspsdk/src/kernel/psploadcore.h) —
  `SceModule` struct, `sceKernelFindModuleBy*`, `SCE_KERNEL_MAX_MODULE_SEGMENT`.
- [external/pspsdk/src/user/pspmodulemgr.h](external/pspsdk/src/user/pspmodulemgr.h) —
  `SceKernelModuleInfo`, `sceKernelQueryModuleInfo`, `sceKernelGetModuleIdList`,
  load/start/stop/unload, `PSP_MEMORY_PARTITION_*`.
- [external/pspsdk/src/kernel/pspmodulemgr_kernel.h](external/pspsdk/src/kernel/pspmodulemgr_kernel.h) —
  `sceKernelGetModuleList`, `sceKernelLoadModuleBuffer`.
- [external/pspsdk/src/kernel/psputilsforkernel.h](external/pspsdk/src/kernel/psputilsforkernel.h) —
  kernel D/I-cache invalidate + gzip/deflate decompress.
- [external/pspsdk/src/user/psputils.h](external/pspsdk/src/user/psputils.h) —
  D-cache writeback/invalidate range+all, I-cache invalidate range+all.
- [external/pspsdk/src/user/pspiofilemgr.h](external/pspsdk/src/user/pspiofilemgr.h) —
  `sceIoOpen/Read/Write/Close/Lseek/Remove/Mkdir/Dopen…`.
- [external/pspsdk/src/user/pspiofilemgr_fcntl.h](external/pspsdk/src/user/pspiofilemgr_fcntl.h) —
  `PSP_O_*` open flags, `PSP_SEEK_*`.
- [external/pspsdk/src/user/pspmoduleinfo.h](external/pspsdk/src/user/pspmoduleinfo.h) —
  `PSP_MODULE_INFO`, `PSP_MAIN_THREAD_ATTR`, `PSP_HEAP_SIZE_KB`, `PSP_MODULE_KERNEL`.
- [external/pspsdk/src/sdk/pspsdk.h](external/pspsdk/src/sdk/pspsdk.h) —
  `pspSdkDisableInterrupts` / `pspSdkSetK1` and other SDK helpers.
- [external/pspsdk/src/user/pspkerneltypes.h](external/pspsdk/src/user/pspkerneltypes.h) —
  `SceUID`, `SceSize`, `SceMode`, `SceOff`.
- [external/pspsdk/src/user/pspintrman.h](external/pspsdk/src/user/pspintrman.h) and
  [external/pspsdk/src/kernel/pspintrman_kernel.h](external/pspsdk/src/kernel/pspintrman_kernel.h) —
  CPU interrupt suspend/resume and per-IRQ enable/disable.

Reference implementation:

- [src/main.c](src/main.c) — the live plugin: firmware gate (`module_start`),
  module enumeration + per-segment scan (`patch_wave_strings` / `patch_range`),
  cache writeback-invalidate after each patch, bounded poll worker thread, and the
  matched `sceIoOpen`/`sceIoClose` file patterns. **The authoritative example of every
  pattern above.**

PSPSDK samples (relative to repo root):

- [external/pspsdk/src/samples/kernel/loadmodule/main.c](external/pspsdk/src/samples/kernel/loadmodule/main.c) —
  `sceKernelLoadModule` + `sceKernelStartModule` with `SceKernelLMOption` and the
  `1`/`2` (kernel/user) partition IDs; kernel-mode setup in a `constructor`.
- [external/pspsdk/src/samples/kernel/fileio/main.c](external/pspsdk/src/samples/kernel/fileio/main.c) —
  `sceIo*` file-I/O usage from a kernel sample.
- [external/pspsdk/src/samples/prx/testprx/main.c](external/pspsdk/src/samples/prx/testprx/main.c) —
  minimal PRX skeleton (`module_start` / `module_stop`, `PSP_MODULE_INFO`).
