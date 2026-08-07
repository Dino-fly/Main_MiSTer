#!/bin/sh
#
# Makes a Classic Home install survive update_all.
#
# Run it once, after copying the release onto the card. It keeps a verified copy of the
# firmware (and the menu core, if this release carries one) in a folder no MiSTer
# database is allowed to write, and adds a block to /media/fat/linux/user-startup.sh
# that puts the file back at boot if an updater has replaced it.
#
# Why it is needed: `MiSTer` and `menu.rbf` belong to the official distribution_mister
# database. An update_all run overwrites both with the official build, moves ours to
# .MiSTer.old and asks for a reboot, which silently reverts the front-end. No
# third-party database is permitted to supply either path, so there is no way to ship
# this as a database entry; a boot hook is the mechanism that is left.
#
# Why user-startup.sh: /etc/init.d/S99user runs /media/fat/linux/user-startup.sh at
# boot, and `linux` is one of the four root folders the downloader refuses to write for
# every database (invalid_root_folders), as is our store beneath it. MiSTer.ini is
# protected the same way, which is why the front-end's own settings survive an update
# and only these two files do not.
#
# Other projects use user-startup.sh too (MiSTer_SAM installs its launcher there), so
# the block is delimited by markers: installing again replaces our own block in place
# of duplicating it, and nothing else in the file is touched. The file is syntax
# checked before and after, and left alone if our edit is what broke it.
#
#   classic_home_protect.sh [--from PATH] [--menu PATH] [--no-menu] [--worker PATH] [--force]
#
#     --from PATH    firmware to protect, default the `MiSTer` in the card root
#     --menu PATH    menu core to protect, default the `menu.rbf` in the card root
#     --no-menu      protect the firmware only, even in a release that carries a core
#     --worker PATH  the boot script to install, default the copy the archive placed in
#                    linux/classic-home/ or the one beside this script
#     --force        skip the "is this really our build" check on the firmware
#
# CHOME_ROOT overrides /media/fat, for the test harness in support/classicui/test/.

set -u

# Stamped by make_sdcard_root.sh when the release is packaged, so the installer refuses
# to protect a firmware that is not the one shipped beside it. Beta 2 nearly went out
# assembled from whatever bin/MiSTer happened to hold after the tree had moved on; the
# same mistake on the card is a user protecting the official binary and wondering why
# the front-end never comes back.
EXPECT_MD5=""
EXPECT_MENU_MD5=""
RELEASE_NAME=""

# Present only in our firmware - these ini keys are string literals in cfg.cpp and so
# sit in .rodata. Verified absent from the official 20260603 and 20260707 builds. It is
# the fallback when this copy of the script was not stamped with an md5.
#
# One per product: the Classic Home build carries both, the SNAC-only build carries
# SNAC_PAD alone. Testing only for the Classic Home key made this refuse to protect the
# SNAC firmware outright - "does not look like a Classic Home build" - which would have
# left every SNAC-only user with no defence against update_all at all.
MARKERS="CLASSICUI_SCREENSCRAPER SNAC_PAD"
MIN_SIZE=131072

ROOT=${CHOME_ROOT:-/media/fat}
STORE=$ROOT/linux/classic-home
LIST=$STORE/protected.list
STARTUP=$ROOT/linux/user-startup.sh
WORKER_NAME=classic-home-restore.sh

HOOK_BEGIN="# CLASSIC-HOME-RESTORE-HOOK-BEGIN (managed by classic_home_protect.sh - do not edit)"
HOOK_END="# CLASSIC-HOME-RESTORE-HOOK-END"
HOOK_TOKEN_BEGIN=CLASSIC-HOME-RESTORE-HOOK-BEGIN
HOOK_TOKEN_END=CLASSIC-HOME-RESTORE-HOOK-END

self_dir=$(dirname "$0")

FROM=''
MENU=''
WORKER=''
NOMENU=0
FORCE=0

die() {
	echo "classic_home_protect: $1" >&2
	exit 1
}

# The header comment is the manual: printing it beats a fixed line range that drifts
# every time the comment is edited.
usage() {
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

need_arg() {
	[ -n "${2:-}" ] || die "$1 needs a value"
}

while [ $# -gt 0 ]; do
	case $1 in
	--from)
		need_arg --from "${2:-}"
		FROM=$2
		shift
		;;
	--menu)
		need_arg --menu "${2:-}"
		MENU=$2
		shift
		;;
	--worker)
		need_arg --worker "${2:-}"
		WORKER=$2
		shift
		;;
	--no-menu) NOMENU=1 ;;
	--force) FORCE=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "unknown option '$1'" ;;
	esac
	shift
done

md5_of() {
	if command -v md5sum >/dev/null 2>&1; then
		md5sum "$1" 2>/dev/null | cut -d' ' -f1
	elif command -v md5 >/dev/null 2>&1; then
		md5 -q "$1" 2>/dev/null
	else
		echo ''
	fi
}

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

# -a and then without it: BSD grep needs -a to match inside a binary, busybox grep does
# not document -a and rejects options it does not know. See classic-home-restore.sh.
has_marker() {
	for m in $MARKERS; do
		grep -q -a "$m" "$1" 2>/dev/null && return 0
		grep -q "$m" "$1" 2>/dev/null && return 0
	done
	return 1
}

# Copy verified: written to a temp name in the destination folder, compared byte for
# byte, then renamed. The rename is atomic within the filesystem, so a half-written
# file is never visible under the real name.
install_file() {
	_src=$1
	_dst=$2
	_tmp=$(dirname "$_dst")/.chome-install.$(basename "$_dst").$$
	rm -f "$_tmp" 2>/dev/null
	cp "$_src" "$_tmp" 2>/dev/null || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	cmp -s "$_src" "$_tmp" || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	mv -f "$_tmp" "$_dst" 2>/dev/null || {
		rm -f "$_tmp" 2>/dev/null
		return 1
	}
	return 0
}

# Probed by name, not by hashing something: /dev/null is not usable as a test input
# because some md5sum builds refuse anything that is not a regular file.
command -v md5sum >/dev/null 2>&1 || command -v md5 >/dev/null 2>&1 ||
	die "no md5sum (or md5) on this system - cannot record what it is protecting"

# ---------------------------------------------------------------- the firmware

[ -n "$FROM" ] || FROM=$ROOT/MiSTer
[ -f "$FROM" ] || die "$FROM does not exist. Copy the release's MiSTer into the card root first."

fw_size=$(size_of "$FROM")
case "$fw_size" in
'' | *[!0-9]*) die "cannot size $FROM" ;;
esac
[ "$fw_size" -ge "$MIN_SIZE" ] || die "$FROM is only $fw_size bytes - that is not a firmware"
elf_arm_ok "$FROM" || die "$FROM is not a 32-bit ARM ELF executable"
fw_md5=$(md5_of "$FROM")

if [ -n "$EXPECT_MD5" ]; then
	if [ "$fw_md5" != "$EXPECT_MD5" ]; then
		echo "classic_home_protect: $FROM is not the firmware from this release." >&2
		echo "  found    md5 $fw_md5" >&2
		echo "  expected md5 $EXPECT_MD5${RELEASE_NAME:+  ($RELEASE_NAME)}" >&2
		echo "Copy SD-CARD-ROOT/MiSTer into the card root and run this again. If an" >&2
		echo "updater has already replaced it, that is exactly what this fixes - but it" >&2
		echo "has to store our build, not the official one." >&2
		exit 1
	fi
elif [ "$FORCE" != 1 ]; then
	has_marker "$FROM" ||
		die "$FROM does not look like one of our builds (none of: $MARKERS). Use --force if you are sure."
fi

# ---------------------------------------------------------------- the menu core

menu_md5=''
menu_size=''
menu_warn=''
if [ "$NOMENU" != 1 ] && [ -n "$EXPECT_MENU_MD5" ]; then
	[ -n "$MENU" ] || MENU=$ROOT/menu.rbf
	if [ ! -f "$MENU" ]; then
		menu_warn="$MENU does not exist, so the menu core is NOT protected"
	else
		menu_size=$(size_of "$MENU")
		menu_md5=$(md5_of "$MENU")
		if [ "$menu_md5" != "$EXPECT_MENU_MD5" ]; then
			menu_warn="$MENU has md5 $menu_md5, this release ships $EXPECT_MENU_MD5 - the menu core is NOT protected"
			menu_md5=''
		fi
	fi
fi

# ---------------------------------------------------------------- the boot worker

if [ -z "$WORKER" ]; then
	if [ -f "$self_dir/$WORKER_NAME" ]; then
		WORKER=$self_dir/$WORKER_NAME
	elif [ -f "$STORE/$WORKER_NAME" ]; then
		WORKER=$STORE/$WORKER_NAME
	else
		die "cannot find $WORKER_NAME. It should be at $STORE/$WORKER_NAME (the archive puts it there) - pass --worker PATH."
	fi
fi
[ -s "$WORKER" ] || die "$WORKER is empty"
syntax_ok "$WORKER" || die "$WORKER does not parse as a shell script - refusing to install it into the boot path"

# ---------------------------------------------------------------- do it

changed=''
note() { changed="$changed  $1
"; }

mkdir -p "$STORE" 2>/dev/null || die "cannot create $STORE"

if [ "$(readlink -f "$WORKER" 2>/dev/null || echo "$WORKER")" != "$(readlink -f "$STORE/$WORKER_NAME" 2>/dev/null || echo "$STORE/$WORKER_NAME")" ]; then
	install_file "$WORKER" "$STORE/$WORKER_NAME" || die "cannot install $STORE/$WORKER_NAME"
	note "installed $STORE/$WORKER_NAME"
fi
chmod 755 "$STORE/$WORKER_NAME" 2>/dev/null || true
[ -x "$STORE/$WORKER_NAME" ] || die "$STORE/$WORKER_NAME will not take an execute bit"

if [ -f "$STORE/MiSTer" ] && cmp -s "$FROM" "$STORE/MiSTer"; then
	note "kept $STORE/MiSTer (already the same build)"
else
	install_file "$FROM" "$STORE/MiSTer" || die "cannot store the firmware at $STORE/MiSTer"
	note "stored the firmware as $STORE/MiSTer ($fw_size bytes, md5 $fw_md5)"
fi

if [ -n "$menu_md5" ]; then
	if [ -f "$STORE/menu.rbf" ] && cmp -s "$MENU" "$STORE/menu.rbf"; then
		note "kept $STORE/menu.rbf (already the same core)"
	else
		install_file "$MENU" "$STORE/menu.rbf" || die "cannot store the menu core at $STORE/menu.rbf"
		note "stored the menu core as $STORE/menu.rbf ($menu_size bytes, md5 $menu_md5)"
	fi
fi

# Written last, after both files are in the store, because the manifest is what the boot
# hook trusts: it must never name a file that is not there yet.
new_list=$LIST.new.$$
{
	echo "# Classic Home protected files. Written by classic_home_protect.sh - do not edit."
	echo "# ${RELEASE_NAME:-unnamed release}, stored $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	echo "# kind md5 size stored-name card-path"
	echo "arm-elf $fw_md5 $fw_size MiSTer MiSTer"
	[ -n "$menu_md5" ] && echo "rbf $menu_md5 $menu_size menu.rbf menu.rbf"
	true
} >"$new_list" 2>/dev/null || die "cannot write $new_list"

if [ -f "$LIST" ] && cmp -s "$new_list" "$LIST"; then
	rm -f "$new_list"
else
	mv -f "$new_list" "$LIST" || {
		rm -f "$new_list"
		die "cannot write $LIST"
	}
	note "wrote $LIST"
fi

# ---------------------------------------------------------------- the hook

mkdir -p "$(dirname "$STARTUP")" 2>/dev/null || die "cannot create $(dirname "$STARTUP")"

old_ok=1
if [ -f "$STARTUP" ]; then
	syntax_ok "$STARTUP" || old_ok=0
	# grep -c prints 0 and exits 1 when it matches nothing, so the assignment is enough
	# and a `|| echo 0` would put two numbers in the variable.
	nb=$(grep -c "$HOOK_TOKEN_BEGIN" "$STARTUP" 2>/dev/null)
	ne=$(grep -c "$HOOK_TOKEN_END" "$STARTUP" 2>/dev/null)
	[ -n "$nb" ] || nb=0
	[ -n "$ne" ] || ne=0
	if [ "$nb" != "$ne" ]; then
		die "$STARTUP has $nb hook-begin markers and $ne hook-end ones. It has been edited by hand into a state this script will not guess at - remove the block yourself and run this again."
	fi
fi

new_startup=$STARTUP.new.$$
: >"$new_startup" 2>/dev/null || die "cannot write $new_startup"

if [ -f "$STARTUP" ]; then
	# Drop any block of ours wherever it was and keep every other line byte for byte.
	# The replacement is appended below, so re-running moves our block to the end - the
	# order does not matter to us and this cannot go wrong the way an in-place splice
	# can.
	awk -v b="$HOOK_TOKEN_BEGIN" -v e="$HOOK_TOKEN_END" '
		index($0, b) { skip = 1; next }
		index($0, e) { skip = 0; next }
		skip { next }
		{ print }
	' "$STARTUP" >>"$new_startup" || die "cannot rewrite $STARTUP"
else
	printf '#!/bin/sh\n#\n# Run at boot by /etc/init.d/S99user. Created by classic_home_protect.sh;\n# other tools add to this file too, so add to it rather than replacing it.\n' >>"$new_startup"
	note "created $STARTUP"
fi

{
	echo "$HOOK_BEGIN"
	echo "# Puts the Classic Home firmware back if an updater replaced it. Silent when"
	echo "# there is nothing to do; see $STORE/restore.log."
	echo "if [ -x $STORE/$WORKER_NAME ]; then"
	echo "	$STORE/$WORKER_NAME || true"
	echo "fi"
	echo "$HOOK_END"
} >>"$new_startup" || die "cannot write $new_startup"

if [ "$old_ok" = 1 ] && ! syntax_ok "$new_startup"; then
	rm -f "$new_startup"
	die "the edited $STARTUP does not parse, and the original did - nothing was changed. Please report this."
fi

if [ -f "$STARTUP" ] && cmp -s "$new_startup" "$STARTUP"; then
	rm -f "$new_startup"
	note "kept $STARTUP (hook already installed)"
else
	mv -f "$new_startup" "$STARTUP" || {
		rm -f "$new_startup"
		die "cannot replace $STARTUP"
	}
	note "installed the boot hook into $STARTUP"
fi
chmod 755 "$STARTUP" 2>/dev/null || true
[ -x "$STARTUP" ] || echo "classic_home_protect: warning: $STARTUP is not executable and S99user may not run it" >&2
sync 2>/dev/null || true

# ---------------------------------------------------------------- say what happened

echo "Classic Home update protection is installed.${RELEASE_NAME:+ ($RELEASE_NAME)}"
echo
printf '%s' "$changed"
[ "$old_ok" = 1 ] || echo "  note: $STARTUP already had a syntax error before this ran - left as found, plus our block"
[ -z "$menu_warn" ] || echo "  WARNING: $menu_warn"
echo
echo "From now on, if update_all or the MiSTer Downloader replaces the firmware, the next"
echo "boot puts ours back and says so in $STORE/restore.log."
echo "Nothing is restored while the right build is already in place."
echo
echo "To undo all of this, run classic_home_unprotect.sh."
exit 0
