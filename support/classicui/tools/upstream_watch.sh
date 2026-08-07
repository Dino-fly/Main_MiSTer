#!/bin/bash
#
# Watches upstream for a new release and opens a draft PR that merges it.
#
#   upstream_watch.sh [--detect-only] [--dry-run] [--retry] [--force]
#                     [--repo DIR] [--upstream-ref REF] [--base BRANCH]
#                     [--last RELEASE] [--state-dir DIR] [--jobs N]
#                     [--toolchain DIR] [--ss-env FILE]
#   upstream_watch.sh --install-schedule | --uninstall-schedule | --schedule-status
#   upstream_watch.sh --scrub    < some-log     # prove the redaction works
#
# MiSTer-devel/Main_MiSTer has no tags and no release branches. A release is a
# dated *binary committed into the repository* under releases/MiSTer_YYYYMMDD, so
# "a new upstream release" is precisely "a commit that adds a new releases/MiSTer_*
# file". That is the thing this looks for, because that is the cadence users
# actually receive - syncing continuously would mean merging work upstream has not
# shipped yet and cannot be compared against anything a player is running.
#
# On finding one it merges that exact commit (not upstream master, which has moved
# on since) into a throwaway work branch off the published deploy-all, cross-builds
# for ARM, runs the Classic Home harness, and only then pushes and opens a *draft*
# PR. A merge that does not build, or that drops a harness check, never reaches
# GitHub - it is reported here instead and left for a human, because the last sync
# hit two conflicts and both needed a judgement call that no script can make (our
# snacpad_poll() landing on the same line as upstream's new mac_poll(), and a
# whole-file MiSTer.ini conflict that was only line endings).
#
# Nothing about this script is allowed to print a ScreenScraper credential. The
# build needs them - they are compiled into the binary - so the container gets the
# env file mounted, but every log line that could carry one goes through scrub()
# before it is written anywhere, and no build log is ever quoted into a PR body.

set -euo pipefail

# --------------------------------------------------------------------------- args

DETECT_ONLY=0
DRY_RUN=0
RETRY=0
FORCE=0
REPO=""
UPSTREAM_REF=""
BASE_BRANCH="deploy-all"
LAST_OVERRIDE=""
STATE_DIR=""
JOBS=""
TOOLCHAIN=""
SCHED_ACTION=""
SS_ENV="${SS_ENV:-$HOME/.config/classicui/ss.env}"

die() { printf 'upstream_watch: %s\n' "$*" >&2; exit 2; }

while [ $# -gt 0 ]; do
	case "$1" in
	--detect-only)   DETECT_ONLY=1 ;;
	--dry-run)       DRY_RUN=1 ;;
	--retry)         RETRY=1 ;;
	--force)         FORCE=1 ;;
	--repo)          REPO="$2"; shift ;;
	--upstream-ref)  UPSTREAM_REF="$2"; shift ;;
	--base)          BASE_BRANCH="$2"; shift ;;
	--last)          LAST_OVERRIDE="$2"; shift ;;
	--state-dir)     STATE_DIR="$2"; shift ;;
	--jobs)          JOBS="$2"; shift ;;
	--toolchain)     TOOLCHAIN="$2"; shift ;;
	--ss-env)        SS_ENV="$2"; shift ;;
	--install-schedule)   SCHED_ACTION=install ;;
	--uninstall-schedule) SCHED_ACTION=uninstall ;;
	--schedule-status)    SCHED_ACTION=status ;;
	--scrub)              SCRUB_ONLY=1 ;;
	# The header block, however long it grows, rather than a hardcoded line range
	# that silently truncates the usage the next time a paragraph is added.
	-h|--help)       awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
	*)               die "unknown option: $1" ;;
	esac
	shift
done

# The repository defaults to the one this script is checked out in, so that a
# launchd job pointing at the canonical checkout needs no path in its plist and a
# test run out of a scratch worktree operates on that worktree instead.
if [ -z "$REPO" ]; then
	REPO="$(git -C "$(dirname "$(realpath "$0")")" rev-parse --show-toplevel)"
fi
[ -d "$REPO/.git" ] || [ -f "$REPO/.git" ] || die "not a git checkout: $REPO"

: "${STATE_DIR:=${XDG_STATE_HOME:-$HOME/.local/state}/classicui/upstream-watch}"
mkdir -p "$STATE_DIR" "$STATE_DIR/logs" "$STATE_DIR/reports" "$STATE_DIR/failed" "$STATE_DIR/work"

RUN_TS="$(date -u +%Y%m%dT%H%M%SZ)"
RUNLOG="$STATE_DIR/logs/run-$RUN_TS.log"

# --------------------------------------------------------------- logging, secrets

# Every credential in the env file is replaced by <redacted> before anything is
# written to a log, a report or a terminal. The values only ever exist inside this
# awk process: putting them in a shell variable would mean one stray `set -x`, or
# one `ps` at the wrong moment, leaks the password that the whole ss_creds.sh
# header dance exists to keep off the command line.
#
# Substitution is index/substr rather than gsub, because gsub takes its needle as a
# regular expression and a password containing '.' or '+' would then match - and,
# far worse, one containing an unbalanced '[' would make awk fail and pass the text
# through unredacted at exactly the moment it mattered.
scrub() {
	if [ -r "$SS_ENV" ]; then
		awk -v envfile="$SS_ENV" '
		BEGIN {
			n = 0
			while ((getline line < envfile) > 0) {
				if (line !~ /^[A-Za-z_][A-Za-z0-9_]*=/) continue
				v = substr(line, index(line, "=") + 1)
				sub(/^["\047]/, "", v); sub(/["\047]$/, "", v)
				if (length(v) >= 4) secrets[++n] = v
			}
			close(envfile)
		}
		{
			for (i = 1; i <= n; i++) {
				while ((p = index($0, secrets[i])) > 0)
					$0 = substr($0, 1, p - 1) "<redacted>" substr($0, p + length(secrets[i]))
			}
			print
		}'
	else
		cat
	fi
}

# Exposed so that the redaction can be checked without taking it on trust: pipe a
# line containing a credential through `upstream_watch.sh --scrub` and see it come
# back redacted. There is no way to verify a filter like this by reading it.
if [ "${SCRUB_ONLY:-0}" = 1 ]; then scrub; exit 0; fi

say() { printf '%s\n' "$*" | scrub | tee -a "$RUNLOG"; }
note() { printf '  %s\n' "$*" | scrub | tee -a "$RUNLOG"; }

# ------------------------------------------------------------------------ schedule

# Once a day, at 05:15 local, and the interval is the one design decision here worth
# arguing about. Upstream cut its last four releases on 20 Feb, 25 Mar, 3 Jun and
# 7 Jul - roughly monthly, never twice in a fortnight. Hourly would be 720 fetches
# to catch one event and would still not make the PR land any sooner than the
# morning somebody reads it. Weekly would be cheaper and would sit on a release for
# up to seven days for no benefit, since the expensive part only runs when there
# genuinely is one: the daily cost is a single `git fetch`, and the Docker build
# fires at most once a month. Daily is where latency stops improving and cost stops
# mattering.
#
# 05:15 rather than midnight because the run wants the machine to itself. A clean
# ARM build under Rosetta plus the harness is a serious amount of CPU, the owner has
# hour-long builds of his own going during the day, and finishing before he sits
# down means a new release is already a draft PR by the time he looks.
#
# launchd rather than crontab, and StartCalendarInterval rather than StartInterval:
# a calendar job whose time passed while the Mac was asleep runs when it wakes,
# where a crontab entry is simply missed. On a laptop that is shut overnight that is
# the difference between a watcher and a decoration.
SCHED_LABEL="com.dino-fly.classicui.upstream-watch"
SCHED_PLIST="$HOME/Library/LaunchAgents/$SCHED_LABEL.plist"
SCHED_HOUR=5
SCHED_MINUTE=15

if [ -n "$SCHED_ACTION" ]; then
	# The scheduled job must point at the canonical checkout, never at whichever
	# scratch worktree this copy of the script happens to be sitting in - those get
	# deleted, and a launchd job pointing into a deleted worktree fails every day
	# forever. The main worktree is the parent of the shared .git directory.
	sched_repo="$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)")"
	sched_script="$sched_repo/support/classicui/tools/upstream_watch.sh"

	# ...unless told otherwise. The canonical checkout is the right default but it is
	# not always a usable one: it has whatever branch its owner is working on checked
	# out, and if that branch does not carry this file the job is a daily no-op until
	# somebody merges. Seen for real - the checkout was on `snac-pr` while this tool
	# lived on `deploy-all`. CLASSICUI_WATCH_SCRIPT points the job at a stable copy
	# instead (say ~/.local/bin/upstream_watch.sh), which is also the answer for
	# anyone who would rather a nightly job did not depend on the tree they edit.
	#
	# The repo it operates on is still resolved at run time by the copy that runs, so
	# an override only changes which script launchd starts, not what it works on.
	if [ -n "${CLASSICUI_WATCH_SCRIPT:-}" ]; then
		sched_script="$CLASSICUI_WATCH_SCRIPT"
		case "$sched_script" in
		/*) ;;
		*) echo "CLASSICUI_WATCH_SCRIPT must be an absolute path: $sched_script" >&2; exit 2 ;;
		esac
	fi

	case "$SCHED_ACTION" in
	install)
		mkdir -p "$(dirname "$SCHED_PLIST")"
		# launchd hands a job a bare PATH with no /opt/homebrew and no
		# /usr/local, so docker, gh and git are all missing from it. A watcher
		# that works by hand and dies at 05:15 with "docker: command not found"
		# is the classic way to ship this broken, so the PATH is spelled out.
		#
		# The wrapper checks the script exists before running it. This tool is
		# committed on a feature branch; until that branch is merged the
		# canonical checkout has no such file, and the job should say so once a
		# day rather than emit a launchd spawn error.
		cat > "$SCHED_PLIST" <<-PLIST
		<?xml version="1.0" encoding="UTF-8"?>
		<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
		<plist version="1.0">
		<dict>
		  <key>Label</key><string>$SCHED_LABEL</string>
		  <key>ProgramArguments</key>
		  <array>
		    <string>/bin/sh</string>
		    <string>-c</string>
		    <string>S=$sched_script; if [ -x "\$S" ]; then exec "\$S"; else echo "upstream_watch: \$S is not in the checkout yet (branch not merged) - nothing to do."; fi</string>
		  </array>
		  <key>WorkingDirectory</key><string>$sched_repo</string>
		  <key>EnvironmentVariables</key>
		  <dict>
		    <key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
		  </dict>
		  <key>StartCalendarInterval</key>
		  <dict>
		    <key>Hour</key><integer>$SCHED_HOUR</integer>
		    <key>Minute</key><integer>$SCHED_MINUTE</integer>
		  </dict>
		  <key>RunAtLoad</key><false/>
		  <key>ProcessType</key><string>Background</string>
		  <key>Nice</key><integer>5</integer>
		  <key>StandardOutPath</key><string>$STATE_DIR/logs/launchd.out</string>
		  <key>StandardErrorPath</key><string>$STATE_DIR/logs/launchd.err</string>
		</dict>
		</plist>
		PLIST
		launchctl bootout "gui/$(id -u)/$SCHED_LABEL" >/dev/null 2>&1 || true
		launchctl bootstrap "gui/$(id -u)" "$SCHED_PLIST" \
			|| launchctl load -w "$SCHED_PLIST" \
			|| die "launchctl refused $SCHED_PLIST"
		echo "installed $SCHED_LABEL: daily at $(printf '%02d:%02d' "$SCHED_HOUR" "$SCHED_MINUTE") local"
		echo "  plist:  $SCHED_PLIST"
		echo "  runs:   $sched_script"
		echo "  logs:   $STATE_DIR/logs/"
		[ -x "$sched_script" ] || echo "  NOTE: that path does not exist yet - the job no-ops until the branch carrying this script is checked out there."
		;;
	uninstall)
		launchctl bootout "gui/$(id -u)/$SCHED_LABEL" >/dev/null 2>&1 \
			|| launchctl unload -w "$SCHED_PLIST" >/dev/null 2>&1 || true
		rm -f "$SCHED_PLIST"
		echo "removed $SCHED_LABEL"
		;;
	status)
		echo "label:  $SCHED_LABEL"
		echo "plist:  $SCHED_PLIST $([ -f "$SCHED_PLIST" ] && echo '(present)' || echo '(MISSING)')"
		echo "script: $sched_script $([ -x "$sched_script" ] && echo '(present)' || echo '(NOT IN THE CHECKOUT YET)')"
		echo "state:  $STATE_DIR"
		echo "last handled release: $(cat "$STATE_DIR/last_release" 2>/dev/null || echo '<none recorded>')"
		echo "--- launchctl ---"
		launchctl print "gui/$(id -u)/$SCHED_LABEL" 2>&1 \
			| grep -E 'state|program|path|runs = |last exit|next fire' || echo "not loaded"
		;;
	esac
	exit 0
fi

# --------------------------------------------------------------------- exit paths

WORKTREE=""
WORK_BRANCH=""
PUSHED=0

# Unattended means nobody is going to notice a worktree left mid-merge, and the
# person who trips over it is whoever next runs `git worktree list` and wonders
# which of eleven directories is real. So the merge is aborted, the worktree is
# removed and the branch is deleted on every path out of here - including the
# signal paths, which is why the trap covers INT and TERM as well as EXIT.
cleanup() {
	local rc=$?
	set +e
	if [ -n "$WORKTREE" ] && [ -d "$WORKTREE" ]; then
		git -C "$WORKTREE" merge --abort >/dev/null 2>&1
		git -C "$REPO" worktree remove --force "$WORKTREE" >/dev/null 2>&1 \
			|| rm -rf "$WORKTREE"
	fi
	git -C "$REPO" worktree prune >/dev/null 2>&1
	if [ -n "$WORK_BRANCH" ] && [ "$PUSHED" -eq 0 ]; then
		git -C "$REPO" branch -D "$WORK_BRANCH" >/dev/null 2>&1
	fi
	# rm -rf, not rmdir: the lock directory holds the pid file that identifies its
	# owner, so rmdir always fails on it and every run after the first announced
	# that it was clearing a stale lock left by a process that had exited cleanly
	# a second earlier.
	if [ -n "${LOCKDIR:-}" ]; then rm -rf "$LOCKDIR" >/dev/null 2>&1; fi
	return $rc
}
trap cleanup EXIT INT TERM

# A report is the failure channel. It goes to stdout so that whatever ran this
# picks it up, and to a file so that it is still there tomorrow when somebody asks
# what happened. Loud on purpose: the alternative to a shouty report is a script
# that quietly stops syncing and is only noticed months later.
report_failure() {
	local subject="$1"; shift
	local f="$STATE_DIR/reports/${REL:-unknown}-$RUN_TS.txt"
	{
		echo "=============================================================="
		echo "UPSTREAM SYNC FAILED: $subject"
		echo "release:  ${REL:-n/a}"
		echo "commit:   ${REL_SHA:-n/a}"
		echo "base:     $BASE_BRANCH ${BASE_SHA:-n/a}"
		echo "when:     $RUN_TS"
		echo "run log:  $RUNLOG"
		echo "--------------------------------------------------------------"
		cat
		echo "=============================================================="
		echo "Nothing was pushed. No PR was opened. Resolve by hand:"
		echo "  git worktree add -b upstream-sync/${REL:-REL} /tmp/sync $FORK_REMOTE/$BASE_BRANCH"
		echo "  git -C /tmp/sync merge ${REL_SHA:-REL_SHA}"
		echo "Then rerun with --retry to clear the recorded failure."
	} | scrub | tee -a "$RUNLOG" > "$f"
	cat "$f"
	[ "$DRY_RUN" -eq 1 ] || touch "$STATE_DIR/failed/${REL:-unknown}"
}

# ---------------------------------------------------------------- single instance

# mkdir is the atomic primitive available in every shell; flock is not on macOS.
# A run can take twenty minutes under Rosetta, so an overlapping run is not
# hypothetical - and two of them merging the same release into the same branch name
# is exactly the "two PRs for one release" that idempotency has to prevent.
LOCKDIR="$STATE_DIR/lock"
if ! mkdir "$LOCKDIR" 2>/dev/null; then
	holder="$(cat "$LOCKDIR/pid" 2>/dev/null || echo '?')"
	if [ "$holder" != '?' ] && kill -0 "$holder" 2>/dev/null; then
		echo "upstream_watch: pid $holder is already running; nothing to do."
		LOCKDIR=""
		exit 0
	fi
	echo "upstream_watch: clearing a stale lock left by pid $holder"
	rm -rf "$LOCKDIR"; mkdir "$LOCKDIR" || die "cannot take the lock"
fi
echo $$ > "$LOCKDIR/pid"

# --------------------------------------------------------------------- remotes

remote_matching() {
	local pat="$1" r
	for r in $(git -C "$REPO" remote); do
		case "$(git -C "$REPO" remote get-url "$r")" in
		*"$pat"*) echo "$r"; return 0 ;;
		esac
	done
	return 1
}

UPSTREAM_REMOTE="$(remote_matching 'MiSTer-devel/Main_MiSTer')" \
	|| die "no remote points at MiSTer-devel/Main_MiSTer"
FORK_REMOTE="$(remote_matching 'Dino-fly/Main_MiSTer')" \
	|| die "no remote points at Dino-fly/Main_MiSTer"
FORK_SLUG="$(git -C "$REPO" remote get-url "$FORK_REMOTE" \
	| sed -e 's#.*[:/]\([^/]*/[^/]*\)$#\1#' -e 's#\.git$##')"

say "upstream_watch $RUN_TS  repo=$REPO upstream=$UPSTREAM_REMOTE fork=$FORK_REMOTE ($FORK_SLUG)"

if [ -z "$UPSTREAM_REF" ]; then
	git -C "$REPO" fetch --quiet "$UPSTREAM_REMOTE" master || die "fetch $UPSTREAM_REMOTE failed"
	git -C "$REPO" fetch --quiet "$FORK_REMOTE" || die "fetch $FORK_REMOTE failed"
	UPSTREAM_REF="$UPSTREAM_REMOTE/master"
fi

# --------------------------------------------------------------------- detection

releases_at() {
	git -C "$REPO" ls-tree --name-only "$1" releases/ \
		| sed -n 's#^releases/\(MiSTer_[0-9]\{8\}\)$#\1#p' | sort
}

# The floor is what we have already handled. First ever run has no state, and
# seeding it from the newest release already merged into the base is the only
# sensible answer - the alternative is a first run that decides all 95 historical
# releases are new and tries to merge the oldest one.
if [ -n "$LAST_OVERRIDE" ]; then
	LAST="$LAST_OVERRIDE"
	note "floor from --last: $LAST (real state not consulted or written)"
elif [ -s "$STATE_DIR/last_release" ]; then
	LAST="$(cat "$STATE_DIR/last_release")"
	note "floor from state: $LAST"
else
	LAST="$(releases_at "$FORK_REMOTE/$BASE_BRANCH" | tail -1)"
	note "no state yet; seeding the floor from $FORK_REMOTE/$BASE_BRANCH: $LAST"
fi
[ -n "$LAST" ] || die "cannot establish a floor release"

NEW="$(releases_at "$UPSTREAM_REF" | awk -v last="$LAST" '$0 > last')"

if [ -z "$NEW" ]; then
	say "no new upstream release at $UPSTREAM_REF (newest is $(releases_at "$UPSTREAM_REF" | tail -1), floor is $LAST) - nothing to do."
	exit 0
fi

# Oldest first, one per run. Two releases outstanding is a backlog, and merging
# them in a single PR would hide which of the two broke something.
REL="$(printf '%s\n' "$NEW" | head -1)"
COUNT="$(printf '%s\n' "$NEW" | wc -l | tr -d ' ')"
say "new upstream release: $REL  ($COUNT outstanding)"
if [ "$COUNT" -gt 1 ]; then
	note "also pending, one run each: $(printf '%s\n' "$NEW" | tail -n +2 | tr '\n' ' ')"
fi

# The commit that ADDED the file, not the newest commit that touched it. Taking
# the last line of the log handles a path that was added, removed and re-added:
# the first addition is the release, later ones are repository housekeeping.
REL_SHA="$(git -C "$REPO" log --diff-filter=A --format=%H "$UPSTREAM_REF" -- "releases/$REL" | tail -1)"
[ -n "$REL_SHA" ] || die "$REL exists in the tree but no commit adds it"
REL_SUBJ="$(git -C "$REPO" log -1 --format=%s "$REL_SHA")"
REL_DATE="$(git -C "$REPO" log -1 --format=%cs "$REL_SHA")"
say "added by $REL_SHA on $REL_DATE: $REL_SUBJ"

if [ "$DETECT_ONLY" -eq 1 ]; then
	exit 0
fi

# --------------------------------------------------------------- already handled?

WORK_BRANCH="upstream-sync/$REL"

if [ -f "$STATE_DIR/failed/$REL" ] && [ "$RETRY" -eq 0 ] && [ "$FORCE" -eq 0 ]; then
	say "$REL was already attempted and failed; the report is in $STATE_DIR/reports/."
	say "Not retrying on a schedule - fix it and rerun with --retry."
	exit 0
fi
if [ "$RETRY" -eq 1 ]; then rm -f "$STATE_DIR/failed/$REL"; fi

# Three independent answers to "have I done this already", because state on disk
# is the one that a reinstalled machine loses and GitHub is the one that matters.
if [ "$FORCE" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
	if git -C "$REPO" ls-remote --exit-code --heads "$FORK_REMOTE" "$WORK_BRANCH" >/dev/null 2>&1; then
		say "$WORK_BRANCH already exists on $FORK_REMOTE - a previous run got this far."
		echo "$REL" > "$STATE_DIR/last_release"
		exit 0
	fi
	if command -v gh >/dev/null 2>&1; then
		existing="$(gh pr list --repo "$FORK_SLUG" --head "$WORK_BRANCH" --state all \
			--json url --jq '.[0].url' 2>/dev/null || true)"
		if [ -n "$existing" ] && [ "$existing" != "null" ]; then
			say "a PR for $REL already exists: $existing"
			echo "$REL" > "$STATE_DIR/last_release"
			exit 0
		fi
	fi
fi

# The owner has hour-long ARM builds running in his own checkout, and starting a
# second Rosetta-emulated compile on top of one is how a nightly job earns itself
# a place in the trash. Deferring costs a day; upstream cuts a release a month.
#
# `pgrep` for the compiler is the obvious check and it is worthless: the compiler
# runs inside Docker, whose processes live in a Linux VM that the host process
# table cannot see, so pgrep would find nothing however busy the machine was.
# Running ubuntu:20.04 containers is the visible proxy - every build and every
# harness run in this tree is one.
#
# --force skips this, because somebody running it by hand has already decided.
BUSY="$(docker ps --filter ancestor=ubuntu:20.04 -q 2>/dev/null | grep -c . || true)"
if [ "$FORCE" -eq 0 ] && [ "${BUSY:-0}" -gt 0 ]; then
	say "$BUSY ubuntu:20.04 container(s) already running - a build or harness is in flight."
	say "Deferring $REL to the next run rather than competing for the emulator."
	exit 0
fi

# ----------------------------------------------------------------------- worktree

BASE_SHA="$(git -C "$REPO" rev-parse "$FORK_REMOTE/$BASE_BRANCH")"
if git -C "$REPO" rev-parse --verify --quiet "$BASE_BRANCH" >/dev/null; then
	local_base="$(git -C "$REPO" rev-parse "$BASE_BRANCH")"
	[ "$local_base" = "$BASE_SHA" ] || note "note: local $BASE_BRANCH ($(git -C "$REPO" rev-parse --short "$local_base")) differs from $FORK_REMOTE/$BASE_BRANCH; building on the published one."
fi

WORKTREE="$STATE_DIR/work/$REL"
rm -rf "$WORKTREE"
git -C "$REPO" worktree prune
git -C "$REPO" branch -D "$WORK_BRANCH" >/dev/null 2>&1 || true
git -C "$REPO" worktree add -b "$WORK_BRANCH" "$WORKTREE" "$BASE_SHA" >>"$RUNLOG" 2>&1 \
	|| die "cannot create the work worktree"
say "work branch $WORK_BRANCH off $BASE_BRANCH ${BASE_SHA:0:9} in $WORKTREE"

# -------------------------------------------------------------------------- merge

# Ask git whether there is anything to merge, before merging. The first version of
# this asked afterwards, by checking whether the working tree was dirty - and a
# freshly created worktree is *always* dirty in this repository, because upstream
# normalised line endings to LF and lib/miniz/ChangeLog.md still checks out as CRLF.
# So a release already contained in deploy-all looked like a real merge, went on to
# a full ARM build, and would have opened a pull request whose only content was that
# line-ending churn. --is-ancestor is the question that was actually being asked.
if git -C "$WORKTREE" merge-base --is-ancestor "$REL_SHA" HEAD; then
	say "$REL_SHA is already an ancestor of $BASE_BRANCH - recording $REL as handled, nothing to merge."
	if [ "$DRY_RUN" -eq 0 ]; then echo "$REL" > "$STATE_DIR/last_release"; fi
	exit 0
fi

MERGED_LOG="$(git -C "$WORKTREE" log --no-merges --format='%h %s' "$BASE_SHA..$REL_SHA")"
MERGED_N="$(printf '%s\n' "$MERGED_LOG" | grep -c . || true)"
DIFFSTAT="$(git -C "$WORKTREE" diff --stat "$BASE_SHA...$REL_SHA" | tail -1 | sed 's/^ *//')"

# --no-commit so that no commit exists until it has built and passed. A merge commit
# created first and validated second leaves a broken commit on a branch when the
# build dies, and the honest thing to do with a merge that does not compile is to
# never have recorded it.
if ! git -C "$WORKTREE" merge --no-commit --no-ff "$REL_SHA" >>"$RUNLOG" 2>&1; then
	conflicts="$(git -C "$WORKTREE" diff --name-only --diff-filter=U)"
	report_failure "merge conflicts in $REL" <<-EOF
	$MERGED_N upstream commits do not merge cleanly. Conflicted paths:

	$conflicts

	Both conflicts in the previous sync were mechanical, and these may be too:
	user_io.cpp conflicts because our snacpad_poll() sits on the same line as a
	new upstream poll - keep both. MiSTer.ini conflicts whole-file when line
	endings disagree; compare with the endings equalised before reading anything
	into it, ours has been a strict superset.
	EOF
	exit 1
fi

# The index only, never the working tree: see the --is-ancestor comment above. An
# empty index after a merge that was not a no-op would mean git merged something and
# produced nothing, which is worth stopping for rather than committing.
if git -C "$WORKTREE" diff --cached --quiet; then
	report_failure "merge of $REL staged nothing" <<-EOF
	$REL_SHA is not an ancestor of $BASE_BRANCH, so there was something to merge,
	but the merge left an empty index. Something is wrong with the merge itself
	rather than with its result, and committing an empty merge is not the answer.
	EOF
	exit 1
fi
say "merged cleanly: $MERGED_N upstream commits, $DIFFSTAT"

# -------------------------------------------------------------------------- build

if [ -z "$TOOLCHAIN" ]; then
	main_wt="$(dirname "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)")"
	for c in "$REPO"/gcc-arm-*-arm-none-linux-gnueabihf "$main_wt"/gcc-arm-*-arm-none-linux-gnueabihf; do
		if [ -x "$c/bin/arm-none-linux-gnueabihf-g++" ]; then TOOLCHAIN="$c"; break; fi
	done
fi
[ -n "$TOOLCHAIN" ] || { report_failure "no ARM toolchain" </dev/null; exit 1; }

BUILDLOG="$STATE_DIR/logs/$REL-build.log"
say "cross-building for ARM with $(basename "$TOOLCHAIN")"

# `make clean` first is not politeness. Incremental make in this tree does not
# reliably rebuild after a checkout, and what comes out the other side is a single
# binary holding two different struct layouts - which links, runs, and is wrong.
#
# The credentials file is mounted, never echoed. It has to be: devid and
# devpassword are compiled in, and a build without them produces a binary whose
# ScreenScraper path is compile-time dead, so a green build here would say nothing
# about the code the release actually ships.
docker run --rm --platform linux/amd64 \
	-v "$WORKTREE":/mister \
	-v "$SS_ENV":/ss.env:ro \
	-v "$TOOLCHAIN":/tc:ro \
	-w /mister ubuntu:20.04 bash -c \
	'apt-get update -qq && apt-get install -qq -y make >/dev/null 2>&1
	 export PATH=/tc/bin:$PATH
	 make SS_ENV=/ss.env clean >/dev/null
	 make SS_ENV=/ss.env -j'"${JOBS:-\$(nproc)}" \
	>"$BUILDLOG" 2>&1 || {
		# Not `tail`. The build runs with -j, so by the time it gives up the last
		# forty lines are the names of files that started compiling after the
		# error and the error itself is a hundred lines up - the first attempt at
		# this reported a wall of filenames and not one word about what broke.
		report_failure "ARM build failed for $REL" <<-EOF
		The merge is clean but does not compile. The whole log is $BUILDLOG;
		these are the lines that say why, with any credential redacted:

		$(grep -nE 'error:|Error [0-9]+|undefined reference|ld returned' "$BUILDLOG" | head -30)

		Last few lines, for make's own verdict:

		$(tail -6 "$BUILDLOG")
		EOF
		exit 1
	}

[ -f "$WORKTREE/bin/MiSTer" ] || { report_failure "build produced no bin/MiSTer" <<<"$(tail -20 "$BUILDLOG")"; exit 1; }
BIN_SIZE="$(wc -c < "$WORKTREE/bin/MiSTer" | tr -d ' ')"
BIN_SHA="$(shasum -a 256 "$WORKTREE/bin/MiSTer" | cut -c1-16)"
say "built bin/MiSTer, $BIN_SIZE bytes, sha256 $BIN_SHA..."

# ------------------------------------------------------------------------ harness

TESTLOG="$STATE_DIR/logs/$REL-harness.log"
say "running support/classicui/test/run.sh"
harness_rc=0
"$WORKTREE/support/classicui/test/run.sh" >"$TESTLOG" 2>&1 || harness_rc=$?

SUMMARY="$(grep -E '^[0-9]+ checks, [0-9]+ failures$' "$TESTLOG" | tail -1 || true)"
CHECKS="$(printf '%s' "$SUMMARY" | awk '{print $1}')"
FAILS="$(printf '%s' "$SUMMARY" | awk '{print $3}')"

# Three separate ways this can be wrong and all three are fatal: a nonzero exit, a
# nonzero failure count, and no summary line at all. The third is the one worth
# spelling out - a harness that crashed halfway prints no summary, and treating a
# missing summary as "no failures reported" would turn the loudest possible signal
# into a green tick. There is no path in here that edits a test to make it pass.
if [ "$harness_rc" -ne 0 ] || [ -z "$SUMMARY" ] || [ "${FAILS:-1}" -ne 0 ]; then
	report_failure "HARNESS FAILED for $REL" <<-EOF
	exit status $harness_rc, summary line: ${SUMMARY:-<none printed>}

	The last few lines the harness printed, in case it died rather than failed:

	$(tail -8 "$TESTLOG")

	Full log is $TESTLOG. The checks that failed, which is the part that matters:

	$(grep -n 'FAIL' "$TESTLOG" | head -40)

	This is a real regression in the merge, not a flaky test. Do not relax a
	check to clear it.
	EOF
	exit 1
fi
say "harness: $CHECKS checks, $FAILS failures"

# ------------------------------------------------------------------ commit and PR

COMMITMSG="$STATE_DIR/reports/$REL-commit.txt"
cat > "$COMMITMSG" <<EOF
Merge upstream release $REL into the Classic Home line

Upstream has no tags, so its releases are the dated binaries committed under
releases/. The commit that added releases/$REL on $REL_DATE is what this
merges - ${REL_SHA:0:9}, not upstream master, which has moved on since and
contains work no player has been given yet.

$MERGED_N upstream commits since ${BASE_SHA:0:9}, $DIFFSTAT. The merge was clean.
Cross-built for ARM at $BIN_SIZE bytes and the Classic Home harness reported
$CHECKS checks, $FAILS failures.

upstream commit: $REL_SHA

Merged by support/classicui/tools/upstream_watch.sh on $RUN_TS.
EOF

PRBODY="$STATE_DIR/reports/$REL-pr.md"
cat > "$PRBODY" <<EOF
Upstream cut release \`$REL\` on $REL_DATE. This merges it into the Classic Home line.

**What this is.** MiSTer-devel/Main_MiSTer has no tags and no release branches; a
release is a dated binary committed under \`releases/\`. The commit that added
\`releases/$REL\` is [\`${REL_SHA:0:9}\`](https://github.com/MiSTer-devel/Main_MiSTer/commit/$REL_SHA)
("$REL_SUBJ"), and that commit is the merge point. Upstream master has moved past
it and is deliberately not included.

**Base.** \`$BASE_BRANCH\` at \`${BASE_SHA:0:9}\`.
**Merge.** Clean, no conflicts.
**Upstream commits brought in.** $MERGED_N, $DIFFSTAT

<details><summary>Upstream commits</summary>

\`\`\`
$MERGED_LOG
\`\`\`

</details>

**ARM build.** \`bin/MiSTer\`, $BIN_SIZE bytes, sha256 \`$BIN_SHA...\`, built with
$(basename "$TOOLCHAIN") and real ScreenScraper credentials compiled in, so the
ScreenScraper path in this binary is the one that ships rather than the
compile-time-dead version.

**Harness.** \`support/classicui/test/run.sh\`: **$CHECKS checks, $FAILS failures**,
ScreenScraper gate holds.

Draft, because a machine merged this and nobody has read the upstream diff yet.
Opened by \`support/classicui/tools/upstream_watch.sh\` at $RUN_TS.
EOF

if [ "$DRY_RUN" -eq 1 ]; then
	say "--- dry run: the commit message would be ---"
	cat "$COMMITMSG"
	say "--- dry run: the PR body would be ---"
	cat "$PRBODY"
	say "--- dry run: nothing pushed, no PR opened, state not advanced ---"
	exit 0
fi

git -C "$WORKTREE" commit --no-verify -F "$COMMITMSG" >>"$RUNLOG" 2>&1 \
	|| { report_failure "could not commit the merge" <<<"$(tail -20 "$RUNLOG")"; exit 1; }
MERGE_SHA="$(git -C "$WORKTREE" rev-parse HEAD)"

# HTTPS, not SSH: the fork's push path over SSH does not authenticate on this
# machine, and a nightly job is the worst place to discover that.
git -C "$WORKTREE" push --quiet "$FORK_REMOTE" "HEAD:refs/heads/$WORK_BRANCH" \
	|| { report_failure "push to $FORK_REMOTE failed" </dev/null; exit 1; }
PUSHED=1
say "pushed $WORK_BRANCH ${MERGE_SHA:0:9} to $FORK_REMOTE"

PR_URL="$(gh pr create --repo "$FORK_SLUG" --draft \
	--base "$BASE_BRANCH" --head "$WORK_BRANCH" \
	--title "Merge upstream release $REL" --body-file "$PRBODY" 2>&1 | tail -1)"
case "$PR_URL" in
https://*) say "draft PR: $PR_URL" ;;
*) report_failure "branch is pushed but gh pr create failed" <<<"$PR_URL"; exit 1 ;;
esac

# State last, so that a crash anywhere above means the next run tries again rather
# than silently skipping a release it never actually shipped.
echo "$REL" > "$STATE_DIR/last_release"
printf '%s\t%s\t%s\t%s\n' "$RUN_TS" "$REL" "$MERGE_SHA" "$PR_URL" >> "$STATE_DIR/handled.tsv"
rm -f "$STATE_DIR/failed/$REL"
say "recorded $REL as handled."
