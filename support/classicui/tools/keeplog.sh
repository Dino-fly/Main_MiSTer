#!/bin/sh
#
# Keep the firmware's log across core loads and reboots.
#
# Why this is not a copy. MiSTer writes /tmp/debug.txt on a RAM disk, and app_restart()
# re-execs the firmware on every core load - which truncates it. So the log of the session
# that interests you is destroyed by the very next thing you do, and a mirror of it is
# destroyed at the same moment. The first version of this script mirrored, and the log of a
# Saturn launch was gone before it could be read.
#
# So this appends. Every tick it compares the size of /tmp/debug.txt against what it had
# last time:
#
#   grew        append only the new bytes
#   shrank      the firmware restarted, so the file is a new session: mark it and take all
#   same        nothing to do
#
# The accumulated file therefore holds every session since boot, in order, with a marker
# between them - which is what reading a crash needs, because the interesting lines are the
# ones written before the machine went away.
#
# Rotated at a size cap rather than at boot, for the same reason: a reboot is the event
# under investigation, and rotating on it throws away the evidence.

OUT=/media/fat/classicui/debug-all.txt
SRC=/tmp/debug.txt
CAP=16777216          # 16 MB, then rotate to .1 - the card has room and a log is cheap
TICK=2

mkdir -p "$(dirname "$OUT")"
last=0

# A boot marker, so sessions can be told apart from restarts within one boot.
{
	echo ""
	echo "=== keeplog: boot at $(date '+%Y-%m-%d %H:%M:%S'), uptime $(cut -d' ' -f1 /proc/uptime)s ==="
} >> "$OUT"

while :; do
	if [ -f "$SRC" ]; then
		now=$(wc -c < "$SRC" 2>/dev/null || echo 0)

		if [ "$now" -lt "$last" ]; then
			# Truncated: the firmware re-execed. Everything in there is new.
			{
				echo ""
				echo "=== keeplog: firmware restarted (core load) at $(date '+%H:%M:%S'), uptime $(cut -d' ' -f1 /proc/uptime)s ==="
			} >> "$OUT"
			cat "$SRC" >> "$OUT" 2>/dev/null
			last=$now
		elif [ "$now" -gt "$last" ]; then
			# Grew: append the tail only. tail -c +N counts from 1, hence the +1.
			tail -c "+$((last + 1))" "$SRC" >> "$OUT" 2>/dev/null
			last=$now
		fi
	else
		last=0
	fi

	sz=$(wc -c < "$OUT" 2>/dev/null || echo 0)
	if [ "$sz" -gt "$CAP" ]; then
		mv "$OUT" "$OUT.1"
		echo "=== keeplog: rotated at $(date '+%Y-%m-%d %H:%M:%S') ===" > "$OUT"
	fi

	sleep "$TICK"
done
