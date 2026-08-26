# Making LuaOpenGL stop using glBegin

Design spec. Written 2026-08-26. Implements item 1 of [MACOS_PERFORMANCE.md](MACOS_PERFORMANCE.md#1-make-luaopengl-stop-using-glbegin).

Built on `macos/integration` and extracted to an upstream Recoil pull request afterwards. It is platform neutral. Immediate mode is slow on every driver, so this pays most on macOS without being a macOS carve-out.

## What changes

Every Lua drawing call goes through `glVertex4f` one vertex at a time, 790,748 of them a frame on Metal Factions. `gl.BeginEnd` will instead collect a block into memory and draw it with one `glDrawArrays`. Widgets see no difference.

Vertex specification stays fixed-function. That is the design, not a compromise. It is the only way to keep `glTexEnv`, `GL_LIGHTING`, fog, and the `gl_Vertex` and `gl_MultiTexCoord0` built-ins that widgets read inside `gl.UseShader`. Routing through `RenderBuffers` was the original proposal and cannot do this, because there is no way to make generic attribute data arrive as `gl_MultiTexCoord0`. That would break shipped content, which defeats the reason for the change: it reaches games nobody updates any more.

## Decisions

| Question | Decision | Rests on |
|---|---|---|
| Buffer, or keep immediate mode? | Buffer per `gl.BeginEnd` block | 74% to 86% of the frame is LuaUI |
| Which draw path? | `glVertexPointer` and `glDrawArrays` | Only path preserving fixed-function state |
| Does the driver defect follow? | No, `glDrawArrays` is clean | Measured, 60,000 pairs, with a control |
| Vertex pointer size? | Pinned at 4 floats, never varies | Varying it corrupts 30 frames of 30 |
| Mid-block state changes? | Rejected, as GL already does | Preserves current behaviour |
| Nested `gl.BeginEnd`? | No-op, joins the outer block | Matches what widgets see today |
| Display lists? | Done. Compiled again, with a flush around `glCallList` | Measured, 20 frames of 20 |
| Config gate? | `LuaImmediateModeBuffering`, default 1 | Needed for an interleaved A/B |

## The accumulator

A new `rts/Lua/LuaImmediateBatch.{h,cpp}`.

```cpp
class LuaImmediateBatch {
public:
    bool Active() const;
    void Begin(GLenum mode);
    void Vertex(float x, float y, float z, float w);
    void Color(const float* rgba);
    void Normal(float x, float y, float z);
    void TexCoord(unsigned unit, float s, float t, float r, float q);
    void SecondaryColor(float r, float g, float b);
    void FogCoord(float f);
    void EdgeFlag(bool e);
    void End();
};
```

The six `glBeginBatch` calls in `LuaOpenGL.cpp` become `Begin` and `End`. The per-vertex Lua functions append instead of emitting. `glBeginBatch` keeps its flush for the engine's own callers in `GuiHandler.cpp`, `HUDDrawer.cpp`, `HAPFSPathDrawer.cpp`, `DynWater.cpp`, `Combiner.cpp` and `GeometryBuffer.cpp`, which are a separate job.

### Storage

One `std::vector<float>` per attribute, not one interleaved struct. Fixed-function pointers each take their own base and stride, so separate arrays cost nothing, and a struct holding every attribute would carry about 120 bytes a vertex for a widget that only sets position. The vectors are members and are reused between blocks, so steady state allocates nothing.

### An attribute array is enabled only if the block touched it

A widget that never calls `gl.Color` leaves `GL_COLOR_ARRAY` disabled, and `glDrawArrays` uses the current colour. That is exactly what immediate mode does. No guessing, and nothing to pay for attributes nobody used.

### Back-fill on late activation

A widget that emits five vertices and then calls `gl.Color` needs colours for those five. On an attribute's first touch at vertex `k` above zero, read the block-start value once with `glGetFloatv` and write `k` copies.

That is the only `glGetFloatv` in the design. It fires only on that path, so the sync point stays off the hot path.

### The pointer size never varies

Position is always `glVertexPointer(4, GL_FLOAT, 0, ...)`. Every texture coordinate is always four floats.

This is not tidiness. Varying the pointer size between draws corrupts the frame on KosmicKrisp, measured at 30 frames of 30 and 553,659 stray pixels in the worst one, while changing only the pointer address stays clean. It is legal GL and the driver gets it wrong.

The arity mitigation therefore does not disappear. It changes shape, from widening 790,748 vertices a frame to setting one constant.

### Nested blocks reproduce today's outcome

Today a nested `gl.BeginEnd` hits `GL_INVALID_OPERATION` on the inner `glBegin`, its vertices join the outer primitive, and the inner `glEnd` terminates the outer one early. A depth counter that makes nested `Begin` and `End` no-ops gives the first two and fixes the third.

Raising a Lua error would be cleaner and is the wrong call. Content that currently draws something would start drawing nothing, in games that cannot be updated.

### Colour state survives the block

In real GL a colour set inside a block persists after it. The accumulator records per vertex and issues one `glColor4f` with the final value at `End`, so the state that follows the block matches, at one call rather than one a vertex.

## Mid-block state changes are rejected

`CheckDrawingEnabled` at `LuaOpenGL.cpp:1176` is a single shared inline guard called 100 times, which is the hook. It gains an inside-batch rejection, exempting the functions the GL specification allows between `glBegin` and `glEnd`: `Vertex`, `Color`, `Normal`, `TexCoord`, `MultiTexCoord`, `SecondaryColor`, `FogCoord`, `EdgeFlag`, and `Shape` and `BeginEnd` themselves.

Everything else becomes a no-op inside a block, which is what the driver already does. Without this, a widget calling `gl.Texture` between two `gl.Vertex` calls would go from having no effect to changing the texture for the whole primitive.

Cost is one branch per `gl.*` call.

~~`gl.Rect` needs no change. It goes through `glRectf` at `LuaOpenGL.cpp:2897` and was never a batch.~~ **Wrong, and it cost a session.** Never having been a batch is exactly why it broke. `glRectf` never went through `glBeginBatch`, so it never got the flush that separates batches, which went unnoticed while everything after it was also immediate mode and therefore also flushed. Once Lua drawing is buffered, an unseparated `glDrawArrays` follows it and the driver loses the draws that follow: dirty in 30 frames of 30, clean in 30 of 30 with a flush between. `gl.Rect` now emits through the accumulator like everything else, which removes the mixture rather than paying for another flush.

### Two gaps, accepted and documented

`gl.CallList` inside a block is legal GL only when the list contains only legal commands. Modelling that is not worth it, so it is rejected inside a block.

`gl.Material` mid-primitive is legal GL and cannot be expressed as one draw. It will take effect for the whole primitive rather than part of it.

Both are rare. Shipping the simplification and writing it down beats building machinery for them.

## Display lists, done 2026-08-26

Buffering did make compiled lists correct again, and the reasoning below was half right in a way worth recording.

**Right:** `glDrawArrays` is compiled into a list with its data dereferenced at compile time. Measured, including a case that overwrites the shared buffer between `glEndList` and `glCallList` to try to catch the driver holding the pointer. It does not.

**Wrong:** that this was the one thing a flush could never reach, and that the remaining risk was the build menu. Neither was the problem. Compiling a list is clean in every arrangement the probe can express. What corrupts the frame is *replaying* a list holding a textured `glDrawArrays` while live client array draws are interleaved with the replays. A `glFlush` either side of `glCallList` reaches it, 20 dirty frames of 20 without and 0 of 20 with.

The deferral is gone and lists compile by default, worth 16.3 fps against 34.1 on Splinter Faction at 1400x850. The 8192-list cap stays, because `LuaDisplayListMode = 0` still reaches the deferred path.

Full account in [MACOS_PERFORMANCE.md](MACOS_PERFORMANCE.md), including the five hypotheses that died first.

## The config gate

`LuaImmediateModeBuffering`, default 1, with 0 restoring the `glBegin` path.

This is more configurability than the work needs, and the justification is specific to this project. The measurement rules say to put both sides inside one run, because between-run drift exceeds most effects. The path choice is per block, so a runtime flag is the only way to get an interleaved A/B on a change whose entire point is a frame time number. It is also the bisect lever and the escape hatch for a change that touches every widget in every game.

Proposing its removal is a later pull request, once the buffered path has run against real games for a while.

## Order of work

Each step has a check. Do not start the next one until the check passes.

1. Write `LuaImmediateBatch` with Catch2 tests, not wired in. `End` fills a plain `BatchLayout` value saying the mode, the vertex count, and which arrays are active with their bases. A separate `Issue(const BatchLayout&)` makes the GL calls. Activation, back-fill and the pinned pointer size all live on the pure side and test without a GL context. **Check: the tests pass.**
2. Wire the six `glBeginBatch` sites behind the config. **Check: SplinterFaction starts and draws.**
3. Prove pixel identity. Frozen scene, `install-probe.sh --shots`, both paths, SplinterFaction and Metal Factions. **Check: the screenshots match.**
4. Turn off the flush and the arity widening for the Lua path, which are dead once buffering is on. **Check: the `--loops` amplifier scores 0 dirty frames.**
5. Retire the display list deferral. **Check: build menu scroll, 0 of 37 frames damaged.** **Done 2026-08-26, but not against this check.** The build menu was never the fault. What was checked instead: 0 dirty frames of 20 in the probe, and a human driving the game through the three interactions that reproduced the artefact. The frame count this step asked for was not run.
6. Measure. `SPRING_FPS_LOG=5 run-capped.sh 120`, interleaved. **Check: a frame time number with the Mesa commit beside it.**

Steps 1 and 2 are most of the code. Step 3 holds most of the risk and most of the time, because identical output across two games is a stronger claim than it sounds.

## What this rests on

- `glDrawArrays` does not merge, and varying the pointer size corrupts. Measured 2026-08-26, Mesa `56588ef0665`, `coding-agents/test-scripts/batch_merge_probe.c`. Table in [MACOS_PERFORMANCE.md](MACOS_PERFORMANCE.md#gldrawarrays-does-not-merge-tested-2026-08-26)
- LuaUI is 74% to 86% of the frame, measured on two unrelated games
- `ForceImmediateModeFlush = 0` scored 27.98 fps against 14.98. That is what the flush costs, and buffering earns it back without breaking rendering
- Recoil runs a compatibility context. `ForceCoreContext` defaults to 0 at `GlobalRendering.cpp:75`, and under a core context `gl.Vertex` is already dead, so nothing here makes that case worse

## Not decided

- ~~Whether step 5 works at all. If compiled lists still damage the build menu with buffering on, the deferral stays and the 47 list-compiling widgets stay blocked~~ **Decided 2026-08-26.** It works, and the build menu was never the obstacle. The 47 widgets are unblocked
- What the frame time actually becomes. The flush is worth 1.87x on its own, and removing 790,748 calls a frame is worth something further, but nobody has measured the sum
- Whether upstream wants the config gate kept
