# PSP SDK Graphics APIs

> Generated from `external/pspsdk` headers + samples. Verify signatures against the header before use.

Header sources (read in full for this document):

- [pspgu.h](external/pspsdk/src/gu/pspgu.h) — `sceGu*` immediate / display-list graphics
- [pspgum.h](external/pspsdk/src/gum/pspgum.h) — `sceGum*` matrix-stack helpers
- [pspge.h](external/pspsdk/src/ge/pspge.h) — `sceGe*` low-level Graphics Engine
- [pspdisplay.h](external/pspsdk/src/display/pspdisplay.h) — `sceDisplay*` framebuffer / vblank

## Overview

The PSP renders through the **GE** (Graphics Engine), a fixed-function GPU driven by a *display list* — a buffer of GE commands the GE reads from main RAM / VRAM via DMA. The layers stack like this:

- **GE** ([pspge.h](external/pspsdk/src/ge/pspge.h)) — the lowest level. You enqueue raw display lists (`sceGeListEnQueue`), set/restore GE contexts (`PspGeContext`), query VRAM (`sceGeEdramGetAddr` / `sceGeEdramGetSize`), and sync (`sceGeDrawSync` / `sceGeListSync`). Most apps never call these directly.
- **GU** ([pspgu.h](external/pspsdk/src/gu/pspgu.h)) — the *Graphics Utility* library. It builds GE command lists for you behind a friendly API: init, draw buffers, render state, clears, textures, `sceGuDrawArray`. This is the layer wavezbg-style plugins and most homebrew use.
- **GUM** ([pspgum.h](external/pspsdk/src/gum/pspgum.h)) — a matrix-stack layer on top of GU (think OpenGL `glMatrixMode`/`glLoadIdentity`). `sceGum*` functions maintain projection/view/model/texture stacks and flush them to the GE; `sceGumDrawArray` is the transform-aware counterpart of `sceGuDrawArray`.
- **Display** ([pspdisplay.h](external/pspsdk/src/display/pspdisplay.h)) — the scan-out hardware: set the LCD mode, point it at a framebuffer, and wait for vertical blank. GU's `sceGuSwapBuffers` cooperates with this.

### Typical frame loop

From [cube.c](external/pspsdk/src/samples/gu/cube/cube.c) and [sprite.c](external/pspsdk/src/samples/gu/sprite/sprite.c), every frame is:

```
sceGuStart(GU_DIRECT, list);     // begin building a display list
  ... clears, matrices, textures, sceGu(m)DrawArray ...
sceGuFinish();                    // terminate the list
sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);  // wait until GE is done
sceDisplayWaitVblankStart();      // avoid tearing
sceGuSwapBuffers();               // flip draw <-> display buffer
```

The one-time setup (`sceGuInit`, draw/disp/depth buffers, viewport, scissor, enables) runs once before the loop, itself wrapped in a `sceGuStart`/`sceGuFinish`/`sceGuSync` pair.

## Key APIs

### Init / display-list lifecycle ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `int sceGuInit(void);` | Initialise the GU system. MUST be the first GU call. |
| `void sceGuTerm(void);` | Shut down GU when no longer needed. |
| `int sceGuStart(int ctype, void* list);` | Begin filling a display context. `ctype` = `GU_DIRECT` / `GU_CALL` / `GU_SEND`; `list` must be 16-byte aligned. |
| `int sceGuFinish(void);` | Finish the current list, restore parent context; returns list size. |
| `int sceGuFinishId(unsigned int id);` | Like `sceGuFinish` but passes a 16-bit id to the finish callback. |
| `int sceGuSync(int mode, int what);` | Wait for the display list / GE. Typical: `sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE)`. |
| `void* sceGuSwapBuffers(void);` | Swap draw and display buffers; returns the new draw buffer. |
| `void* sceGuGetMemory(int size);` | Allocate scratch memory on the current display list (invalidated on next list fill). |
| `int sceGuCallList(const void* list);` | Call a previously generated display list. |
| `int sceGuSendList(int mode, const void* list, PspGeContext* context);` | Send a list to the GE directly (`GU_TAIL` / `GU_HEAD`). |
| `int sceGuCheckList(void);` | Return the size of the current display list. |

### Buffers & display config ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `void sceGuDrawBuffer(int psm, void* fbp, int fbw);` | Set the render target (pixel format `GU_PSM_*`, VRAM pointer, block-aligned width). |
| `void sceGuDrawBufferList(int psm, void* fbp, int fbw);` | Set draw buffer directly without storing it in the context. |
| `void sceGuDispBuffer(int width, int height, void* dispbp, int dispbw);` | Set the buffer that is scanned out to the LCD. |
| `void sceGuDepthBuffer(void* zbp, int zbw);` | Set the depth buffer location/width. |
| `int sceGuDisplay(int state);` | Turn display on/off (`GU_DISPLAY_ON` / `GU_DISPLAY_OFF`). |
| `void sceGuViewport(int cx, int cy, int width, int height);` | Set the viewport (center + size in the 4096×4096 virtual space). |
| `void sceGuOffset(unsigned int x, unsigned int y);` | Set the virtual-coordinate offset (typically `2048 - w/2`, `2048 - h/2`). |
| `void sceGuScissor(int x, int y, int w, int h);` | Set the scissor rect (only active when `GU_SCISSOR_TEST` is enabled). |
| `void sceGuDepthRange(int near, int far);` | Set depth range; note the depth buffer is inverted (65535..0). |
| `void* guGetStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm);` | Allocate a draw/disp/depth buffer in VRAM; returns a VRAM-relative pointer. |
| `void* guGetStaticVramTexture(unsigned int width, unsigned int height, unsigned int psm);` | Allocate a texture in VRAM. |

### Clears & per-draw color ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `void sceGuClear(int flags);` | Clear buffers; OR of `GU_COLOR_BUFFER_BIT` / `GU_DEPTH_BUFFER_BIT` / `GU_STENCIL_BUFFER_BIT`. |
| `void sceGuClearColor(unsigned int color);` | Set the clear color (0xAABBGGRR). |
| `void sceGuClearDepth(unsigned int depth);` | Set the clear depth (0x0000–0xffff). |
| `void sceGuClearStencil(unsigned int stencil);` | Set the clear stencil value (0–255). |
| `void sceGuColor(unsigned int color);` | Set the current primitive color (overridden by per-vertex colors). |
| `void sceGuPixelMask(unsigned int mask);` | Mask which pixel bits are writable (0xAABBGGRR; 1 = blocked). |

### Render state ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `void sceGuEnable(int state);` | Enable a GE state (e.g. `GU_DEPTH_TEST`, `GU_TEXTURE_2D`, `GU_BLEND`, `GU_CULL_FACE`, `GU_SCISSOR_TEST`, `GU_ALPHA_TEST`). |
| `void sceGuDisable(int state);` | Disable a GE state. |
| `void sceGuSetStatus(int state, int status);` | Enable/disable a state by value. |
| `int sceGuGetStatus(int state);` | Query whether a state is enabled. |
| `void sceGuSetAllStatus(int status);` | Set all 22 states from a bitmask. |
| `int sceGuGetAllStatus(void);` | Read all 22 states as a bitmask. |
| `void sceGuDepthFunc(int function);` | Choose the depth-test comparison (`GU_GEQUAL`, etc.). |
| `void sceGuDepthMask(int mask);` | Mask depth-buffer writes (`GU_TRUE` disables Z writes). |
| `void sceGuFrontFace(int order);` | Set front-face winding (`GU_CW` / `GU_CCW`) for culling. |
| `void sceGuShadeModel(int mode);` | `GU_FLAT` or `GU_SMOOTH` shading. |
| `void sceGuAlphaFunc(int func, int value, int mask);` | Alpha-test comparison. |
| `void sceGuBlendFunc(int op, int src, int dest, unsigned int srcfix, unsigned int destfix);` | Set blend op + factors. |
| `void sceGuLogicalOp(int op);` | Set the color logic op (needs `GU_COLOR_LOGIC_OP`). |

### Drawing ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `void sceGuDrawArray(int prim, int vtype, int count, const void* indices, const void* vertices);` | Draw primitives. **Does not** apply the GUM matrix transform — for transformed draws use `sceGumDrawArray`. |
| `void sceGuDrawArrayN(int primitive_type, int vertex_type, int vcount, int primcount, const void* indices, const void* vertices);` | Draw N batches of primitives. |
| `void sceGuBeginObject(int vtype, int count, const void* indices, const void* vertices);` | Begin conditional (bounding-box) rendering. |
| `int sceGuEndObject(void);` | End conditional rendering. |

`prim`: `GU_POINTS`, `GU_LINES`, `GU_LINE_STRIP`, `GU_TRIANGLES`, `GU_TRIANGLE_STRIP`, `GU_TRIANGLE_FAN`, `GU_SPRITES`.

`vtype` is an OR of format flags: texture (`GU_TEXTURE_8BIT`/`16BIT`/`32BITF`), color (`GU_COLOR_5650`/`5551`/`4444`/`8888`), normal (`GU_NORMAL_8BIT`/`16BIT`/`32BITF`), position (`GU_VERTEX_8BIT`/`16BIT`/`32BITF`), `GU_WEIGHT_*`, `GU_INDEX_*`, `GU_WEIGHTS(n)`, `GU_VERTICES(n)`, and `GU_TRANSFORM_3D` / `GU_TRANSFORM_2D`. In-vertex member order is fixed: weights, texture coords, color, normal, position. **Every member must be 16-bit aligned.**

### Textures & CLUT ([pspgu.h](external/pspsdk/src/gu/pspgu.h))

| Signature | Purpose |
| --- | --- |
| `void sceGuTexImage(int mipmap, int width, int height, int tbw, const void* tbp);` | Set the current texture map. `tbp` must be 16-byte aligned; width/height power-of-2. |
| `void sceGuTexMode(int tpsm, int maxmips, int mc, int swizzle);` | Set texture format / mip count / multiclut / swizzle. |
| `void sceGuTexFunc(int tfx, int tcc);` | How the texture combines with the fragment (`GU_TFX_*` / `GU_TCC_RGB`|`GU_TCC_RGBA`). |
| `void sceGuTexFilter(int min, int mag);` | Min/mag filter (`GU_NEAREST` / `GU_LINEAR` / mipmap variants). |
| `void sceGuTexWrap(int u, int v);` | `GU_REPEAT` or `GU_CLAMP` per axis. |
| `void sceGuTexScale(float u, float v);` | Scale UVs (3D T&L only, not `GU_TRANSFORM_2D`). |
| `void sceGuTexOffset(float u, float v);` | Offset UVs (3D T&L only). |
| `void sceGuTexEnvColor(unsigned int color);` | Constant color for `GU_TFX_BLEND`. |
| `void sceGuTexLevelMode(unsigned int mode, float bias);` | Mipmap level mode + bias. |
| `void sceGuTexMapMode(int mode, unsigned int lu, unsigned int lv);` | `GU_TEXTURE_COORDS` / `GU_TEXTURE_MATRIX` / `GU_ENVIRONMENT_MAP`. |
| `void sceGuTexProjMapMode(int mode);` | Texture projection source. |
| `void sceGuTexFlush(void);` | Flush the texture page-cache after writing into a texture region. |
| `void sceGuTexSync();` | Stall the pipeline until a `sceGuCopyImage` upload completes. |
| `void sceGuCopyImage(int psm, int sx, int sy, int width, int height, int srcw, void* src, int dx, int dy, int destw, void* dest);` | GE-accelerated image blit (data 16-byte aligned). |
| `void sceGuClutLoad(int num_blocks, const void* cbp);` | Upload a CLUT (16-byte aligned). |
| `void sceGuClutMode(unsigned int cpsm, unsigned int shift, unsigned int mask, unsigned int csa);` | Set CLUT pixel format / index decode. |

### Matrix stack — GUM ([pspgum.h](external/pspsdk/src/gum/pspgum.h))

| Signature | Purpose |
| --- | --- |
| `void sceGumMatrixMode(int mode);` | Select the active stack: `GU_PROJECTION` / `GU_VIEW` / `GU_MODEL` / `GU_TEXTURE`. |
| `void sceGumLoadIdentity(void);` | Load identity into the current matrix. |
| `void sceGumLoadMatrix(const ScePspFMatrix4* m);` | Load a matrix onto the stack. |
| `void sceGumMultMatrix(const ScePspFMatrix4* m);` | Multiply current matrix by `m`. |
| `void sceGumPushMatrix(void);` / `void sceGumPopMatrix(void);` | Push / pop the current stack. |
| `void sceGumStoreMatrix(ScePspFMatrix4* m);` | Copy the current matrix out into `m`. |
| `void sceGumPerspective(float fovy, float aspect, float near, float far);` | Apply a perspective projection. |
| `void sceGumOrtho(float left, float right, float bottom, float top, float near, float far);` | Apply an orthographic projection. |
| `void sceGumLookAt(ScePspFVector3* eye, ScePspFVector3* center, ScePspFVector3* up);` | Build a look-at view matrix. |
| `void sceGumRotateX(float angle);` / `RotateY` / `RotateZ` | Rotate around one axis (radians). |
| `void sceGumRotateXYZ(const ScePspFVector3* v);` / `RotateZYX` | Rotate around all three axes (radians). |
| `void sceGumRotate(const ScePspFQuaternion* q);` | Apply a quaternion rotation. |
| `void sceGumTranslate(const ScePspFVector3* v);` | Translate. |
| `void sceGumScale(const ScePspFVector3* v);` | Scale (used to map 16-/8-bit vertex space to float units). |
| `void sceGumUpdateMatrix(void);` | Force-flush dirty matrices to the GE. |
| `void sceGumFullInverse();` / `void sceGumFastInverse();` | Invert the current matrix (full / orthonormal-only). |
| `void sceGumDrawArray(int prim, int vtype, int count, const void* indices, const void* vertices);` | Like `sceGuDrawArray`, but applies the current GUM matrices. |
| `void sceGumDrawArrayN(int prim, int vtype, int count, int a3, const void* indices, const void* vertices);` | Batched transformed draw. |

> Note: there is also a parallel set of *standalone* helpers (`gumLoadIdentity`, `gumPerspective`, `gumMultMatrix`, `gumFastInverse`, vector/quaternion math, etc.) that operate on a caller-supplied `ScePspFMatrix4*` instead of the stack. `sceGumBeginObject` / `sceGumEndObject` exist but the header marks them **NOT YET IMPLEMENTED** — do not rely on them.

### Display / scan-out ([pspdisplay.h](external/pspsdk/src/display/pspdisplay.h))

| Signature | Purpose |
| --- | --- |
| `int sceDisplaySetMode(int mode, int width, int height);` | Set LCD mode (`PSP_DISPLAY_MODE_LCD`, 480×272). |
| `int sceDisplayGetMode(int *pmode, int *pwidth, int *pheight);` | Read the current mode. |
| `int sceDisplaySetFrameBuf(void *topaddr, int bufferwidth, int pixelformat, int sync);` | Point scan-out at a framebuffer. `pixelformat` is `PspDisplayPixelFormats`; `sync` is `PSP_DISPLAY_SETBUF_NEXTHSYNC`/`NEXTVSYNC`. |
| `int sceDisplayGetFrameBuf(void **topaddr, int *bufferwidth, int *pixelformat, int sync);` | Read current framebuffer info. |
| `int sceDisplayWaitVblankStart(void);` | Block until the next vertical-blank start (use before swap to avoid tearing). |
| `int sceDisplayWaitVblankStartCB(void);` | Same, but processes callbacks while waiting. |
| `int sceDisplayWaitVblank(void);` / `int sceDisplayWaitVblankCB(void);` | Wait for vblank (with/without callbacks). |
| `unsigned int sceDisplayGetVcount(void);` | Vertical-blank pulse count so far. |
| `float sceDisplayGetFramePerSec(void);` | Current frames-per-second. |
| `int sceDisplayIsForeground(void);` / `int sceDisplayIsVblank(void);` | Query foreground / vblank-active state. |

### Low-level GE ([pspge.h](external/pspsdk/src/ge/pspge.h))

| Signature | Purpose |
| --- | --- |
| `void * sceGeEdramGetAddr(void);` | Get the base address of eDRAM (VRAM). |
| `unsigned int sceGeEdramGetSize(void);` | Get VRAM size in bytes. |
| `int sceGeEdramSetSize(int size);` | Set enabled eDRAM size (0x200000 / 0x400000). |
| `int sceGeEdramSetAddrTranslation(int width);` | Set eDRAM address-translation width. |
| `int sceGeListEnQueue(const void *list, void *stall, int cbid, PspGeListArgs *arg);` | Enqueue a display list at the tail of the GE queue. |
| `int sceGeListEnQueueHead(const void *list, void *stall, int cbid, PspGeListArgs *arg);` | Enqueue at the head of the queue. |
| `int sceGeListDeQueue(int qid);` | Cancel a queued/running list. |
| `int sceGeListUpdateStallAddr(int qid, void *stall);` | Move a queue's stall address (lets the GE consume more of the list). |
| `int sceGeListSync(int qid, int syncType);` | Wait for / peek a list's status (`PspGeListState`). |
| `int sceGeDrawSync(int syncType);` | Wait for / peek overall drawing completion. |
| `int sceGeSaveContext(PspGeContext *context);` / `int sceGeRestoreContext(const PspGeContext *context);` | Save / restore full GE state. |
| `int sceGeSetCallback(PspGeCallbackData *cb);` / `int sceGeUnsetCallback(int cbid);` | Register / unregister signal+finish callbacks. |
| `unsigned int sceGeGetCmd(int cmd);` | Read a GE command register (0–0xFF). |
| `int sceGeGetMtx(int type, void *matrix);` | Read a GE matrix (`PspGeMatrixTypes`). |
| `int sceGeBreak(int mode, PspGeBreakParam *pParam);` / `int sceGeContinue(void);` | Interrupt / resume the drawing queue. |

## Correct usage patterns

The list buffer must be statically allocated and 16-byte aligned (from every sample):

```c
static unsigned int __attribute__((aligned(16))) list[262144];

#define BUF_WIDTH  512   // VRAM line stride, power of 2 (NOT the visible width)
#define SCR_WIDTH  480
#define SCR_HEIGHT 272
```

One-time init (from [cube.c](external/pspsdk/src/samples/gu/cube/cube.c)). Note `sceGuInit()` comes first, then the whole config is wrapped in a `sceGuStart`/`sceGuFinish`/`sceGuSync`:

```c
void* fbp0 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888); // draw buffer
void* fbp1 = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888); // display buffer
void* zbp  = guGetStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444); // depth buffer

sceGuInit();                                       // FIRST GU call

sceGuStart(GU_DIRECT, list);
sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_WIDTH);
sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
sceGuDepthBuffer(zbp, BUF_WIDTH);
sceGuOffset(2048 - (SCR_WIDTH/2), 2048 - (SCR_HEIGHT/2));
sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
sceGuDepthRange(65535, 0);                          // inverted depth
sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
sceGuEnable(GU_SCISSOR_TEST);
sceGuDepthFunc(GU_GEQUAL);
sceGuEnable(GU_DEPTH_TEST);
sceGuFrontFace(GU_CW);
sceGuShadeModel(GU_SMOOTH);
sceGuEnable(GU_CULL_FACE);
sceGuEnable(GU_TEXTURE_2D);
sceGuFinish();
sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

sceDisplayWaitVblankStart();
sceGuDisplay(GU_TRUE);
```

Per-frame loop with double buffering and vsync:

```c
while (running()) {
    sceGuStart(GU_DIRECT, list);

    sceGuClearColor(0xff554433);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    // transformed draw (cube.c): GUM matrices THEN sceGumDrawArray
    sceGumMatrixMode(GU_PROJECTION); sceGumLoadIdentity();
    sceGumPerspective(75.0f, 16.0f/9.0f, 0.5f, 1000.0f);
    sceGumMatrixMode(GU_MODEL);      sceGumLoadIdentity();
    // ... sceGumTranslate / sceGumRotateXYZ ...
    sceGumDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D,
                    12*3, 0, vertices);

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}
```

Key points drawn from the samples:

- **`GU_DIRECT` vs `GU_SEND`/`GU_CALL`.** `GU_DIRECT` (used everywhere in the samples) renders as the list fills — `sceGuFinish` just updates the GE stall address so the whole list runs. `GU_CALL` builds a reusable sub-list for `sceGuCallList`; `GU_SEND` buffers a list for a later `sceGuSendList`.
- **Vertex format / alignment.** Vertices live in a 16-byte-aligned static array (`struct Vertex __attribute__((aligned(16)))`, see [cube.c](external/pspsdk/src/samples/gu/cube/cube.c)) and the struct member order must match the `vtype` order (texture, color, position). Per-frame dynamic vertices use `sceGuGetMemory` so they live in the current display list (see [sprite.c](external/pspsdk/src/samples/gu/sprite/sprite.c) and [blit.c](external/pspsdk/src/samples/gu/blit/blit.c)).
- **2D vs 3D.** Use `GU_TRANSFORM_3D` + `sceGumDrawArray` for transformed geometry; use `GU_TRANSFORM_2D` + `sceGuDrawArray` for screen-space sprites/blits (see [blit.c](external/pspsdk/src/samples/gu/blit/blit.c), which uses `GU_VERTEX_16BIT|GU_TRANSFORM_2D` with `GU_SPRITES`).
- **Double-buffer addresses.** `fbp0` is the draw buffer, `fbp1` the display buffer; `sceGuSwapBuffers()` flips them and returns the new draw buffer. [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) captures the return value (`fbp0 = sceGuSwapBuffers();`) so its `pspDebugScreen` writes target the freshly-swapped buffer.
- **CPU-written texture data must be flushed.** [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) calls `sceKernelDcacheWritebackAll();` after generating/swizzling its pixel buffer in RAM, before the GE samples it.

## Common mistakes

- **Not flushing the data cache before the GE reads CPU-written data.** The GE reads via DMA and does not see data still sitting in the CPU's write-back cache. After writing vertices, textures, or CLUTs in RAM, call `sceKernelDcacheWritebackAll()` (or `sceKernelDcacheWritebackInvalidateAll()`); [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) does this before `sceGuTexImage`/draw. Symptom: stale or garbage texels/geometry. (Concerns `sceGuTexImage`, `sceGuClutLoad`, `sceGuDrawArray`.)
- **Unaligned display list.** `sceGuStart`'s `list` pointer must be 16-byte aligned (`__attribute__((aligned(16)))`); an unaligned list corrupts GE command parsing. (Concerns `sceGuStart`.)
- **Unaligned vertices/textures/CLUT.** Vertex members must be 16-bit aligned, and texture/CLUT/`sceGuCopyImage` data must be 16-byte (quad-word) aligned per the header notes. (Concerns `sceGuDrawArray`, `sceGuTexImage`, `sceGuClutLoad`, `sceGuCopyImage`.)
- **Expecting `sceGuDrawArray` to apply the matrix stack.** It does **not** — only `sceGumDrawArray` applies the GUM projection/view/model transforms. Mixing them up gives untransformed (or wrongly transformed) geometry. (Concerns `sceGuDrawArray` vs `sceGumDrawArray`.)
- **Mixing absolute and VRAM-relative pointers.** `guGetStaticVramBuffer` / `sceGuDrawBuffer` / `sceGuDispBuffer` / `sceGuDepthBuffer` work in VRAM-relative offsets; the absolute CPU address is obtained from `sceGeEdramGetAddr()` (e.g. [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) adds `0x4000000` style offsets, and `sceGuCopyImage`'s doc example offsets the framebuffer). Don't pass an absolute address where a relative one is expected, or vice-versa. (Concerns `sceGuDrawBuffer`, `sceGuDispBuffer`, `sceGeEdramGetAddr`.)
- **Not waiting for vblank before swapping (tearing).** Call `sceDisplayWaitVblankStart()` before `sceGuSwapBuffers()`; skipping it (as the commented-out line in [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) shows, done there only to benchmark raw FPS) causes visible tearing. (Concerns `sceDisplayWaitVblankStart`, `sceGuSwapBuffers`.)
- **Reading rendered data before the GE finishes.** Always `sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE)` (or `sceGeDrawSync`) before the CPU reads or reuses the framebuffer/list; the GE runs asynchronously to the CPU. (Concerns `sceGuSync`, `sceGuFinish`.)
- **Calling GU functions before `sceGuInit`.** The header states `sceGuInit` MUST be called first; any other GU call before it leaves state undetermined. (Concerns `sceGuInit`.)

## Sample references

- [cube.c](external/pspsdk/src/samples/gu/cube/cube.c) — canonical init + per-frame loop; textured, GUM-transformed 3D cube (`sceGumPerspective` / `sceGumRotateXYZ` / `sceGumDrawArray`).
- [sprite.c](external/pspsdk/src/samples/gu/sprite/sprite.c) — `GU_SPRITES` billboards built each frame into `sceGuGetMemory`, with alpha test and `GU_TFX_MODULATE`/`GU_TCC_RGBA`.
- [blit.c](external/pspsdk/src/samples/gu/blit/blit.c) — 2D image blits (`GU_TRANSFORM_2D`), texture swizzling, `sceKernelDcacheWritebackAll`, and capturing the `sceGuSwapBuffers` return value.
- [callbacks.c](external/pspsdk/src/samples/gu/common/callbacks.c) — the exit-callback pattern (`sceKernelRegisterExitCallback` + a callback thread) that drives the `running()` loop condition used by the other samples.
