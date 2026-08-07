#!/bin/sh
#
# Undoes classic_home_protect.sh: takes the Classic Home block out of
# /media/fat/linux/user-startup.sh, and puts the official firmware back if we still
# have the copy we displaced.
#
#   classic_home_unprotect.sh [--keep-firmware] [--purge]
#
#     --keep-firmware  remove the hook only; leave whatever firmware is on the card
#     --purge          also delete /media/fat/linux/classic-home entirely
#
# Only our own marked block is removed; anything else in user-startup.sh - MiSTer_SAM's
# launcher, the user's own lines - is preserved byte for byte, and the file itself is
# left in place even if our block was all it held, because deleting a file other tools
# also write is not ours to do.
#
# About .MiSTer.old: that is NOT the official firmware. update_all backs up the file it
# replaces, which is ours, so restoring from it would put Classic Home back. The
# official build this script can offer is the one the boot hook set aside as
# MiSTer.official when it displaced it. Without that, fetch the official firmware with
# update_all or the MiSTer Downloader.
#
# CHOME_ROOT overrides /media/fat, for the test harness in support/classicui/test/.

set -u

ROOT=${CHOME_ROOT:-/media/fat}
STORE=$ROOT/linux/classic-home
STARTUP=$ROOT/linux/user-startup.sh
MIN_SIZE=131072

HOOK_TOKEN_BEGIN=CLASSIC-HOME-RESTORE-HOOK-BEGIN
HOOK_TOKEN_END=CLASSIC-HOME-RESTORE-HOOK-END

KEEP_FW=0
PURGE=0

die() {
	echo "classic_home_unprotect: $1" >&2
	exit 1
}

# The header comment is the manual; see classic_home_protect.sh.
usage() {
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

while [ $# -gt 0 ]; do
	case $1 in
	--keep-firmware) KEEP_FW=1 ;;
	--purge) PURGE=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "unknown option '$1'" ;;
	esac
	shift
done

size_of() {
	wc -c <"$1" 2>/dev/null | tr -d ' \t'
}

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

syntax_ok() {
	sh -n "$1" >/dev/null 2>&1
}

changed=''
note() { changed="$changed  $1
"; }

# ---------------------------------------------------------------- the hook

if [ ! -f "$STARTUP" ]; then
	note "$STARTUP does not exist - no hook to remove"
else
	nb=$(grep -c "$HOOK_TOKEN_BEGIN" "$STARTUP" 2>/dev/null)
	ne=$(grep -c "$HOOK_TOKEN_END" "$STARTUP" 2>/dev/null)
	[ -n "$nb" ] || nb=0
	[ -n "$ne" ] || ne=0
	if [ "$nb" = 0 ] && [ "$ne" = 0 ]; then
		note "$STARTUP holds no Classic Home block - left untouched"
	elif [ "$nb" != "$ne" ]; then
		die "$STARTUP has $nb hook-begin markers and $ne hook-end ones - it has been edited by hand into a state this script will not guess at. Remove the block yourself."
	else
		old_ok=1
		syntax_ok "$STARTUP" || old_ok=0
		new=$STARTUP.new.$$
		awk -v b="$HOOK_TOKEN_BEGIN" -v e="$HOOK_TOKEN_END" '
			index($0, b) { skip = 1; next }
			index($0, e) { skip = 0; next }
			skip { next }
			{ print }
		' "$STARTUP" >"$new" 2>/dev/null || {
			rm -f "$new" 2>/dev/null
			die "cannot rewrite $STARTUP"
		}
		if [ "$old_ok" = 1 ] && ! syntax_ok "$new"; then
			rm -f "$new"
			die "the edited $STARTUP does not parse, and the original did - nothing was changed. Please report this."
		fi
		mv -f "$new" "$STARTUP" || {
			rm -f "$new"
			die "cannot replace $STARTUP"
		}
		chmod 755 "$STARTUP" 2>/dev/null || true
		note "removed the Classic Home block from $STARTUP"
		if ! grep -qEv '^[[:space:]]*(#.*)?$' "$STARTUP" 2>/dev/null; then
			note "$STARTUP now holds only comments; left in place because other tools use it too"
		fi
	fi
fi

# ---------------------------------------------------------------- the firmware

restore_official() {
	_name=$1
	_kind=$2
	_src=$STORE/$_name.official
	_dst=$ROOT/$_name
	[ -f "$_src" ] || return 2
	_sz=$(size_of "$_src")
	case "$_sz" in
	'' | *[!0-9]*) return 1 ;;
	esac
	[ "$_sz" -ge "$MIN_SIZE" ] || return 1
	if [ "$_kind" = arm-elf ]; then
		elf_arm_ok "$_src" || return 1
	fi
	if [ -f "$_dst" ] && cmp -s "$_src" "$_dst"; then
		note "$_dst is already the official build"
		return 0
	fi
	_tmp=$ROOT/.chome-unprotect.$_name.$$
	rm -f "$_tmp" 2>/dev/null
	cp "$_src" "$_tmp" 2>/dev/null || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	cmp -s "$_src" "$_tmp" || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	chmod 755 "$_tmp" 2>/dev/null || true
	if [ "$_kind" = arm-elf ] && [ ! -x "$_tmp" ]; then
		rm -f "$_tmp" 2>/dev/null
		return 1
	fi
	mv -f "$_tmp" "$_dst" 2>/dev/null || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	note "put the official $_name back from $_src ($_sz bytes)"
	return 0
}

fw_advice=0
if [ "$KEEP_FW" = 1 ]; then
	note "left $ROOT/MiSTer as it is (--keep-firmware)"
else
	restore_official MiSTer arm-elf
	case $? in
	0) ;;
	2)
		fw_advice=1
		note "no official firmware kept at $STORE/MiSTer.official - $ROOT/MiSTer left as it is"
		;;
	*) note "WARNING: $STORE/MiSTer.official exists but did not verify - $ROOT/MiSTer left as it is" ;;
	esac
	if [ -f "$STORE/menu.rbf.official" ]; then
		restore_official menu.rbf rbf ||
			note "WARNING: $STORE/menu.rbf.official did not verify - $ROOT/menu.rbf left as it is"
	fi
fi

# ---------------------------------------------------------------- the store

if [ "$PURGE" = 1 ]; then
	if [ -d "$STORE" ]; then
		rm -rf "$STORE" 2>/dev/null && note "deleted $STORE" || note "WARNING: could not delete $STORE"
	fi
elif [ -d "$STORE" ]; then
	note "kept $STORE (the stored firmware and the log); --purge deletes it"
fi
sync 2>/dev/null || true

echo "Classic Home update protection is removed."
echo
printf '%s' "$changed"
if [ "$fw_advice" = 1 ]; then
	echo
	echo "The card still has whatever firmware was on it. If that is the Classic Home build"
	echo "and you want the official one, run update_all or the MiSTer Downloader - and note"
	echo "that $ROOT/.MiSTer.old is not it: an updater puts the file it replaced there, so"
	echo "that copy is ours."
fi
exit 0
