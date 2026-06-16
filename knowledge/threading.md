Generated from external/pspsdk headers + samples + src/main.c. Verify signatures against the header before use.

# PSP Threading (ThreadMan)

## Overview

The PSP runs a **priority-based, preemptive** scheduler. Each thread has a priority where a **lower number means higher priority** (the header repeatedly notes "the lower the number the higher the priority"). At any instant the runnable thread with the numerically lowest priority runs; threads of equal priority share the CPU via the ready queue (see `sceKernelRotateThreadReadyQueue`). A higher-priority thread becoming runnable preempts a lower-priority one immediately.

Thread states are enumerated by `enum PspThreadStatus` in [external/pspsdk/src/user/pspthreadman.h](external/pspsdk/src/user/pspthreadman.h):

| State                 | Value | Meaning                                                   |
| --------------------- | ----- | --------------------------------------------------------- |
| `PSP_THREAD_RUNNING`  | 1     | Currently executing                                       |
| `PSP_THREAD_READY`    | 2     | Runnable, waiting for the CPU                             |
| `PSP_THREAD_WAITING`  | 4     | Blocked on a wait (sema/event/sleep/delay/thread-end/etc) |
| `PSP_THREAD_SUSPEND`  | 8     | Suspended                                                 |
| `PSP_THREAD_STOPPED`  | 16    | Created but not started, or exited                        |
| `PSP_THREAD_KILLED`   | 32    | Killed by the thread manager (stack overflow)             |

**Why VSH plugins must not block the main/VSH thread:** this repo is a kernel-mode VSH/XMB plugin. The XMB and its plugins are cooperatively sharing the firmware's threads; if plugin code runs on (or blocks) a VSH/system thread, the menu UI stalls or the system can hang. The plugin therefore does **no work in `module_start`** beyond a version check and spawning its own worker thread; all polling/patching happens on that dedicated thread, which delays with `sceKernelDelayThread` (yielding the CPU) rather than busy-spinning, and self-deletes when finished. See [src/main.c](src/main.c) `module_start` / `main_thread`.

## Key APIs

All signatures below are copied verbatim from [external/pspsdk/src/user/pspthreadman.h](external/pspsdk/src/user/pspthreadman.h) unless noted otherwise. Only APIs present in the headers are listed.

### Thread entry signature

```c
/* external/pspsdk/src/base/psptypes.h */
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
```

A thread function therefore has the form `int func(SceSize args, void *argp)`. `args` is the byte length passed to `sceKernelStartThread`'s `arglen`, and `argp` points at the bytes passed as `argp`. (This repo's `main_thread` ignores both: `int main_thread(SceSize args, void *argp)` with `(void)args; (void)argp;`.)

### Thread lifecycle

```c
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                             int stackSize, SceUInt attr, SceKernelThreadOptParam *option);
int sceKernelDeleteThread(SceUID thid);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelExitThread(int status);
int sceKernelExitDeleteThread(int status);
int sceKernelTerminateThread(SceUID thid);
int sceKernelTerminateDeleteThread(SceUID thid);
int sceKernelGetThreadId(void);
int sceKernelGetThreadCurrentPriority(void);
int sceKernelChangeThreadPriority(SceUID thid, int priority);
```

Notes:
- `sceKernelCreateThread` returns the thread UID (`SceUID`), or an error code (`< 0`). `attr` is zero or more of `enum PspThreadAttributes` (below). `option` is normally `NULL`.
- `sceKernelExitThread` exits the calling thread but leaves it in a stopped state holding its resources; `sceKernelExitDeleteThread` exits **and deletes** the calling thread (no separate `sceKernelDeleteThread` needed). This is what a self-terminating worker should call so it never lingers.
- `sceKernelTerminateThread` / `sceKernelTerminateDeleteThread` forcibly stop (and optionally delete) *another* thread by UID. Forcible termination can leak resources the target held — prefer cooperative exit.
- `sceKernelChangeThreadPriority`: lower `priority` value = higher priority.

Thread attribute flags — `enum PspThreadAttributes`:

```c
enum PspThreadAttributes
{
    PSP_THREAD_ATTR_VFPU         = 0x00004000, /* Enable VFPU access */
    PSP_THREAD_ATTR_USER         = 0x80000000, /* Start the thread in user mode */
    PSP_THREAD_ATTR_USBWLAN      = 0xa0000000, /* Part of the USB/WLAN API */
    PSP_THREAD_ATTR_VSH          = 0xc0000000, /* Part of the VSH API */
    PSP_THREAD_ATTR_SCRATCH_SRAM = 0x00008000, /* Allow scratchpad use (not on V1.0) */
    PSP_THREAD_ATTR_NO_FILLSTACK = 0x00100000, /* Don't fill stack with 0xFF on creation */
    PSP_THREAD_ATTR_CLEAR_STACK  = 0x00200000, /* Clear the stack when the thread is deleted */
};
```

(Compatibility aliases `THREAD_ATTR_VFPU` / `THREAD_ATTR_USER` also exist.) This repo passes `attr = 0` to `sceKernelCreateThread` for `wavezbg_thread`, since it is created from a kernel-mode PRX and needs neither VFPU nor a forced user-mode start.

Status structs (for `sceKernelReferThreadStatus`, set `.size` before calling):

```c
typedef struct SceKernelThreadInfo {
    SceSize     size;
    char        name[32];
    SceUInt     attr;
    int         status;
    SceKernelThreadEntry entry;
    void *      stack;
    int         stackSize;
    void *      gpReg;
    int         initPriority;
    int         currentPriority;
    int         waitType;
    SceUID      waitId;
    int         wakeupCount;
    int         exitStatus;
    SceKernelSysClock runClocks;
    SceUInt     intrPreemptCount;
    SceUInt     threadPreemptCount;
    SceUInt     releaseCount;
} SceKernelThreadInfo;
```

### Sleep / delay / wait

```c
int sceKernelSleepThread(void);
int sceKernelSleepThreadCB(void);
int sceKernelWakeupThread(SceUID thid);
int sceKernelDelayThread(SceUInt delay);          /* delay in microseconds */
int sceKernelDelayThreadCB(SceUInt delay);        /* delay in microseconds, services callbacks */
int sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout);
int sceKernelWaitThreadEndCB(SceUID thid, SceUInt *timeout);
```

- `sceKernelSleepThread` blocks the caller until another thread calls `sceKernelWakeupThread` on it. The `*CB` variant additionally services callbacks while sleeping (this is what an exit-callback thread parks in).
- `sceKernelDelayThread(usec)` blocks the caller for a fixed time, yielding the CPU. `1000000` = one second; this repo uses `100000` (100 ms) per poll cycle.
- `sceKernelWaitThreadEnd` blocks until the named thread ends; `timeout` is a pointer to a microsecond timeout (`NULL` = wait forever).

### Synchronization primitives

**Semaphores** ([pspthreadman.h](external/pspsdk/src/user/pspthreadman.h)):

```c
SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam *option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);
int sceKernelWaitSemaCB(SceUID semaid, int signal, SceUInt *timeout);
int sceKernelPollSema(SceUID semaid, int signal);
int sceKernelReferSemaStatus(SceUID semaid, SceKernelSemaInfo *info);
```

```c
typedef struct SceKernelSemaInfo {
    SceSize size;
    char    name[32];
    SceUInt attr;
    int     initCount;
    int     currentCount;
    int     maxCount;
    int     numWaitThreads;
} SceKernelSemaInfo;
```

`sceKernelWaitSema` blocks until the count reaches at least `signal`, then decrements by it; `sceKernelSignalSema` increments. `sceKernelPollSema` is the non-blocking test. The `*CB` wait variant services callbacks while blocked.

**Event flags** ([pspthreadman.h](external/pspsdk/src/user/pspthreadman.h)):

```c
SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits, SceKernelEventFlagOptParam *opt);
int sceKernelSetEventFlag(SceUID evid, u32 bits);
int sceKernelClearEventFlag(SceUID evid, u32 bits);
int sceKernelPollEventFlag(int evid, u32 bits, u32 wait, u32 *outBits);
int sceKernelWaitEventFlag(int evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout);
int sceKernelWaitEventFlagCB(int evid, u32 bits, u32 wait, u32 *outBits, SceUInt *timeout);
int sceKernelDeleteEventFlag(int evid);
int sceKernelReferEventFlagStatus(SceUID event, SceKernelEventFlagInfo *status);
```

```c
typedef struct SceKernelEventFlagInfo {
    SceSize size;
    char    name[32];
    SceUInt attr;
    SceUInt initPattern;
    SceUInt currentPattern;
    int     numWaitThreads;
} SceKernelEventFlagInfo;

enum PspEventFlagAttributes {
    PSP_EVENT_WAITSINGLE   = 0x00,   /* waited on by a single thread */
    PSP_EVENT_WAITMULTIPLE = 0x200,  /* waited on by multiple threads */
};
enum PspEventFlagWaitTypes {
    PSP_EVENT_WAITAND   = 0,    /* wait for all bits in the pattern */
    PSP_EVENT_WAITOR    = 1,    /* wait for one or more bits */
    PSP_EVENT_WAITCLEAR = 0x20, /* clear the matched bits on success */
};
```

The `wait` argument to the poll/wait functions is one or more `PspEventFlagWaitTypes` OR'ed together.

**Lightweight mutex** (present in the header — `SceLwMutexWorkarea` workarea, not a UID):

```c
int sceKernelCreateLwMutex(SceLwMutexWorkarea *workarea, const char *name, SceUInt32 attr, int initialCount, u32 *optionsPtr);
int sceKernelDeleteLwMutex(SceLwMutexWorkarea *workarea);
int sceKernelTryLockLwMutex(SceLwMutexWorkarea *workarea, int lockCount);
int sceKernelLockLwMutex(SceLwMutexWorkarea *workarea, int lockCount, unsigned int *pTimeout);
int sceKernelUnlockLwMutex(SceLwMutexWorkarea *workarea, int lockCount);

enum PspLwMutexAttributes {
    PSP_LW_MUTEX_ATTR_THFIFO    = 0x0000U, /* waiters queued FIFO */
    PSP_LW_MUTEX_ATTR_THPRI     = 0x0100U, /* waiters queued by priority */
    PSP_LW_MUTEX_ATTR_RECURSIVE = 0x0200U, /* recursive lock by the owner */
};
```

(Note: the full-weight `sceKernelCreateMutex` is **not** in this version of the header — only the lightweight mutex is. See "APIs omitted".)

**Message boxes** (present in the header):

```c
SceUID sceKernelCreateMbx(const char *name, SceUInt attr, SceKernelMbxOptParam *option);
int sceKernelDeleteMbx(SceUID mbxid);
int sceKernelSendMbx(SceUID mbxid, void *message);
int sceKernelReceiveMbx(SceUID mbxid, void **pmessage, SceUInt *timeout);
int sceKernelReceiveMbxCB(SceUID mbxid, void **pmessage, SceUInt *timeout);
int sceKernelPollMbx(SceUID mbxid, void **pmessage);
int sceKernelCancelReceiveMbx(SceUID mbxid, int *pnum);
int sceKernelReferMbxStatus(SceUID mbxid, SceKernelMbxInfo *info);
```

A message must begin with a `SceKernelMsgPacket` header (the kernel uses its `next` field to chain the queue).

### Callbacks

```c
typedef int (*SceKernelCallbackFunction)(int arg1, int arg2, void *arg);

int sceKernelCreateCallback(const char *name, SceKernelCallbackFunction func, void *arg);
int sceKernelDeleteCallback(SceUID cb);
int sceKernelNotifyCallback(SceUID cb, int arg2);
int sceKernelCheckCallback(void);
```

CB-suffixed wait functions that allow callbacks to run while the thread is blocked:
`sceKernelSleepThreadCB`, `sceKernelDelayThreadCB`, `sceKernelWaitThreadEndCB`, `sceKernelWaitSemaCB`, `sceKernelWaitEventFlagCB`, `sceKernelReceiveMbxCB` (and `sceKernelDelaySysClockThreadCB`, the VPL/FPL/MsgPipe `*CB` allocate/receive variants).

**Critical rule:** a callback registered against a thread only fires while that thread is parked in a `*CB` wait (or when it explicitly calls `sceKernelCheckCallback`). A thread that never enters a `*CB` wait will never service its callbacks. This is exactly why the canonical exit-callback thread ends in `sceKernelSleepThreadCB()` and loops forever there.

## Correct usage patterns

### 1. Create + start a worker thread (mirrors src/main.c)

This is the real pattern from [src/main.c](src/main.c) `module_start`. A kernel-mode PRX must not work on a system thread, so it spawns its own:

```c
/* attr = 0: created from a kernel-mode PRX, no VFPU and no forced user mode.
 * 0x18 priority, 0x8000 (32 KB) stack. Peak stack use is ~6.5 KB (the
 * 6176-byte BMP buffer in write_bmp plus call frames), so 32 KB is generous. */
SceUID thid = sceKernelCreateThread("wavezbg_thread", main_thread, 0x18, 0x8000, 0, NULL);
if (thid >= 0) {
    sceKernelStartThread(thid, args, argp);
}
```

### 2. Self-terminating worker loop (so it never lingers)

Also from [src/main.c](src/main.c) `main_thread`. It polls on a bounded loop, yielding the CPU each cycle with `sceKernelDelayThread`, and always reaches `sceKernelExitDeleteThread` so the thread frees itself regardless of outcome:

```c
int main_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    read_config();
    generate_wave_bmp();

    /* ~60 s cap: 600 attempts * 100 ms. Breaks early on success. */
    for (int attempts = 0; attempts < 600; attempts++) {
        if (patch_wave_strings()) {
            break;
        }
        sceKernelDelayThread(100000); /* 100 ms, in microseconds */
    }

    sceKernelExitDeleteThread(0); /* exit AND delete self - no leak, no linger */
    return 0;                     /* not reached */
}
```

Key points: the delay (not a busy spin) yields the CPU between polls; the loop is **bounded** so a never-succeeding patch still terminates; `sceKernelExitDeleteThread` both exits and reclaims the thread in one call.

### 3. Standard exit-callback setup

From [external/pspsdk/src/samples/gu/common/callbacks.c](external/pspsdk/src/samples/gu/common/callbacks.c) and [external/pspsdk/src/samples/kernel/threadstatus/main.c](external/pspsdk/src/samples/kernel/threadstatus/main.c). A dedicated thread creates the callback, registers it, then parks in `sceKernelSleepThreadCB` so the callback can fire:

```c
/* The callback itself: signature is SceKernelCallbackFunction. */
int exit_callback(int arg1, int arg2, void *common)
{
    sceKernelExitGame();   /* from psploadexec.h - return to XMB */
    return 0;
}

/* The callback thread: creates the callback, registers it, then SLEEPS IN A
 * *CB WAIT so the callback is actually serviced. */
int CallbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);   /* psploadexec.h, not threadman */
    sceKernelSleepThreadCB();              /* park here; callbacks fire while parked */
    return 0;
}

/* Spawn the callback thread. (0x11 priority, 0xFA0 = 4000-byte stack here.) */
int SetupCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}
```

Note `sceKernelRegisterExitCallback` is declared in [external/pspsdk/src/user/psploadexec.h](external/pspsdk/src/user/psploadexec.h), not in pspthreadman.h. (This repo does not register an exit callback — a boot-time VSH plugin has no Home-button game to exit — but this is the standard pattern for normal apps.)

## Common mistakes

- **Too-small stack.** `sceKernelCreateThread`'s `stackSize` must cover the deepest call frame *and* any large stack buffers. `write_bmp` alone uses a 6176-byte stack buffer; src/main.c sizes the thread at `0x8000` (32 KB) for margin. Undersizing risks a stack overflow, which the thread manager reports as `PSP_THREAD_KILLED`.
- **Never deleting threads (resource leak).** Calling only `sceKernelExitThread` leaves the thread stopped but still allocated. A self-terminating worker should call `sceKernelExitDeleteThread`; a thread joined by another should be cleaned up with `sceKernelDeleteThread` after it has exited. src/main.c uses `sceKernelExitDeleteThread(0)` precisely so the worker never lingers.
- **Blocking the VSH/main thread.** Never run long work on (or block) a system thread from a VSH plugin — the XMB stalls or hangs. Do work on your own thread (the `module_start` -> `sceKernelCreateThread` pattern).
- **Busy-waiting instead of `sceKernelDelayThread`.** A tight `while` poll starves lower-priority threads and pins the CPU. Insert `sceKernelDelayThread(usec)` to yield between polls (src/main.c delays 100 ms per attempt).
- **Callbacks never firing.** A callback created with `sceKernelCreateCallback` only runs while its thread is in a `*CB` wait (`sceKernelSleepThreadCB`, `sceKernelDelayThreadCB`, `sceKernelWaitSemaCB`, ...) or calls `sceKernelCheckCallback`. If the thread sits in the non-CB variants, the callback is silent.
- **Wrong priority starving the system.** Lower number = higher priority. Giving a busy worker too high a priority (small number) can starve VSH/system threads. A background worker should sit at a modest priority (src/main.c uses `0x18`).
- **Passing args/argp incorrectly.** `sceKernelStartThread(thid, arglen, argp)` delivers `arglen` bytes at `argp` to the entry function's `(SceSize args, void *argp)`. `argp` must remain valid until the thread has read it. If the thread takes no arguments, pass `0, NULL` (samples pass `0, 0`).

## Sample references

- [external/pspsdk/src/user/pspthreadman.h](external/pspsdk/src/user/pspthreadman.h) — the ThreadMan library header; every thread/sema/event-flag/mbx/callback/VTimer signature, struct, and enum above is copied from here.
- [external/pspsdk/src/kernel/pspthreadman_kernel.h](external/pspsdk/src/kernel/pspthreadman_kernel.h) — kernel-only thread helpers (`sceKernelIsUserModeThread`, `sceKernelGetUserLevel`, KTLS, `SceKernelThreadKInfo`); no user-facing create/start/exit funcs of their own.
- [external/pspsdk/src/base/psptypes.h](external/pspsdk/src/base/psptypes.h) — defines `typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);`, the thread entry signature.
- [external/pspsdk/src/user/psploadexec.h](external/pspsdk/src/user/psploadexec.h) — declares `sceKernelRegisterExitCallback` and `sceKernelExitGame` used by the exit-callback pattern (these live in LoadExec, not ThreadMan).
- [external/pspsdk/src/samples/gu/common/callbacks.c](external/pspsdk/src/samples/gu/common/callbacks.c) — minimal `callbackThread` / `setupCallbacks` exit-callback example parked in `sceKernelSleepThreadCB`.
- [external/pspsdk/src/samples/kernel/threadstatus/main.c](external/pspsdk/src/samples/kernel/threadstatus/main.c) — same callback/thread pattern plus `sceKernelReferThreadStatus` usage (reading `SceKernelThreadInfo`).
- [src/main.c](src/main.c) — this repo's real worker-thread lifecycle: `module_start` creates `wavezbg_thread` (32 KB stack, prio `0x18`), `main_thread` polls with `sceKernelDelayThread(100000)` then self-deletes via `sceKernelExitDeleteThread(0)`.
