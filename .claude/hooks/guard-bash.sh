#!/usr/bin/env bash
# guard-bash.sh - PreToolUse hook for the Bash tool. Reads the tool-call JSON on stdin.
# Exit 2 + a line on stderr blocks the command; exit 0 lets it run.
#
# Always blocked (fail-closed, works even without jq):
#   - broad container/process kills: docker kill/rm/stop $(docker ps ...), pkill -f, kill -1,
#     killall on anything but the literal `MiSTer`
#   - rm of /media/fat/MiSTer
# Blocked unless a human set a one-time marker with `approve-once` (consumed on use):
#   - gh issue/pr/discussion/release/repo writes, gh api with a write method   -> approve-once gh
#   - git push (never to `origin` = MiSTer-devel; a local push-guard may veto) -> approve-once push
#   - git push --force / -f / +ref                                               -> approve-once force-push
# Blocked unless mister-deploy wrote a fresh md5-verified marker (< 30 min):
#   - cp/scp/mv/rsync/tee/dd/redirect onto /media/fat/MiSTer (reads from it are fine)
# Blocked in the main checkout only: make clean, git checkout/switch of a branch.
# Everything else, including every read, passes. Non-destructive checks fail open.
CFG="${CLASSICUI_CFG:-$HOME/.config/classicui}"
MAIN="${MAIN_REPO:-$HOME/workspace/Main_MiSTer}"
IN=$(cat 2>/dev/null)
deny() { printf 'BLOCKED by .claude/hooks/guard-bash.sh: %s\n' "$1" >&2; exit 2; }

CMD=""; CWD=""
if command -v jq >/dev/null 2>&1; then
	CMD=$(printf '%s' "$IN" | jq -r '.tool_input.command // empty' 2>/dev/null)
	CWD=$(printf '%s' "$IN" | jq -r '.cwd // empty' 2>/dev/null)
fi
[ -n "$CMD" ] || CMD="$IN"   # no jq or unparsable input: scan the raw text, fail closed on the destructive set
has() { grep -qE "$1" <<<"$CMD"; }

# ---- 1. destructive, no override ---------------------------------------------------
has 'docker +(container +)?(kill|rm|stop|restart)[^|;&]*\$\(docker +ps' && deny "broad container kill - name the container"
has 'docker +ps +-a?q[^|]*\|[^|]*xargs[^|]*docker +(container +)?(kill|rm|stop|restart)' && deny "broad container kill - name the container"
has '\bpkill\b[^|;&]*[[:space:]]-f([[:space:]]|$)' && deny "pkill -f misses the tar|ssh children; kill by PID (ps ... | awk | xargs kill)"
has '\bkill\b( +-[0-9A-Za-z]+)* +(-- +)?-1([[:space:]]|$)' && deny "kill -1 signals every process"
if has '(^|[^A-Za-z0-9_])killall([^A-Za-z0-9_]|$)'; then
	# every killall must be the literal `killall [-flags] MiSTer` followed by ; && | quote, space or end
	# (BSD sed has no \b, so count occurrences instead of stripping them)
	ALL=$(grep -oE '(^|[^A-Za-z0-9_])killall([^A-Za-z0-9_]|$)' <<<"$CMD" | wc -l | tr -d ' ')
	OK=$(grep -oE '(^|[^A-Za-z0-9_])killall( +-[0-9A-Za-z]+)* +MiSTer([^A-Za-z0-9_.-]|$)' <<<"$CMD" | wc -l | tr -d ' ')
	[ "$ALL" = "$OK" ] || deny "killall on anything but the literal MiSTer process"
fi
has '\brm\b[^|;&]*[[:space:]][^[:space:]]*/media/fat/MiSTer(["'"'"'[:space:];&|]|$)' && deny "never rm the firmware; mister-deploy keeps MiSTer.pre-* rollbacks"

# ---- helpers for the marker-gated checks (fail open on their own errors) ------------
consume() { [ -f "$CFG/allow-once.$1" ] && rm -f "$CFG/allow-once.$1"; }
md5_marker_fresh() {
	[ -f "$CFG/md5-verified" ] || return 1
	local age; age=$(( $(date +%s) - $(stat -f %m "$CFG/md5-verified" 2>/dev/null || echo 0) ))
	[ "$age" -lt 1800 ]
}

# ---- 2. public output and pushes: one-time human approval ---------------------------
if has '\bgh +(issue|pr|discussion|release) +(comment|create|close|edit|merge|review|delete|reopen|lock|unlock|upload|ready|transfer|pin|unpin)\b' \
	|| has '\bgh +(repo|gist) +(create|delete|edit|rename|archive|fork|sync)\b' \
	|| has '\bgh +api\b[^|;&]*(-X +(POST|PUT|PATCH|DELETE)|--method[= ]+(POST|PUT|PATCH|DELETE)|[[:space:]]-[fF][[:space:]]|--field|--raw-field|--input)'; then
	consume gh || deny "public GitHub write - draft it for the owner; a human runs 'approve-once gh' to allow exactly one"
fi
if has '\bgit\b[^|;&]*[[:space:]]push([[:space:]]|$)'; then
	has '[[:space:]]push\b[^|;&]*[[:space:]](origin|[^[:space:]]*MiSTer-devel[^[:space:]]*)([[:space:]]|$)' && deny "push to origin (MiSTer-devel upstream) is never allowed on this fork"
	if has '[[:space:]]push\b[^|;&]*([[:space:]]--force(-with-lease)?\b|[[:space:]]-f([[:space:]]|$)|[[:space:]]\+[A-Za-z])'; then
		consume force-push || deny "force push rewrites published history - 'approve-once force-push' after explicit agreement"
	else
		consume push || deny "git push - a human runs 'approve-once push' to allow exactly one"
	fi
	# A per-machine check outside the repository may veto the push; its reason is its own.
	if [ -x "$CFG/push-guard" ] && [ -n "$CWD" ]; then
		WHY=$("$CFG/push-guard" "$CWD" 2>&1) || deny "local push-guard refused: ${WHY:-no reason given}"
	fi
fi

# ---- 3. firmware onto the device only after an md5-verified staging ------------------
if has '/media/fat/MiSTer(["'"'"'[:space:];&|]|$)'; then
	WRITE=0
	has '>[[:space:]]*[^[:space:]]*/media/fat/MiSTer(["'"'"'[:space:];&|]|$)' && WRITE=1
	has '\bdd\b[^|;&]*of=[^[:space:]]*/media/fat/MiSTer(["'"'"'[:space:];&|]|$)' && WRITE=1
	while IFS= read -r seg; do
		[ -n "$seg" ] || continue
		toks=$(tr -d "\"'" <<<"$seg" | awk '{$1=$1; print}')
		last=${toks##* }
		case " $toks " in *" cp "*|*" mv "*|*" rsync "*|*" scp "*|*" install "*|*" tee "*|*" put "*)
			[[ "$last" == */media/fat/MiSTer ]] && WRITE=1 ;;
		esac
	done < <(sed -E 's/(&&|\|\||;|\|)/\n/g' <<<"$CMD")
	[ "$WRITE" = 1 ] && ! md5_marker_fresh && deny "writing /media/fat/MiSTer without a fresh md5-verified marker - use mister-deploy (stage, verify, rollback copy)"
fi

# ---- 4. the main checkout: no make clean, no branch switch ---------------------------
# Judged by the directory the command actually runs in: its `-C X`, else the last `cd X`
# before it in the same command, else the session cwd - and "main checkout" means
# `git rev-parse --show-toplevel` there equals $MAIN, so every other worktree is free.
effective_dir() {  # $1 = command segment
	local seg="$1" d prefix
	d=$(grep -oE '(^|[[:space:]])-C[[:space:]]+[^[:space:]]+' <<<"$seg" | head -1 | awk '{print $NF}' | tr -d "\"'")
	if [ -z "$d" ]; then
		prefix=${CMD%%"$seg"*}
		d=$(grep -oE '(^|[;&|][[:space:]]*|^[[:space:]]*)cd[[:space:]]+[^[:space:];&|]+' <<<"$prefix" | tail -1 | sed -E 's/.*cd[[:space:]]+//' | tr -d "\"'")
	fi
	[ -n "$d" ] || d="$CWD"
	d=${d/#\~/$HOME}
	case "$d" in /*) ;; *) d="$CWD/$d" ;; esac
	printf '%s' "$d"
}
is_main_dir() { local top; top=$(git -C "$1" rev-parse --show-toplevel 2>/dev/null) || return 1; [ "$top" = "$MAIN" ]; }
while IFS= read -r seg; do
	[ -n "$seg" ] || continue
	if grep -qE '(^|[^A-Za-z0-9_])make([^A-Za-z0-9_]|$)' <<<"$seg" && grep -qE '[[:space:]]clean([[:space:]]|$)' <<<"$seg"; then
		is_main_dir "$(effective_dir "$seg")" && deny "make clean in the main checkout - long builds run there; use a worktree (mister-build refuses it too)"
	fi
	if grep -qE '(^|[^A-Za-z0-9_])git([^A-Za-z0-9_]|$)' <<<"$seg" && grep -qE '[[:space:]](checkout|switch)([[:space:]]|$)' <<<"$seg" && ! grep -qE '[[:space:]](--|--help|-h)([[:space:]]|$)' <<<"$seg"; then
		is_main_dir "$(effective_dir "$seg")" && deny "branch switch in the main checkout - use a worktree (git worktree add ../Main_MiSTer-<x> <branch>)"
	fi
done < <(sed -E 's/(&&|\|\||;|\|)/\n/g' <<<"$CMD")
exit 0
