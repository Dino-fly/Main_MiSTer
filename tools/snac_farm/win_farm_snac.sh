#!/bin/bash
# Run N concurrent builds on VINNIEPC, one build_via_windows_snac.sh per worker.
#
# This is win_farm.sh from the wave-2 farm with three changes:
#
#   1. WIN and BASE are passed through, so it can be pointed at the current
#      lease (.23) and at the refresh work directory instead of the wave-2 one.
#   2. The split is not round-robin. Quartus build times across these cores span
#      8 minutes to nearly four hours, and round-robin can drop two long cores
#      into one worker while another idles. The list is read as-is and long
#      cores named in SLOW_FIRST are pulled to the front of their own worker.
#   3. Workers are recorded in workers.pid so they can be stopped by PID.
#      pkill -f leaves the tar pipelines running into containers that no longer
#      have an owner, which is how Docker got wedged in wave 2.
#
# Sizing: each Quartus process is effectively single-threaded, so throughput is
# parallel builds, not threads per build. ~2.5GB peak each; the Docker VM here
# has 23.5GiB and the wave-2 farm topped out at 8 workers, so 6 is inside both.

BASE=${BASE:-/Users/derek/workspace/snac-refresh}
TOOLS=$(cd "$(dirname "$0")" && pwd)
LIST=${1:?usage: win_farm_snac.sh <listfile> [workers]}
N=${2:-6}
TAG=${TAG:-win}
# Cores known to run long, given a worker to themselves and started first.
SLOW_FIRST=${SLOW_FIRST:-Arcade-RushnAttack_MiSTer}

cd "$BASE" || exit 1
rm -f "${TAG}"_part_* workers.pid

slow=$(grep -xF "$SLOW_FIRST" "$LIST" 2>/dev/null)
grep -vxF "$SLOW_FIRST" "$LIST" | grep -ve '^\s*$' > "$BASE/.rest.$$"

# Worker 0 gets the slow core alone; the rest are dealt round-robin over 1..N-1.
[ -n "$slow" ] && printf '%s\n' "$slow" > "$(printf '%s_part_%02d' "$TAG" 0)"
start=$([ -n "$slow" ] && echo 1 || echo 0)
awk -v n="$N" -v s="$start" -v tag="$TAG" \
    '{ printf "%s\n", $0 > sprintf("%s_part_%02d", tag, s + (NR % (n - s))) }' \
    "$BASE/.rest.$$"
rm -f "$BASE/.rest.$$"

for i in $(seq 0 $((N-1))); do
	f=$(printf '%s_part_%02d' "$TAG" "$i")
	[ -s "$f" ] || continue
	JOB="snacjob_${TAG}_$i" NPROC=${NPROC:-2} HARD_CAP=${HARD_CAP:-18000} \
		nohup "$TOOLS/build_via_windows_snac.sh" "$f" \
		> "${TAG}_worker_$i.log" 2>&1 &
	echo "$!" >> workers.pid
	echo "worker $i (pid $!) started on $(wc -l < "$f" | tr -d ' ') cores: $(tr '\n' ' ' < "$f")"
	sleep 3
done
echo "pids in $BASE/workers.pid - stop with: kill \$(cat $BASE/workers.pid)"
