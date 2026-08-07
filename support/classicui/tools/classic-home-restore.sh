#!/bin/sh
#
# Puts the Classic Home firmware back after an updater has replaced it.
#
# MiSTer's official updater (update_all -> downloader) owns the file `MiSTer` in the
# card root: it is listed in the distribution_mister database, so a run replaces our
# build with the official one, moves ours to /media/fat/.MiSTer.old and asks for a
# reboot. `menu.rbf` is owned the same way. Nothing about that is a bug we can fix on
# their side - a third-party database is forbidden from supplying either file
# (db_entity.py: no_distribution_mister_invalid_paths) - so the only place to put the
# firmware back is our own hook at boot.
#
# This script is that hook's worker. It is called from /media/fat/linux/user-startup.sh
# and it is installed, together with the stored firmware, by classic_home_protect.sh.
#
# WHY THE STORE LIVES UNDER linux/
#
# The downloader refuses to write anything whose first path component is `linux`,
# `screenshots`, `savestates` or `downloader` (invalid_root_folders), for EVERY
# database including the official one. `classicui/` has no such protection: any
# database may ship `classicui/<anything>`, and we already ship
# classicui/disctitles.txt ourselves, so the day a Classic Home database exists that
# folder becomes downloader-managed and files in it that the database no longer lists
# are candidates for deletion. A spare copy of the firmware is exactly the kind of
# unlisted file that would disappear. So the store is
# /media/fat/linux/classic-home/ - one subfolder, inside a root folder no database may
# touch, and not in the boot payload's own namespace where u-boot and the kernel image
# live (u-boot loads fixed filenames; a subfolder it never opens is inert).
#
# WHAT IT WILL AND WILL NOT DO
#
# It restores a file only when the copy on the card is NOT one of ours. For the
# firmware that is decided by a string only our build contains (the CLASSICUI ini keys
# are in its .rodata; checked against the official 20260603 and 20260707 builds, which
# do not contain it). So a user who copies a newer Classic Home firmware onto the card
# by hand, without re-running the installer, is left alone rather than silently
# downgraded on the next boot.
#
# menu.rbf has no equivalent marker - it is an FPGA bitstream with no strings in it -
# so for that file "differs from our stored copy" is the trigger, and hand-copying a
# newer menu.rbf without re-running the installer WILL be reverted at the next boot.
# The installer is the documented way to update; that limit is stated in the guide.
#
# Nothing is ever restored from a copy that has not been verified first: the size must
# equal what was recorded when it was stored, the md5 must match, and the firmware
# must additionally be a little-endian 32-bit ARM ELF executable. The new file is
# written to a temp name, compared byte for byte against the source, and only then
# renamed into place - a rename inside one filesystem is atomic, so there is no
# instant at which the card has no `MiSTer`, and it also does not disturb a firmware
# process that is already running from that inode (copying over a running executable
# would fail with ETXTBSY, or worse, truncate it).
#
# It is silent when there is nothing to do. Everything it actually does, and every
# check that stopped it, goes to restore.log in the store, which is on the card and so
# survives the reboot that follows an update. Timestamps can read 1970: there is no
# battery-backed clock on this hardware and the network is usually not up yet.
#
# It never reboots and never restarts the firmware process. A restore that happens
# after /media/fat/MiSTer has already been launched takes effect at the following
# boot; killing and relaunching the firmware by hand leaves it with no supervisor
# (inittab starts it once with ::sysinit and does not respawn it), which is a far worse
# failure than one boot of the stock menu.
#
# Usage: classic-home-restore.sh [-v] [-n]
#   -v  also say on stdout what is being done
#   -n  dry run: check everything, change nothing
#
# CHOME_ROOT overrides /media/fat. That exists for the test harness in
# support/classicui/test/; on a device it is never set.

ROOT=${CHOME_ROOT:-/media/fat}
STORE=$ROOT/linux/classic-home
LIST=$STORE/protected.list
LOG=$STORE/restore.log

# Only our build has this in it. Keep it in step with cfg.cpp's ini table.
MARKER=CLASSICUI_SCREENSCRAPER

# Below this, a file is not a firmware or a core however well its header reads. Ours is
# ~1.0 MB and a menu core ~1.5 MB; the real guard is the recorded size and md5, this
# only rejects the obviously absurd before any of that runs.
MIN_SIZE=131072

# Keep the log bounded: it lives on the card and nothing else prunes it.
LOG_MAX=400
LOG_KEEP=200

verbose=0
dry=0
for a in "$@"; do
	case "$a" in
	-v) verbose=1 ;;
	-n) dry=1 ;;
	*) ;;
	esac
done

say() {
	[ "$verbose" = 1 ] && echo "classic-home-restore: $1"
	return 0
}

log() {
	say "$1"
	[ -d "$STORE" ] || return 0
	# A read-only or full card must not turn a logging failure into a script failure.
	echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') $1" >>"$LOG" 2>/dev/null || return 0
	n=$(wc -l <"$LOG" 2>/dev/null | tr -d ' \t')
	case "$n" in
	'' | *[!0-9]*) return 0 ;;
	esac
	if [ "$n" -gt "$LOG_MAX" ]; then
		if tail -n "$LOG_KEEP" "$LOG" >"$LOG.trim" 2>/dev/null; then
			mv -f "$LOG.trim" "$LOG" 2>/dev/null || rm -f "$LOG.trim" 2>/dev/null
		else
			rm -f "$LOG.trim" 2>/dev/null
		fi
	fi
	return 0
}

size_of() {
	wc -c <"$1" 2>/dev/null | tr -d ' \t'
}

# md5sum is busybox's name for it and is what the device has; md5 is BSD/macOS, which
# is where the test harness runs. Absent both, callers fall back to size-only checking
# for the firmware and skip the core entirely.
md5_of() {
	if command -v md5sum >/dev/null 2>&1; then
		md5sum "$1" 2>/dev/null | cut -d' ' -f1
	elif command -v md5 >/dev/null 2>&1; then
		md5 -q "$1" 2>/dev/null
	else
		echo ''
	fi
}

have_md5() {
	command -v md5sum >/dev/null 2>&1 || command -v md5 >/dev/null 2>&1
}

# 7f 45 4c 46 | class 1 (32-bit) | data 1 (little endian) | e_type 2 or 3 | e_machine 40
# (EM_ARM). od is used rather than hexdump or `file` because busybox always has it and
# its -tx1 output is the same everywhere.
elf_arm_ok() {
	h=$(dd if="$1" bs=1 count=20 2>/dev/null | od -An -v -tx1 2>/dev/null | tr -d ' \t\n')
	[ ${#h} -eq 40 ] || return 1
	case "$h" in
	7f454c460101*) ;;
	*) return 1 ;;
	esac
	et=$(printf '%s' "$h" | cut -c33-36)
	em=$(printf '%s' "$h" | cut -c37-40)
	case "$et" in
	0200 | 0300) ;;
	*) return 1 ;;
	esac
	[ "$em" = 2800 ]
}

# grep, not strings: strings is not on the device.
#
# Twice, because one form is not enough anywhere. BSD grep (which is what the test
# harness on a Mac has) returns 1 for a match in a file it decides is binary unless it
# is given -a; busybox's grep does not list -a in its usage, treats every file as text
# anyway, and errors out on options it does not know. So: ask with -a, and if that call
# itself fails, ask again without it.
#
# If neither can see inside the file we report "not ours", which lets the restore go
# ahead. That is the right way round: not restoring at all defeats the whole hook, while
# the case it gets wrong is a firmware copied on by hand instead of through the
# installer.
has_marker() {
	grep -q -a "$MARKER" "$1" 2>/dev/null && return 0
	grep -q "$MARKER" "$1" 2>/dev/null
}

# The user's own off switch. Someone who has deliberately gone back to the official
# firmware and left the store in place should not be argued with every boot; the
# supported route is classic_home_unprotect.sh, this is the one-file escape hatch.
[ -e "$STORE/disabled" ] && exit 0

if [ ! -f "$LIST" ]; then
	# The hook is installed but the store is not. Worth one line per boot - the log is
	# capped - because it is a broken install rather than a normal state.
	log "nothing to do: $LIST is missing (re-run classic_home_protect.sh)"
	exit 0
fi

md5_ok=1
have_md5 || md5_ok=0

# Fields: kind md5 size stored-name card-path. Written by classic_home_protect.sh; none
# of the names may contain a space.
#
# Piped through tr because the card is FAT and everything on it gets opened in a Windows
# editor sooner or later - MiSTer.ini is CRLF for exactly that reason. A stray CR would
# otherwise ride along on the last field and send the restore to a file called
# "MiSTer\r". The loop runs in a subshell as a result, which costs nothing: nothing after
# it reads anything the loop set.
#
# `|| [ -n "$kind" ]` so a last line with no newline on it is still processed.
tr -d '\r' <"$LIST" | while read -r kind md5 size name dest rest || [ -n "$kind" ]; do
	case "$kind" in
	'' | \#*) continue ;;
	esac
	[ -n "$dest" ] || continue

	# The destinations we manage are files in the card root. Anything with a path
	# separator, or a name that could climb out of it, is a corrupt manifest: stop
	# rather than work out what was meant.
	case "$dest" in
	*/* | .. | .)
		log "refusing manifest entry '$dest': not a card-root filename"
		continue
		;;
	esac

	src=$STORE/$name
	dst=$ROOT/$dest

	# A directory called MiSTer should not be possible, but if it were, the mv at the end
	# would move our firmware *into* it and the card would still have no firmware.
	if [ -e "$dst" ] && [ ! -f "$dst" ]; then
		log "cannot restore $dest: $dst exists and is not a regular file"
		continue
	fi

	if [ ! -f "$src" ]; then
		log "cannot restore $dest: stored copy $src is missing"
		continue
	fi

	# Same file already in place: the common case, and it says nothing.
	if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
		continue
	fi

	reason="differs from our stored copy"
	if [ ! -f "$dst" ]; then
		reason="was missing from the card root"
	elif [ "$kind" = arm-elf ] && has_marker "$dst"; then
		# A Classic Home build we did not store. Theirs is newer than ours, or they
		# built their own; either way it is not the updater's official file and
		# putting our copy back would be a downgrade behind their back.
		log "leaving $dest alone: it is a Classic Home build we did not store (re-run classic_home_protect.sh to protect it)"
		continue
	fi

	# From here on the stored copy is about to be trusted, so verify it completely.
	ssize=$(size_of "$src")
	case "$ssize" in
	'' | *[!0-9]*)
		log "cannot restore $dest: cannot size $src"
		continue
		;;
	esac
	if [ "$ssize" != "$size" ]; then
		log "cannot restore $dest: $src is $ssize bytes, manifest says $size - stored copy is damaged"
		continue
	fi
	if [ "$ssize" -lt "$MIN_SIZE" ]; then
		log "cannot restore $dest: $src is only $ssize bytes"
		continue
	fi
	if [ "$kind" = arm-elf ] && ! elf_arm_ok "$src"; then
		log "cannot restore $dest: $src is not a 32-bit ARM ELF executable"
		continue
	fi
	if [ "$md5_ok" = 1 ]; then
		sm=$(md5_of "$src")
		if [ "$sm" != "$md5" ]; then
			log "cannot restore $dest: $src has md5 $sm, manifest says $md5 - stored copy is damaged"
			continue
		fi
	elif [ "$kind" != arm-elf ]; then
		# Without md5 the only integrity evidence for a bitstream is its size, and a
		# wrong menu.rbf is a machine with no menu. The firmware still has its ELF
		# header to vouch for it, so only this case is skipped.
		log "skipping $dest: no md5sum on this system to verify the stored copy with"
		continue
	fi

	if [ "$dry" = 1 ]; then
		log "would restore $dest from $src ($reason)"
		continue
	fi

	tmp=$ROOT/.chome-restore.$dest.$$
	rm -f "$tmp" 2>/dev/null
	if ! cp "$src" "$tmp" 2>/dev/null; then
		rm -f "$tmp" 2>/dev/null
		log "cannot restore $dest: writing $tmp failed (card full or read-only?)"
		continue
	fi
	if ! cmp -s "$src" "$tmp"; then
		rm -f "$tmp" 2>/dev/null
		log "cannot restore $dest: the copy at $tmp did not come back identical"
		continue
	fi
	chmod 755 "$tmp" 2>/dev/null || true
	if [ "$kind" = arm-elf ] && [ ! -x "$tmp" ]; then
		rm -f "$tmp" 2>/dev/null
		log "cannot restore $dest: $tmp will not take an execute bit"
		continue
	fi

	# Keep whatever we are displacing, so the uninstaller has an official build to put
	# back. Best effort only: its failure must not stop the restore. .MiSTer.old is no
	# use for this - the updater puts the file it replaced there, which is ours.
	if [ -f "$dst" ]; then
		keep=''
		if [ "$kind" = arm-elf ]; then
			if elf_arm_ok "$dst" && ! has_marker "$dst"; then keep=1; fi
		else
			ksz=$(size_of "$dst")
			case "$ksz" in
			'' | *[!0-9]*) ;;
			*) [ "$ksz" -ge "$MIN_SIZE" ] && keep=1 ;;
			esac
		fi
		if [ -n "$keep" ]; then
			ktmp=$STORE/.official.$name.$$
			if cp "$dst" "$ktmp" 2>/dev/null && cmp -s "$dst" "$ktmp"; then
				mv -f "$ktmp" "$STORE/$name.official" 2>/dev/null || rm -f "$ktmp" 2>/dev/null
			else
				rm -f "$ktmp" 2>/dev/null
			fi
		fi
	fi

	if mv -f "$tmp" "$dst" 2>/dev/null; then
		sync 2>/dev/null || true
		log "restored $dest from $src ($ssize bytes; it $reason)"
	else
		rm -f "$tmp" 2>/dev/null
		log "cannot restore $dest: renaming $tmp over $dst failed"
	fi
done

exit 0
