#!/bin/bash
# Put the perf probe widget into the throwaway game, or take it back out.
#
#   install-probe.sh            copy it in, overwriting any earlier copy
#   install-probe.sh --remove   take it out again
#
# The game copy is throwaway but has to end up unmodified, so the widget lives in
# the repo and this is the only thing that touches the game.
set -eu

GAME=$HOME/dev/spring-testdata/games/SplinterFaction_0.1.78.sdd
DEST=$GAME/LuaUI/Widgets/test_perf_probe.lua
SRC=$(dirname "$0")/widget_perf_probe.lua

if [ ! -d "$GAME/LuaUI/Widgets" ]; then
	echo "no widget directory at $GAME/LuaUI/Widgets"
	exit 1
fi

if [ "${1:-}" = "--remove" ]; then
	if [ -e "$DEST" ]; then
		rm "$DEST"
		echo "removed $DEST"
	else
		echo "nothing at $DEST"
	fi
	exit 0
fi

cp "$SRC" "$DEST"
echo "installed $DEST"
