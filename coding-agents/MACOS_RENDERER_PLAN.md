# macOS renderer: decomposition plan

Status: S0 through S4b, S6 and S6b are built and on branches, see each layer. I4 is open as PR #3163. S5, S7 and S8 are plan only, and no code, branches or PRs exist for those unless a PR number is given.

Written 2026-08-01 against `macos/test-integration` at 32c28394a5 (17 commits ahead of `origin/master`, 0 behind).

## 1. The actual delta, measured

Everything in this section was checked on this machine today, not read off the old PRs.

### The engine already builds

`engine-legacy` configures and compiles clean on `macos/test-integration` with Homebrew GCC 16, Ninja and ccache. No source changes. Output is a 33 MB Mach-O arm64 binary at `build-macos-legacy/spring`.

```
cmake -S . -B build-macos-legacy -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/opt/homebrew/bin/gcc-16 -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-16
cmake --build build-macos-legacy -j 10 --target engine-legacy
```

Zero compile errors, zero link errors. One benign linker warning about duplicate static libraries.

This is the single biggest correction to the old picture. #2991 carried around 25 build fixes. The great majority were AppleClang and libc++ problems that do not exist under Homebrew GCC, and most of the rest have since merged upstream. There is no "make it compile" layer to write.

`otool -L` on the binary shows what it links:

- `/opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib`
- `/System/Library/Frameworks/OpenGL.framework`

### The engine dies at GL context creation

Running `./spring --window --isolation` gets through streflop init (NEON, FPCR verified), watchdog, video mode enumeration and window creation, then aborts:

```
[GR::CreateGLContext] created main GL2.0 core-context
[GR::CreateGLContext] created main GL2.0 compatibility-context
[GR::CreateGLContext] created main GL2.1 core-context
[GR::CreateGLContext] created main GL2.1 compatibility-context
[GR::CreateGLContext] created main GL3.0 core-context
[GR::CreateGLContext] error ("Failed creating OpenGL context at version requested") creating main GL3.0 compatibility-context
... core succeeds to 4.1, compatibility fails at every version >= 3.0 ...
Fatal: [ExitSpringProcess] errorMsg="[GR::CreateGLContext] error ... creating main GL3.0 compatibility-context"
```

`CGlobalRendering::CreateGLContext` (rts/Rendering/GlobalRendering.cpp:462) sweeps every version in both profiles and keeps `cmpCtx`, the lowest compatibility context at or above `minCtx`. Apple's GL offers compatibility only at 2.0 and 2.1, so `cmpCtx.x` stays 0 and the function calls `handleerror` and returns null.

Two things follow. Apple's OpenGL.framework can never satisfy the engine, so the Mesa route is not an optimisation, it is the only route. And #2991's stated reason for unlinking OpenGL.framework, a bus error inside `+[NSOpenGLContext currentContext]` on macOS 26, did not reproduce here. The failure is a clean, handled context-creation failure. Treat that rationale as unverified.

### The Mesa substrate is present but does not work

`brew install mesa` (26.1.4) ships everything needed, verified in the keg:

- `lib/libEGL.dylib`
- `lib/dri/zink_dri.dylib`
- `lib/libvulkan_kosmickrisp.dylib`
- `share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`
- `include/EGL`, `include/GL`

Homebrew's formula builds macOS with `-Dgallium-drivers=llvmpipe,zink` and `-Dvulkan-drivers=kosmickrisp,swrast` on Sequoia and newer. So no from-source Mesa build is needed just to get the pieces.

A standalone EGL probe compiled against that keg gives a working baseline:

```
EGL 1.5 vendor='Mesa Project'
[COMPAT] GL_RENDERER = llvmpipe (LLVM 22.1.8, 128 bits)
[COMPAT] GL_VERSION  = 4.6 (Compatibility Profile) Mesa 26.1.4
[CORE]   GL_VERSION  = 4.6 (Core Profile) Mesa 26.1.4
```

GL 4.6 compatibility profile, which is exactly what the engine needs and what Apple refuses to give.

Switching to hardware fails:

```
MESA: error: Zink requires the nullDescriptor feature of KHR/EXT robustness2.
libEGL warning: egl: failed to create dri2 screen
eglInitialize FAILED 0x3001
```

Identical failure against KosmicKrisp and against MoltenVK 1.4.2. Not bypassable by `MESA_KK_EXPERIMENTAL=1`, `MESA_KK_DEBUG=force_robustness`, `MESA_KK_DISABLE_WORKAROUNDS=all`, or either `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` mode. Zink on lavapipe fails differently ("failed to choose pdev").

Mesa MR 41313 is `kk: Support nullDescriptor` by squidbus, dated 2026-05-11, reviewed by the KosmicKrisp lead. It implements the feature in KosmicKrisp rather than working around it in Zink. `git tag --contains` puts it in `26.2-branchpoint`, `mesa-26.2.0-rc1`, `rc2` and `rc3`. It is not in 26.1.

`brew install --HEAD mesa` cannot deliver it. Homebrew's formula sets `-Dgallium-rusticl=true` and `-Dtools=etnaviv,...`, both of which trigger Mesa's Rust bindings layer, which needs the `syn` crate through pkg-config. There is no Homebrew formula providing it.

### Building 26.2.0-rc3 from source does work

Verified on this machine. Mesa `mesa-26.2.0-rc3` built with a minimal configuration gives a hardware GL context:

```
GL_VENDOR   = Mesa
GL_RENDERER = zink Vulkan 1.4(Apple M1 Pro (MESA_KOSMICKRISP))
GL_VERSION  = 4.6 (Compatibility Profile) Mesa 26.2.0-rc3 (git-5db7111ffb)
GLSL        = 4.60
```

That is the compatibility profile the engine needs, on the GPU. See S0 for the recipe.

Three caveats, all of which matter downstream.

**The GL version is forced, not native.** Without `MESA_GL_VERSION_OVERRIDE=4.6` and `MESA_GLSL_VERSION_OVERRIDE=460`, Zink on KosmicKrisp reports `GL_VERSION = 2.1` and `eglCreateContext` returns `EGL_BAD_MATCH` for every request at 3.0 or above. The overrides make Mesa claim 4.6 rather than making it true. Anything Zink cannot actually back will now fail at draw time instead of at query time. This is a far wider correctness caveat than the geometry-shader one, because it applies to the whole GL surface rather than one stage.

**Zink warns about the device on startup:**

```
WARNING: Some incorrect rendering might occur because the selected Vulkan device
(Apple M1 Pro) doesn't support base Zink requirements: have_EXT_custom_border_color
```

**The geometry-shader lie is confirmed on hardware.** Under the override, `GL_MAX_GEOMETRY_OUTPUT_VERTICES` returns 1024 on a device with no geometry stage. This was previously inferred from #2991's comments. It is now measured.

### Mesa's macOS EGL is surfaceless only

The probe reports the full EGL extension string. It contains `EGL_KHR_surfaceless_context` and no platform extension at all. No `EGL_EXT_platform_base`, no macOS platform, nothing that would let `eglCreateWindowSurface` target a CAMetalLayer.

This independently confirms #2991's core design decision. With stock Mesa there is no window surface, so the engine must render into a pbuffer and get those pixels onto the screen itself. #3060's direct-present design needs the patched "surfaceless platform that creates window surfaces" hack from the Minecraft gist, which is a Mesa fork, not a Mesa build.

### SDL

Homebrew's `sdl2` is an alias for `sdl2-compat` 2.32.70, which is the SDL2 API on top of SDL3 3.4.12. The real SDL2 formula is gone. Per your decision we build on that.

It exports `SDL_GetWindowWMInfo` and contains the `SDL.window.cocoa.window` SDL3 property string, so the NSView and CAMetalLayer grab that #2991 does through `SDL_SysWMinfo` should work. Not yet exercised end to end.

### Dependency declaration gap

`rts/builds/legacy/CMakeLists.txt` calls `find_package_static(Fontconfig 2.11 REQUIRED)`. Neither the `Brewfile` (PR #3038) nor the macOS CI gate (PR #3134) installs fontconfig. The build here only succeeded because fontconfig is installed on this machine for unrelated reasons. Both lists need it before `engine-legacy` can be a CI target.

### Summary of what is missing

| Gap | Nature |
|---|---|
| GL 3.0+ compatibility context | Runtime. Needs Mesa EGL, not Apple GL |
| Working Zink on Metal | External. Needs Mesa past MR 41313 |
| Getting pbuffer pixels to screen | Code. Readback plus Metal present |
| Viewport and window-size contract | Code. Backing pixels vs logical points |
| Mouse coordinate space | Code. Follows from the above |
| Geometry shaders | Capability lie. Needs a guard plus game content |
| fontconfig not declared | Build metadata |
| Shipping Mesa to users | Packaging |

## 2. The layers

### Bucket 1: independent PRs off `origin/master`

These are not part of the stack. Each stands on its own merits.

Most of what could be extracted from #2991 already has been. Merged: #3071 glad GLX, #3065 SpringApp X11 include, #3045 legacy X11 guard, #3034 SDLMain removal, #3047 CUtils scandir, #3026 smmalloc INLINE, #3025 SafeUtil, #3024 SolLua, #3023 LuaTextures logging, #3074 `<algorithm>` includes, #3076 archive narrowing, #3078 LuaMenu LuaSocket. Open and already yours: #3160, #3161, #3073, #3072, #3041, #3044, #3046, #3042, #3038, #3035, #3036.

That leaves four.

**I1. Core-profile ARB extension check, PR #3022, already open**

Purpose: `CheckGLExtensions` fails on a core-profile context because ARB extensions folded into GL 1.3/2.0/3.0 are no longer advertised by name. Skip the legacy name check when `glContextIsCore`.

Touches: `rts/Rendering/GlobalRendering.cpp`, one early return.

Reviewer check: run any core-profile context on Linux. The check should no longer reject it.

Risk: **this PR has CHANGES_REQUESTED from sprunk.** The objection is that the `else` branch setting `glContextIsCore` is a heuristic when `GL_CONTEXT_PROFILE_MASK` reports 0. The stack depends on this landing or on an agreed alternative. Resolve it before building on top.

**I2. `CTextureAtlas::GetTexID` null guard**

Purpose: `atlasTex` stays null when `Finalize`/`Allocate` fails, so `GetTexID()` and `DisOwnTexture()` dereference null. A failed atlas should degrade to no texture, not crash.

Touches: `rts/Rendering/Textures/TextureAtlas.h`, two accessors.

Reviewer check: purely defensive, readable from the diff. No behaviour change on a healthy atlas.

Risk: low. A reviewer may reasonably ask whether hiding the failure is right, and whether a log line belongs there. Have that answer ready.

**I3. fontconfig in the Brewfile and the CI gate**

Purpose: `engine-legacy` requires Fontconfig 2.11 and neither dependency list declares it.

Touches: `Brewfile` (currently PR #3038), the brew install step in the macOS CI workflow (currently PR #3134).

Reviewer check: a clean machine following the Brewfile can configure `engine-legacy`.

Risk: low. Best folded into #3038 and #3134 as extra commits rather than opened as a fourth PR.

**I4. `--window` cannot turn fullscreen off. DONE, PR #3163**

Purpose: `CGlobalRendering::SetFullScreen` computes the windowed case and then throws it away.

```cpp
fullScreen = (cfgFullScreen && !cliWindowed  );
fullScreen = (cfgFullScreen ||  cliFullScreen);
```

The second line is a plain assignment, so `--window` does nothing whenever the config says fullscreen. Not macOS specific, found here because the display mode change that exclusive fullscreen forces is very visible on a Retina Mac: the desktop drops out of its 2x mode and every other window's text goes tiny.

**The date in the earlier draft was wrong.** `git log -S` puts the two-line form in `d1eec3d177`, April 2017, not `acc116b1a0`. That commit rewrote a working if/else chain into two assignments and dropped the first result. The chain gave `--window` priority over `--fullscreen`, so the fix is `!cliWindowed && (cfgFullScreen || cliFullScreen)` rather than the `(cfg && !cliWindowed) || cliFullScreen` suggested here before. The two differ only when both flags are passed.

Branch `fix/window-cli-flag` off `origin/master`, one commit, `9cae4335c6`. Also cherry-picked to the front of the borderless branch below, since that layer needs it to test anything.

Touches: `rts/Rendering/GlobalRendering.cpp`, one line.

Verified: with `Fullscreen = 1` in the config, `--window` gives a decorated window and the log reads `windowed::decorated`.

Risk: low, but it changes behaviour that has been wrong for nine years, so somebody may be relying on it. `SetFullScreen` writes its result back to the config, so `--window` now persists `Fullscreen = 0`, the same way `--fullscreen` has always persisted `Fullscreen = 1`.

**Deliberately not extracted:**

The HiDPI window-size persistence fix from #2991. That was PR #3021 and you closed it as a no-op on master, correctly. Nothing on master reassigns `winSizeX` to backing pixels, so re-querying SDL in `SaveWindowPosAndSize` returns the same value. It only becomes a real fix once layer S3 makes `winSize` mean pbuffer pixels. It belongs in the stack, in the same commit that creates the divergence.

### Bucket 2: the stack on `macos/test-integration`

Suggested integration branch `macos/renderer-integration`, maintained the same way. Rebased onto upstream, self-emptying as pieces land.

Be blunt with reviewers about this: **layers S1 through S3 produce nothing visible.** S1 gets a GL context and then a black window. S2 adds a file nothing calls. Only at the end of S3 does a pixel reach the screen. Anyone reviewing S1 in isolation is reviewing plumbing.

---

**S0. Substrate: a Mesa build where Zink initialises. DONE, with caveats**

Not engine code. A pinned recipe plus a probe, kept in `coding-agents/` or `docs/`.

Purpose: prove that Zink on KosmicKrisp gives a hardware GL 4.6 compatibility context on Apple Silicon, and pin the exact Mesa revision that does it.

Result: yes, on `mesa-26.2.0-rc3`. Recipe below, verified 2026-08-01.

```bash
git clone --filter=blob:none https://gitlab.freedesktop.org/mesa/mesa.git
cd mesa && git checkout mesa-26.2.0-rc3
python3 -m venv .venv && .venv/bin/pip install mako packaging setuptools

PATH="$PWD/.venv/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/llvm/bin:$PATH" \
PKG_CONFIG_PATH="/opt/homebrew/opt/libclc/share/pkgconfig:/opt/homebrew/opt/spirv-llvm-translator/lib/pkgconfig" \
meson setup build-zink \
  --buildtype=release --prefix="$HOME/dev/mesa-install" \
  -Dplatforms=macos -Degl=enabled -Degl-native-platform=surfaceless \
  -Dgallium-drivers=zink -Dvulkan-drivers=kosmickrisp \
  -Dmoltenvk-dir=/opt/homebrew/opt/molten-vk \
  -Dgallium-rusticl=false -Dtools= -Dglx=disabled -Dgbm=disabled \
  -Dllvm=enabled -Dvideo-codecs= -Dopengl=true -Dgles1=disabled -Dgles2=disabled

PATH="$PWD/.venv/bin:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/llvm/bin:$PATH" \
  ninja -C build-zink install
```

Homebrew build prerequisites: `meson ninja bison llvm libclc spirv-tools spirv-llvm-translator molten-vk vulkan-loader vulkan-headers`.

Five things had to be found the hard way, all of which belong in the recipe rather than in anyone's memory.

- Mesa needs the Python `mako`, `packaging` and `setuptools` modules. No Homebrew formula provides `mako`, so use a venv rather than touching the Homebrew Python.
- Homebrew's `llvm` is keg-only, so `llvm-config` must be put on `PATH` explicitly.
- `libclc` and `spirv-llvm-translator` need their pkg-config directories on `PKG_CONFIG_PATH`. KosmicKrisp pulls in CLC, so `-Dllvm=disabled` is not an option.
- Zink on macOS requires `-Dmoltenvk-dir` at configure time even when the target is KosmicKrisp.
- Apple's `/usr/bin/bison` is 2.3 and cannot parse Mesa's grammar files. Homebrew's bison 3.8.2 is keg-only. Meson bakes the bison path into `build.ninja` at configure time, so putting it on `PATH` only for the build step does nothing. It must be set for `meson setup`.

Runtime environment:

```bash
EGL_PLATFORM=surfaceless
MESA_LOADER_DRIVER_OVERRIDE=zink
GALLIUM_DRIVER=zink
LIBGL_DRIVERS_PATH=$PREFIX/lib
VK_DRIVER_FILES=$PREFIX/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
MESA_GL_VERSION_OVERRIDE=4.6
MESA_GLSL_VERSION_OVERRIDE=460
```

Note Mesa 26.2 uses a single `libgallium-<version>.dylib` megadriver, so there is no `lib/dri/zink_dri.dylib` any more and `LIBGL_DRIVERS_PATH` points at `lib` itself. Also note the loader resolves `@rpath/libvulkan.1.dylib`, which `DYLD_FALLBACK_LIBRARY_PATH` does not satisfy. The consumer binary needs an rpath entry for wherever `libvulkan.1.dylib` lives. That is a preview of the S7 packaging problem.

What a reviewer can verify: run the probe, get `zink Vulkan 1.4(Apple M1 Pro (MESA_KOSMICKRISP))`.

Remaining risk, and it is now the largest in the plan: **the GL 4.6 context is a forced claim, not a measured capability.** Without the overrides Zink reports GL 2.1. We are telling the engine that 4.6 is available and finding out at draw time where that is untrue. The geometry-shader guard in S5 is the first known instance of this. There will very likely be others, and they will surface as rendering bugs rather than as clean failures.

Secondary risk: 26.2.0-rc3 is a release candidate, so the pin will need moving to 26.2.0 final and then tracking.

---

**S1. EGL context creation**

Purpose: on macOS, create the GL context through Mesa EGL instead of SDL, and stop linking Apple's OpenGL.framework.

Touches:
- `rts/builds/legacy/CMakeLists.txt`, locate Mesa EGL, link `Metal`, `QuartzCore`, `IOSurface`, `objc`, drop `OpenGL::GL` on Apple
- `rts/Rendering/GlobalRendering.cpp`, `__APPLE__`-guarded EGL init, destroy, make-current, plus `gladLoadGLLoader(eglGetProcAddress)` and dropping `SDL_WINDOW_OPENGL` from the window flags

Do not copy #2991's `SPRING_MAC_LIBEGL` cache variable verbatim. Write a proper `FindMesaEGL.cmake` that finds the Homebrew keg by default and takes an override, and do not hardcode anyone's home directory the way #3060 does.

Reviewer check: `engine-legacy` builds, `otool -L` shows Mesa's libEGL and no OpenGL.framework, and the engine logs an EGL 1.5 context at GL 4.6 compatibility and gets past `CreateGLContext` into `PostInit`. The window stays black.

Risk: medium. Two places assume an `SDL_GLContext`. `glContext` is typed as `SDL_GLContext` and #2991 casts the `EGLContext` into it, which works but is a lie the code tells itself. Consider a small `__APPLE__` branch instead. Shutdown must call `eglDestroyContext`, not `SDL_GL_DeleteContext`. ExaDev hit a crash on exactly this.

---

**S2. `MetalPresent.mm`**

Purpose: the entire Objective-C surface of the port, in one file. Attach a CAMetalLayer to the SDL window's NSView, own an IOSurface-backed MTLTexture, and blit it to the drawable with a one-triangle render pass.

Touches: `rts/System/Platform/Mac/MetalPresent.{h,mm}`, new, plus source lists in the legacy build. The ObjC and clang split from #3073 already handles compiling it.

Interface, kept C so the GCC side never sees Objective-C:

```c
bool  MacMetalPresent_Init(void* caMetalLayer);
void* MacMetalPresent_AcquireIOSurfaceBuffer(int w, int h, size_t* outRowBytes);
void  MacMetalPresent_PresentIOSurface(bool flipY);
void  MacMetalPresent_PresentBGRA(int w, int h, const void* pixels, bool flipY);
```

Reviewer check: it compiles under clang while the rest builds under GCC, and a `SPRING_MAC_PRESENT_TEST`-style flash of solid colours puts visible frames on screen. That test hook is worth keeping for this layer and deleting later.

Risk: medium. #2991 reaches the NSView through raw `objc_msgSend` casts from C++ in `GlobalRendering.cpp`, including a hand-rolled `CGRect` struct. Since S2 already exists as a `.mm` file, move that code into it and write it as normal Objective-C. That removes the fragile ABI casting and keeps `GlobalRendering.cpp` free of `objc/runtime.h`.

---

**S3. Present path and the size contract. DONE**

Purpose: wire the present into `SwapBuffers` and fix what the engine means by window size.

Branch `macos/renderer-s3-present`, three commits off S2. The main menu draws, windowed and fullscreen, at full backing resolution.

- `c25bfb210a` the size contract. The Metal layer goes up before the EGL context and reports its drawable size, and the pbuffer is created that size. `ReadWindowPosAndSize` takes `winSize` from the pbuffer. `SaveWindowPosAndSize` writes the resolution config from `SDL_GetWindowSize`, which is the #3021 change and is now a real fix.
- `ef41a47ab3` the present. New `rts/System/Platform/Mac/MacGLPresent.{h,cpp}`, called from `SwapBuffers` in place of `SDL_GL_SwapWindow`. `glReadPixels` writes straight into the IOSurface, so no copy sits between GL and the texture the GPU samples.
- `b487bad86a` reverts the S2 test hook.

**The size numbers, measured.** The earlier accounts disagreed because they were different modes, not because anything was wrong.

| | fullscreen (exclusive) | windowed |
|---|---|---|
| `SDL_GetWindowSize` | 3024x1964 | 1200x800 |
| `SDL_GL_GetDrawableSize` | 3024x1964 | 1200x800 |
| `NSWindow.backingScaleFactor` | 1.0 | 2.0 |
| CAMetalLayer drawable | 3024x1964 | 2400x1600 |

`SDL_WINDOW_FULLSCREEN` asks for a display mode change, so macOS leaves its 2x HiDPI mode for a native 1x one and points equal pixels. Windowed keeps 2x. Note that SDL reports the drawable in points in both cases, because the window is not created with `SDL_WINDOW_ALLOW_HIGHDPI` and SDL does not own a GL context for it. That is why the layer, not SDL, is asked for the size.

**The PBO path was dropped, on measurement.** #2991's 40 to 55 ms per frame does not reproduce. At 2400x1600 the present costs about 1.9 ms of real work:

| segment | cost |
|---|---|
| acquiring the IOSurface (waiting for the last present) | 0.01 ms |
| waiting for the GPU to finish the frame | 6.0 ms |
| the readback copy itself, 15.4 MB | 1.4 ms |
| Metal encode and present | 0.5 ms |

The 6 ms is not the readback's cost, it is the frame being drawn, and nothing on the present side removes it. A double-buffered PBO measured 0.7 ms per frame *worse* than reading straight into the IOSurface: `glReadPixels` into a PBO still blocks on this driver, so the PBO buys no run-ahead and only adds a 15 MB copy out of the mapped buffer. Total 8.05 ms against 7.10 ms. Double-buffering the IOSurface was dropped for the same reason: the acquire it would decouple costs 0.01 ms.

If a driver ever makes `glReadPixels` into a PBO asynchronous, revisit. Until then the PBO is cost without benefit.

Reviewer check: **this is the first layer with a visible result.** The main menu draws. Resizing is S6, the pbuffer does not track the window yet.

Deliberately not in this layer: the downsample and non-Retina knobs, the four `SPRING_MAC_*` environment variables and the inline timing harness from #2991. Also `SPRING_FRAME_CAPTURE` and `SPRING_MAC_DUMP_FRAME`, which belong in S8 as CI instrumentation, and one of which is more useful as an engine-wide feature than a macOS one.

---

**The Zink abort, diagnosed, and no longer reproducing**

Found during S2, chased during S3. **It has not been seen since.** On 2026-08-02 it did not fire in eight menu runs, including five instances at once, nor in a three minute game to frame 4668, nor in the user's own longer session. The claim below that it "will block playing anything" is now contradicted by evidence. Treat it as a rare event of unknown cause rather than a blocker, and stop planning around it until it comes back.

The engine takes SIGABRT on a Zink threaded-context worker somewhere between 20 and 60 seconds in, and sometimes not at all in a minute. The abort is inside Metal, not inside Zink:

```
abort
IOGPU        IOGPUMetalCommandBufferStorageAllocResourceAtIndex
AGXMetalG13X AGX::ContextCommon<...>::newCommand(unsigned long, bool)
AGXMetalG13X ...beginComputePass / computeCommandEncoder
kosmickrisp  mtl_new_compute_command_encoder / cs_get_compute / kk_CmdCopyBuffer2
kosmickrisp  vk_common_CmdCopyBuffer / kk_cmd_tramp_CmdCopyBuffer
libgallium   zink_copy_buffer / zink_resource_copy_region / tc_batch_execute
libgallium   util_queue_thread_func
```

So Metal fails to allocate storage for a command buffer and aborts the process rather than returning an error. At the point of the abort the process holds 990 MB of `IOAccelerator` across 1710 regions.

What is ruled out: the watchdog, the pbuffer size mismatch, and the present path. It happens with the present path wired and unwired, and it happened before any present existed. `zink_copy_buffer` is ordinary buffer traffic, not the readback.

What is not yet known: whether the 1710 regions grow without bound, which would make this a resource leak in Zink or KosmicKrisp rather than a burst. The next step is to sample the `IOAccelerator` region count over time and see whether it climbs. If it climbs, the bug is upstream and the pin has to move. If it plateaus, the engine is asking for more GPU allocations than the driver can hold, and the engine side is worth looking at.

Getting past it is not an engine change on current evidence.

---

**S4. Input coordinate space. DONE**

Purpose: SDL reports mouse events in logical points. After S3 the viewport is in backing pixels. Without scaling the cursor reads half its real height, so you have to aim below a button to hover it and the error grows down the screen.

Branch `macos/renderer-s4-input`, two commits off S3.

- `852884ce73` the scale. `CGlobalRendering::PointToPixel` and `PixelToPoint`, backed by `pixelsPerPointX/Y` computed in `ReadWindowPosAndSize` as the ratio of the pbuffer to `SDL_GetWindowSize`. The ratio, not `backingScaleFactor`: exclusive fullscreen drops the display to 1x where the two sizes agree, so the ratio is right in both modes and the backing scale is not.
- `96713bd736` the conversion, in `InputHandler::PushEvents`, plus the two paths that are not events.

**It is not `MouseInput.cpp`, which is where the old plan and #2991 both put it.** The main menu is aGui, and aGui elements register their own `InputHandler` handler and read `ev.button.x` and `ev.motion.x` directly (`Button.cpp`, `List.cpp`, `Window.cpp`), hit-testing against `GuiElement::screensize`, which `SpringApp` sets from `viewSize`. RmlUi does the same against `winSize`. Converting in `MouseInput` alone fixes the game and leaves the menu broken, which is the stated reviewer check. Converting where events enter the engine covers all three consumers in one place, with no `#ifdef` anywhere in the event handling.

Two mouse reads do not arrive as events and are converted on their own: `SDL_GetMouseState` in the wheel branch of `aGui/List.cpp`, and `WarpPos`, which hands a position back to SDL and so converts the other way.

Verified on screen. Windowed: hover lands on the button under the cursor, clicking Edit Settings opens the settings list, and the wheel scrolls that list with the row under the pointer highlighted. Fullscreen: confirmed by hand.

**Testing this without a human needs care.** `CGWarpMouseCursorPosition` teleports the pointer without delivering a motion event, so SDL keeps reporting the old position until something really moves. A synthetic click after a warp lands wherever the pointer was before. Post an explicit `.mouseMoved` after the warp, or the harness will tell you the coordinate space is broken when it is not.

**Exclusive fullscreen makes the pointer feel slow, and that is not this layer.** The mode change takes the desktop from a 1512x982 point space to a native 3024x1964 one, and macOS moves the pointer in points, so the same hand movement crosses half the screen. Nothing in the engine can fix it. Borderless fullscreen avoids it by not changing the mode, which is S6b.

---

**S4b. Render resolution knob. DONE**

Purpose: let a machine draw at the window's point size instead of its backing size, trading sharpness for a large saving on the S3 present path.

Branch `macos/renderer-s4b-resolution`, one commit off S4. `b2cec98be7`.

`CONFIG(bool, MacHiDPIRendering)`, default on, reaching `MacMetalPresent_Init` as an argument. It sets `layer.contentsScale` rather than `layer.drawableSize`, because AppKit derives the drawable from the scale when it lays a hosted layer out, so a drawable set behind its back would not survive a resize. Nothing else in the size contract changes: the pbuffer follows the layer, `winSize` follows the pbuffer, and `pixelsPerPoint` becomes 1 so S4's conversion goes back to the identity.

Measured with it off, windowed at 1210x800: `1210x800 points at 1.0x, 1210x800 drawable`, `winSize=<1210,800>`, the menu visibly upscaled, and hover still lands on the button under the cursor. The saving itself is arithmetic, not measured: the readback drops from 15.4 MB to 3.8 MB per frame, so roughly 1.4 ms to 0.35 ms at S3's measured rate, and the fragment work drops to a quarter.

The trap to avoid is treating this as a fix for the fullscreen pointer speed. It is not: exclusive fullscreen is already 1x, so the knob only makes it blurrier. What it buys is making borderless fullscreen affordable, since borderless keeps the 2x desktop and would otherwise always pay for the full backing resolution.

This is #2991's non-Retina knob, deliberately dropped from S3 and taken up here on its own merits.

---

**The first real game run, 2026-08-02**

A game runs. SplinterFaction 0.1.78 on AcidicQuarry, single player, reached frame 4668 at 31 to 38 fps. Terrain, detail textures, normal mapping, unit models, the minimap, the build menu, icons and every UI panel all draw correctly. This is the first evidence that the port is more than a menu.

**The data dir matters more than anything else here.** Dropping `--isolation` lets `~/.spring` supply `base/springcontent.sdz`, which on this machine is from January 2025 and is missing the shaders this engine asks for: `SMFShadingTextureVertProg.glsl`, `Icons2DVS.glsl`, `IconsFS.glsl` and `groundDecals.lua`. The ground shading shader then fails to validate and the terrain draws black. That is a stale content problem and not a renderer one, and it wasted the first run. Use `--isolation-dir ~/dev/spring-testdata`, which holds this build's `base/`, the repo's fonts and LuaUI, and symlinks to one game and one map.

**`fillModeNonSolid` is the second capability lie, and it is worse than the geometry-shader one.** Mesa says it plainly at startup:

```
MESA: warning: Incorrect rendering will happen because the Vulkan device
doesn't support the 'fillModeNonSolid' feature
```

Metal has no wireframe polygon fill mode, so `VK_POLYGON_MODE_LINE` does not exist and Zink cannot honour `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)`. Every outline the engine draws that way comes out as a filled triangle instead. On screen that is large black and grey wedges over the terrain, red triangles over the minimap, and filled polygons over the resource bar.

There are 35 `glPolygonMode` call sites outside `rts/lib`. The ones that hurt are `Game/UI/StartPosSelecter.cpp` (the placement phase), `Game/UI/GuiHandler.cpp` (build placement ghosts), `Game/SelectedUnitsHandler.cpp`, `Rendering/CommandDrawer.cpp` and `Rendering/Units/UnitDrawer.cpp`. `Lua/LuaOpenGL.cpp` passes the mode straight through from Lua, so game content can reach it too.

Unlike the geometry-shader case this one **is** detectable, because the driver knows and says so, and a filled triangle where an outline belongs is far more visible than a missing effect. Worth its own layer.

**Done, branch `macos/renderer-polygonmode-line`, three commits off `macos/renderer-borderless`.** The capability has no GL query, so `CGlobalRendering::ProbePolygonModeLine` measures it: rasterise a triangle into a 32x32 FBO in `GL_LINE` mode and count lit pixels, since a driver that filled it lights about four times as many. The result is `supportPolygonModeLine`, logged next to the other capability flags and exposed as `Platform.glSupportPolygonModeLine`. The rectangle-outline sites (`CommandDrawer`, `GuiHandler`'s option LEDs, `StartPosSelecter`'s ready box, `SelectedUnitsHandler`) now draw their four edges as line primitives on every platform rather than asking for a filled rect in line mode, which also fixes `StartPosSelecter` drawing a second filled quad over the first everywhere. The nanoframe wireframe stage is skipped when the capability is absent, because a solid model there reads as a finished unit, and the GL4 path submits its index list as `GL_LINES` instead. Verified on screen.

`VK_EXT_depth_clip_enable` is missing too, with the same "incorrect rendering will happen" warning and no symptom yet identified.

**One shader still fails to validate:** `FX Shader is not valid: active samplers with a different type refer to the same texture image unit`. Cause not yet chased.

**Two input problems, neither investigated:**

- The mouse cursor is not visible in game. Note that `screencapture` never records the cursor, so no screenshot can confirm or deny this. It needs a human or a different capture method.
- Cmd-Tab does not switch away. The overlay appears and focus stays put. `SDL_SetWindowGrab` through `CGlobalRendering::SetWindowInputGrabbing` is the obvious suspect, and the log shows `Input grabbing is enabled!` repeatedly, which comes from the `GrabInput` action executor, so something is issuing it.

**SplinterFaction cannot test S5.** It uses no geometry shaders at all, so a different game is needed to exercise that path.

---

**The Lua drawing artefacts, diagnosed 2026-08-02**

The stray skewed geometry over everything Lua draws is a Zink bug, reproduced standalone outside the engine. **Consecutive `glBegin`/`glEnd` batches that use different vertex formats corrupt the second format's vertices.** No GL error is raised.

`~/dev/macos-probes/fmt_probe.c` is the reproducer. On `zink Vulkan 1.4(Apple M1 Pro (MESA_KOSMICKRISP))`, Mesa 26.2.0-rc3:

| test | result |
|---|---|
| F1 to F8, vertex format varying *inside* one batch | all OK |
| G1, a run of no-texcoord batches then a run of texcoord batches | **WRONG**, 19875 stray pixels |
| G2, format alternating every single batch | OK |
| G3, the load screen's exact batch shape | **WRONG**, 715 stray pixels |
| G4, a shader draw issued after the transition | OK |
| G5, G1 again with every batch leading with a texcoord | OK |
| G6, the same with colour, texcoord and normal | OK |

**It is Zink or KosmicKrisp, not Mesa's shared immediate-mode code, and not a stale pin.** Measured properly by building `origin/staging/26.2` with `-Dgallium-drivers=zink,llvmpipe`, so one binary drives both. llvmpipe passes all fifteen, Zink on KosmicKrisp fails the same three. Same source, same build, same probe. The earlier llvmpipe comparison confounded driver with version and that caveat is now retired.

**Moving the pin does not help.** `staging/26.2`, 30 commits past rc3, fails identically. That includes `05d1d627f79 zink: Fix zink_bo_unmap synchronization for client pointer support`, which looked like a strong candidate because immediate mode is the client-pointer path. It is not this bug.

**ExaDev has not solved it either.** Their shipped release `engine-macos-arm64-2026.06.08-gaefcef0` bundles `libgallium-26.2.0-devel.dylib` with KosmicKrisp. Running the probe against their bundled Mesa fails the same three tests with byte-identical numbers. So the difference between their working build and our artefacts is not the driver.

Three independently built Mesas, from revisions weeks apart, produce identical stray-pixel counts. The defect is fully deterministic and depends only on the sequence of batches the application submits. That rules out the race theory and means the trigger is what the game draws, not what the driver was built from. Beyond All Reason evidently does not produce the triggering sequence and SplinterFaction does.

G2 passing and G1 failing is the shape of the bug. Alternating every batch is fine, a *run* of one format followed by a run of another is not. That matches Mesa accumulating consecutive same-format batches into one buffer and mishandling the boundary.

**Only the batch at the boundary is corrupted.** Measured with `empty_mod.sdz` as the game, so no content but the diagnostic load screen draws anything. Five bands, all the same 28-vertex rounded rectangle:

| band | batch | result |
|---|---|---|
| A | no texcoords, no texture | clean |
| B | texcoords, no texture | **sprawls across the top half** |
| C | texcoords, texture bound | clean |
| D | no texcoords, texture bound | clean |
| E | texcoords, no texture, identical to B | clean |

B and E are the same construct and only B is wrong, so the trigger is not "texcoords with no texture bound".

**Which batch gets corrupted is not predictable from the drawing order.** A second run, with nothing textured and nothing blended so that texcoord presence was the only variable, drew six bands in the order no, no, yes, yes, no, yes. Only band 6 was corrupted. Band 3 is also the first texcoord batch after a run without them and it is clean, which is the exact opposite of the first run, where the first such batch was the corrupted one and the later one was clean. Bands 1 to 5 draw correctly and only look fragmentary because band 6's sprawl covers them.

Two tests, contradictory under any "first boundary batch" rule. What survives is that the corruption tracks something below the API, most likely the offset or alignment a batch happens to land at inside Mesa's immediate-mode buffer, which moves as the batch count and vertex count change. That also fits the standalone results, where G1 with 18 batches per run scattered heavily, G3 with the load screen's shape scattered lightly, and G2 alternating every batch not at all.

**A sacrificial warm-up batch does not help.** One texcoord-carrying batch drawn first with `gl.ColorMask(false, ...)` leaves the picture bit for bit unchanged. There is no cheap "absorb the bad batch" mitigation.

The `empty_mod.sdz` harness is worth keeping. It has no LuaIntro and an empty `LuaUI/`, so a loose `LuaIntro/main.lua` in the data dir is the only thing drawing, and `~/dev/macos-probes/empty_script.txt` starts it. Note `gametype` must be the modinfo `name` ("Empty Mod"), not its `game` field. For bisecting a real game's Lua rather than replacing it, extract the `.sdz` to a folder ending in `.sdd` and the engine picks it up.

This is what SplinterFaction's load screen hits. `LuaIntro/Addons/main.lua` draws its panel through `RectRound`, whose `DrawRectRound` emits 28 vertices as 7 quads in one `GL.QUADS` batch, with `gl.TexCoord` on some vertices, immediately after batches that emit none.

Verified in-engine by overriding `LuaIntro/main.lua`. `CLuaIntro::LoadFile` uses `SPRING_VFS_RAW_FIRST`, so a loose `LuaIntro/main.lua` in the data dir beats the game archive. That is the cheapest way to get a controlled Lua drawing test inside the real engine with no rebuild. Escalating from a plain rounded rect to the game's exact construct, the band that first emits texcoords after a band that did not is the one that sprawls. The bands after it, which keep the same format, draw clean.

**Normalising the vertex format is not the fix. That reading was wrong, and it is now disproven.** G5 and G6 pass, but they pass by accident of their batch shape, not because a uniform format avoids the bug. Run against batches shaped the way `gl.Shape` and `gl.BeginEnd` really produce them, every normalising variant is *worse* than doing nothing: re-emitting the current colour, texcoord and normal before `glBegin`, after `glBegin`, with fixed values, or with a pinned vertex arity all fail 16 of 17 sweep positions against naive's 15, and they start failing with no preceding batches at all. That is the standalone reproduction of the full-screen rainbow smear the previous engine attempt produced.

**Fixed, 2026-08-03. See "The immediate-mode batching bug" below.**

Seen in game as well as on the load screen: on the start position screen the top-left resource panel draws as a skewed parallelogram with fine hatching, and a purple band of the same hatching runs from it down the full height of the map.

**The invisible cursor is NOT explained by this.** G4 shows a shader draw after the transition is unaffected, and `CMouseCursor::Draw` uses a render buffer and a real shader, not immediate mode. Still open.

Dead ends, all tested this session, do not redo:

- **glthread.** Zink turns it on by default (`driinfo_zink.h` has `DRI_CONF_MESA_GLTHREAD_DRIVER(true)` against gallium's `false`), so it looked promising. It is not on here. `mesa_glthread=false` produces no `ATTENTION: default value ... overridden by environment` line, which Mesa prints only when the override changes the value. The artefact is identical either way.
- **Vertex format varying inside a batch**, which is what `gl.Shape` and `gl.BeginEnd` produce. F1 to F7 all pass.
- **`gl_ModelViewProjectionMatrix` going stale after immediate mode**, the obvious cursor theory. F8 passes.
- **The screen matrices.** `UpdateScreenMatrices` logs `vpx=60, vpy=-804, vsx=2760, vsy=1720, ssx=1512, ssy=982`. The desktop size is in points while the viewport is in backing pixels, so the `ssy >= wsy` invariant on line 1781 is violated and `vpy` goes negative. The arithmetic still cancels out and the projection is self-consistent, so it is not the cause of this. It is worth tidying anyway.

A trap that cost four runs: `Intro:DrawLoadScreen` inherits a **0..1 ortho**, not pixel space. `LuaOpenGL::EnableCommon` does not call `SetupScreenMatrices`. The game's own load screen compensates with `gl.PushMatrix()` then `gl.Scale(1/vsx, 1/vsy, 1)`. A test that draws in pixel coordinates lands far off screen and looks exactly like "Lua draws nothing".

---

**The immediate-mode batching bug, root-caused and fixed 2026-08-03**

The Lua drawing artefacts are fixed. Verified by A/B on one binary against the six-band `empty_mod` harness: with the mitigation on all six bands draw as clean rounded rectangles, with it forced off by config the cyan band sprawls skewed across the whole screen. Both images are phase dumps of the same frame of the same run configuration.

**What it is.** Mesa's `vbo_exec` accumulates consecutive `glBegin`/`glEnd` batches into one persistently mapped buffer and issues them together. Zink on KosmicKrisp renders that wrongly once the buffer already holds another batch and a later batch widens its vertex format part way through, which is exactly what `gl.Shape` and `gl.BeginEnd` produce. No GL error is raised.

**The measurement that settles it.** `src/mesa/vbo/vbo_exec_draw.c` was instrumented to print the buffer offset, stride, vertex size, primitive count and attribute layout of every immediate-mode draw. Two runs of the same probe, one on Zink and one on llvmpipe out of the same Mesa binary, produce a **byte-identical** stream of draws. llvmpipe renders it correctly and Zink does not. So Mesa's state tracker output is not at fault and the defect is wholly inside zink or kosmickrisp.

**What fixes it, and what does not.** Only `glFlush()` immediately *before* `glBegin` works, 17 of 17 sweep positions clean. Everything else fails:

| candidate | result |
|---|---|
| `glFlush()` before `glBegin` | **clean, 17 of 17** |
| `glFlush()` after `glEnd` | fails, same as naive |
| a blend enable/disable before `glBegin` | fails, same as naive |
| normalising the vertex format, four variants | worse than naive |
| a matrix push/pop before `glBegin` | fails, and never flushed anything |
| disabling `ARB_buffer_storage`, `ZINK_DEBUG=sync`, `ZINK_DEBUG=noreorder` | no effect |

The blend toggle is the important one. It makes Mesa issue the accumulated batches, so the trace shows the **same 38 draws as the `glFlush` case, byte for byte**, and it is still wrong. So the fix is not emptying Mesa's buffer. It is submitting the command stream. That points at a hazard on the reused vertex buffer inside the driver rather than anything about vertex layout.

**The engine change.** `CGlobalRendering::ProbeImmediateModeBatching()` reproduces the defect at startup and reports `18658 stray, 13071 unlit`, which is byte-identical to the standalone reproducer. So detection is not impossible after all: the previous probe simply did not reproduce the triggering shape. `supportImmediateModeBatching` is logged with the other capability flags. `glBeginBatch()` in `myGL.h` wraps `glBegin` and submits first when the flag is false, and all 46 non-`rts/lib` `glBegin` call sites now go through it. `ForceImmediateModeFlush` overrides the probe for A/B testing, -1 measures, 0 never submits, 1 always does.

Still open on this: **the cost has not been measured.** `GuiHandler` alone has 28 call sites, so a busy game frame may pay a lot of command-buffer submits. Measure before deciding whether this ships as is or whether `LuaOpenGL` should stop using immediate mode altogether, which is the real fix.

Also still open: the Mesa working tree at `~/dev/mesa` carries the uncommitted `VBO_TRACE` instrumentation in `vbo_exec_draw.c`. Revert it once the upstream bug report is written. `~/dev/macos-probes/off_probe.c` is the reproducer and holds all the candidate mitigations.

---

**What is left in game, and a measurement hazard that invalidates clean dumps**

The load screen is fixed and the in-game artefacts are not. Four things are established about what remains.

**The present path is innocent.** A dump taken at the top of `SwapBuffers` is byte identical to the last dump inside `CGame::Draw`. Whatever is wrong is wrong in the framebuffer before anything Metal touches it.

**The artefact is drawn in the input-receivers phase.** Counting strongly red pixels across phases in a frame that showed the band: 28 at `1-world`, 42 after `CInputReceiver::DrawReceivers()`, then 76030 after `luaInputReceiver->Draw()`. So LuaUI's `DrawScreen` draws it, not the engine's own receivers.

**Two attractive theories are dead, both disproved standalone before touching engine code.** Display lists render correctly on Zink, so `gl.CreateList` plus `gl.CallList` is not it even though the resource bar uses them. `glRectf`, which `gl.Rect` calls and which Mesa expands into `glBegin` internally where the engine's wrapper cannot reach, also renders correctly.

**The instrumentation suppresses the bug.** Two runs of the same binary and the same script, differing only by one extra `DumpFramePhase` call, gave 76030 red pixels in one and at most 2015 across eight sampled frames in the other. A phase dump is a full-framebuffer `glReadPixels`, which forces a command-stream submission, which is exactly the thing that fixes the bug. So **a clean phase dump is not evidence of absence**, and any conclusion drawn from one needs re-checking. This is the likeliest reason earlier dumps looked clean while the screen did not.

That last point sets the method for whoever picks this up. Do not sample eight phases per frame while hunting this. Dump only `7-presented`, which sits next to the present path's own full-framebuffer readback and so costs almost nothing. `SPRING_PHASE_DUMP` now takes a label substring for exactly this.

**A repeatable automated harness now exists.** Three pieces, all in the throwaway SplinterFaction copy at `~/dev/spring-testdata/games/SplinterFaction_0.1.78.sdz`, which is a real copy rather than a symlink so the pristine archive in `~/.spring/games/` stays clean:

- `LuaRules/Gadgets/game_spawn.lua` has its two pre-game deadlines cut from 900 frames to 30. The gadget already force-assigns on timeout, so no new logic was needed. Without this the faction and placement gates eat the first 60 seconds and units never spawn.
- `LuaUI/Widgets/zzz_autoselect.lua` keeps every unit selected. The artefact is far stronger when a selection exists, and this removes the human from the loop.
- `LuaUI/Config/SF_data.lua` `order = 0` disables a widget, so subsets can be driven from a script.

Peak count of strongly red pixels across dumped frames is the detector. A clean frame reads in the hundreds, the band in the hundreds of thousands.

**The artefact scales with how much is drawn, so there is no single culprit widget.**

| widgets enabled | peak red |
|---|---|
| resource bar only | 387 |
| build and order menu only | 18402 |
| four selection-driven | 233357 |
| twelve static | 515424 |

**The mitigation works in game, and is not complete.** Same binary, all 131 widgets, only `ForceImmediateModeFlush` differing:

| | peak red | per frame |
|---|---|---|
| forced on | 19604 | 19604, 9145, 333, 393 |
| forced off | 444902 | 364, 423, 444902, 305372, 288783 |

A 23x reduction in the peak and most frames clean. The load screen also regresses visibly when it is forced off, which independently confirms the override works.

**The residual is the same class of bug.** The band appears at a different depth in the draw order from frame to frame, sometimes above the bottom-left button grid, sometimes between the buttons and their backing panel, sometimes behind all of it. That is what a corrupted batch looks like: it lands wherever that batch sat in the sequence, and which batch is corrupted changes.

The resource bar has its own separate corruption, visible with that widget alone enabled while the red band is absent. It is a different symptom and has not been characterised.

**The shared code is `RectRound`, and it explains the split.** `gui_static_resourcebar.lua` builds its panels through `DrawPanel`, which is two `RectRound` calls plus a `TexRect` accent strip from `LuaUI/Images/staticgui_accent.png`. `RectRound` is `gl.BeginEnd(GL.QUADS, DrawRectRound, ...)`, the same 28-vertex construct the load screen uses. Eleven widgets share it verbatim and 47 use display lists.

The difference is where it runs. The load screen calls it live every frame, so `glBeginBatch` applies and it is fixed. The widgets call it inside `gl.CreateList`, so `gl.BeginEnd` executes at compile time where the flush is useless, and the geometry is replayed later by `glCallList` where nothing flushed.

`glCallListBatch` was added to close that, flushing before `glCallList` in `LuaOpenGL::CallList`. **The result is not a clear win and should not be reported as one.** Peak red 21569 with four of five frames at baseline, against 19604 with two of four before. The sample is too small to separate. It needs a longer run or repeated runs before anyone claims it helps.

Two leads not yet followed. The mitigation may simply be insufficient in a frame with textures, blending and hundreds of batches, which the standalone sweep never modelled. And no probe has yet reproduced a display list replayed among many live immediate-mode batches, which is the case that matters here and is not the case the earlier list probe tested.

**The cursor is not a failed cursor replacement.** `gui_customcursorsets.lua` calls `Spring.ReplaceMouseCursor` for 23 cursor names and ignores every return value, which looked like a strong candidate. It is not: a run with a warning wired into `CMouseHandler::ReplaceMouseCursor` logged no failures at all.

Worth knowing anyway, and separable from macOS: `ReplaceMouseCursor` swaps the new cursor in before checking it, so a replacement that loads no frames destroys the working cursor and leaves one that draws nothing. The function returns validity as a bool, which suggests non-destructive failure was intended. Refusing the swap is a behaviour change though, since content could rely on it to hide the cursor deliberately, so this is left as a warning only and the behaviour question is for upstream.

---

**The invisible cursor is not an engine bug, on current evidence**

Every candidate is ruled out by measurement rather than reading. `HardwareCursorApple::IsValid()` returns false unconditionally, so macOS always takes the software path and `SetCursor` hides the OS cursor, which is why nothing else can be covering for it. Instrumenting `CMouseCursor::Draw` in a real run gives a wholly healthy draw: `frames=1`, a valid texture, a valid shader, `viewSize=<2760,1720>`, and matrix params that put a 32x32 quad exactly at the pointer.

Reading the framebuffer either side of `rb.Submit` shows the draw changing 185 pixels of a 48x48 box with no GL error, and the phase dumps confirm it survives to the presented frame: `5-cursor` differs from `4-rml` by a 28x28 block at the pointer, and `6-screenpost` is identical to `5-cursor`.

So under `empty_mod` the cursor is drawn correctly and reaches the screen. The next step is the same instrumented run under SplinterFaction, since the report came from there and game content is the only remaining difference. Do not re-derive any of the above.

---

**S5. Geometry-shader capability guard, not upstreamable, see section 3**

Purpose: Zink advertises geometry shader support it does not have, because KosmicKrisp reports `geometryShader = false` (Metal has no geometry stage) while `GL_MAX_GEOMETRY_OUTPUT_VERTICES` still returns a positive number. Any GL query for the capability lies, so shader programs with a geometry stage link and then misbehave.

Touches: `rts/Lua/LuaShaders.cpp` `CreateShader`, and `rts/Rendering/Shaders/LuaShaderContainer.cpp`, which also compiles `GL_GEOMETRY_SHADER` objects for engine-side Lua-defined shaders. #2991 only handled the first. The second is a real gap.

Reviewer check: with a geometry-shader widget loaded, the engine logs the strip and keeps rendering instead of producing garbage.

Risk: high, and mostly outside the engine. Stripping the geometry stage does not make a shader correct, it makes it link. Widgets that expand points into quads need a Lua-side no-geometry-shader path, which lives in Beyond All Reason content, not here. Until that content exists this layer buys a running renderer with visibly wrong effects.

---

**S6. Resize. DONE**

Purpose: the pbuffer is a fixed-size allocation, so a window resize must recreate it.

Branch `macos/renderer-s6-resize`, two commits off S4b.

- `acb5734abd` keeps the layer's drawable in step. **S3's comment was wrong about this.** Hosting the layer does get AppKit to resize it, but a CAMetalLayer sizes its drawable from its bounds only until something sets the drawable itself, and S3 has to set it before AppKit's first layout. So the drawable was stuck at the opening size forever. `MacMetalPresent_GetDrawableSize` now recomputes it from the layer's bounds and scale.
- `6a65b5b53f` swaps the pbuffer. `MacEGL::ResizeSurface` creates a new one, makes it current and destroys the old, keeping the context and everything in it. It is called from `ReadWindowPosAndSize`, so one place decides what the framebuffer is and `pixelsPerPointX/Y` stays consistent for free.

Verified: a hand resize logs `framebuffer is now 2274x800`, `winSize` follows, the menu re-lays out for the new aspect ratio rather than stretching, and hover still lands on the button under the cursor.

**The shutdown half was already done.** `DestroyWindowAndContext` has called `MacEGL::DestroyContext` since S1, and that does `eglMakeCurrent(none)`, `eglDestroyContext`, `eglDestroySurface` and `eglTerminate`. The plan called it missing on the strength of ExaDev's `c709dc2a6`, which fixes it on a branch that did not have it.

Still open here, not done:

- Repeated resizes are unproven. One resize works. Nobody has dragged continuously and watched for leaks, and every resize allocates a new pbuffer.

**A testing note that cost an hour.** `run-macos.sh` ends in `exec ./spring`, so the process appears in `ps` as `./spring --isolation` with no path in it. `pgrep -f build-macos-legacy/spring` matches nothing, which reads as "the engine died" while instances quietly pile up and later tests drive whichever window happens to be on top. Match on `./spring` and count them. Related: synthetic `CGEvent` drags cannot resize a window, because the window server ignores posted events on the frame, so that step needs a human.

---

**S6b. Borderless fullscreen. DONE**

Purpose: be full screen without the display mode change, so the desktop keeps its 2x scale and the pointer keeps its speed.

Branch `macos/renderer-borderless`, two commits off S6: the I4 cherry-pick, then `1291d29b33`.

**Most of this already worked, which is why the layer is one guard and not a feature.** Measured on this tree before writing anything:

| config | what happens today |
|---|---|
| `Fullscreen=0, WindowBorderless=1` | borderless window over the usable screen, 2x kept, menu bar visible |
| `Fullscreen=1, WindowBorderless=1` | macOS fullscreen Space, 3024x1898 drawable at 2x, no mode change |
| `Fullscreen=1, WindowBorderless=0`, the shipped default | exclusive, mode change, desktop drops to 1x |

So the gap was only that the default asks SDL for the mode-changing kind. `GetFullScreenFlag` returns `SDL_WINDOW_FULLSCREEN_DESKTOP` unconditionally under `__APPLE__`, used by `CreateSDLWindow`, `SetWindowAttributes` and `LogDisplayMode`. The log line follows the flag asked for, so it stops claiming `fullscreen::exclusive` when it is not.

Verified on the default config: fills the screen, `fullscreen::non-exclusive` at 3024x1898, hover highlights the button under the cursor, a click opens the settings list, and a 100 point screen region still captures as 200 pixels while the engine runs, so the desktop is still at 2x and nothing else on it moved.

Two things that look like defects and are not, both confirmed with the user:

- The top 33 points stay black. That is the menu bar and notch area, which macOS reserves. Filling it is not wanted.
- SDL's fullscreen-desktop puts the window in its own Space. That is the macOS convention and is wanted. `SDL_VIDEO_MAC_FULLSCREEN_SPACES=0` does avoid it, and does then cover the full 982 points, but it is the wrong behaviour for a Mac app.

A measurement worth reusing: `screencapture -R x,y,100,100` and then reading the PNG's pixel size tells you the desktop's scale factor without looking at anyone's windows. 200x200 means 2x, 100x100 means a mode change happened.

`SaveWindowPosAndSize` returns early when `fullScreen`, so none of this writes anything back to the resolution config.

---

**S7. Packaging**

Purpose: a user who is not building Mesa from source can run this.

Touches: a new `.app` bundle assembly, `install_name_tool` relocation of the Mesa and Vulkan dylibs into the bundle, ICD manifests rewritten to bundle-relative paths, the runtime environment (`MESA_LOADER_DRIVER_OVERRIDE=zink`, `EGL_PLATFORM=surfaceless`, `VK_DRIVER_FILES`), and code signing.

Reviewer check: the `.app` runs on a machine with no Homebrew.

Risk: high, and largely non-technical.

- Licensing and distribution. Shipping Mesa artefacts is a decision for the project, not a build detail.
- The pinned Mesa from S0 is not a release, so someone has to own rebuilding it.
- `DYLD_LIBRARY_PATH` is the obvious way to point Zink at `libvulkan.1.dylib`, and it is the wrong way. It is stripped for signed and hardened binaries, and ExaDev hit a SIGBUS that they fixed by removing it. Use `@rpath` and `install_name_tool` instead.
- Gatekeeper. ExaDev's `zink-probe` branch works around this with a self-signed certificate added as a trusted root, which is fine for CI and not fine for users.

ExaDev's `zink-probe` branch is the best available reference for the mechanics: bundle assembly, dylib install-name rewriting, DMG creation, and a smoke test. Read it, do not fork it.

---

**S8. CI**

Purpose: keep the renderer from silently rotting.

Touches: the macOS workflow from #3134. Add fontconfig, add `engine-legacy` to the target list, and add a headless run that asserts a non-blank frame.

Reviewer check: the workflow goes green and fails if the engine stops drawing.

Risk: medium. GitHub's `macos-26` arm64 runners are real hardware, and ExaDev's `zink-probe` ran there successfully on 2026-06-13 (run 27460811690, all steps green). But **the logs have expired and that workflow prints `GL_RENDERER` without asserting on it**, so a green run does not prove it got hardware rather than llvmpipe. Re-run the probe before designing anything around CI hardware rendering.

## 3. What cannot go upstream

**S5, the geometry-shader guard.** Three reasons, and they compound.

The capability cannot be detected. Zink advertises geometry shader support that KosmicKrisp cannot deliver, and `GL_MAX_GEOMETRY_OUTPUT_VERTICES` returns a positive value regardless. A platform-neutral PR would need a runtime probe that does not exist. What is left is "if `__APPLE__`, strip the geometry stage", which is a hardcoded platform assumption in shared shader code. A reviewer would be right to reject it.

Stripping is not fixing. A shader whose geometry stage is removed links and draws the wrong thing. Making it correct needs no-geometry-shader variants of the affected widgets, which live in Beyond All Reason content. The engine change alone has no defensible standalone benefit.

The lie may not be permanent. It is a gap in Zink and KosmicKrisp, both under active development. Upstreaming a workaround for someone else's temporary bug commits Recoil to carrying it after the bug is gone.

Keep it on the integration branch, clearly commented as a workaround for a specific Mesa and KosmicKrisp state, with the version it was observed against.

The nearest thing to a defensible upstream change is unrelated to macOS. `LuaShaderContainer.cpp` and `LuaShaders.cpp` compile geometry shaders through two separate code paths. Unifying that is a real cleanup and could go upstream on its own merits, which would then give S5 one place to guard instead of two. Worth doing first if you want to minimise the unupstreamable surface.

## 4. Made redundant by upstream

Do not port these.

**Already merged upstream.** glad GLX loader guard (#3071), SpringApp X11 include guard (#3065), legacy X11 non-Apple guard (#3045), SDL 1.2 SDLMain removal (#3034), CUtils scandir const (#3047), smmalloc INLINE leak (#3026), SafeUtil includes (#3025), SolLua `sol::lua_nil` (#3024), LuaTextures failure logging (#3023), `<algorithm>` includes for libc++ (#3074), archive narrowing and aggregate init (#3076), LuaMenu LuaSocket from base VFS (#3078), MemPoolTypes thread-id cast (#3040), float3 streflop include (#3028).

**Redundant under Homebrew GCC.** The entire AppleClang and libc++ tail of #2991: `Cpp23Compat.hpp`, the assimp `.inl` edits, `smmalloc_generic.cpp`, `float3.h`, `MemPoolTypes.h`, `testMutex.cpp`, the gflags dual-namespace hack, and `GLTFParser.cpp`'s `INLINE` undef. Proof: `engine-legacy` builds with none of them applied. The same applies to ExaDev's `macos-layer` build fixes, which target AppleClang on macos-15.

**Redundant given current master.** #2991 wraps `glxHandler.{h,cpp}` in `#ifndef __APPLE__` and guards `GetVideoMemInfoMESA` in `myGL.cpp`. Master's `glxHandler.cpp` already compiles to stubs on Apple and glad no longer builds `glad_glx.c` there, so both are unnecessary, again proved by the successful build. It also double-nests `#ifndef __APPLE__` around `GLX::Load` and `GLX::Unload` twice, which is a merge artefact.

**Superseded.** The vendored OpenAL EFX headers under `include/Mac/AL/` are obsoleted by #3072, which uses openal-soft.

**Not applicable.** Everything in #3060's `rts/System/SDLCompat/` shim directory, 18 files existing to present an SDL2 surface over SDL3. sdl2-compat already does that, better and outside our tree. `FindMesaEGL.cmake` and `FindSDL3.cmake` from that PR hardcode `/Users/yeojun/Desktop/...`.

**Reclassified, not redundant.** #2991's HiDPI window-size persistence fix. It went upstream as #3021 and was correctly closed as a no-op on master. It becomes a genuine fix once S3 lands, so it moves into the stack rather than being dropped.

## 5. Open questions

1. **Is a forced GL version acceptable?** S0 passes only with `MESA_GL_VERSION_OVERRIDE=4.6`. Native is 2.1. This is not a macOS quirk we can guard around, it is the engine being told a capability set that is partly untrue, with failures landing at draw time. It may still be the right call, because the alternative is waiting for Zink and KosmicKrisp to close the gap honestly. But it should be a decision someone makes on purpose, not a side effect of an environment variable in a launcher script.

2. **Who owns the Mesa pin?** 26.2.0-rc3 works. It is a release candidate, so the pin moves to 26.2.0 final and then needs tracking. Building it is a documented 15 minute job now (see S0) but it is still a from-source dependency with five non-obvious prerequisites. If nobody wants to own it, S7 has no answer and the port is developer-only.

3. **Do we ship Mesa?** Bundling Mesa and Vulkan dylibs in a `.app` is a project-level distribution and licensing decision, not a build detail. The alternative, telling users to `brew install mesa` and pin a revision, is honest but narrows the audience sharply.

4. **Is the platform floor acceptable?** KosmicKrisp needs Metal 4, so macOS 26 and Apple Silicon. Intel Macs and anything older get nothing, ever, on this path.

5. **Who does the BAR no-geometry-shader content?** Without it S5 gives a running renderer with visibly wrong effects. That may be an acceptable first milestone or may not be, and it is not our call.

6. **Does "playable" include sound?** #3072's openal-soft path is on `macos/test-integration` and compiles, but has not been exercised with audio actually playing. If sound is in scope it needs its own verification pass.

7. **Is a readback present path acceptable to land at all?** It is a stopgap. The zero-copy path needs `VK_EXT_external_memory_metal` to grow IOSurface handle support in KosmicKrisp and a new `MESA_memory_object_metal` consumer in Zink. #2991 estimates roughly a month of upstream work. Landing the readback path means carrying it for a while. Measured in S3, the cost is smaller than #2991 made it look: 1.4 ms to copy a 2400x1600 frame and 0.5 ms to present it. The 6 ms that looks like readback is the GPU drawing the frame, which no present path avoids.

8. **Should PR #3022 be resolved first?** It has CHANGES_REQUESTED and the stack depends on it. Either land it, agree an alternative, or accept that S1 carries the change on the integration branch.

## Reference material

Read these, do not branch from them.

- PR #2991, `iamaperson000:upstream-pr-macos-apple-silicon`. 28 commits, 53 files. The architecture is sound and the commit titles are a good decomposition guide. Most of the diff is dead weight now.
- `ExaDev/RecoilEngine` branch `macos-layer`. #2991 plus real hardening: EGL shutdown, pbuffer resize, headless framework linking. Built with AppleClang.
- `ExaDev/RecoilEngine` branch `zink-probe`. Packaging and CI reference: `.app` assembly, dylib relocation, DMG, signing, smoke test.
- PR #3060, `yeojuny:codex/macos-runtime-substrate`. Different architecture (SDL3, direct present), needs a patched Mesa. Not the chosen path.
- <https://docs.mesa3d.org/drivers/kosmickrisp.html>
- <https://gist.github.com/lucamignatti/5312f5e937de2ba44256ecba6de54cc2>, the Minecraft on Zink plus KosmicKrisp writeup. The practical source on the nullDescriptor problem.
- Mesa MR 41313, makes `nullDescriptor` optional for Zink.
