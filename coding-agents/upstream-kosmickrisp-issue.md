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

I hit this porting the Recoil RTS engine, a fork of SpringRTS, to macOS. On zink over kosmickrisp the memory a render pass allocates never comes back, so footprint tracks how many render passes the process has ever issued rather than how many are alive. It happens on every run.

The attached reproducer makes a 512x512 RGBA texture, allocates every mip level, calls `glGenerateMipmap`, deletes the texture, and loops. It never draws and never presents. `glGenerateMipmap` is one blit per mip level, so one render pass each.

It prints its own `phys_footprint` as it goes, and takes the count as its only argument. The variations below are environment variables it reads, so one binary covers all four. 100 textures each time:

| what the loop does | phys_footprint |
|---------------|----------------|
| creates each texture and generates its mips | 2626 MiB |
| same, but skips `glGenerateMipmap` (`NO_GENMIP=1`) | 224 MiB |
| generates mips, with a `glFinish` after every texture (`SYNC=1`) | 161 MiB |
| generates mips, but running on llvmpipe | 36 MiB |

Only one texture is ever alive, since each is deleted before the next is made, so I expected this to stay flat. llvmpipe does. On kosmickrisp it retains roughly 26 MiB per `glGenerateMipmap` of a 1.33 MiB texture, linear in the count: 661 MiB at 25 textures, 992 at 50, 2339 at 100.

This is not work sitting in a queue. That 2626 MiB is measured after a full `glFinish`, and if I hold the process open and read it again five idle seconds later (`HOLD=1`) it is still 2624 MiB.

The `glFinish` per texture does not free anything either, it just stops more than one render pass being in flight at a time. My guess is that the pool grows to the high-water mark of concurrent render passes and never shrinks, but I have not confirmed that.

Throttling submission does not help. In the engine I tried a `glFinish` per texture, a zink flush every 64 blits, a flush plus timeline wait every 32 blits, `ZINK_DEBUG=sync`, and dropping the `batch_states_count` throttle in `post_submit` from 5000 to 8. I checked each one actually fired. None of them bounded the footprint, which would follow if a single batch state can hold thousands of render passes.

### Regression

Yes, bisected to:

```
c08dba83025533c3863349f147376b52528c4b3a
kk: Move to Metal4 command encoding
Aitor Camacho, 2026-06-16, MR !42268
```

Same tree, meson options, and reproducer binary, 100 textures:

| commit | peak | after 5s idle |
|--------|------|---------------|
| `56588ef0665`, the commit before | 482mb | 369mb |
| `mesa-26.2.0-rc3` | 2626mb | 2624mb |

On the older commit the memory also comes back, and it doesn't grow at a flat rate: 482mb at 100 textures, 938 at 200, 2060 at 500, 3204 at 1000.

`kk_encoder_start_internal` used to take command buffers from a Metal 3 command queue via `mtl_new_command_buffer(queue)`. Now `cs_start_render` and `kk_start_compute_encoder` each create fresh `MTL4CommandBuffer`'s from the device, plus a `MTL4CommandAllocator` that only reclaims on reset.

This also explains something that confused me for a while. Two other people have ported this engine the same way without hitting it, and both pinned Mesa from before June. I only started recently, so I picked up a build with the change in it.

Every released 26.2 tag has this, and 26.1 cannot run zink at all because `nullDescriptor` support landed on 2026-05-11 in MR !41313, so it fails with `Zink requires the nullDescriptor feature of KHR/EXT robustness2`. Far as I can tell there's no released Mesa where this works.

### Any extra information would be greatly appreciated

How I narrowed it down, in case it saves anyone the same afternoon:

1. It is not zink's device memory. Counting bytes at zink's `vkAllocateMemory` and `vkFreeMemory` tops out at 2.3gb over 301 allocations while the process reaches 28gb.
2. `vmmap -summary` puts the growth in `IOAccelerator (graphics)`, and it is the region count that climbs: 314, then 18636, then 51476, then 108971, reaching 12.5gb.
3. `heap -s` names them. `IOGPUMetalPooledResource` goes from 672 to 46842, `MTLResourceList` from 42 to 2691.
4. `malloc_history -allByCount` gives the call site: `zink_blit`, `kk_CmdBlitImage2`, `vk_meta_blit_image`, `kk_CmdBeginRendering`, `cs_start_render`, `mtl_new_render_command_encoder_with_descriptor`.
5. Counters I added to `kk_cmd_buffer.c` say the objects themselves are released fine, 102000 command buffers created in 12 seconds against 257520 resets, with never more than about 1000 live. The region count follows the cumulative number created at roughly six regions each, not the number live.

The reproducer makes this look milder than it is, so for severity, here is what it does to the engine:
 - One level load issues 102000 Metal command buffers in 12 seconds, 68177 render passes and 33823 compute, and hits 30 to 41gb before the OOM killer takes it.
 - On `56588ef0665` the same load finishes and sits flat at 7.3gb. Before I capped my test runs, one reached 54gb and I had to power cycle the machine to get it back.

Once memory is high it also aborts inside `kk_CmdBeginRendering` when Metal can no longer create a render command encoder, which I assume is just exhaustion rather than a second bug.