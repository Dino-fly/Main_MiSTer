#!/usr/bin/env bash
# Synthetic PreToolUse JSON cases for the three hooks. Prints PASS/FAIL per case.
H="$(cd "$(dirname "$0")" && pwd)"
export CLASSICUI_CFG=$(mktemp -d)   # isolated marker dir: no real markers touched
MAIN="${MAIN_REPO:-$HOME/workspace/Main_MiSTer}"
pass=0; fail=0
case_() {  # hook expect(block|allow) cwd command
	local hook=$1 exp=$2 cwd=$3 cmd=$4 rc out
	out=$(jq -cn --arg c "$cmd" --arg w "$cwd" '{tool_name:"Bash",tool_input:{command:$c},cwd:$w}' | "$H/$hook" 2>&1 >/dev/null); rc=$?
	local got=allow; [ $rc = 2 ] && got=block
	if [ "$got" = "$exp" ]; then pass=$((pass+1)); printf 'PASS %-5s %s\n' "$exp" "$cmd"; else fail=$((fail+1)); printf 'FAIL want %s got %s (rc %s): %s\n   %s\n' "$exp" "$got" "$rc" "$cmd" "$out"; fi
}
B=guard-bash.sh; S=guard-secrets.sh
echo "== guard-bash: destructive"
case_ $B block "$MAIN-ui" 'docker kill $(docker ps -q)'
case_ $B block "$MAIN-ui" 'docker rm -f $(docker ps -aq)'
case_ $B block "$MAIN-ui" 'docker ps -q | xargs docker stop'
case_ $B block "$MAIN-ui" 'pkill -f build_via_windows'
case_ $B block "$MAIN-ui" 'pkill -9 -f tar'
case_ $B block "$MAIN-ui" 'killall tar'
case_ $B block "$MAIN-ui" 'ssh root@h "killall MiSTer; killall sshd"'
case_ $B block "$MAIN-ui" 'kill -9 -1'
case_ $B block "$MAIN-ui" 'ssh root@h rm /media/fat/MiSTer'
case_ $B allow "$MAIN-ui" 'ssh -n root@h "cd /media/fat && killall MiSTer; mv MiSTer.new MiSTer; setsid ./MiSTer &"'
case_ $B allow "$MAIN-ui" 'killall -9 MiSTer'
case_ $B allow "$MAIN-ui" "ssh -n root@h 'cd /media/fat && killall MiSTer; cp MiSTer.pre-1025 MiSTer.old.t; ls MiSTer.new-* 2>/dev/null; echo'"
case_ $B allow "$MAIN-ui" 'ssh root@h "killall MiSTer && mv MiSTer.new MiSTer"'
case_ $B allow "$MAIN-ui" 'ssh root@h "killall MiSTer"'
case_ $B block "$MAIN-ui" 'killall -9 *'
case_ $B block "$MAIN-ui" 'ssh root@h "killall MiSTer; killall -9 *"'
case_ $B block "$MAIN-ui" 'killall MiSTerX'
case_ $B allow "$MAIN-ui" 'docker kill snacjob_w3'
case_ $B allow "$MAIN-ui" 'ps aux | grep -E "[b]uild_via|[t]ar cf" | awk "{print \$2}" | xargs kill -9'
case_ $B allow "$MAIN-ui" 'kill -9 12345'
echo "== guard-bash: public output / push"
case_ $B block "$MAIN-ui" 'gh issue comment 6 --body "x"'
case_ $B block "$MAIN-ui" 'gh pr create --draft --title t'
case_ $B block "$MAIN-ui" 'gh release create v6 bin/MiSTer'
case_ $B block "$MAIN-ui" 'gh discussion comment 5 -b hi'
case_ $B block "$MAIN-ui" 'gh issue close 1'
case_ $B block "$MAIN-ui" 'gh api -X POST repos/x/y/issues/1/comments -f body=hi'
case_ $B allow "$MAIN-ui" 'gh issue view 6 --comments'
case_ $B allow "$MAIN-ui" 'gh pr list --state all'
case_ $B allow "$MAIN-ui" 'gh api repos/MiSTer-devel/Main_MiSTer/commits --jq ".[0].sha"'
case_ $B allow "$MAIN-ui" 'gh release list'
case_ $B block "$MAIN-ui" 'git push fork classic-ui'
case_ $B block "$MAIN-ui" 'git push origin master'
case_ $B block "$MAIN-ui" 'git push --force fork classic-ui'
case_ $B allow "$MAIN-ui" 'git fetch origin && git log --oneline -3 origin/master'
case_ $B allow "$MAIN-ui" 'git status --short'
touch "$CLASSICUI_CFG/allow-once.gh";   case_ $B allow "$MAIN-ui" 'gh issue comment 6 --body "approved text"'
[ -f "$CLASSICUI_CFG/allow-once.gh" ] && echo "FAIL marker not consumed" || echo "PASS gh marker consumed"
case_ $B block "$MAIN-ui" 'gh issue comment 6 --body "second one"'
touch "$CLASSICUI_CFG/allow-once.push"; case_ $B block "$MAIN-ui" 'git push origin master'
echo "   (marker still present after upstream refusal: $([ -f "$CLASSICUI_CFG/allow-once.push" ] && echo yes || echo no))"
printf '#!/bin/sh\nexit 0\n' > "$CLASSICUI_CFG/push-guard"; chmod +x "$CLASSICUI_CFG/push-guard"
case_ $B allow "$MAIN" 'git push fork snac-pr'
[ -f "$CLASSICUI_CFG/allow-once.push" ] && echo "FAIL push marker not consumed" || echo "PASS push marker consumed"
touch "$CLASSICUI_CFG/allow-once.push"; printf '#!/bin/sh\necho vetoed; exit 1\n' > "$CLASSICUI_CFG/push-guard"
case_ $B block "$MAIN" 'git push fork snac-pr'
rm -f "$CLASSICUI_CFG/push-guard"
echo "== guard-bash: firmware onto device"
case_ $B block "$MAIN-ui" 'scp bin/MiSTer root@h:/media/fat/MiSTer'
case_ $B block "$MAIN-ui" 'ssh root@h "cp /tmp/x /media/fat/MiSTer"'
case_ $B block "$MAIN-ui" 'cat bin/MiSTer | ssh root@h "cat > /media/fat/MiSTer"'
case_ $B allow "$MAIN-ui" 'scp root@h:/media/fat/MiSTer ./MiSTer.backup'
case_ $B allow "$MAIN-ui" 'ssh root@h "md5sum /media/fat/MiSTer"'
case_ $B allow "$MAIN-ui" 'ssh root@h "cp -p /media/fat/MiSTer /media/fat/MiSTer.pre-20261025"'
case_ $B allow "$MAIN-ui" 'scp bin/MiSTer root@h:/media/fat/MiSTer.new'
printf 'x h 0\n' > "$CLASSICUI_CFG/md5-verified"; case_ $B allow "$MAIN-ui" 'scp bin/MiSTer root@h:/media/fat/MiSTer'
touch -t 202001010000 "$CLASSICUI_CFG/md5-verified"; case_ $B block "$MAIN-ui" 'scp bin/MiSTer root@h:/media/fat/MiSTer'
echo "== guard-bash: main checkout (judged by the directory the command runs in)"
case_ $B block "$MAIN" 'make clean && make'
case_ $B block "$MAIN" 'git switch x'
case_ $B block "$MAIN" 'git checkout classic-ui'
case_ $B block "$MAIN-ui" "cd $MAIN && git switch x"
case_ $B block "$MAIN-ui" "git -C $MAIN checkout y"
case_ $B block "$MAIN-ui" "cd $MAIN && make clean"
case_ $B block "$MAIN-ui" "make -C $MAIN clean"
case_ $B block "$MAIN-ui" "cd $MAIN; git switch -c z"
case_ $B allow "$MAIN" "cd $MAIN-ui && git switch -c x"
case_ $B allow "$MAIN" "cd $MAIN-ui && git switch -c classicui-filters 2>&1 | tail -1 && git add -u && git -c core.hooksPath=/dev/null commit -q -F - <<'EOF'
msg
EOF"
case_ $B allow "$MAIN" "git -C $MAIN-ui switch classic-ui"
case_ $B allow "$MAIN" "cd $MAIN-ui && make clean"
case_ $B allow "$MAIN" "cd ../Main_MiSTer-ui && make clean"
case_ $B allow "$MAIN-ui" 'make clean'
case_ $B allow "$MAIN-ui" 'git checkout -b feature'
case_ $B allow "$MAIN/.claude/worktrees/agent-a819560b5eeb321a8" 'make clean && git switch -c t'
case_ $B allow "$MAIN" 'git checkout -- cfg.cpp'
case_ $B allow "$MAIN" 'git worktree add ../Main_MiSTer-x classic-ui'
case_ $B allow "$MAIN" 'git switch --help'
echo "== guard-bash: malformed input fails closed on destructive, open otherwise"
out=$(printf 'not json docker kill $(docker ps -q)' | "$H/$B" 2>&1); [ $? = 2 ] && echo "PASS raw destructive blocked" || echo "FAIL raw destructive allowed"
printf 'not json ls -la' | "$H/$B" 2>/dev/null; [ $? = 0 ] && echo "PASS raw harmless allowed" || echo "FAIL raw harmless blocked"
echo "== guard-secrets"
case_ $S block "$MAIN-ui" 'cat ~/.config/classicui/ss.env'
case_ $S block "$MAIN-ui" 'ssh root@h "sed -n 1,12p /media/fat/MiSTer.ini"'
case_ $S block "$MAIN-ui" 'ssh root@h "head -20 /media/fat/MiSTer.ini"'
case_ $S block "$MAIN-ui" 'grep -i pass /media/fat/MiSTer.ini'
case_ $S block "$MAIN-ui" 'grep -n "=" MiSTer.ini'
case_ $S block "$MAIN-ui" 'cat bin/ss_credentials.h'
case_ $S block "$MAIN-ui" 'source ~/.config/classicui/ss.env; echo $SS_DEVID'
case_ $S block "$MAIN-ui" 'cat /media/fat/linux/wpa_supplicant.conf'
printf 'local.private\n' > "$CLASSICUI_CFG/private-files"
case_ $S block "$MAIN-ui" 'cat ~/.config/classicui/local.private'
case_ $S allow "$MAIN-ui" 'cat ~/.config/classicui/local.public'
case_ $S allow "$MAIN-ui" "grep -n '^main=' MiSTer.ini"
case_ $S allow "$MAIN-ui" 'ssh root@h "grep -n \"^\\[\" /media/fat/MiSTer.ini"'
case_ $S allow "$MAIN-ui" 'cut -d= -f1 ~/.config/classicui/ss.env'
case_ $S allow "$MAIN-ui" "grep -Eiv 'pass|devid|user' MiSTer.ini"
case_ $S allow "$MAIN-ui" 'grep -c classicui MiSTer.ini'
case_ $S allow "$MAIN-ui" 'scp root@h:/media/fat/MiSTer.ini ./MiSTer.ini.bak'
case_ $S allow "$MAIN-ui" 'md5 -q ~/.config/classicui/ss.env; wc -l MiSTer.ini'
case_ $S allow "$MAIN-ui" 'cat MiSTer.ini.template'
case_ $S allow "$MAIN-ui" 'cat cfg.cpp | grep -n ini'
case_ $S allow "$MAIN-ui" "sed -E 's/=.*/=.../' ~/.config/classicui/ss.env"
echo "== guard-edit"
E=guard-edit.sh
out=$(jq -cn --arg f "$HOME/workspace/snac-build/build_via_windows7.sh" '{tool_name:"Edit",tool_input:{file_path:$f}}' | "$H/$E" 2>&1); r=$?; echo "edit of idle script -> rc $r (expect 0 since no worker runs now)"
tmp_dir=$(mktemp -d); tmp_running="$tmp_dir/runme_hooktest.sh"; printf 'sleep 20\n' > "$tmp_running"
bash "$tmp_running" >/dev/null 2>&1 & RP=$!; sleep 1
out=$(jq -cn --arg f "$tmp_running" '{tool_name:"Edit",tool_input:{file_path:$f}}' | "$H/$E" 2>&1); r=$?; [ $r = 2 ] && echo "PASS running script edit blocked" || echo "FAIL running script edit allowed (rc $r)"
kill $RP 2>/dev/null; rm -rf "$tmp_dir"
out=$(jq -cn '{tool_name:"Write",tool_input:{file_path:"/tmp/notes.md"}}' | "$H/$E" 2>&1); [ $? = 0 ] && echo "PASS non-script write allowed" || echo "FAIL non-script write blocked"
echo "== guard-claims (Stop)"
TD=$(mktemp -d)
# claim_case expect name final_text [tool_command tool_result]
claim_case() {
	local exp=$1 name=$2 text=$3 cmd=${4:-} res=${5:-} active=${6:-false} rc got
	python3 - "$TD/t.jsonl" "$text" "$cmd" "$res" <<'PY'
import json, sys
path, text, cmd, res = sys.argv[1:5]
L = [{"type": "user", "message": {"role": "user", "content": "do the thing"}}]
if cmd:
    L.append({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": cmd}}]}})
    L.append({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t1", "content": res}]}})
L.append({"type": "assistant", "message": {"content": [{"type": "text", "text": text}]}})
open(path, "w").write("\n".join(json.dumps(x) for x in L) + "\n")
PY
	jq -cn --arg p "$TD/t.jsonl" --argjson a "$active" '{transcript_path:$p, stop_hook_active:$a}' | "$H/guard-claims.py" >/dev/null 2>&1; rc=$?
	got=allow; [ $rc = 2 ] && got=block
	if [ "$got" = "$exp" ]; then pass=$((pass+1)); printf 'PASS %-5s %s\n' "$exp" "$name"; else fail=$((fail+1)); printf 'FAIL want %s got %s: %s\n' "$exp" "$got" "$name"; fi
}
claim_case block "deployed, no evidence"          "Deployed the new build."
claim_case allow "deployed, DEPLOY OK"            "Deployed the new build." "mister-deploy bin/MiSTer" "DEPLOY OK host=h md5=0123"
claim_case allow "deployed, md5 read back"        "It is deployed." "ssh h md5sum /media/fat/MiSTer" "0123456789abcdef0123456789abcdef  /media/fat/MiSTer"
claim_case allow "not deployed"                   "The build is not deployed yet."
claim_case allow "second stop always passes"      "Deployed the new build." "" "" true
claim_case block "merged, no merge ran"           "Merged into classic-ui."
claim_case allow "merged, git merge ran"          "Merged into classic-ui." "git -C ../wt merge --no-ff -q topic" "abc1234 Merge topic"
claim_case block "merged, merge conflicted"       "Merged into classic-ui." "git merge topic" "CONFLICT (content): Merge conflict in a.cpp"
claim_case block "merged, only merge-base"        "Merged into classic-ui." "git merge-base --is-ancestor a b" ""
claim_case allow "merged, read back from log"     "It was merged into classic-ui as 0f0dc724." "git log --oneline -1" "0f0dc724 Merge classicui-ngpc-ss: NGPC"
claim_case allow "no claim at all"                "Here is the summary."
rm -rf "$TD"
rm -rf "$CLASSICUI_CFG"
echo "== TOTAL pass=$pass fail=$fail"
