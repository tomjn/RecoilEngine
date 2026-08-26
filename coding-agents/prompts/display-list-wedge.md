# Task: find what draws a screen-filling wedge when Lua display lists are compiled

A large dark triangular wedge, filled with a fan of thin lines radiating from a single apex, covers up to half the screen in SplinterFaction. It appears only when `gl.CreateList` is allowed to compile rather than defer. Your job is to find the cause. Fixing it comes after, and only once you can say what it is.

Screenshots: `~/dev/spring-testdata/artefacts/2026-08-26-display-list-wedge/wedge-1.png` through `wedge-6.png`. Look at them before anything else.

Read `coding-agents/MACOS_PERFORMANCE.md` first, at least the summary table, "Settled: do not reopen", and the three 2026-08-26 evidence sections. It holds the investigation this continues and several traps that have each cost a session.

## Why this matters

Compiling the lists instead of deferring them measures 31.32 fps against 17.16, a 1.82x, with the build menu rendering identically on a frozen scene. That is the largest remaining win on the macOS renderer and it is currently unavailable because of this artefact.

Be careful with that number. Turning a mitigation off and quoting the frame rate is a trap this project has fallen into more than once. The goal is to remove the *reason* the deferral exists, not to disable it and accept breakage. `MACOS_PERFORMANCE.md` has the standing warning.

## How to reproduce

The deferral is one line, `LuaOpenGL.cpp` in `LuaOpenGL::CreateList`:

```cpp
if (!globalRendering->supportImmediateModeBatching) {
```

Change it to `if (!globalRendering->supportImmediateModeBatching && !luaImmediateBuffering) {`, rebuild `engine-legacy`, and lists compile whenever buffering is on, which is the default.

Then run it and interact. A timed probe will not find this, because the artefact is triggered by interaction:

```sh
SPRING_FPS_LOG=10 coding-agents/test-scripts/run-capped.sh 600 ~/dev/spring-testdata/logs/wedge.log
```

Triggers a human found, all reproducible:

- selecting the starting unit
- hovering a slider in the Settings dialog, which does not need a unit selected and gives a slightly different result
- building an energy structure through a path that does not use the build widget

It is easier to see with a small window. The wedge changes dimensions as you zoom, tracking where the map is, and a thin band of colour across the centre shifts as the mouse moves.

**You cannot do this part yourself.** It needs a human driving the game. Ask, and give them a short list of what to try.

## What is already ruled out

Do not re-derive these. Each was measured on 2026-08-26.

| Ruled out | Evidence |
|---|---|
| The old red triangle artefacts | Gone. Human confirmed none in a 4m37s session with lists compiled |
| Arity and flush mitigations | Untouched by this change, both still active |
| `glDrawArrays` merging like `glBegin` | Clean over 60,000 adjacent pairs, `batch_merge_probe.c` |
| Mixing `glBegin` and `glDrawArrays` | Clean 30 of 30 |
| Reusing one client array buffer per batch | Clean 30 of 30, but see below, this was never tested *with list compilation* |
| Changing current colour between batches | Clean 30 of 30 |
| `GL_QUADS` versus `GL_LINE_LOOP` | Clean 30 of 30 |
| `glRectf` followed by `glDrawArrays` | **Not clean.** 30 of 30 dirty. Already fixed, see `glRectBatch` |

## Leading hypothesis

`IssueBatch` in `LuaOpenGL.cpp` hands `glDrawArrays` a raw pointer into `std::vector` storage that `LuaImmediateBatch` clears and refills for every subsequent batch. The vector's `data()` does not move, so every batch shares one address.

The GL specification says a `glDrawArrays` compiled into a display list dereferences its array data at compile time. If Mesa keeps the pointer instead, replaying the list reads whatever is in that buffer now, which is the most recent batch's vertices.

That fits all three of the human's observations. Wedge dimensions tracking the map means the bad vertices are world-space coordinates from another draw. The band shifting with the mouse means the source is whatever drew most recently. A fan from a single apex is what you get when a primitive is drawn with another batch's data at a different length.

**Test it in `batch_merge_probe.c`, not in the engine.** It is standalone, offline and takes seconds per iteration. Add a mode that compiles batches into a display list with `glNewList(GL_COMPILE)` while using the shared scratch buffer, replays it, and compares. That is the one cell of the matrix never tested. If it reproduces there, you have it without touching the engine at all.

If confirmed, the fix is to give each batch storage that is not reused while a list is compiling.

## Also look at the map and the game

The hypothesis above is mine and it may be wrong. The artefact could equally be a widget or the map doing something that only breaks once its list is genuinely compiled, since on this branch no Lua list has ever compiled before.

- SplinterFaction is at `~/dev/spring-testdata/games/SplinterFaction_0.1.78.sdd`. 61 widgets call `gl.CreateList`. You may modify them freely for debugging, the copy is throwaway, but leave it unmodified when you finish.
- The map is under `~/dev/spring-testdata/maps/`. The wedge tracks map geometry, so map rendering is a live suspect, not just UI.
- A fan from an apex suggests `GL_TRIANGLE_FAN`. Grep both the game and the engine for fans. `GuiHandler.cpp` uses several.

Find which draw call produces it. `SPRING_PHASE_DUMP=1 SPRING_PHASE_DUMP_DIR=<dir>` writes a quarter-size PPM between draw phases and will attribute it to a phase. Note the dumps stop at `6-screenpost`, so anything appearing on screen but not in the dump is drawn later.

## Method, learned the hard way

These cost most of a day on 2026-08-26. They are not optional advice.

**Recolour before you score anything.** A flat backdrop makes "not drawn" and "drawn in the wrong colour" the same picture. That misled three separate readings in a row, including a cyan sentinel that failed because the sentinel was being lost too. Give every element a distinct opaque colour, and stripe or texture the background so a missing shape shows the pattern through it.

**Put both cases in one frame.** Draw the same thing twice, once through the path under test and once directly, and compare within a single screenshot. That removes run-to-run drift entirely. `coding-agents/test-scripts/widget_list_probe.lua` already does this and is a good starting point.

**Vary the order.** The same probe showed what looked like a display list bug for hours. Drawing the direct copy first moved the artefact onto the direct copy, proving it followed draw order rather than lists. Whenever A fails and B does not, swap them.

**Amplify.** Draw many trials a frame rather than one, the way `widget_loop_amp.lua` draws 2000 circles. It turns "did it happen" into "how often".

**Audit what your probe cannot see.** `batch_merge_probe.c` scored lit against unlit for a day, so it reported clean for every wrong-colour defect while a wrong-colour defect was being hunted. Ask what class of fault your measurement is blind to.

**Watch the live window.** A human noticed the shapes flickering in briefly, which no single screenshot could show. Screenshots at one instant will confidently tell you something never renders when it renders sometimes.

## Harness notes that will otherwise cost you time

- `run-capped.sh` enforces a memory ceiling. A run peaks near 8 GiB on a 16 GB machine and an unbounded one has frozen the machine. Always use it.
- Standalone probes: put the Mesa prefix ahead of Homebrew in `DYLD_LIBRARY_PATH`, or you silently test Homebrew's Mesa. Check the `GL_VERSION` a probe prints. The pin reports `26.2.0-devel (git-56588ef066)`.
- Do not pass `--config` a shared file. The engine rewrites whatever config it is given on exit.
- Remove any probes you install from the game when you finish, with `install-probe.sh --remove`.

## What to produce

A statement of the cause with evidence, and a recommendation. If the fix is small and you are confident, implement it behind the existing `LuaImmediateModeBuffering` config so both paths can be compared in one session. Do not enable compiled lists by default until a human has driven the game and confirmed the wedge is gone.

Record what you find in `MACOS_PERFORMANCE.md`, including anything you rule out. Half the value of that document is the list of things nobody needs to try again.
