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

## Capturing what happened

`SPRING_PHASE_DUMP=1 SPRING_PHASE_DUMP_DIR=<dir>` writes a quarter-size PPM between draw phases, so a stray polygon can be attributed to the phase that drew it. Convert with `python3 ~/dev/macos-probes/ppm2png.py <file.ppm>`.

Note the phase dumps stop at `6-screenpost`, the last point in `CGame::Draw`. Anything that appears on screen but not in that dump is being drawn somewhere else. The engine's own screenshot action captures the finished frame and is the control for exactly that case.
