#!/bin/bash
# Build cores using VINNIEPC as the compile engine.
#
# This is build_via_windows6.sh from the wave-2 farm with five changes, listed
# here so it can be reviewed as a diff rather than read as a new program. The
# transport, the container handling and the timeout policy are untouched,
# because they are what took six iterations to get right.
#
#   1. WIN is an environment variable. The hard-coded 192.168.1.19 is a Wi-Fi
#      lease that has since moved to .23; a farm that cannot be pointed at the
#      machine is a farm that stops working on its own.
#   2. The patch is verified before the hour of compute is spent. patch_sys.py
#      edits at regex anchors and reports a missing anchor, but nothing checked
#      that the result was actually wired up, so a core could compile cleanly
#      with no pad in it.
#   3. The artifact is named from the Quartus revision and a datecode that beats
#      upstream's release, not from the repository name. Wave 2 named it
#      ${REPO%_MiSTer}.rbf, which is why three cores shipped under names no .mra
#      can resolve - see outname.py and naming.py.
#   4. The manifest gains upstream_release and out_rbf columns, appended after
#      the existing five so awk '$5' and every existing reader keep working.
#   5. Known-bad cores are refused unless FORCE=1. Thirteen cores in the record
#      fail to compile from a clean checkout; retrying them every sweep costs
#      hours of someone else's PC for a guaranteed failure.
#
# Deliberately NO bind mounts: on macOS they cut Quartus to 11% CPU, and on
# colima they silently resolved to an empty directory. docker cp avoids both.
# The image's own WORKDIR /build is a named volume; the build runs in /work,
# which is where the source is streamed, so that volume is incidental.
#
# Quartus is native x86 here, so no NUM_PARALLEL_PROCESSORS=1 workaround is
# needed - the futex collapse was a Rosetta artefact. 4 CPUs is safe on an
# 8-core/32GB box.

WIN=${WIN:-dinoi@192.168.1.23}
BASE=${BASE:-/Users/mister/workspace/snac-refresh}
TOOLS=${TOOLS:-$(cd "$(dirname "$0")" && pwd)}
MODULE=${MODULE:-$BASE/psx_snac_pad.sv}
WORK=$BASE/work-win
OUT=${OUT:-$BASE/cores}
LOGS=${LOGS:-$BASE/logs-win}
JOB=${JOB:-snacjob_win}
MANIFEST=${MANIFEST:-$BASE/builds-snac.tsv}
LIST=${1:?usage: build_via_windows_snac.sh <listfile>}
NPROC=${NPROC:-2}
STALL=5400
HARD_CAP=10800
FORCE=${FORCE:-0}

[ -f "$MODULE" ] || { echo "no framework module at $MODULE"; exit 1; }
mkdir -p "$WORK" "$OUT" "$LOGS"
[ -f "$MANIFEST" ] || printf 'core\trepo\tupstream_sha\tupstream_date\tresult\tupstream_release\tout_rbf\n' > "$MANIFEST"
SSHOPT="-o ControlMaster=auto -o ControlPath=/tmp/snacssh-%r@%h:%p -o ControlPersist=30m"
w() { ssh -n $SSHOPT -o ConnectTimeout=20 "$WIN" "$@" 2>&1 | grep -av -iE "post-quantum|store now, decrypt|openssh.com|^\*\* "; }

# Cores whose last recorded result was a compile failure, from stale.py.
known_fail() {
	[ "$FORCE" = 1 ] && return 1
	[ -f "$BASE/stale-report.tsv" ] || return 1
	awk -F'\t' -v c="$1" '$1==c && $3=="KNOWN_FAIL"{f=1} END{exit !f}' \
		"$BASE/stale-report.tsv"
}

record() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >> "$MANIFEST"; }

build_one() {
	REPO=$1
	NAME=${REPO%_MiSTer}; NAME=${NAME%_MISTer}; NAME=${NAME%_Mister}
	LOG=$LOGS/$NAME.log
	say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

	if known_fail "$NAME"; then
		echo "[$(date +%H:%M:%S)] RESULT $NAME SKIPPED_KNOWN_FAIL (FORCE=1 to override)"
		return 0
	fi
	: > "$LOG"

	rm -rf "${WORK:?}/$REPO"
	say "clone $REPO"
	ok=0
	for a in 1 2 3 4 5; do
		git clone --depth 1 -q "https://github.com/MiSTer-devel/$REPO.git" "$WORK/$REPO" >>"$LOG" 2>&1 && { ok=1; break; }
		say "clone attempt $a failed"; rm -rf "${WORK:?}/$REPO"; sleep $((a * 20))
	done
	[ $ok -eq 0 ] && { say "RESULT $NAME CLONE_FAIL"; return 2; }

	SHA=$(git -C "$WORK/$REPO" rev-parse HEAD)
	SHA_DATE=$(git -C "$WORK/$REPO" log -1 --format=%cI)
	say "upstream $SHA"

	if ! python3 "$TOOLS/../patch_sys.py" "$WORK/$REPO" "$MODULE" >>"$LOG" 2>&1; then
		say "RESULT $NAME PATCH_FAIL"
		record "$NAME" "$REPO" "$SHA" "$SHA_DATE" PATCH_FAIL - -
		rm -rf "${WORK:?}/$REPO"; return 3
	fi
	if ! python3 "$TOOLS/verify_patch.py" "$WORK/$REPO" "$MODULE" >>"$LOG" 2>&1; then
		say "RESULT $NAME PATCH_VERIFY_FAIL"
		record "$NAME" "$REPO" "$SHA" "$SHA_DATE" PATCH_VERIFY_FAIL - -
		rm -rf "${WORK:?}/$REPO"; return 3
	fi

	OUT_RBF=$(python3 "$TOOLS/outname.py" "$WORK/$REPO" --work "$BASE" --core "$NAME" 2>>"$LOG")
	if [ -z "$OUT_RBF" ]; then
		say "RESULT $NAME NO_QPF"
		record "$NAME" "$REPO" "$SHA" "$SHA_DATE" NO_QPF - -
		rm -rf "${WORK:?}/$REPO"; return 4
	fi
	REL=$(python3 "$TOOLS/outname.py" "$WORK/$REPO" --work "$BASE" --core "$NAME" --print datecode 2>/dev/null)
	say "will install as $OUT_RBF"

	if [ -f "$OUT/$OUT_RBF" ]; then
		say "RESULT $NAME ALREADY_BUILT $OUT_RBF"
		rm -rf "${WORK:?}/$REPO"; return 0
	fi

	w "docker rm -f $JOB" >/dev/null 2>&1
	say "create container on VINNIEPC"
	cp "$TOOLS/win_container_build.sh" "$WORK/$REPO/build.sh"
	w "docker create --name $JOB --cpus $NPROC -e NPROC=$NPROC raetro/quartus:17.0 bash /work/build.sh" >>"$LOG" 2>&1

	say "stream source to VINNIEPC"
	# docker cp will not create a missing destination directory, so the tar
	# carries "work/" as its top level and is unpacked at "/"
	STAGE="$WORK/stage-$NAME"
	rm -rf "$STAGE"; mkdir -p "$STAGE"
	mv "$WORK/$REPO" "$STAGE/work"
	if ! tar cf - -C "$STAGE" work | ssh -o ControlPath=none -o ConnectTimeout=20 "$WIN" "docker cp - $JOB:/" >>"$LOG" 2>&1; then
		say "RESULT $NAME COPYIN_FAIL"; w "docker rm -f $JOB" >/dev/null 2>&1; rm -rf "$STAGE"; return 9
	fi

	say "compile $NAME ($NPROC cpus, native x86)"
	w "docker start $JOB" >>"$LOG" 2>&1

	# Liveness from container state alone: the per-poll `docker exec` was half
	# the API traffic that wedged the engine. A hung build is caught by HARD_CAP.
	start=$SECONDS; verdict=running
	while [ -n "$(w "docker ps -q -f name=$JOB" | tr -d '[:space:]')" ]; do
		[ $((SECONDS-start)) -gt $HARD_CAP ] && { verdict=overran; break; }
		sleep 120
	done

	w "docker logs $JOB" >>"$LOG" 2>&1
	m=$(((SECONDS-start)/60))

	if grep -q RBF_OK "$LOG"; then
		ssh -n -o ControlPath=none -o ConnectTimeout=20 "$WIN" "docker cp $JOB:/work/BUILT.rbf -" 2>/dev/null | tar xf - -O BUILT.rbf > "$OUT/$OUT_RBF" 2>>"$LOG"
		if [ -s "$OUT/$OUT_RBF" ]; then
			say "RESULT $NAME OK ${m}m $(wc -c < "$OUT/$OUT_RBF" | tr -d ' ')b -> $OUT_RBF"
			record "$NAME" "$REPO" "$SHA" "$SHA_DATE" OK "$REL" "$OUT_RBF"
		else
			rm -f "$OUT/$OUT_RBF"
			say "RESULT $NAME COPYOUT_FAIL after ${m}m"
			record "$NAME" "$REPO" "$SHA" "$SHA_DATE" COPYOUT_FAIL "$REL" "$OUT_RBF"
		fi
	elif [ "$verdict" != running ]; then
		say "RESULT $NAME $verdict after ${m}m"
		record "$NAME" "$REPO" "$SHA" "$SHA_DATE" "$verdict" "$REL" "$OUT_RBF"
	else
		say "RESULT $NAME BUILD_FAIL after ${m}m"
		record "$NAME" "$REPO" "$SHA" "$SHA_DATE" BUILD_FAIL "$REL" "$OUT_RBF"
	fi

	w "docker rm -f $JOB" >/dev/null 2>&1
	rm -rf "${WORK:?}/$REPO" "${WORK:?}/stage-$NAME"
}

total=$(grep -cve '^\s*$' "$LIST")
n=0
while read -r repo; do
	[ -z "$repo" ] && continue
	n=$((n+1))
	echo "=== [$n/$total] $repo ($(date '+%F %H:%M:%S')) ==="
	build_one "$repo"
done < "$LIST"
echo "=== WINDOWS BATCH COMPLETE $(date '+%F %H:%M:%S') ==="
