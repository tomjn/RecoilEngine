# Upstream report: unbounded memory growth from queued mipmap generation on zink over kosmickrisp

Draft for <https://gitlab.freedesktop.org/mesa/mesa/-/issues>. Not filed yet.

## Where this goes and why

File it in **mesa/mesa against kosmickrisp**. All three candidates live in that one repository, so there is a single tracker either way. What differs is who should pick it up.

- **Not mesa core.** llvmpipe runs the identical GL calls through the identical state tracker path and stays flat, so `st_generate_mipmap` and the GL layer are not implicated.
- **Not zink, mostly.** Zink's own device memory accounting is flat: 2.3 GiB over 301 `vkAllocateMemory` calls while the process reached 28 GiB. Zink asks for a lot of render passes, which is legal, and it cannot see what each one costs underneath.
- **Kosmickrisp.** `cs_start_render` and `kk_start_compute_encoder` each create a new `MTL4CommandBuffer` plus encoder, and the IOAccelerator allocations behind one are held until the work retires. Measured at roughly 2.5 MiB per render pass. That is the disproportionate part.

One honest caveat to include when filing: this has not been isolated below kosmickrisp, so whether Metal itself is slow to reclaim, or kosmickrisp is asking for too much per encoder, is not established. Either way the design choice of one command buffer per encoder is kosmickrisp's.

Worth mentioning in the same report, because a maintainer will ask: zink's only back-pressure is `check_oom_flush` on `bs->resource_size` and a throttle in `post_submit` at `batch_states_count > 5000`. Both are blind to per-command-buffer driver cost, so on this driver nothing throttles.

## Environment

- Mesa 26.2.0-rc3, built from source at `29b9c04a0da` (2026-07-29)
- zink over kosmickrisp, `EGL_PLATFORM=surfaceless`
- Apple M1 Pro, 16 GB, macOS 15 (Darwin 25.5.0)
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

It is not a leak in the sense of memory never freed. `glFinish` per iteration drains it completely. It is unbounded growth of work that has been queued and not yet retired, with nothing applying back-pressure. An application that queues faster than the GPU retires grows without limit, and on unified memory that is real system pressure that ends in the OOM killer.

Note for anyone measuring this: `ps -o rss` is useless here. IOAccelerator allocations are real memory on Apple Silicon and RSS does not count them. We measured 50 GB of `phys_footprint` against an RSS of 1349 MB.

## How it was attributed

1. Zink's device memory is not it. Counting bytes at zink's `vkAllocateMemory` and `vkFreeMemory` tops out at 2.3 GiB over 301 allocations while `phys_footprint` reaches 28 GiB.
2. `vmmap -summary` puts the growth in `IOAccelerator (graphics)`, and it is region count that grows: 314, 18636, 51476, 108971, reaching 12.5 GiB.
3. `heap -s` names the classes. `IOGPUMetalPooledResource` goes from 672 to 46842, `MTLResourceList` from 42 to 2691.
4. `malloc_history -allByCount` gives the call site: `zink_blit` to `kk_CmdBlitImage2` to `vk_meta_blit_image` to `kk_CmdBeginRendering` to `cs_start_render` to `mtl_new_render_command_encoder_with_descriptor`.
5. Counters in `kk_cmd_buffer.c` show the objects are released correctly, so nothing is lost: 102000 command buffers created in 12 seconds against 257520 resets, with live count never above 1000. Region count tracks the cumulative number created at about six regions each, not the number live.

## Impact

Found while porting a real application, the Recoil RTS engine. A level load creates 102000 Metal command buffers in 12 seconds, split 68177 render passes to 33823 compute, and reaches 30 GiB of `phys_footprint` before the process is killed. An uncapped run reached 54 GB and made the machine unresponsive enough to need a power cycle. It also produces an abort inside `kk_CmdBeginRendering` when Metal can no longer create a render command encoder, which is memory exhaustion rather than a separate defect.

## Not yet reconciled

In the standalone reproducer a `glFinish` per iteration bounds the growth completely. In the engine, three different synchronisation points made no difference: a per-frame `glFinish`, a flush every 64 blits, and a flush plus timeline wait every 32 blits inside `zink_blit`. `ZINK_DEBUG=sync` also made no difference. Either the engine's peak is reached between two syncs, since a load uploads thousands of textures per drawn frame, or those particular waits did not drain what they were expected to. Worth stating in the report rather than claiming a clean throttling story.
