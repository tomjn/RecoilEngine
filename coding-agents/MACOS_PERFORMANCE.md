# macOS renderer performance

Reference, not a narrative. Read the summary, then jump to what you need.

Last updated 2026-08-26. Companion to `MACOS_RENDERER_PLAN.md`, which holds the upstreaming decomposition and the raw investigation history.

## Contents

- [Summary](#summary) - the whole picture in one table
- [Current understanding](#current-understanding) - where the frame time goes
- [Settled: do not reopen](#settled-do-not-reopen) - closed questions and why
- [Open work, ranked](#open-work-ranked) - what is actually worth doing
- [Measurement rules](#measurement-rules) - read before running anything
- [Harnesses and traps](#harnesses-and-traps) - how to run a benchmark that means something
- [Evidence](#evidence) - the numbers behind the claims

---

## Summary

| Question | Answer | Confidence |
|---|---|---|
| Where does the frame go? | The game's LuaUI, 74% to 86% | Measured, 2 games |
| Why is LuaUI slow? | Lua drawing is real immediate mode, `glVertex4f` per vertex | Measured, 790,748 verts/frame |
| Is the present path the problem? | No, 3% | Measured twice |
| Is render scaling a way out? | No, flat 38ms penalty for any non-1:1 present | 6 runs |
| Are render pass breaks the problem? | Probably not. Model demoted | `rp` found nothing |
| Is it the engine's fault? | Mostly no. Empty Mod runs at 80 fps | Measured |
| What is the one big engine-side fix? | Nothing large is left that does not cost correctness | Measured 2026-08-26 |
| Would a buffered draw hit the same driver defect? | No. `glDrawArrays` is clean over 60,000 pairs | Measured, with a control |
| What causes the display list wedge? | Replaying a textured list beside live draws. Not compiling, not buffer reuse | Reproduced offline, 20 of 20 |
| Can compiled lists ship now? | Yes. A flush around `glCallList` fixes it. Now the default | Confirmed in game, 2026-08-26 |
| What did buffering Lua actually buy? | 5.7%, cleanly separated but small | 2 runs a side |
| What does the deferral cost? | 2.09x, and it was buying nothing. Removed | 16.3 vs 34.1 fps, same scene |

**Headline numbers**, Splinter Faction, 3024x1832, built-in display:

| Configuration | fps | frame |
|---|---|---|
| Empty Mod, same map | 80 | 12.5ms |
| Splinter Faction, LuaUI disabled | 59.6 | 16.8ms |
| Splinter Faction, as shipped | 15.9 | 62.9ms |

---

## Current understanding

- **LuaUI is the frame.** 74% on Splinter Faction, 86% on Metal Factions, measured on a scene with one unit. Two unrelated games with unrelated widget sets agree.
- **The mechanism is immediate mode.** All Lua drawing goes through `glVertex4f` one vertex at a time (`LuaOpenGL.cpp:313`, `myGL.cpp:573`). 790,748 Lua vertices a frame on Metal Factions.
- **The renderer's own floor is fine.** 2.3ms a Mpixel with no game content. The often-quoted 11.3ms a Mpixel is Splinter Faction's scene, not a property of this port.
- **The driver has a real defect on top.** KosmicKrisp mis-renders consecutive immediate-mode batches. The engine works around it with uniform vertex and texture coordinate arity, a per-batch `glFlush`, and display list deferral. That costs 1.87x and is deliberate.
- **The workarounds now cover display lists too.** 47 widgets compile drawing into display lists. A `glFlush` cannot be recorded into a list, which was long read as meaning nothing could reach a baked batch. That was wrong. The fault was never in the baked batch, it was in replaying a textured list beside live drawing, and a flush either side of `glCallList` reaches it. The deferral is gone and lists compile by default.

Everything above points at one fix, and the plan reaches it twice independently: `LuaOpenGL` should not use `glBegin` at all.

---

## Settled: do not reopen

Each of these was measured. Reopening one needs new information, not a new idea.

| Topic | Verdict | Evidence |
|---|---|---|
| Present path | 3%, not the problem | `PLAN:1096`, `:1137` |
| Render scaling, `MacRenderScale` | Dead end, flat 38ms non-1:1 penalty | `PLAN:1112`-`:1180` |
| `MacHiDPIRendering = 0` | Same dead end, different route | `PLAN:1112` |
| Raw fill rate | Driver is 1000x faster than we achieve | `PLAN:1098` |
| CPU-side drawing | CPU is idle in a draw-bound frame | `PLAN:1344` |
| Engine feature sweep | 17.0 vs 15.9 fps, inside drift | `PLAN:1330` |
| 60Hz display as a ceiling | It is not | `PLAN:1225` |
| Buffering, 1 vs 2 IOSurfaces | 15.9 vs 16.1 fps | `ab-doublebuf-*.log` |
| `ZINK_DEBUG=rp` renderpass tracking | No signal, p = 0.167 | Below, 2026-08-25 |
| Compiled display lists | **Superseded.** They ship now. The cause was found and fixed, see below | `PLAN:1299`, then 2026-08-26 |
| `ForceImmediateModeFlush = 0` | Not a lever. It is the price of correct output | Below, 2026-08-25 |
| `normalise` as a default | No. Reintroduces merging on top of the flush | `PLAN:1261` |
| `glDrawArrays` inside a display list | Clean in 10 arrangements, including a deliberately overwritten shared buffer | Below, 2026-08-26 |
| The wedge is a smear of map texture | No. It is a UI rectangle with a flung vertex | Below, 2026-08-26 |

### The two that keep getting reopened

**`ForceImmediateModeFlush = 0` is not a performance option.** It is a capability probe override. Setting it to 0 asserts the driver renders batches correctly, which is false on KosmicKrisp. It turns off five things at once:

| Site | Mitigation | Defect it fixes |
|---|---|---|
| `LuaOpenGL.cpp:319` | vertex arity widening | load screen corruption |
| `LuaOpenGL.cpp:273` | texcoord arity widening | resource bar |
| `LuaOpenGL.cpp:295` | multi-texcoord arity widening | same family |
| `myGL.cpp:570` | per-batch `glFlush` | consecutive batches merging |
| `LuaOpenGL::CallList` | `glFlush` either side of `glCallList` | textured list replay corrupting live draws |

Worth 1.87x and 1.65 GB. Also reverts every artefact this project has fixed.

**Compiled display lists cannot ship**, tested in isolation with arity and flush still on:

| Configuration | Frames with build menu content destroyed |
|---|---|
| compiled, `-` | 15 of 35, and 5 of 36 |
| compiled, `normalise` | 6 of 36 |
| compiled, `listflush` | 10 of 37 |
| **deferred, what ships** | **0 of 37** |

Only visible while the menu scrolls, because the widget recompiles on each scroll step. A frozen scene reads as clean.

---

## Open work, ranked

### 1. Make `LuaOpenGL` stop using `glBegin`

**Started 2026-08-26.** The plan concludes it twice (`PLAN:621`, `myGL.cpp:567`) and nobody had acted.

Buffer each `gl.BeginEnd` block and replay it as one `glDrawArrays`, fed by `glVertexPointer` and friends the way `rts/Rendering/GL/VertexArray.cpp` already does. Widgets observe no change.

Vertex specification stays fixed-function, which is the point rather than a compromise. It is the only way to preserve `glTexEnv`, `GL_LIGHTING`, fog, and the `gl_Vertex` and `gl_MultiTexCoord0` built-ins that `gl.UseShader` widgets read. A `RenderBuffers` port cannot feed generic attribute data to `gl_MultiTexCoord0`, so it would break existing content, which defeats the reason for doing this at all.

What it buys:

- Every existing widget in every existing game, with no game-side change and no upstreaming to N games
- Retires all three mitigations, plus the probe. No immediate-mode batches means nothing to merge
- Reaches inside display lists, which no flush ever can, unblocking the 47 widgets that compile them
- Attacks 74% to 86% of the frame at its mechanism

Why it is credible: platform-neutral. Immediate mode is slow on every driver, so this is a general Recoil improvement that pays most on macOS, not a platform carve-out. It deletes the macOS delta rather than adding to it.

Scope: `LuaOpenGL.cpp` has six `glBeginBatch` calls and six matching `glEnd`. That is the whole surface to convert. The accumulate-and-replay machinery already exists in `VertexArray.{h,cpp}`.

Risks, after the 2026-08-26 probe:

- **Settled.** `glDrawArrays` does not merge, so the flush goes. See [Evidence](#gldrawarrays-does-not-merge-tested-2026-08-26)
- **Settled.** Fixed-function state and legacy shader built-ins survive, because vertex specification stays fixed-function
- **New constraint.** The vertex pointer size must never vary between draws or the driver corrupts the frame
- Open: `LuaOpenGL` has no "inside `BeginEnd`" state, so nothing stops a widget calling other `gl.*` functions between two `gl.Vertex` calls. Today that is a silently ignored `GL_INVALID_OPERATION`. Buffered, it would take effect
- Open: display list recording. `glDrawArrays` is compiled into a list with its data dereferenced at compile time, which should retire the deferral, but that is reasoned rather than measured
- Weeks, not an afternoon, with regression risk across every widget in every game

### 2. Bisect which widgets cost the time

Downgraded, and possibly out of scope entirely. Widgets are the game developer's domain. Fixes upstream to each game separately and never reach games that stopped being updated, which is the population that most needs them. Item 1 reaches them all without anyone's cooperation.

Leads if pursued: build menu costs about 10 fps when open, deselecting the starting unit gains about 10 (`PLAN:1324`).

### 3. Capture a Metal System Trace

Never done. Instruments or an Xcode GPU capture, sampling the GPU from outside the process.

**This is not the same as the instrumented Mesa branch, and neither substitutes for the other:**

| | `macos-diagnostics` branch | Metal System Trace |
|---|---|---|
| Answers | how many, how big | how long, and where |
| Vantage | inside the driver | outside the process |
| Gives | counts of submits, blits, allocations, command buffers | per-encoder timings, shader cost, stalls |
| Cost | a Mesa rebuild | no rebuild |

The counters found the leak and the batching bug. They cannot say where GPU time goes, which is the open question. A trace is the only thing that can say whether Zink emits bad shaders or stalls somewhere invisible from inside the engine.

### 4. Count render passes for real. The instrument already exists.

The "about 200 passes a frame" figure is `57ms / 0.266ms`, not a count.

**Most of this is already built.** `337c87ae087` on the `macos-diagnostics` branch of `~/dev/mesa` adds `kk_dbg_count_cmdbuf_new(render)` in `kk_cmd_buffer.c`, which counts render against non-render command buffers, plus submit, blit and allocation counters in `zink_batch.c`, `zink_blit.c` and `zink_bo.c`. All environment gated.

**Why it has only ever been run during loading.** That branch is based on a main that carries `c08dba83025`, the Metal 4 encoding commit, so it leaks about 5 GiB a second and dies at roughly 12 seconds. It never reaches a steady-state frame. That is why the only figure we have is 68177 render passes against 33823 compute passes in 12 seconds of loading, which is not a frame rate number and was never meant to be.

**The fix is to move the instrumentation, not to write it.** Rebase `337c87ae087` onto the [MR 43697](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43697) branch, which carries the same Metal 4 encoding but does not leak, and the counters reach gameplay. That turns the single most load-bearing inference in this document into a measurement, and it is a rebase plus a build rather than new work.

### 5. Track upstream Mesa [issue 15998](https://gitlab.freedesktop.org/mesa/mesa/-/work_items/15998)

Pins our Mesa. [MR 43697](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43697) tested 2026-08-25 and it works. Detail in [Evidence](#evidence).

---

## Measurement rules

Read these before running anything. Every one was learned by getting it wrong.

- **Run-to-run variance is 1 to 2%.** An effect below that cannot be confirmed at any sample size worth paying for. Do not chase single digits.
- **Put both sides inside one run** wherever the switch toggles per frame. Between-run drift and focus loss both exceed most effects. `SPRING_DIAG_CELLS` does this and lives on `macos/renderer-diagnostics`, not on `macos/integration`.
- **Absolute figures do not survive a display change.** Empty Mod costs 16.9ms on the external 5120x2880 panel against 12.5ms on the built-in one, same everything else. Re-baseline after plugging in.
- **A frozen scene hides rebuild-triggered artefacts.** Compiled lists first read as clean for exactly this reason.
- **Verify the scene is what you think.** One sweep reported 40.4 fps and a 2.5x win, which was an empty map with a menu over it.
- **Check the sim frame before trusting any figure.** A paused game renders happily at 27.8 fps.
- **Record the Mesa commit beside every number.** Figures without one cost a rebuild to disambiguate.
- **Give a probe a positive control.** A probe that fails to reproduce a known defect reads exactly like a probe that found nothing, and the first version of `batch_merge_probe.c` did.
- **Check which Mesa a standalone probe loaded.** `DYLD_LIBRARY_PATH=/opt/homebrew/lib` beats the path linked into the binary and substitutes Homebrew's Mesa. See `test-scripts/README.md`.

---

## Harnesses and traps

### Splinter Faction, unattended. Use this by default.

```sh
SPRING_FPS_LOG=5 coding-agents/test-scripts/run-capped.sh 120 /path/to/log.log
```

Runs windowed, enforces a memory ceiling, needs no intervention. Every measurement in the plan was taken on it. Add `--config /path/to/copy.cfg` to change settings without touching the shared `springsettings.cfg`, which is a known contamination trap.

### BAR benchmark. Needs a human.

```sh
"$HOME/.spring/engine/macos_arm64/<build>/spring" \
  --isolation-dir "$HOME/dev/spring-testdata" \
  "$HOME/dev/spring-testdata/benchmark_bar.txt"
```

- **It pauses at sim frame 0 or 1** for the scenario briefing, in fullscreen as well as windowed. Log line is `tomjn paused the game` after `Scenario: Spawning on frame, 0, 0`.
- `debugcommands` are keyed to sim frames, so nothing fires while paused, including the benchmark itself at frame 15.
- Left alone it reports about 27.8 fps for a paused game indefinitely.
- Focus is not the cause. Window activation does not fix it. Synthetic keystrokes need an Accessibility grant and `PLAN:1221` says do not revisit that route.
- The camera decides what is drawn, so a run with a different view is a different benchmark.

### Memory

A run peaks near 8 GiB on 16 GB of RAM. A leaking driver reaches 54 GB and freezes the machine past the point where the out-of-memory dialog responds. `run-capped.sh` polls and kills. Note `ps` RSS stays near 1.3 GB even at 29 GB of real footprint, so RSS shows nothing.

---

## Evidence

### Lua buffering measured, and where the rest of the win is, 2026-08-26

**All figures on the external 5120x2880 panel, the only active display, at 2x scaling, so they do not compare to anything above.** Mesa `56588ef0665`, SplinterFaction, frozen scene, 3024x1832 backing, `SPRING_FPS_LOG=5`, mean of the last six samples a run.

| Configuration | fps | frame | Runs |
|---|---|---|---|
| `LuaImmediateModeBuffering = 0`, the old path | 16.06 | 62.3ms | 2 |
| `LuaImmediateModeBuffering = 1` | 16.97 | 58.9ms | 2 |
| buffering on, plus compiled display lists | 31.29 | 32.0ms | 1 |
| buffering on, plus `ForceImmediateModeFlush = 0` | 31.35 | 31.9ms | 1 |

Buffering alone is worth 5.7%, with every buffered sample above every legacy sample, 16.46 to 17.73 against 15.36 to 16.31. A clean separation of a small effect.

**The rest of the gap is the display list deferral, and it is not available.** Compiling lists reaches 31.29 and `ForceImmediateModeFlush = 0` reaches 31.35, and the second of those turns off the deferral as well. So the engine's own 40 `glBeginBatch` callers, 28 of them in `GuiHandler.cpp`, are not where the remaining time goes.

Read that 1.84x the same way as the flush's 1.87x, which is to say as the price of correct output rather than as headroom. Both figures come from turning a mitigation off, and both bring the artefacts back with them. The measurement in this very section says so: with lists compiled, the build menu renders wrong. That is the deferral doing its job.

This trap has caught this project more than once. A large number appears, the mitigation it disabled is forgotten, and the number gets chased. `ForceImmediateModeFlush = 0` is the usual route in, and [Settled: do not reopen](#settled-do-not-reopen) has the standing warning.

**Compiled lists differ from deferred ones, and the deferred side may be the wrong one.** 39,082 differing pixels of 450,000 on the build menu, against a same-configuration noise floor of 25. Reverted pending an explanation.

The difference is not geometry. Neither a horizontal shift nor a horizontal scale improves the match, both were swept, and magnified crops show identical icon positions, sizes and text placement. It is a blend difference, and the compiled side is lighter.

| Sample in the menu | compiled | deferred | implied background |
|---|---|---|---|
| (30,480) | 29 | 8 | 97 |
| (30,300) | 73 | 21 | 243 |

`DrawPanel` in `gui_static_buildordermenu.lua` lays a black overlay at `ui_opacity`, which is 0.7 in both runs, so each application leaves 0.3 of what is behind it. Two applications leave 0.09. The ratio 0.3 against 0.09 is 3.33, and both samples fit it at very different background values.

A ratio of 3.33 is what one extra application of that overlay would give, but the widget does not do that in either path. `DrawPanel` is called from inside the two `gl.CreateList` closures and nowhere else, each list is called once a frame, and the deferred `gl.CallList` runs its closure exactly once at `LuaOpenGL.cpp:6613`. So the arithmetic fits a mechanism the code rules out, and the real one is still unknown.

What is established: geometry is identical, the difference is in blending, the compiled side is lighter by about 3.3x in dark areas, and both sides are stable frame to frame, 2.2 differing pixels between two frames against 2.9 for the deferred path. Nothing is replaying stale buffer contents.

Next step is a minimal reproduction rather than more reading of a 1,434 line widget: one throwaway widget that draws the same rectangle twice, once through a compiled list and once directly, and a pixel comparison of the two. That separates the engine's list handling from anything this widget does.

### Rendering is unchanged by buffering, tested 2026-08-26

The gate for the rewrite. Screenshots at 20s and 40s into a frozen scene, compared with `compare -metric AE`.

| Comparison | Whole screen | Build menu region |
|---|---|---|
| same configuration, two runs | 30,950 | 25 |
| buffered against the old path | 30,672 | 50 |

Out of 5.5M and 450,000 pixels. The cross-configuration difference is smaller than the run-to-run difference, so buffering is indistinguishable from the old path at the resolution this harness can measure.

The noise floor is the profiler text and the unit animating, both of which differ between any two runs. The build menu column is the tighter test, because it is static UI drawn through `gl.BeginEnd`, and there buffering moves 50 pixels against a floor of 25.

This does not prove pixel identity. It proves any difference is below about 0.01% on static UI.

### `glRectf` followed by `glDrawArrays` loses draws, found 2026-08-26

The defect buffering exposed, and the reason `gl.Rect` is now buffered too.

`gl.Rect` goes through `glRectf`, an immediate-mode primitive that never passed through `glBeginBatch` and so never received the flush. That was invisible while everything after it was also immediate mode, and therefore also flushed. Buffering changed what follows into `glDrawArrays`, and that pair is the one the driver gets wrong.

| Case, `batch_merge_probe.c`, GL_QUADS | Dirty frames | Missing pixels |
|---|---|---|
| `glRectf` then immediate, flush, reference | 0 of 30 | 0 |
| **`glRectf` then arrays, no separator** | **30 of 30** | **1,439,280** |
| `glRectf` then arrays, flush | 0 of 30 | 0 |

Also worth noting from the same run: `GL_QUADS` immediate mode with no separator is clean 30 of 30, so the original merging defect does not fire for quads at all. The mitigation was characterised entirely on line loops.

The fix routes `gl.Rect` through the accumulator instead of adding another flush, which removes the mixture rather than paying for it.

**The engine's own 17 `glRectf` calls need the same treatment, and this part is not about buffering.** `CVertexArray` already emits `glDrawArrays` and is used by `glExtra.cpp`, `GrassDrawer` and `AdvWater`, so an engine `glRectf` followed by one of those hits the same defect with no Lua involved. `HAPFSPathDrawer` contains both. That makes it a latent bug on this branch that buffering widened rather than created, though it has not been demonstrated in a running frame.

`glRectBatch` in `myGL` wraps them, flushing after rather than before. `glBeginBatch` separates itself from what precedes it, which suffices when its neighbours are also immediate mode. `glRectf` has to separate itself from what follows, because there is nothing on that side to do it.

Measured at 17.16 fps against 16.97 without the wrapper, so the flushes cost nothing detectable. A first attempt read 14.04 and was wrong: a 45 second run compared against 70 second runs, with a pre-freeze loading sample in the mean.

`glBeginBatch` needs no equivalent change. `glBegin` followed by `glDrawArrays` is clean 30 of 30 in the same probe, so the defect is specific to `glRectf`.

**How it was found, because the method mattered more than the result.** It presented as a display list bug for hours. A widget drawing one figure twice a frame, once through a list and once directly, showed the list copy losing its first two batches. Drawing the direct copy first moved the artefact to the direct copy, which proved it followed draw order and had nothing to do with lists.

Three false readings came before that, all from a flat backdrop:

- black backdrop, quads read as dropped, they were the wrong colour
- orange backdrop, quads read as dropped, indistinguishable from being drawn in the backdrop colour
- a cyan sentinel before the call did not separate them either, because the sentinel was lost as well

A striped backdrop separated them, and then suppressed the artefact, because the fifty extra `gl.Rect` calls changed what was being measured. Watching the live window was what showed the squares flickering in occasionally, which no single screenshot could.

### `glDrawArrays` does not merge, tested 2026-08-26

The question that gates item 1. If a buffered draw merged the way `glBegin` does, the flush would survive the rewrite and most of the win with it.

`coding-agents/test-scripts/batch_merge_probe.c`, Mesa `56588ef0665`, 2000 LINE_LOOP circles a frame on a 1280x900 pbuffer, 30 frames a case. Geometry copied from `widget_loop_amp.lua`, which is the configuration already measured positive. Ground truth is the driver's own output with a flush between batches.

| Case | Dirty frames | Worst frame |
|---|---|---|
| immediate, flush, the reference | 0 of 30 | 0 |
| arrays, finish, second ground truth | 0 of 30 | 0 |
| **immediate, no separator, positive control** | **27 of 30** | **40 stray** |
| **arrays, no separator** | **0 of 30** | **0** |
| arrays, flush | 0 of 30 | 0 |
| **arrays, varying pointer size** | **30 of 30** | **553,659 stray** |
| immediate, varying arity, flush | 0 of 30 | 0 |

The positive control reproduces the published figure, 27 of 30 frames against 31 of 31, worst 40 stray pixels against a median of 28. So the probe exercises the defect.

**`glDrawArrays` is clean across 60,000 adjacent pairs**, against a defect that fires about once per 2000 pairs under `glBegin`. The flush can go.

**Varying the vertex pointer size between draws corrupts, badly.** Plain `arrays` changes the pointer address every batch and stays clean, so it is the size change alone. That is legal GL and the driver gets it wrong, so a buffered path must pin the pointer to four floats and never vary it. The arity mitigation does not disappear, it changes shape from 790,748 widened vertices a frame to one constant.

The last row is a null result, not a refutation. The arity defect was found on the load screen with textures bound, which this probe does not set up.

A first attempt at this probe drew 4-vertex squares 10 pixels apart and found nothing, including nothing from the positive control. The spacing is what makes a merge visible: the amplifier puts circles 22 pixels apart so the connecting segment cannot be confused with the shapes.

### Display lists are not the wedge's mechanism, tested 2026-08-26

The wedge is the artefact that appears when `gl.CreateList` is allowed to compile, and it is what keeps the deferral in place. The leading hypothesis was that `IssueBatch` hands `glDrawArrays` a pointer into a vector `LuaImmediateBatch` clears and refills, and that Mesa keeps the pointer rather than dereferencing it at compile time.

**It does not.** `batch_merge_probe.c` grew a display list mode and the hypothesis died on the first run. Every case below is 0 dirty frames of 10, Mesa `56588ef0665`, same 2000 batch grid as the section above.

| Case | Dirty frames |
|---|---|
| list, immediate | 0 of 10 |
| list, arrays, own array a batch | 0 of 10 |
| list, arrays, reused buffer | 0 of 10 |
| list, arrays, own array, buffer overwritten before replay | 0 of 10 |
| **list, arrays, reused buffer, buffer overwritten before replay** | **0 of 10** |
| every batch from its own list | 0 of 10 |
| lists and direct draws alternating | 0 of 10 |
| list, arrays, varying pointer size | 0 of 10 |
| list and direct, with a texcoord array bound | 0 of 10 |
| quads, list, texcoords | 0 of 10 |

The bold row is the one that settles it. The shared buffer is overwritten with a ring at 0.9 NDC between `glEndList` and `glCallList`, so a driver holding the pointer would replay a screen-sized ring over 2000 seven pixel circles. It replayed the circles. Mesa dereferences at compile time, as the specification requires. The list also replayed with 0 missing pixels, so it genuinely compiled and executed all 2000 draws rather than compiling to nothing.

Two further results came out of the same run:

**A flush fixes the varying pointer size defect.** 10 of 10 dirty without a separator, 0 of 10 with one. So it is the same separator-class defect as batch merging, not a separate fault.

**A display list is immune to the varying pointer size defect.** Compiling alternating size 2 and size 4 draws into one list and replaying it is clean, because compilation bakes the arrays into one internal format and there is no size change left to get wrong. This is the opposite of what was expected: a list cannot record a `glFlush`, so lists looked like the one place a separator could never reach.

Do not spend another session on "the driver mishandles `glDrawArrays` inside a display list". It handles every arrangement the probe can express.

### The wedge found: textured list replays beside live draws, 2026-08-26

The section above is the five dead ends. This is the cause, reproduced offline in the same probe once it was given the one ingredient it had never had: a texture coordinate array.

**A display list replay that contains a textured `glDrawArrays` corrupts the frame when live client array draws are interleaved with the replays.**

| Case | Dirty frames | Extra pixels |
|---|---|---|
| every batch from a list, textured | 0 of 20 | 0 |
| all live drawing, textured | 0 of 10 | 0 |
| lists and live draws mixed, untextured | 0 of 10 | 0 |
| **lists and live draws mixed, textured** | **20 of 20** | **13,138,920** |
| mixed textured, texcoord pointer rebound each draw | 20 of 20 | 13,138,920 |
| mixed textured, flush around `glCallList` | 0 of 20 | 0 |
| mixed textured, flush between every draw | 0 of 20 | 0 |
| mixed textured, finish between every draw | 0 of 20 | 0 |

657,000 wrong pixels a frame on a 1,152,000 pixel surface, 57% of the frame. The lists in that table are compiled once, before the loop, and only replayed inside it, so compiling is not involved. It is replay.

Three ingredients, and every one is needed:

- a display list containing `glDrawArrays`
- a texture coordinate array bound when it was compiled
- live client array draws interleaved with the replays

Drop any one and it is clean. That is why the deferral hides it completely: a deferred list runs its function live, so there is no replay to interleave with anything.

**The fix is a `glFlush` either side of `glCallList`**, which is clean 0 of 20 and is the one placement `LuaOpenGL::CallList` can implement. Rebinding the array pointers before each draw changes nothing, 20 of 20 dirty, so this is a synchronisation fault rather than stale client array state. It is far cheaper than the per-batch flush in `glBeginBatch`, because a frame has thousands of batches and tens of list replays.

**Confirmed in game 2026-08-26.** A human drove Splinter Faction with lists compiling and the flush in, ran all three triggers that reproduced the wedge before, and saw no artefacts. Compiled lists are now the default, `LuaDisplayListMode = 1`.

Measured Splinter Faction, fixed start, 1400x850 window, 10 second samples:

| Configuration | fps |
|---|---|
| `LuaDisplayListMode = 0`, deferred, the old default | 16.1, 16.3, 16.5, 16.3, 16.3 |
| default, compiled, with the flush | 34.1, 34.3, 34.3, 34.3, 33.1 |

2.09x. Not comparable to the 1.82x quoted before this work, which was measured at 3024x1832.

An earlier run of the same compiled configuration gave 22.7 to 30.5 and was much noisier. The cause of that spread was not chased. Both runs beat the deferred figure by a wide margin, so the direction is not in question, but a careful A/B is still owed if the exact multiplier matters.

**Why it looks like a wedge.** `gui_options.lua` and `gui_tooltip.lua` draw every panel with `RectRound`, which is `gl.BeginEnd(GL.QUADS, ...)` with `bgcorner` bound, so all UI panel drawing is textured. Corrupting one vertex of a rounded rectangle leaves the rest in place, which draws a quad stretched from an apex. Many such rectangles sharing an apex is the fan of thin lines. The dark fill and the thin coloured line along the hypotenuse in the screenshots are a panel's own fill and border.

**Why the triggers are what they are.** Hovering a slider calls `WG['tooltip'].ShowTooltip` at `gui_options.lua:1000` and a checkbox never does. Selecting a unit and placing a building both raise tooltips too. The tooltip widget replays a list while other widgets draw live, which is the mixture above. Scrolling the widget list only recompiles, and compiling alone is clean, which is why a human scrolled without seeing anything.

**What the probe still cannot see.** No matrices, no shaders, no actual bound texture, no depth, no framebuffer changes. The exact driver mechanism is not established. Changing the texcoord array contents changes the wrecked geometry, so texcoord data reaches position somehow, but the geometry does not track the texcoord values one for one, so "position is read from the texcoord array" is not supported and is not claimed here.

**What the artefact actually is**, from screenshots a human took on 2026-08-26 in `~/dev/spring-testdata/artefacts/2026-08-26-display-list-wedge/`. Earlier readings called it a smear of map texture. It is not. It is a dark panel fill with a thin coloured border running the full length of its hypotenuse, which is a UI rectangle drawn with a vertex flung away, and the fan of thin lines is many such rectangles sharing one bad apex. Corrections from the human that narrow it further:

- Scrolling the widget list produced no artefact in the session the screenshots came from, so list recompilation on its own is not the trigger
- The build menu widget drew correctly throughout, so this is not the build menu defect recorded above, which is a separate fault
- The wedge draws above the GUI and below the cursor and the tooltip, which pins the corrupted draw to a slot late in `DrawScreen`
- Hovering a slider triggers it, hovering a checkbox does not

`LuaDisplayListMode` splits compiling from replaying. 0 defers, 1 compiles and replays, 2 compiles on the same schedule and then discards the replay, running the Lua function live instead. An artefact that survives mode 2 was caused by compiling a list, one that only appears in mode 1 was caused by replaying it. Mode 2 is a diagnostic, not a shipping configuration. The answer below came from the probe rather than from mode 2, so mode 2 was never needed in the end. It stays because the next list fault will want the same split, and because it is the cheapest way to ask "compile or replay" of any future artefact.

### `ZINK_DEBUG=rp`, tested 2026-08-25, no signal

| Run | Config | fps | Sim frames |
|---|---|---|---|
| A1 | baseline | 15.74 | 3191 |
| A2 | baseline | 15.79 | 3184 |
| B1 | `rp` | 15.23 | 3122 |
| B2 | `rp` | 15.47 | 3135 |

Logs at `build-macos-legacy/rp-sf-*.log`. Flag confirmed parsed via `ZINK_DEBUG=help`.

Every baseline beat every `rp` run, but with n=2 an arm that is 1 of 6 assignments, p = 0.167. The two `rp` runs differ by 1.6% from each other, so variance is 1 to 2%, and a 2.6% gap sits inside it. Not worth more runs: 2.6% either way is not a lever at 15.8 fps. `rp` cannot be tested with interleaved cells because `zink_debug` is read once at screen creation.

What survives: if tracking were worth the 20% to 40% the pass-break model implies, four runs would have shown it.

**Do not repeat `ZINK_DEBUG=rp,rploads,rpstores`.** Those flags do the opposite of what the names suggest. Per `zink_context.c:3194` and `:3551` they turn existing `DONT_CARE` attachments into `CLEAR` to expose code relying on undefined contents. They add work.

### `ForceImmediateModeFlush = 0`, tested 2026-08-25

| Run | Config | `immediate-mode batching` | fps | frame | Peak footprint |
|---|---|---|---|---|---|
| D1 | baseline | 0 | 14.98 | 66.8ms | 8028 MiB |
| E1 | `ForceImmediateModeFlush = 0` | 1 | 27.98 | 35.7ms | 6377 MiB |

Logs at `build-macos-legacy/dl-{D1,E1}.log`. Reproduces `PLAN:1259` independently.

Rendering visibly broken with it on: red triangles, resource bar corruption, misshapen load screen bars.

### The render pass break model, demoted

Probe figures are solid. The model built on them is not.

| | 3024x1832 | 1512x916 |
|---|---|---|
| 64 blended quads, one pass | 3.72ms | 1.56ms |
| 64 blended quads, one pass each | 20.48ms | 4.24ms |
| implied cost of a pass break | 0.266ms | 0.043ms |

0.266ms for a 22MB store plus 22MB reload is about 166 GB/s, this machine's memory bandwidth. Real mechanism on tiled hardware.

Demoted because the count was never measured, `rp` found nothing to coalesce, and the LuaUI bisect explains the frame without it.

### Mesa pin and [issue 15998](https://gitlab.freedesktop.org/mesa/mesa/-/work_items/15998)

Building `~/dev/mesa` at `56588ef0665`, the commit before `c08dba83025 kk: Move to Metal4 command encoding`, which leaks about 5 GiB a second under the engine. Filed as [issue 15998](https://gitlab.freedesktop.org/mesa/mesa/-/work_items/15998), reproducer at `coding-agents/test-scripts/kk_mipmap_leak.c`. Full account at `PLAN:754`.

[MR 43697](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43697) tested 2026-08-25 across five builds:

| Config | Mesa | Reproducer | Engine peak | Result |
|---|---|---|---|---|
| A, the pin | `56588ef0665` | 481mb | 8065 MiB | survives 120s |
| B, control | `73a03c61101` | 2625mb | ceiling at 43s | killed |
| C, [MR 43697](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43697) | `a0c05f0da7f` | 474mb | 12288 MiB | survives 120s |
| D, Zink flag alone | B + `RELEASE_RESOURCES` | 2627mb | ceiling at 39s | killed |
| E, both | C + `RELEASE_RESOURCES` | 474mb | 7049 MiB | survives 120s |

[MR 43697](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43697) fixes it. The Zink flag alone does nothing, because pre-MR KosmicKrisp declares the reset flag `UNUSED`. Together they are worth 5 GB, and that two-line Zink change is worth proposing separately.

If the MR lands, the pin can move, which reopens released Mesa versions.

**Do not adopt Metal 4 for performance until this is fixed.** Any figure quoted for what Metal 4 buys was measured on a leaking driver.

### Comparison points

Direction only. Different content, not just different present paths.

| Stack | Result |
|---|---|
| sebvries, MoltenVK, direct present | 45 fps at 0.5k units, 12 fps at 2k |
| This build, KosmicKrisp, readback | 3.3 fps at 1300 units |
| lucamignatti, Minecraft, direct present | 70 fps, far lighter scene |

The same engine on the same driver runs Empty Mod at 80 fps. Do not read the gap as a renderer gap.

### The BAR benchmark run this document started from

BAR scenario 23, `corak armpw 650 10 2040`, 2000 sim frames, 3024x1832, 8320 units spawned.

| Span | Frames | Mean | p50 | p95 | p99 | Max |
|---|---|---|---|---|---|---|
| Sim | 1809 | 17.47ms | 16.6 | 23.0 | 27.2 | 41.2 |
| Update | 252 | 4.75ms | 4.0 | 15.4 | 20.3 | 35.5 |
| Draw | 252 | 58.58ms | 32.4 | 212 | 824 | 1818 |

3.3 fps overall. Those spans come from BAR's own gadget and do not cover the whole frame, leaving about 114ms a frame unaccounted. The p50 of 32ms against a p99 of 824ms is a tail problem distinct from being uniformly slow, and nobody has chased it.
