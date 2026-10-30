#!/usr/bin/env bash
# Smoke tests for .claude/scripts: every tool parses, answers --help, and runs its
# --dry-run against a fake ~/.config/classicui with no device present, so a
# `set -e`/pipefail exit (the mister-deploy class of bug: a silent exit 1 on success)
# or a dry-run that reaches for ssh shows up here and not on the device.
# Prints PASS/FAIL per case and a TOTAL line. Run next to hooks/test-hooks.sh.
S=$HOME/workspace/Main_MiSTer/.claude/scripts
WT=${WT:-$HOME/workspace/Main_MiSTer-ui}   # any worktree with a Makefile
export CLASSICUI_CFG=$(mktemp -d)
printf 'MISTER_HOST=127.0.0.1\nMISTER_USER=root\n' > "$CLASSICUI_CFG/devices.env"
pass=0; fail=0
case_() {  # expected-rc name command...
	local exp=$1 name=$2; shift 2
	local out rc
	# macOS has no `timeout`: a watchdog kills a case that hangs (a dry-run reaching for ssh)
	# (the watchdog must not hold the substitution's stdout, or every case waits the full 25 s)
	out=$( "$@" 2>&1 & p=$!; ( sleep 25; kill "$p" 2>/dev/null ) >/dev/null 2>&1 & w=$!; wait "$p"; r=$?; kill "$w" 2>/dev/null; exit "$r" ); rc=$?
	if [ "$rc" = "$exp" ]; then pass=$((pass+1)); printf 'PASS %-34s rc=%s\n' "$name" "$rc"
	else fail=$((fail+1)); printf 'FAIL %-34s want rc %s got %s\n   %s\n' "$name" "$exp" "$rc" "$(tail -n 3 <<<"$out")"; fi
}
echo "== syntax"
for f in "$S"/*; do [ -f "$f" ] || continue; case_ 0 "bash -n $(basename "$f")" bash -n "$f"; done
echo "== --help"
for t in mister-find mister-build mister-deploy mister-capture mister-ui mister-log mister-fresh mister-harness upstream-merge-trial farm-poll; do
	case_ 0 "$t --help" "$S/$t" --help
done
case_ 2 "approve-once (usage)" "$S/approve-once"
echo "== --dry-run, no device, fake config"
case_ 0 "mister-build --dry-run"   "$S/mister-build" "$WT" --allow-no-creds --dry-run
case_ 0 "mister-deploy --dry-run"  "$S/mister-deploy" "$WT/bin/MiSTer" --dry-run
case_ 0 "mister-capture --dry-run" "$S/mister-capture" smoke --dry-run
case_ 0 "mister-ui --dry-run"      "$S/mister-ui" state --dry-run
case_ 0 "mister-ui goto --dry-run" "$S/mister-ui" goto gbc "Oracle" --dry-run
case_ 0 "mister-log --dry-run"     "$S/mister-log" since 0 -g 'ui ' --dry-run
case_ 0 "mister-fresh --dry-run"   "$S/mister-fresh" --restart --sys gbc --game x --launch --dry-run
case_ 0 "mister-harness --dry-run" "$S/mister-harness" "$WT" --dry-run
case_ 2 "mister-harness no arg"    "$S/mister-harness"
case_ 2 "mister-build main refused" "$S/mister-build" $HOME/workspace/Main_MiSTer --allow-no-creds --dry-run
echo "== real read-only runs"
d=$(mktemp -d); mkdir -p "$d/logs" "$d/cores"
case_ 0 "farm-poll empty dir"      "$S/farm-poll" --dir "$d"
case_ 2 "farm-poll no dir"         "$S/farm-poll" --dir "$d/none"
rm -rf "$d"
case_ 0 "approve-once --status"    "$S/approve-once" --status
case_ 0 "upstream-merge-trial --no-fetch" "$S/upstream-merge-trial" --no-fetch --target classic-ui classic-ui
rm -rf "$CLASSICUI_CFG"
echo "== TOTAL pass=$pass fail=$fail"
[ "$fail" = 0 ]
