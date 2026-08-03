#!/bin/bash
# Run the engine with a hard memory ceiling and a hard time limit.
#
# A leak in the engine can take this machine to 54 GB and freeze it so completely
# that the out-of-memory dialog itself stops responding, which costs a power
# cycle. macOS has no cgroup and ulimit -v is useless against Metal allocations,
# so poll RSS and SIGKILL.
#
#   run-capped.sh <seconds> <logfile> [extra spring args...]
#
# Environment passed through, so SPRING_PHASE_DUMP etc still work.
set -u

LIMIT_SECONDS=${1:?seconds}
LOGFILE=${2:?logfile}
shift 2

BUILD=/Users/tomjn/dev/RecoilEngine/build-macos-legacy
SCRIPT=/Users/tomjn/dev/RecoilEngine/coding-agents/test-scripts/splinterfaction-fixed-start.txt
LAUNCHER=${LAUNCHER:-./run-macos.sh}

# kill above this many kilobytes of resident memory
# 16 GB of physical RAM on this machine, usually with a colima VM alongside, so
# the headroom is much smaller than the total suggests
MAX_RSS_KB=${MAX_RSS_KB:-3145728}   # 3 GiB
POLL_SECONDS=2

cd "$BUILD" || exit 1

"$LAUNCHER" --isolation-dir ~/dev/spring-testdata --window "$SCRIPT" "$@" \
	> "$LOGFILE" 2>&1 &
PID=$!

echo "engine pid $PID, ceiling $((MAX_RSS_KB / 1024)) MiB, limit ${LIMIT_SECONDS}s"

KILLED_FOR_MEMORY=0
ELAPSED=0
PEAK_KB=0

while kill -0 "$PID" 2>/dev/null; do
	RSS_KB=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
	if [ -n "$RSS_KB" ]; then
		[ "$RSS_KB" -gt "$PEAK_KB" ] && PEAK_KB=$RSS_KB

		# the series, not just the peak. A peak cannot tell a leak from a cache
		# that fills and then plateaus, and those need different fixes.
		echo "$ELAPSED $((RSS_KB / 1024))" >> "$LOGFILE.rss"
		if [ "$RSS_KB" -gt "$MAX_RSS_KB" ]; then
			echo "MEMORY CEILING HIT at $((RSS_KB / 1024)) MiB after ${ELAPSED}s, killing"
			KILLED_FOR_MEMORY=1
			kill -9 "$PID" 2>/dev/null
			break
		fi
	fi

	if [ "$ELAPSED" -ge "$LIMIT_SECONDS" ]; then
		kill -TERM "$PID" 2>/dev/null
		break
	fi

	sleep "$POLL_SECONDS"
	ELAPSED=$((ELAPSED + POLL_SECONDS))
done

sleep 3
if pgrep -f '\./spring' > /dev/null; then
	pkill -9 -f '\./spring'
	sleep 1
fi

echo "peak resident $((PEAK_KB / 1024)) MiB over ${ELAPSED}s"
if [ "$KILLED_FOR_MEMORY" = 1 ]; then
	echo "RESULT: killed for memory, do not trust anything this run produced"
	exit 2
fi
if pgrep -f '\./spring' > /dev/null; then
	echo "RESULT: engine still running after cleanup"
	exit 3
fi
echo "RESULT: clean exit"
