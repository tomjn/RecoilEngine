# Test start scripts for the macOS renderer

Start scripts the engine accepts directly, so a renderer change can be checked against a real game without a lobby. Kept here rather than in a scratch directory so the setup is not lost between sessions.

Run one with:

```bash
cmake --build build-macos-legacy -j 10 --target engine-legacy
cd build-macos-legacy
./run-macos.sh --isolation-dir ~/dev/spring-testdata --window ../coding-agents/test-scripts/<script>.txt
```

`--isolation-dir` is not optional. Without it the engine picks up `~/.spring`, whose `base/springcontent.sdz` is from January 2025 and is missing shaders this engine asks for, and the terrain draws black. That is a stale content problem wearing a renderer costume.

## The scripts

| script | game | what it is for |
|---|---|---|
| `empty-mod.txt` | Empty Mod | an empty game, so a loose `LuaIntro/main.lua` in the data dir is the only thing that draws. The controlled harness for Lua drawing bugs. |
| `splinterfaction-fixed-start.txt` | SplinterFaction 0.1.78 | a real game with a real LuaUI. |

## Rules that cost time to learn

**`startpostype=0`.** Fixed start positions. Anything else lands in a placement screen, which for SplinterFaction is its own LuaUI screen rather than the engine's selector, so it is a different code path and a poor thing to measure against.

**`gametype` is the modinfo `name`, not the `game` field.** For `empty_mod.sdz` that is `Empty Mod`.

**The archives must be findable.** `~/dev/spring-testdata/games/` symlinks to whatever is in `~/.spring/games/`, and the map lives under `~/dev/spring-testdata/maps/`. A version bump in an installed game breaks the `gametype` line here.

## Games available to test against

SplinterFaction is the only game with a LuaUI that has actually been run on this renderer, so every LuaUI finding so far rests on one game's content. That is a real weakness in the evidence.

Metal Factions and MCL are installed. Whether Metal Factions runs at all is unverified, and XTA and Balanced Annihilation may not either. Confirming which of them start is worth doing on its own, before any of them is used as a control.

## Measuring

Four scripts, in the order you use them.

```bash
./install-probe.sh                      # freeze the scene, log the profiler
SPRING_DIAG_CELLS=5:-/flush \
  ./run-measured.sh 60 ~/dev/spring-testdata/logs/run.log
python3 cells.py ~/dev/spring-testdata/logs/run.log
./install-probe.sh --remove             # leave the game unmodified
```

`run-measured.sh` wraps `run-capped.sh`, so the memory ceiling and time limit still apply, and adds focus. It brings the window to the front and then reads frontmost back to confirm it, because `osascript ... set frontmost` succeeds against a process that has no window yet and the engine has none for the first several seconds of loading. It re-asserts focus on a lost poll and voids the run above one lost poll in twenty. It cannot check occlusion, so keep the window uncovered too.

`widget_perf_probe.lua` freezes the scene at sim frame 90 and re-applies one camera state every frame, which also holds the camera against edge scroll and a knocked mouse. It runs `debug 1 0`, enabling the profiler with the on-screen overlay off, and echoes the top timers every five seconds. A frozen scene reads to about 2.5% between intervals where a live one varies by a factor of two.

`SPRING_DIAG_CELLS=<seconds>:<cell>[/<cell>...]` cycles configurations inside one run, so both sides of a comparison see one scene and one focus state. A cell is `-` or a comma list of `flush`, `narrow`, `noflush`, `nopresent`, `finish`, `throttle`. An unknown name discards the whole schedule rather than measuring the baseline twice. Without it, each switch keeps its old meaning as a plain environment variable.

`cells.py` groups by cycle, drops cycle 0 for the loading screen, and runs a paired sign test. Five cycles cannot beat p = 0.0625, so plan the run length for the number of cycles you need.

**Only trust deltas between cells.** Absolute frame rates move about 10% between runs even frozen and focused. Mouse position is one uncontrolled variable, since hovering changes what the UI draws.

## Standalone probes

These need no engine run, so use them first where they can answer the question.

| probe | what it settles |
|---|---|
| `timer_probe.c` | whether GL timer queries work. They do not: Zink on KosmicKrisp does not advertise `GL_ARB_timer_query`, and `glQueryCounter` silently returns zeros |
| `fill_probe.c` | the driver's fill rate, and the cost of a render pass break. A break is a full attachment store and reload at memory bandwidth, 0.266ms at 3024x1832 |
| `off_probe.c` | immediate-mode batching. Disagrees with the engine about the arity fix, so do not trust it for a new rule |
| `kk_mipmap_leak.c` | the KosmicKrisp memory leak, attached to the upstream issue |

Build and run them against the same Mesa the engine uses. Each carries its own command in a comment at the top. `DYLD_LIBRARY_PATH=/opt/homebrew/lib` is needed or Zink cannot find `libvulkan.1.dylib`.

## Capturing what happened

`SPRING_PHASE_DUMP=1 SPRING_PHASE_DUMP_DIR=<dir>` writes a quarter-size PPM between draw phases, so a stray polygon can be attributed to the phase that drew it. Convert with `python3 ~/dev/macos-probes/ppm2png.py <file.ppm>`.

Note the phase dumps stop at `6-screenpost`, the last point in `CGame::Draw`. Anything that appears on screen but not in that dump is being drawn somewhere else. The engine's own screenshot action captures the finished frame and is the control for exactly that case.
