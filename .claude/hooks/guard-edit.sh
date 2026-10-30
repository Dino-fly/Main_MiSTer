#!/usr/bin/env bash
# guard-edit.sh - PreToolUse hook for Edit/Write/MultiEdit.
# Blocks editing a shell script that is currently executing: bash reads scripts by byte
# offset, so an in-place edit silently derails every running instance (four spurious
# farm failures on 2026-09-04). Copy, edit the copy, launch the copy. Fails open.
IN=$(cat 2>/dev/null)
command -v jq >/dev/null 2>&1 || exit 0
F=$(printf '%s' "$IN" | jq -r '.tool_input.file_path // empty' 2>/dev/null)
[ -n "$F" ] || exit 0
case "$F" in *.sh) ;; *) exit 0 ;; esac
B=$(basename "$F")
if ps -axo command 2>/dev/null | grep -v grep | grep -qF -- "$B"; then
	printf 'BLOCKED by .claude/hooks/guard-edit.sh: %s is executing right now; copy it, edit the copy, launch the copy (never edit a running script in place)\n' "$B" >&2
	exit 2
fi
exit 0
