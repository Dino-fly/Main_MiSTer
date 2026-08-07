#!/bin/sh
#
# Assembles the SD-CARD-ROOT archive for a Classic Home release.
#
# A release is not one file. It is the firmware, the disc title table, the two Scripts
# entries that make it survive update_all, and - in a build that also carries PSX
# controllers over SNAC - menu.rbf and a set of rebuilt cores. The archive mirrors the
# card so that installing is merging one folder onto it:
#
#   SD-CARD-ROOT/
#     MiSTer                            replaces the firmware in the card root
#     menu.rbf                          replaces the menu core (SNAC-carrying builds)
#     classicui/disctitles.txt          the disc name table, a new file
#     linux/classic-home/               the boot hook's worker, ready for the installer
#     Scripts/classic_home_protect.sh   installs the hook
#     Scripts/classic_home_unprotect.sh removes it again
#     _Console/*.rbf _Computer/*.rbf _Arcade/cores/*.rbf   our core builds
#     MANIFEST_<name>.txt               every file, its size and its md5
#
# WHY THE FIRMWARE IS AN EXPLICIT ARGUMENT WITH AN EXPECTED MD5
#
# Because beta 2 nearly shipped the wrong binary. The archive was assembled from
# bin/MiSTer after the tree had merged 25 upstream commits, so it would have carried a
# rebuild that nobody had run, under a release note describing hardware tests of a
# different build. Reading bin/MiSTer is exactly the mistake, so this script cannot do
# it: the caller names the file and states the md5 they mean, and a mismatch stops the
# packaging. Copy the md5 from the release you tested, not from the file you are about
# to package - if the two agree, they agree.
#
#   make_sdcard_root.sh --name NAME --firmware PATH --expect-md5 HEX [options]
#
#     --name NAME          release name; names the zip and the manifest
#     --firmware PATH      the firmware to ship. No default, deliberately.
#     --expect-md5 HEX     md5 the firmware must have. Required. A prefix of at least 8
#                          hex digits is accepted, so the short form quoted in release
#                          notes can be pasted straight in.
#     --menu PATH          menu.rbf to ship (SNAC-carrying builds)
#     --disctitles PATH    the file to ship as classicui/disctitles.txt
#     --cores DIR          a tree already shaped like the card: its top level must be
#                          _Console, _Computer, _Arcade, _Other or _Utility. Copied as
#                          it stands.
#     --console-cores DIR  a flat directory of .rbf, placed in _Console/
#     --computer-cores DIR ditto, into _Computer/
#     --arcade-cores DIR   ditto, into _Arcade/cores/
#     --doc PATH           a file to ship at the top of the archive (README.md and so
#                          on). Repeatable.
#     --out DIR            where to build, default bin/release (bin is git-ignored)
#     --keep               do not delete an existing SD-CARD-ROOT first
#
# A flat directory is never classified by guesswork: 274 core names do not sort
# themselves into console, computer and arcade reliably, and a core in the wrong folder
# is a core the user cannot find. Either the tree is already shaped, or the caller says
# which folder each set belongs in.

set -u

die() {
	echo "make_sdcard_root: $1" >&2
	exit 1
}

# The header comment is the manual; see classic_home_protect.sh.
usage() {
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}

need_arg() {
	[ -n "${2:-}" ] || die "$1 needs a value"
}

NAME=''
FIRMWARE=''
EXPECT=''
MENU=''
DISCTITLES=''
CORES=''
CONSOLE=''
COMPUTER=''
ARCADE=''
DOCS=''
OUT=''
KEEP=0

while [ $# -gt 0 ]; do
	case $1 in
	--name)
		need_arg --name "${2:-}"
		NAME=$2
		shift
		;;
	--firmware)
		need_arg --firmware "${2:-}"
		FIRMWARE=$2
		shift
		;;
	--expect-md5)
		need_arg --expect-md5 "${2:-}"
		EXPECT=$2
		shift
		;;
	--menu)
		need_arg --menu "${2:-}"
		MENU=$2
		shift
		;;
	--disctitles)
		need_arg --disctitles "${2:-}"
		DISCTITLES=$2
		shift
		;;
	--cores)
		need_arg --cores "${2:-}"
		CORES=$2
		shift
		;;
	--console-cores)
		need_arg --console-cores "${2:-}"
		CONSOLE=$2
		shift
		;;
	--computer-cores)
		need_arg --computer-cores "${2:-}"
		COMPUTER=$2
		shift
		;;
	--arcade-cores)
		need_arg --arcade-cores "${2:-}"
		ARCADE=$2
		shift
		;;
	--doc)
		need_arg --doc "${2:-}"
		DOCS="$DOCS
$2"
		shift
		;;
	--out)
		need_arg --out "${2:-}"
		OUT=$2
		shift
		;;
	--keep) KEEP=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "unknown option '$1'" ;;
	esac
	shift
done

[ -n "$NAME" ] || die "--name is required"
[ -n "$FIRMWARE" ] || die "--firmware is required. This script will not read bin/MiSTer for you; see the header."
[ -n "$EXPECT" ] || die "--expect-md5 is required. State the md5 of the build you mean to ship."
case $NAME in
*[!A-Za-z0-9._-]*) die "--name '$NAME' has characters that do not belong in a filename" ;;
esac

tools_dir=$(cd "$(dirname "$0")" && pwd) || die "cannot resolve my own directory"
repo_root=$(cd "$tools_dir/../../.." && pwd) || die "cannot resolve the repository root"
[ -n "$OUT" ] || OUT=$repo_root/bin/release

md5_of() {
	if command -v md5sum >/dev/null 2>&1; then
		md5sum "$1" | cut -d' ' -f1
	elif command -v md5 >/dev/null 2>&1; then
		md5 -q "$1"
	else
		die "no md5sum (or md5) on this system"
	fi
}

size_of() {
	wc -c <"$1" | tr -d ' \t'
}

elf_arm_ok() {
	h=$(dd if="$1" bs=1 count=20 2>/dev/null | od -An -v -tx1 | tr -d ' \t\n')
	[ ${#h} -eq 40 ] || return 1
	case "$h" in
	7f454c460101*) ;;
	*) return 1 ;;
	esac
	[ "$(printf '%s' "$h" | cut -c37-40)" = 2800 ]
}

# ---------------------------------------------------------------- check the inputs

[ -f "$FIRMWARE" ] || die "$FIRMWARE does not exist"
elf_arm_ok "$FIRMWARE" || die "$FIRMWARE is not a 32-bit ARM ELF - is that really a MiSTer firmware?"
fw_md5=$(md5_of "$FIRMWARE")
fw_size=$(size_of "$FIRMWARE")

case $EXPECT in
*[!0-9a-fA-F]*) die "--expect-md5 '$EXPECT' is not hexadecimal" ;;
esac
elen=${#EXPECT}
[ "$elen" -ge 8 ] || die "--expect-md5 '$EXPECT' is too short to mean anything; give at least 8 hex digits"
[ "$elen" -le 32 ] || die "--expect-md5 '$EXPECT' is longer than an md5"
expect_lc=$(printf '%s' "$EXPECT" | tr 'A-F' 'a-f')
if [ "$(printf '%s' "$fw_md5" | cut -c1-"$elen")" != "$expect_lc" ]; then
	echo "make_sdcard_root: refusing to package - the firmware is not the one you named." >&2
	echo "  $FIRMWARE" >&2
	echo "  has      md5 $fw_md5" >&2
	echo "  expected md5 $expect_lc" >&2
	echo "This is the check that would have caught beta 2's accidental rebuild. If the" >&2
	echo "binary you tested is somewhere else, package that one." >&2
	exit 1
fi

menu_md5=''
if [ -n "$MENU" ]; then
	[ -f "$MENU" ] || die "$MENU does not exist"
	menu_md5=$(md5_of "$MENU")
fi
[ -z "$DISCTITLES" ] || [ -f "$DISCTITLES" ] || die "$DISCTITLES does not exist"

[ -z "$CORES" ] || [ -d "$CORES" ] || die "--cores $CORES is not a directory"
[ -z "$CONSOLE" ] || [ -d "$CONSOLE" ] || die "--console-cores $CONSOLE is not a directory"
[ -z "$COMPUTER" ] || [ -d "$COMPUTER" ] || die "--computer-cores $COMPUTER is not a directory"
[ -z "$ARCADE" ] || [ -d "$ARCADE" ] || die "--arcade-cores $ARCADE is not a directory"

if [ -n "$CORES" ]; then
	for e in "$CORES"/*; do
		[ -e "$e" ] || continue
		case $(basename "$e") in
		_Console | _Computer | _Arcade | _Other | _Utility) ;;
		*) die "--cores $CORES has '$(basename "$e")' at its top level. A shaped tree may only hold _Console, _Computer, _Arcade, _Other or _Utility; use --console-cores and friends for a flat directory." ;;
		esac
	done
fi

protect=$tools_dir/classic_home_protect.sh
unprotect=$tools_dir/classic_home_unprotect.sh
worker=$tools_dir/classic-home-restore.sh
for f in "$protect" "$unprotect" "$worker"; do
	[ -f "$f" ] || die "$f is missing"
done

# ---------------------------------------------------------------- build the tree

root=$OUT/SD-CARD-ROOT
if [ -d "$root" ] && [ "$KEEP" != 1 ]; then
	rm -rf "$root" || die "cannot clear $root"
fi
mkdir -p "$root" || die "cannot create $root"

# The variable names are prefixed per function because sh has no locals: an earlier
# version had put() and copy_cores() both using $_rel, so the second core of a set was
# copied into a path underneath the first one.
put() {
	_p_src=$1
	_p_rel=$2
	mkdir -p "$root/$(dirname "$_p_rel")" || die "cannot create $root/$(dirname "$_p_rel")"
	cp "$_p_src" "$root/$_p_rel" || die "cannot copy $_p_src to $root/$_p_rel"
	cmp -s "$_p_src" "$root/$_p_rel" || die "$root/$_p_rel did not come back identical to $_p_src"
}

put "$FIRMWARE" MiSTer
# cp keeps the source's mode, and a firmware downloaded from a release asset is often
# 644. The archive should carry the bit even though many unzip tools drop it anyway,
# which is why the install notes still tell network copiers to chmod +x.
chmod 755 "$root/MiSTer"
[ -z "$MENU" ] || put "$MENU" menu.rbf
[ -z "$DISCTITLES" ] || put "$DISCTITLES" classicui/disctitles.txt

put "$worker" linux/classic-home/classic-home-restore.sh
chmod 755 "$root/linux/classic-home/classic-home-restore.sh"

# The two Scripts entries are stamped with the md5s of what this archive actually
# carries, so the installer on the card can refuse to protect a firmware that is not
# ours - the case that matters is a user running it after an updater has already put the
# official build back.
stamp() {
	_st_src=$1
	_st_rel=$2
	mkdir -p "$root/$(dirname "$_st_rel")" || die "cannot create $root/$(dirname "$_st_rel")"
	sed \
		-e "s|^EXPECT_MD5=.*|EXPECT_MD5=\"$fw_md5\"|" \
		-e "s|^EXPECT_MENU_MD5=.*|EXPECT_MENU_MD5=\"$menu_md5\"|" \
		-e "s|^RELEASE_NAME=.*|RELEASE_NAME=\"$NAME\"|" \
		"$_st_src" >"$root/$_st_rel" || die "cannot write $root/$_st_rel"
	chmod 755 "$root/$_st_rel"
}

stamp "$protect" Scripts/classic_home_protect.sh
stamp "$unprotect" Scripts/classic_home_unprotect.sh

# A sed that silently matched nothing would ship an installer that protects anything it
# is pointed at, which is the whole failure this is here to prevent.
grep -q "^EXPECT_MD5=\"$fw_md5\"\$" "$root/Scripts/classic_home_protect.sh" ||
	die "stamping the firmware md5 into Scripts/classic_home_protect.sh did not take"
if [ -n "$menu_md5" ]; then
	grep -q "^EXPECT_MENU_MD5=\"$menu_md5\"\$" "$root/Scripts/classic_home_protect.sh" ||
		die "stamping the menu md5 into Scripts/classic_home_protect.sh did not take"
fi

cores_note=''

# Not a command substitution, on purpose: put() reports a bad copy by dying, and a die
# inside $( ) would kill only the subshell and hand back an empty count while packaging
# carried on.
copy_cores() {
	_dir=$1
	_rel=$2
	_n=0
	for f in "$_dir"/*.rbf; do
		[ -f "$f" ] || continue
		put "$f" "$_rel/$(basename "$f")"
		_n=$((_n + 1))
	done
	[ "$_n" -gt 0 ] || die "$_dir holds no .rbf files"
	cores_note="$cores_note
  $_n cores into $_rel/"
}

if [ -n "$CORES" ]; then
	cp -R "$CORES"/. "$root"/ || die "cannot copy $CORES into $root"
	cores_note="$cores_note
  cores copied from the shaped tree $CORES"
fi
[ -z "$CONSOLE" ] || copy_cores "$CONSOLE" _Console
[ -z "$COMPUTER" ] || copy_cores "$COMPUTER" _Computer
[ -z "$ARCADE" ] || copy_cores "$ARCADE" _Arcade/cores

if [ -n "$DOCS" ]; then
	_oldifs=$IFS
	IFS='
'
	for d in $DOCS; do
		IFS=$_oldifs
		[ -n "$d" ] || continue
		[ -f "$d" ] || die "--doc $d does not exist"
		put "$d" "$(basename "$d")"
		IFS='
'
	done
	IFS=$_oldifs
fi

# MiSTer.ini and downloader.ini are the user's own settings and are the two files no
# database is allowed to ship either. Shipping one would wipe the settings of everyone
# who merges the archive, which is worse than anything the updater does to us.
for forbidden in MiSTer.ini MiSTer_alt.ini MiSTer_alt_1.ini MiSTer_alt_2.ini MiSTer_alt_3.ini downloader.ini; do
	if [ -e "$root/$forbidden" ]; then
		rm -rf "$root"
		die "$forbidden ended up in the archive (from --cores or --doc). Refusing to ship a file that would overwrite the user's settings; $root has been removed."
	fi
done

# ---------------------------------------------------------------- manifest and zip

manifest=MANIFEST_$NAME.txt
mf=$root/$manifest
{
	echo "Classic Home release: $NAME"
	echo "Packaged:             $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	echo "Firmware:             MiSTer  $fw_size bytes  md5 $fw_md5"
	[ -z "$menu_md5" ] || echo "Menu core:            menu.rbf  md5 $menu_md5"
	echo
	echo "Every file in this archive, as md5, size in bytes, path. This manifest is the"
	echo "one file not listed in it."
	echo
} >"$mf" || die "cannot write $mf"

# LC_ALL=C so the order is the same on every machine that packages a release, which is
# what makes two manifests comparable.
find "$root" -type f ! -name "$manifest" | LC_ALL=C sort | while IFS= read -r f; do
	rel=${f#"$root"/}
	printf '%s  %10s  %s\n' "$(md5_of "$f")" "$(size_of "$f")" "$rel"
done >>"$mf" || die "cannot write $mf"

count=$(find "$root" -type f ! -name "$manifest" | wc -l | tr -d ' \t')
total=$(find "$root" -type f ! -name "$manifest" -exec wc -c {} \; | awk '{s += $1} END {printf "%d", s}')

zip=$OUT/$NAME.zip
rm -f "$zip"
if command -v zip >/dev/null 2>&1; then
	(cd "$OUT" && zip -q -r -X "$NAME.zip" SD-CARD-ROOT) || die "zip failed"
elif command -v python3 >/dev/null 2>&1; then
	(cd "$OUT" && python3 -m zipfile -c "$NAME.zip" SD-CARD-ROOT) || die "python3 -m zipfile failed"
else
	die "neither zip nor python3 is available to make the archive"
fi
[ -f "$zip" ] || die "$zip was not created"

echo "Packaged $NAME"
echo "  tree     $root"
echo "  files    $count ($total bytes)"
echo "  firmware $fw_size bytes, md5 $fw_md5 (matched --expect-md5 $expect_lc)"
[ -z "$menu_md5" ] || echo "  menu.rbf md5 $menu_md5"
printf '%s' "$cores_note"
[ -z "$cores_note" ] || echo
echo "  manifest $mf"
echo "  archive  $zip"
echo "           md5 $(md5_of "$zip")"
exit 0
