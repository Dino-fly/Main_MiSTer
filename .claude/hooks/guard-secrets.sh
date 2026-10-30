#!/usr/bin/env bash
# guard-secrets.sh - PreToolUse hook for Bash: do not print credential lines into the transcript.
#
# Blocks a command that would dump a credential-bearing file (MiSTer.ini, any *.env,
# ss_credentials.h, wpa_supplicant.conf, and any name listed one per line in the local
# ~/.config/classicui/private-files) through a printing tool, or
# echo an SS_* variable, UNLESS it is one of the filtered forms:
#   grep -n '^main=' MiSTer.ini          an anchored literal that is not a credential key
#   grep -n '^\[' MiSTer.ini             section headers
#   grep -c|-l|-q ...                    counts / names only
#   grep -v / -Ev 'pass|devid|user|psk'  an inverted credential filter anywhere in the pipe
#   cut -d= -f1 file                     key names only
#   sed 's/=.*/=.../'                    a value redaction
# Reads from the file (scp, cp, md5, wc, ls, stat, test) are not printing and pass.
# Fails open on its own errors - it protects the transcript, not the device.
IN=$(cat 2>/dev/null)
command -v jq >/dev/null 2>&1 || exit 0
CMD=$(printf '%s' "$IN" | jq -r '.tool_input.command // empty' 2>/dev/null)
[ -n "$CMD" ] || exit 0
has() { grep -qE "$1" <<<"$CMD"; }
deny() {
	printf 'BLOCKED by .claude/hooks/guard-secrets.sh: %s\nFiltered alternatives: grep -n "^main=" FILE | grep -n "^\\[" FILE | cut -d= -f1 FILE | grep -Eiv "pass|devid|user|psk" FILE\n' "$1" >&2
	exit 2
}

CFG="${CLASSICUI_CFG:-$HOME/.config/classicui}"
NAMES='MiSTer\.ini|[^[:space:]]*\.env|ss_credentials\.h|wpa_supplicant\.conf'
# More names that must never be printed, kept outside the repository like the files themselves.
if [ -s "$CFG/private-files" ]; then
	while IFS= read -r f; do
		# plain file names: only the dots need escaping; anything else is skipped, not trusted
		grep -qE '^[A-Za-z0-9_.-]+$' <<<"$f" && NAMES="$NAMES|${f//./\\.}"
	done < "$CFG/private-files"
fi
SECRET="($NAMES)"'([[:space:]"'"'"';&|)]|$)'
CRED='(pass|devid|user|psk|secret|token|key|ss_|SS_)'

# echo/printf of a credential variable, or dumping the environment after sourcing an env file
has '\b(echo|printf)\b[^|;&]*\$\{?SS_' && deny "would print an SS_* credential variable"
has '(\bsource\b|(^|[;&|][[:space:]]*)\.[[:space:]])[^|;&]*\.env\b' && has '\b(env|printenv|set|export -p|declare -p)\b' && deny "would dump the environment after sourcing an env file"

has "$SECRET" || exit 0
has '\b(cat|head|tail|less|more|sed|awk|grep|egrep|fgrep|rg|strings|bat|tac|nl|xxd|od|hexdump|cut|sort|uniq|tee|view|vi|vim|nano|open|python3? -c|perl)\b' || exit 0

# filtered forms
has "\bgrep\b[^|;&]*[[:space:]](-[A-Za-z]*v[A-Za-z]*|--invert-match)[^|;&]*$CRED" && exit 0
has '\bgrep\b[^|;&]*[[:space:]]-[A-Za-z]*[clqL][A-Za-z]*([[:space:]]|$)' && exit 0
has "\bcut\b[^|;&]*-d[[:space:]]*[\"']?=[\"']?[^|;&]*-f[[:space:]]*1\b" && exit 0
has '\bsed\b[^|;&]*s/=\\?\.\*' && exit 0
# an anchored literal grep whose literal is not a credential key: '^main=' , '^\[' , '^video_mode'
# (no \b: BSD sed lacks it; quotes and backslashes are stripped so "^\\[" inside an ssh string reads as ^[)
PAT=$(sed -nE "s/.*(^|[^A-Za-z0-9_])grep([[:space:]]+-[A-Za-z]+)*[[:space:]]+(-e[[:space:]]+)?(\"[^\"]*\"|'[^']*'|[^[:space:]]+)[[:space:]]+[^[:space:]]*($NAMES).*/\4/p" <<<"$CMD" | tr -d "\"'\\\\")
if [ -n "$PAT" ] && grep -qE '^\^(\[|[A-Za-z_][A-Za-z0-9_=-]*)$' <<<"$PAT" && ! grep -qiE "$CRED" <<<"$PAT"; then exit 0; fi

deny "would print credential lines from a secrets file (MiSTer.ini / *.env / ss_credentials.h / wpa_supplicant.conf)"
