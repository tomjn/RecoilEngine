# Upstream report: kosmickrisp memory regression from Metal4 command encoding

Working notes. The text actually meant for the tracker is `upstream-kosmickrisp-issue.md`, which follows Mesa's own bug report template. This file keeps the filing rationale and the dead ends, which do not belong in the issue.

## Summary

`c08dba83025`, "kk: Move to Metal4 command encoding", 2026-06-16, made every render pass take its own `MTL4CommandBuffer` and `MTL4CommandAllocator`. The memory behind them is not returned when they are released, so process footprint grows with the number of render passes ever issued and never comes back.

Bisected against the immediately preceding commit, `56588ef0665`, built from the same tree with the same options and driven by the same binary. 100 mipmapped textures:

| | peak | after 5s idle |
|---|---|---|
| `56588ef0665`, pre-Metal4 | 482 MiB | 369 MiB |
| `mesa-26.2.0-rc3`, post-Metal4 | 2626 MiB | 2624 MiB |

A real application makes this fatal. One level load reaches 30 to 41 GiB in 12 seconds and is killed, against a stable 7.3 GiB plateau over a clean 60 second run on the pre-Metal4 commit.

Every released 26.2 tag contains the regression, and 26.1 cannot run zink at all because `nullDescriptor` support only landed on 2026-05-11 in MR 41313. So there is no released Mesa on which this works.

## Where this goes and why

File it in **mesa/mesa against kosmickrisp**. All three candidates live in that one repository, so there is a single tracker either way. What differs is who should pick it up.

- **Not mesa core.** llvmpipe runs the identical GL calls through the identical state tracker path and stays flat, so `st_generate_mipmap` and the GL layer are not implicated.
- **Not zink, mostly.** Zink's own device memory accounting is flat: 2.3 GiB over 301 `vkAllocateMemory` calls while the process reached 28 GiB. Zink asks for a lot of render passes, which is legal, and it cannot see what each one costs underneath.
- **Kosmickrisp.** `cs_start_render` and `kk_start_compute_encoder` each create a new `MTL4CommandBuffer` plus encoder, and the IOAccelerator allocations behind one are never returned. Measured at roughly 2.5 MiB per render pass. Bisected to `c08dba83025`, so this is a regression rather than long-standing behaviour.

One honest caveat to include when filing: this has not been isolated below kosmickrisp, so whether Metal itself fails to reclaim, or kosmickrisp is asking for too much per encoder, is not established. The bisect settles which change introduced it either way.

Worth mentioning in the same report, because a maintainer will ask: zink's only back-pressure is `check_oom_flush` on `bs->resource_size` and a throttle in `post_submit` at `batch_states_count > 5000`. Both are blind to per-command-buffer driver cost, so on this driver nothing throttles.

## Environment

- Mesa 26.2.0-rc3, built from source at `29b9c04a0da` (2026-07-29)
- zink over kosmickrisp, `EGL_PLATFORM=surfaceless`
- Apple M1 Pro, 16 GB, macOS 26.5.2 (Darwin 25.5.0)
- The relevant code is unchanged on `origin/main` as of 2026-08-02: `mtl_new_command_buffer` is still called per render pass in `kk_cmd_buffer.c`, and the release path is the same.

## Reproducer

`kk_mipmap_leak.c`, self-contained, EGL plus libGL only. It creates a texture, allocates every mip level, calls `glGenerateMipmap`, deletes the texture, and repeats. It never draws and never presents. It reports its own `phys_footprint` via `task_info`, so no external tooling is needed.

```
cc -O2 -o kk_mipmap_leak kk_mipmap_leak.c \
  -I$MESA_PREFIX/include -L$MESA_PREFIX/lib \
  -lEGL -Wl,-rpath,$MESA_PREFIX/lib -Wl,-rpath,/opt/homebrew/opt/vulkan-loader/lib

EGL_PLATFORM=surfaceless MESA_GL_VERSION_OVERRIDE=4.6 \
MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
LIBGL_DRIVERS_PATH=$MESA_PREFIX/lib \
VK_DRIVER_FILES=$MESA_PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
./kk_mipmap_leak 100
```

`MESA_GL_VERSION_OVERRIDE=4.6` is needed or the 3.3 core context fails to make current.

## Result

100 textures of 512x512, one binary, switches by environment variable:

| configuration | `phys_footprint` |
|---|---|
| `glGenerateMipmap`, no explicit sync | 2626 MiB |
| `NO_GENMIP=1`, no explicit sync | 224 MiB |
| `glGenerateMipmap`, `SYNC=1` for a `glFinish` per iteration | 161 MiB |
| llvmpipe, `glGenerateMipmap`, no explicit sync | 36 MiB |

So about 26 MiB per `glGenerateMipmap` of a 1.33 MiB texture, and the texture is deleted immediately in every case. It scales linearly: 661 MiB at 25 textures, 992 at 50, 2339 at 100.

**The memory is never returned.** The `done` figure above is printed after a full `glFinish`, so every command buffer has retired, and it still reads 2625 MiB. `HOLD=1` re-reads it after five idle seconds and gets 2624 MiB.

So the shape of the defect is: the IOGPU resource pool grows to the high-water mark of concurrently in-flight render passes and never shrinks. `SYNC=1` does not free anything, it holds the high-water mark down to one texture's worth by never letting more than one render pass be in flight. On unified memory that high-water mark is real system pressure, and it ends in the OOM killer.

That distinction matters for anyone proposing a fix, because throttling submission does not work. Four separate throttles were tried against a real application and none bounded it, see below.

Note for anyone measuring this: `ps -o rss` is useless here. IOAccelerator allocations are real memory on Apple Silicon and RSS does not count them. We measured 50 GB of `phys_footprint` against an RSS of 1349 MB.

## How it was attributed

1. Zink's device memory is not it. Counting bytes at zink's `vkAllocateMemory` and `vkFreeMemory` tops out at 2.3 GiB over 301 allocations while `phys_footprint` reaches 28 GiB.
2. `vmmap -summary` puts the growth in `IOAccelerator (graphics)`, and it is region count that grows: 314, 18636, 51476, 108971, reaching 12.5 GiB.
3. `heap -s` names the classes. `IOGPUMetalPooledResource` goes from 672 to 46842, `MTLResourceList` from 42 to 2691.
4. `malloc_history -allByCount` gives the call site: `zink_blit` to `kk_CmdBlitImage2` to `vk_meta_blit_image` to `kk_CmdBeginRendering` to `cs_start_render` to `mtl_new_render_command_encoder_with_descriptor`.
5. Counters in `kk_cmd_buffer.c` show the objects are released correctly, so nothing is lost: 102000 command buffers created in 12 seconds against 257520 resets, with live count never above 1000. Region count tracks the cumulative number created at about six regions each, not the number live.

## Impact

Found while porting a real application, the Recoil RTS engine. A level load creates 102000 Metal command buffers in 12 seconds, split 68177 render passes to 33823 compute, and reaches 30 GiB of `phys_footprint` before the process is killed. An uncapped run reached 54 GB and made the machine unresponsive enough to need a power cycle. It also produces an abort inside `kk_CmdBeginRendering` when Metal can no longer create a render command encoder, which is memory exhaustion rather than a separate defect.

## Throttling does not fix it

Four throttles were tried against the real application. All of them were verified to take effect, and none bounded the footprint.

| throttle | effect on the mechanism | peak footprint |
|---|---|---|
| none, baseline | | 32 GiB at 12s |
| `glFinish` after every texture built | 500 of 68177 render passes | 33 GiB at 12s |
| flush every 64 blits inside `zink_blit` | 8000 of 68177 render passes | 31 GiB at 12s |
| flush plus timeline wait every 32 blits | verified to fire, 247 times over 8000 blits | 30 GiB at 12s |
| `ZINK_MAX_BATCHES=8`, down from 5000 | live batch states held at 7 to 18, from 126 to 229 | 22 GiB at 14s |

The reason is the same in every case. A single batch state can contain thousands of render passes, so capping outstanding batches does not cap in-flight render passes, and syncing one class of render pass leaves the other 90%. There is no knob at the zink level that bounds what actually matters here, which is the number of render passes in flight at once.

For reference, one level load of this application produces 102000 Metal command buffers in 12 seconds across 40000 submits, split 68177 render passes to 33823 compute, with never more than about 660 command buffers live at a time. Live object counts stay small throughout. It is the pool behind them that grows.
