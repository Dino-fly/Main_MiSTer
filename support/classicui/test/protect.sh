#!/bin/sh
#
# Exercises the update_all survival hook against a fake SD card in a temp directory.
#
# This code runs at boot, on other people's machines, and its worst failure is a card
# with no working `MiSTer` on it - so none of it may be tested for the first time on a
# device. Everything here is a real run of the real scripts with CHOME_ROOT pointed at
# /tmp: the installer, the packager, the generated user-startup.sh (which is executed,
# not read), and the boot worker.
#
# The firmware fixtures are genuine ARM binaries out of releases/: the official one is
# shipped as it stands, and "our" build is a different official release with the marker
# string appended, which makes it a valid ARM ELF that the marker check finds and that
# is byte-different from the one it displaces. That is all the hook looks at.
#
#   support/classicui/test/protect.sh [-k]     -k keeps the temp card for poking at
#
# It needs no toolchain and no device. Run it under busybox too, which is the shell the
# device actually has:
#   docker run --rm -v "$PWD:/w" -w /w busybox:latest sh support/classicui/test/protect.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
tools=$(cd "$here/../tools" && pwd)
repo=$(cd "$here/../../.." && pwd)

keep=0
[ "${1:-}" = "-k" ] && keep=1

checks=0
fails=0

pass() {
	checks=$((checks + 1))
	echo "    ok   $1"
}
fail() {
	checks=$((checks + 1))
	fails=$((fails + 1))
	echo "    FAIL $1"
}
same() { if cmp -s "$1" "$2"; then pass "$3"; else fail "$3 -- $1 differs from $2"; fi; }
differs() { if cmp -s "$1" "$2"; then fail "$3 -- $1 is identical to $2"; else pass "$3"; fi; }
exists() { if [ -f "$1" ]; then pass "$2"; else fail "$2 -- $1 does not exist"; fi; }
absent() { if [ -e "$1" ]; then fail "$2 -- $1 exists"; else pass "$2"; fi; }
has() { if grep -q "$1" "$2" 2>/dev/null; then pass "$3"; else fail "$3 -- '$1' not in $2"; fi; }
hasnt() { if grep -q "$1" "$2" 2>/dev/null; then fail "$3 -- '$1' is in $2"; else pass "$3"; fi; }
counted() {
	n=$(grep -c "$2" "$3" 2>/dev/null)
	[ -n "$n" ] || n=0
	if [ "$n" = "$1" ]; then pass "$4"; else fail "$4 -- found $n, wanted $1"; fi
}
rc_is() { if [ "$1" = "$2" ]; then pass "$3"; else fail "$3 -- exit status $1, wanted $2"; fi; }
lines() { wc -l <"$1" 2>/dev/null | tr -d ' \t'; }

tmp=$(mktemp -d 2>/dev/null || mktemp -d -t chome)
[ -n "$tmp" ] || {
	echo "cannot make a temp directory" >&2
	exit 1
}
cleanup() {
	[ "$keep" = 1 ] && {
		echo
		echo "kept $tmp"
		return 0
	}
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- fixtures

official=$repo/releases/MiSTer_20260707
official_old=$repo/releases/MiSTer_20260603
for f in "$official" "$official_old"; do
	[ -f "$f" ] || {
		echo "missing fixture $f" >&2
		exit 1
	}
done

ours=$tmp/fixture-MiSTer-ours
cp "$official_old" "$ours"
printf 'CLASSICUI_SCREENSCRAPER' >>"$ours"

newer=$tmp/fixture-MiSTer-newer
cp "$official_old" "$newer"
printf 'CLASSICUI_SCREENSCRAPER and then some' >>"$newer"

menu=$tmp/fixture-menu.rbf
official_menu=$tmp/fixture-menu-official.rbf
dd if=/dev/urandom of="$menu" bs=1024 count=1500 2>/dev/null
dd if=/dev/urandom of="$official_menu" bs=1024 count=1500 2>/dev/null

disctitles=$repo/support/classicui/disctitles.generated.txt
[ -f "$disctitles" ] || disctitles=''

md5_of() {
	if command -v md5sum >/dev/null 2>&1; then
		md5sum "$1" | cut -d' ' -f1
	else
		md5 -q "$1"
	fi
}
ours_md5=$(md5_of "$ours")

CHOME_ROOT=$tmp/card
export CHOME_ROOT
card=$CHOME_ROOT
store=$card/linux/classic-home
startup=$card/linux/user-startup.sh
log=$store/restore.log

# A user merging the archive onto the card: everything the packager produced, copied in.
merge_archive() {
	mkdir -p "$card"
	cp -R "$1"/. "$card"/
	chmod 755 "$card/MiSTer" 2>/dev/null || true
	chmod 755 "$store/classic-home-restore.sh" 2>/dev/null || true
	chmod 755 "$card"/Scripts/*.sh 2>/dev/null || true
}

newcard() {
	rm -rf "$card"
	mkdir -p "$card/linux" "$card/config" "$card/games"
	merge_archive "$archive"
}

# What actually happens at boot: S99user runs user-startup.sh.
boot() {
	if [ -f "$startup" ]; then
		sh "$startup"
	fi
}

section() {
	echo
	echo "== $1"
}

# ---------------------------------------------------------------- 0. the packager

section "0. the packager refuses a firmware that is not the one named"
out=$tmp/pack-wrong.txt
sh "$tools/make_sdcard_root.sh" --name chome-test --firmware "$ours" \
	--expect-md5 deadbeefdeadbeef --out "$tmp/rel-bad" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 1 "refused to package"
absent "$tmp/rel-bad/chome-test.zip" "no archive was written"

section "0b. the packager builds the archive when the md5 matches"
out=$tmp/pack.txt
sh "$tools/make_sdcard_root.sh" --name chome-test --firmware "$ours" \
	--expect-md5 "$(printf '%s' "$ours_md5" | cut -c1-8)" \
	${disctitles:+--disctitles "$disctitles"} \
	--out "$tmp/rel" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "packaged"
archive=$tmp/rel/SD-CARD-ROOT
same "$archive/MiSTer" "$ours" "the firmware in the archive is the one named"
exists "$archive/Scripts/classic_home_protect.sh" "the installer is in Scripts/"
exists "$archive/Scripts/classic_home_unprotect.sh" "the uninstaller is in Scripts/"
exists "$archive/linux/classic-home/classic-home-restore.sh" "the boot worker ships under linux/"
exists "$tmp/rel/chome-test.zip" "the zip is named for the release"
exists "$archive/MANIFEST_chome-test.txt" "the manifest is named for the release"
has "^EXPECT_MD5=\"$ours_md5\"\$" "$archive/Scripts/classic_home_protect.sh" "the installer is stamped with the firmware md5"
has "^EXPECT_MENU_MD5=\"\"\$" "$archive/Scripts/classic_home_protect.sh" "no menu md5 is stamped in a build without one"
has "$ours_md5  " "$archive/MANIFEST_chome-test.txt" "the manifest lists the firmware md5"
has " MiSTer\$" "$archive/MANIFEST_chome-test.txt" "the manifest lists the firmware path"
absent "$archive/MiSTer.ini" "no MiSTer.ini in the archive"
echo "    -- manifest:"
sed 's/^/    | /' "$archive/MANIFEST_chome-test.txt"

# ---------------------------------------------------------------- 1. fresh install

section "1. fresh install on a card with no user-startup.sh"
newcard
absent "$startup" "the card starts with no user-startup.sh"
out=$tmp/1.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the installer succeeded"
same "$store/MiSTer" "$ours" "our firmware is stored under linux/"
exists "$store/protected.list" "the manifest was written"
echo "    -- protected.list:"
sed 's/^/    | /' "$store/protected.list"
exists "$startup" "user-startup.sh was created"
echo "    -- user-startup.sh:"
sed 's/^/    | /' "$startup"
counted 1 CLASSIC-HOME-RESTORE-HOOK-BEGIN "$startup" "exactly one hook block"
has "^#!/bin/sh\$" "$startup" "it has a shebang"
if sh -n "$startup"; then pass "user-startup.sh parses"; else fail "user-startup.sh parses"; fi

# ---------------------------------------------------------------- 2. nothing to do

section "2. nothing to do: the right firmware is already in place"
boot
if [ -f "$log" ]; then
	echo "    -- restore.log:"
	sed 's/^/    | /' "$log"
	fail "the boot said nothing"
else
	pass "the boot wrote no log at all"
fi
same "$card/MiSTer" "$ours" "the firmware was left alone"
absent "$store/MiSTer.official" "nothing was set aside"

# ---------------------------------------------------------------- 3. after update_all

section "3. update_all has replaced the firmware"
# What a downloader run leaves behind: the official build in place, ours in .MiSTer.old.
mv "$card/MiSTer" "$card/.MiSTer.old"
cp "$official" "$card/MiSTer"
chmod 755 "$card/MiSTer"
same "$card/MiSTer" "$official" "the card is running the official firmware"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$ours" "our firmware is back"
same "$store/MiSTer.official" "$official" "the official build it displaced was kept"
has "restored MiSTer" "$log" "the log says what it did"
if [ -x "$card/MiSTer" ]; then pass "the restored firmware is executable"; else fail "the restored firmware is executable"; fi
n1=$(lines "$log")

section "3b. the next boot has nothing to do and says nothing"
boot
n2=$(lines "$log")
if [ "$n1" = "$n2" ]; then pass "the log did not grow ($n1 lines)"; else fail "the log grew from $n1 to $n2 lines"; fi
absent "$(echo "$card"/.chome-restore.*)" "no temp file was left in the card root"

# ---------------------------------------------------------------- 4. someone else's file

section "4. an existing user-startup.sh with someone else's content"
newcard
mkdir -p "$card/linux"
cat >"$startup" <<'EOF'
#!/bin/bash
# Written by someone else entirely
[ -x /media/fat/Scripts/MiSTer_SAM_on.sh ] && /media/fat/Scripts/MiSTer_SAM_on.sh
echo "user's own line" >/tmp/mine
EOF
chmod 755 "$startup"
cp "$startup" "$tmp/4-before.sh"
out=$tmp/4.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the installer succeeded"
echo "    -- user-startup.sh:"
sed 's/^/    | /' "$startup"
has "MiSTer_SAM_on.sh" "$startup" "their launcher line survived"
has "user's own line" "$startup" "their own line survived"
has "^#!/bin/bash\$" "$startup" "their shebang survived"
counted 1 CLASSIC-HOME-RESTORE-HOOK-BEGIN "$startup" "our block was appended once"

# ---------------------------------------------------------------- 5. installed twice

section "5. the installer run twice does not duplicate the block"
out=$tmp/5.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the second run succeeded"
counted 1 CLASSIC-HOME-RESTORE-HOOK-BEGIN "$startup" "still exactly one begin marker"
counted 1 CLASSIC-HOME-RESTORE-HOOK-END "$startup" "still exactly one end marker"
counted 1 MiSTer_SAM_on.sh "$startup" "their line is still there exactly once"
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
counted 1 CLASSIC-HOME-RESTORE-HOOK-BEGIN "$startup" "four runs, one block"
echo "    -- user-startup.sh after four runs:"
sed 's/^/    | /' "$startup"

section "5b. a half-deleted block is not guessed at"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
grep -v CLASSIC-HOME-RESTORE-HOOK-END "$startup" >"$startup.x"
mv "$startup.x" "$startup"
cp "$startup" "$tmp/5b-before.sh"
out=$tmp/5b.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 1 "the installer refused"
same "$startup" "$tmp/5b-before.sh" "user-startup.sh was not touched"

# ---------------------------------------------------------------- 6. store damaged

section "6. the stored copy is missing"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
rm -f "$store/MiSTer"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$official" "the card was left as it was found"
has "stored copy" "$log" "the log says the stored copy is missing"

section "7. the stored copy is truncated"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
dd if="$ours" of="$store/MiSTer.trunc" bs=1024 count=300 2>/dev/null
mv "$store/MiSTer.trunc" "$store/MiSTer"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$official" "the card was left as it was found"
has "manifest says" "$log" "the log names the size mismatch"

section "7b. the stored copy is garbage of exactly the right size"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
size=$(wc -c <"$store/MiSTer" | tr -d ' \t')
head -c "$size" /dev/urandom >"$store/MiSTer.g"
mv "$store/MiSTer.g" "$store/MiSTer"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$official" "the card was left as it was found"
has "not a 32-bit ARM ELF" "$log" "the log says it is not an ARM ELF"

section "7c. the stored copy is a valid ARM ELF, but not the one recorded"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
size=$(wc -c <"$store/MiSTer" | tr -d ' \t')
cp "$official_old" "$store/MiSTer.s"
# Pad to the recorded size so only the md5 can tell the difference.
have=$(wc -c <"$store/MiSTer.s" | tr -d ' \t')
head -c $((size - have)) /dev/zero >>"$store/MiSTer.s"
mv "$store/MiSTer.s" "$store/MiSTer"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$official" "the card was left as it was found"
has "stored copy is damaged" "$log" "the log says the md5 does not match"

# ---------------------------------------------------------------- 8. not our business

section "8. a Classic Home build we did not store is left alone"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
cp "$newer" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$newer" "their newer build is still there"
has "leaving MiSTer alone" "$log" "the log says why"

section "8b. the disabled flag switches the hook off"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
: >"$store/disabled"
cp "$official" "$card/MiSTer"
boot
same "$card/MiSTer" "$official" "the official build was left in place"
absent "$log" "and nothing was logged"

section "8c. the firmware missing from the card root is a rescue, not an error"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
rm -f "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$ours" "our firmware was put back"
has "was missing from the card root" "$log" "the log says it was missing"

# ---------------------------------------------------------------- 9. menu.rbf

section "9. a SNAC-carrying release protects menu.rbf too"
out=$tmp/pack-menu.txt
sh "$tools/make_sdcard_root.sh" --name chome-snac --firmware "$ours" \
	--expect-md5 "$ours_md5" --menu "$menu" --out "$tmp/rel-snac" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "packaged with a menu core"
archive=$tmp/rel-snac/SD-CARD-ROOT
has "^EXPECT_MENU_MD5=\"$(md5_of "$menu")\"\$" "$archive/Scripts/classic_home_protect.sh" "the installer is stamped with the menu md5"
newcard
out=$tmp/9.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
sed 's/^/    | /' "$out"
same "$store/menu.rbf" "$menu" "the menu core is stored"
counted 1 "^rbf " "$store/protected.list" "the manifest has an rbf entry"
cp "$official" "$card/MiSTer"
cp "$official_menu" "$card/menu.rbf"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$ours" "the firmware came back"
same "$card/menu.rbf" "$menu" "the menu core came back"
same "$store/menu.rbf.official" "$official_menu" "the displaced menu core was kept"

# ---------------------------------------------------------------- 10. uninstall

section "10. the uninstaller removes the hook and puts the official build back"
newcard
mkdir -p "$card/linux"
cat >"$startup" <<'EOF'
#!/bin/bash
[ -x /media/fat/Scripts/MiSTer_SAM_on.sh ] && /media/fat/Scripts/MiSTer_SAM_on.sh
EOF
chmod 755 "$startup"
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
cp "$official" "$card/MiSTer"
boot
same "$card/MiSTer" "$ours" "our firmware was restored, so an official copy exists to go back to"
out=$tmp/10.txt
sh "$card/Scripts/classic_home_unprotect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the uninstaller succeeded"
echo "    -- user-startup.sh:"
sed 's/^/    | /' "$startup"
hasnt CLASSIC-HOME-RESTORE-HOOK "$startup" "the hook block is gone"
has "MiSTer_SAM_on.sh" "$startup" "their line survived"
same "$card/MiSTer" "$official" "the official firmware is in place"
exists "$store/MiSTer" "the store was kept"
boot
same "$card/MiSTer" "$official" "a boot after uninstalling changes nothing"

section "10b. --purge removes the store as well"
out=$tmp/10b.txt
sh "$card/Scripts/classic_home_unprotect.sh" --purge >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the uninstaller succeeded"
absent "$store" "the store is gone"

section "10c. uninstalling with no official copy kept says so and changes nothing"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
out=$tmp/10c.txt
sh "$card/Scripts/classic_home_unprotect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "the uninstaller succeeded"
same "$card/MiSTer" "$ours" "the firmware on the card was not touched"
has "MiSTer.old is not it" "$out" "it warns that .MiSTer.old is our build"

# ---------------------------------------------------------------- 11. wrong build

section "11. the installer refuses to protect a firmware that is not this release's"
newcard
cp "$official" "$card/MiSTer"
out=$tmp/11.txt
sh "$card/Scripts/classic_home_protect.sh" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 1 "the installer refused"
absent "$store/protected.list" "nothing was stored"
absent "$startup" "no hook was installed"

section "11b. the hook does nothing when the store has no manifest"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
rm -f "$store/protected.list"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$official" "the card was left as it was found"
has "protected.list is missing" "$log" "the log says the manifest is missing"

section "11c. a manifest someone opened in a Windows editor still works"
newcard
sh "$card/Scripts/classic_home_protect.sh" >/dev/null 2>&1
sed 's/$/\r/' "$store/protected.list" >"$store/protected.list.crlf"
mv "$store/protected.list.crlf" "$store/protected.list"
cp "$official" "$card/MiSTer"
boot
echo "    -- restore.log:"
sed 's/^/    | /' "$log"
same "$card/MiSTer" "$ours" "our firmware was restored despite the CRLF line endings"
absent "$card/MiSTer$(printf '\r')" "nothing was written to a name with a stray CR in it"

# ---------------------------------------------------------------- 12. cores

section "12. cores and docs go where they are told, and only there"
mkdir -p "$tmp/flat-console" "$tmp/flat-computer" "$tmp/flat-arcade"
echo core >"$tmp/flat-console/SNES_20260807.rbf"
echo core >"$tmp/flat-console/NES_20260807.rbf"
echo core >"$tmp/flat-computer/Minimig_20260807.rbf"
echo core >"$tmp/flat-arcade/ActFancer.rbf"
echo readme >"$tmp/README.md"
out=$tmp/12.txt
sh "$tools/make_sdcard_root.sh" --name chome-cores --firmware "$ours" --expect-md5 "$ours_md5" \
	--console-cores "$tmp/flat-console" --computer-cores "$tmp/flat-computer" \
	--arcade-cores "$tmp/flat-arcade" --doc "$tmp/README.md" \
	--out "$tmp/rel-cores" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "packaged with cores"
a=$tmp/rel-cores/SD-CARD-ROOT
exists "$a/_Console/SNES_20260807.rbf" "a console core is in _Console/"
exists "$a/_Computer/Minimig_20260807.rbf" "a computer core is in _Computer/"
exists "$a/_Arcade/cores/ActFancer.rbf" "an arcade core is in _Arcade/cores/ with its plain name"
exists "$a/README.md" "the doc is at the top of the archive"
has "_Arcade/cores/ActFancer.rbf\$" "$a/MANIFEST_chome-cores.txt" "the manifest lists the arcade core"

section "12b. a shaped core tree is copied as it stands, and an unshaped one is refused"
mkdir -p "$tmp/shaped/_Console" "$tmp/shaped/_Arcade/cores"
echo core >"$tmp/shaped/_Console/SNES_20260807.rbf"
echo core >"$tmp/shaped/_Arcade/cores/ActFancer.rbf"
out=$tmp/12b.txt
sh "$tools/make_sdcard_root.sh" --name chome-shaped --firmware "$ours" --expect-md5 "$ours_md5" \
	--cores "$tmp/shaped" --out "$tmp/rel-shaped" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 0 "packaged from a shaped tree"
exists "$tmp/rel-shaped/SD-CARD-ROOT/_Console/SNES_20260807.rbf" "the shaped tree landed unchanged"

mkdir -p "$tmp/unshaped/games"
echo core >"$tmp/unshaped/games/x.rbf"
out=$tmp/12c.txt
sh "$tools/make_sdcard_root.sh" --name chome-unshaped --firmware "$ours" --expect-md5 "$ours_md5" \
	--cores "$tmp/unshaped" --out "$tmp/rel-unshaped" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 1 "an unshaped tree is refused rather than guessed at"

section "12d. an archive that would carry MiSTer.ini is thrown away"
mkdir -p "$tmp/withini"
echo 'CLASSICUI=1' >"$tmp/withini/MiSTer.ini"
out=$tmp/12d.txt
sh "$tools/make_sdcard_root.sh" --name chome-ini --firmware "$ours" --expect-md5 "$ours_md5" \
	--doc "$tmp/withini/MiSTer.ini" --out "$tmp/rel-ini" >"$out" 2>&1
rc=$?
sed 's/^/    | /' "$out"
rc_is "$rc" 1 "refused"
absent "$tmp/rel-ini/SD-CARD-ROOT" "the half-built tree was removed"
absent "$tmp/rel-ini/chome-ini.zip" "no archive was written"

# ---------------------------------------------------------------- done

echo
echo "$checks checks, $fails failures"
[ "$fails" = 0 ] || exit 1
exit 0
