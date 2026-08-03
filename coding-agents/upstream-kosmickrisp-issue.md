# Ready to paste into the Mesa tracker

Everything below the line follows `.gitlab/issue_templates/Bug Report.md`, with the Linux-only fields filled in by hand because this is macOS. Our own filing rationale and the full list of dead ends stay in `upstream-kosmickrisp-memory-report.md` and are deliberately not part of this.

**Title**

```
kosmickrisp: memory is never reclaimed per render pass, regression from Metal4 command encoding (c08dba83025), M1 Pro / zink
```

---

### System information

- OS: macOS 26.5.2, build 25F84
- GPU: Apple M1 Pro
- Kernel version: Darwin 25.5.0
- Mesa version: 26.2.0-rc3 built from source at `29b9c04a0da`. `GL_RENDERER = zink Vulkan 1.4(Apple M1 Pro (MESA_KOSMICKRISP))`
- Xserver version: not applicable
- Desktop manager and compositor: not applicable, the reproducer runs `EGL_PLATFORM=surfaceless`

Built with:

```
meson setup build-zink --buildtype=release \
  -Dplatforms=macos -Degl=enabled -Degl-native-platform=surfaceless \
  -Dgallium-drivers=zink -Dvulkan-drivers=kosmickrisp \
  -Dmoltenvk-dir=/opt/homebrew/opt/molten-vk \
  -Dgallium-rusticl=false -Dtools= -Dglx=disabled -Dgbm=disabled \
  -Dllvm=enabled -Dvideo-codecs= -Dopengl=true -Dgles1=disabled -Dgles2=disabled
```

### Describe the issue

On zink over kosmickrisp, the memory a render pass allocates is never returned to the process. `phys_footprint` grows with the number of render passes ever issued, not with the number live, and it does not come back after the work retires.

It reproduces 100% of the time, on every run.

The attached reproducer creates a 512x512 RGBA texture, allocates every mip level, calls `glGenerateMipmap`, deletes the texture, and repeats. It never draws and never presents. `glGenerateMipmap` is one blit, so one render pass, per mip level.

100 textures, one binary, switched by environment variable:

| configuration | phys_footprint |
|---|---|
| `glGenerateMipmap` | 2626 MiB |
| `NO_GENMIP=1` | 224 MiB |
| `glGenerateMipmap`, `SYNC=1` for a glFinish per iteration | 161 MiB |
| llvmpipe, `glGenerateMipmap` | 36 MiB |

Expected: footprint proportional to the textures actually alive, which is one, since each is deleted before the next is made. llvmpipe does this.

Seen instead: about 26 MiB retained per `glGenerateMipmap` of a 1.33 MiB texture. Linear in the count, 661 MiB at 25 textures, 992 at 50, 2339 at 100.

The memory is not merely in flight. The 2626 MiB figure is read after a full `glFinish`, so every command buffer has retired, and `HOLD=1` re-reads it after five idle seconds and still gets 2624 MiB.

`SYNC=1` does not free anything either. It holds the total down by never letting more than one render pass be in flight at a time, which suggests the pool grows to the high-water mark of concurrent render passes and never shrinks.

Throttling submission does not help. Against a real application we tried a `glFinish` per texture, a zink flush every 64 blits, a zink flush plus timeline wait every 32 blits, `ZINK_DEBUG=sync`, and lowering the `batch_states_count` throttle in `post_submit` from 5000 to 8. All were verified to take effect. None bounded the footprint, because a single batch state can hold thousands of render passes.

### Regression

Yes. Bisected to:

```
c08dba83025533c3863349f147376b52528c4b3a
kk: Move to Metal4 command encoding
Aitor Camacho, 2026-06-16, MR !42268
```

Same tree, same meson options, same reproducer binary, 100 textures:

| commit | peak | after 5s idle |
|---|---|---|
| `56588ef0665`, the commit before | 482 MiB | 369 MiB |
| `mesa-26.2.0-rc3` | 2626 MiB | 2624 MiB |

Before that commit the memory is also given back, and it scales sublinearly rather than at a flat rate: 482 MiB at 100 textures, 938 at 200, 2060 at 500, 3204 at 1000.

Before it, `kk_encoder_start_internal` took command buffers from a Metal 3 command queue with `mtl_new_command_buffer(queue)`. After it, `cs_start_render` and `kk_start_compute_encoder` each create a fresh `MTL4CommandBuffer` from the device plus a `MTL4CommandAllocator` that only reclaims on reset.

Note on versions. Every released 26.2 tag contains the regression, and 26.1 cannot run zink at all because `nullDescriptor` support only landed on 2026-05-11 in MR !41313, so it fails with `Zink requires the nullDescriptor feature of KHR/EXT robustness2`. There is therefore no released Mesa on which this works. `origin/main` at 2026-08-02 still has the same per render pass `mtl_new_command_buffer` and the same release path, so this was not tested against a built main, only read.

### Any extra information

How it was attributed, in case it saves someone the same afternoon:

1. Not zink's device memory. Counting bytes at zink's `vkAllocateMemory` and `vkFreeMemory` tops out at 2.3 GiB over 301 allocations while the process reaches 28 GiB.
2. `vmmap -summary` puts the growth in `IOAccelerator (graphics)`, and it is region count that grows: 314, then 18636, then 51476, then 108971, reaching 12.5 GiB.
3. `heap -s` names the classes. `IOGPUMetalPooledResource` goes from 672 to 46842, `MTLResourceList` from 42 to 2691.
4. `malloc_history -allByCount` gives the call site, `zink_blit` to `kk_CmdBlitImage2` to `vk_meta_blit_image` to `kk_CmdBeginRendering` to `cs_start_render` to `mtl_new_render_command_encoder_with_descriptor`.
5. Counters added to `kk_cmd_buffer.c` show the objects are released correctly, 102000 command buffers created in 12 seconds against 257520 resets, with live count never above 1000. Region count tracks the cumulative number created at about six regions each, not the number live.

Anyone measuring this on Apple Silicon should use `phys_footprint` and not RSS. IOAccelerator allocations are real memory here and RSS cannot see them. We measured 50 GB of footprint against an RSS of 1349 MB.

Impact, for severity. Found porting the Recoil RTS engine, a fork of Spring. One level load issues 102000 Metal command buffers in 12 seconds, split 68177 render passes to 33823 compute, and reaches 30 to 41 GiB before the OOM killer takes it. On `56588ef0665` the same load runs to completion and sits flat at 7.3 GiB. An uncapped run reached 54 GB and made a 16 GB machine unresponsive enough to need a power cycle. High memory also produces an abort inside `kk_CmdBeginRendering` when Metal can no longer create a render command encoder, which is exhaustion rather than a separate bug.
