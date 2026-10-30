#!/usr/bin/env bash
# session-start.sh - SessionStart hook (startup, clear, resume, compact). Its stdout is
# added to the conversation as context, so it is kept under ~15 lines: the worktrees and
# their branches, uncommitted counts, and the "Next" section of .claude/ROADMAP.md.
# That section is what /wrap-up leaves for the next session. Read-only; fails open.
MAIN=$HOME/workspace/Main_MiSTer
ROADMAP="$MAIN/.claude/ROADMAP.md"
IN=$(cat 2>/dev/null)
SRC=$(printf '%s' "$IN" | jq -r '.source // "startup"' 2>/dev/null); SRC=${SRC:-startup}
echo "[session-start:$SRC $(date +%F\ %H:%M)] worktrees (branch, uncommitted files):"
git -C "$MAIN" worktree list --porcelain 2>/dev/null | awk '/^worktree /{print substr($0,10)}' | while IFS= read -r wt; do
	[ -d "$wt" ] || continue
	case "$wt" in */.claude/worktrees/*) continue ;; esac
	b=$(git -C "$wt" rev-parse --abbrev-ref HEAD 2>/dev/null)
	n=$(git -C "$wt" status --porcelain 2>/dev/null | grep -vc 'ChangeLog' )
	printf '  %-40s %-26s %s\n' "${wt#"$HOME"/workspace/}" "[$b]" "$([ "$n" = 0 ] && echo clean || echo "$n uncommitted")"
done
if [ -f "$ROADMAP" ]; then
	echo "ROADMAP next (from .claude/ROADMAP.md; /wrap-up maintains it):"
	awk '/^## Next/{f=1; next} /^## /{f=0} f && NF' "$ROADMAP" | head -8 | sed 's/^/  /'
else
	echo "no .claude/ROADMAP.md yet - /wrap-up creates it"
fi
echo "Rules: .claude/WORKFLOW.md (lifecycle, caching, models). Device work: /device-test. End of task: /wrap-up."
exit 0
