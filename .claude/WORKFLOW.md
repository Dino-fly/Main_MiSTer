# Agentic workflow for the MiSTer fork

Written 2026-10-30 against Claude Code 2.1.311. Facts marked **[doc]** were read from
code.claude.com/docs that day; **[measured]** were measured here; **[unverified]** is a
belief. Section 7 lists what was adopted on 2026-10-30.

## 1. Session lifecycle

| phase | what happens | why |
|---|---|---|
| start | `/clear` (or a new session) runs the SessionStart hook (`.claude/hooks/session-start.sh`): worktrees with branch and uncommitted count, the `## Next` block of `.claude/ROADMAP.md`, one line of pointers. About 15 lines. | the next step is in context before the first prompt; no memory file has to be opened to find out where things stand |
| work | one task per session: a feature step, a device test, a merge, a release draft. Long waits go to background tools that print one verdict line (section 3). Device work follows `/device-test`. | a session that does one thing has a context that can be summarised in six lines, which is what `/wrap-up` needs |
| wrap-up | `/wrap-up <summary>`: commits or lists uncommitted work, prunes done items from the handoff doc and ROADMAP.md (done lines move to DONE.md), adds a memory file only for a durable fact, stops stray containers/tasks, prints the block ending in "Good moment to /clear: yes/not yet". | The owner asked for the next roadmap steps written down, done items cleaned out of what the next session loads, and an explicit clear cue |
| clear | The owner runs `/clear` (`/clear <name>` labels the old one for `/resume`). The hook fires again with `source=clear`. | a fresh conversation layer; the system prompt + CLAUDE.md prefix stays cached across it **[doc]** |

Cues to wrap up: the task's verdict line has printed (BUILD OK, HARNESS OK, DEPLOY OK,
capture named), or the statusline shows `WRAP-UP SOON` (context at 60% or more), or
the owner says so. Do not start a second task in the same session after a wrap-up; `/clear`.

## 2. What loads at session start, and the budget

| item | now | budget | notes |
|---|---|---|---|
| `CLAUDE.md` (main checkout, untracked copy) | 64 lines | 80 lines | rules that have cost the most, plus a 5-line pointer to this file |
| `MEMORY.md` (auto-memory index) | about 70 lines / 7.8 KB, grouped by topic, cold topics in `ARCHIVE.md` | 80 lines / 10 KB | first 200 lines / 25 KB load **[doc]**; topic files load on demand only. Slimmed 2026-10-30 from 64 lines / 10 KB of dated status; the old index is kept as `MEMORY-2026-10-30-before-slim.md.bak` |
| session-start hook output | ~15 lines | 20 lines | worktrees + ROADMAP `## Next` |
| `ROADMAP.md` | 25 lines | 40 lines | only open work; `/wrap-up` keeps it so |
| skills, agents | descriptions only until invoked | - | a skill body loads when used; a subagent gets its own system prompt + CLAUDE.md |

Rules: status never goes into memory (it rots within a week and costs every session
after); a durable fact gets a topic file and one index line; a feature's working
knowledge lives in its handoff doc in the worktree (`support/classicui/docs/*-HANDOFF.md`),
which is read once at the start of a feature session, not every session.

## 3. Prompt caching and long waits

- **TTLs [doc]:** on a Claude subscription the main conversation already gets a 1-hour
  cache; subagents, forks and compaction get 5 minutes. The owner's 1 h / 5 min split is
  the default. It is now explicit in `.claude/settings.json`
  (`promptCacheTtl: "1h"`, `subagentPromptCacheTtl: "5m"`, needs 2.1.242+). Over the
  plan limit on usage credits the main TTL drops to 5 minutes regardless. A single
  subagent can opt into 1 h with `experimental: {cacheTtl: 1h}` in its frontmatter
  (2.1.248+): done for `farm-watch`, whose polls block 570 s each.
- **What breaks the cache [doc]:** a system-prompt or tool-list change, a model switch
  (each model caches separately), `/clear`, compaction, many images, an MCP server added
  with tool search off. A CLAUDE.md edit does not break it but also does not apply until
  `/clear`. Skills, permission mode and spawning a subagent keep it.
- **Waits [measured + doc]:** nothing is sent while a tool call runs, so a foreground call
  longer than the TTL returns to a cold cache. With the 1 h main TTL that is only the
  farm and very long builds; with 5 min it was every harness run. Rules:
  - `mister-build` (3-6 min), `mister-harness` (140 s), the farm: always
    `run_in_background`, and do useful work meanwhile; the completion notice is one
    line. The statusline's `cache COLD (cause)` says when a turn re-wrote the prefix.
  - The harness no longer does `apt-get` per run: `support/classicui/test/harness-image.sh`
    builds `mister-harness:ubuntu20` once (18 s **[measured]**), run.sh compiles in
    parallel, and the container is named so a stopped task removes it. 140 s alone,
    3488 checks / 0 failures, same as before (merged into classic-ui, 5902ff5a).
  - A wait with a condition ("tell me when the core is up") is one background Bash
    `until` loop, not repeated polling turns; a stream of events is the Monitor tool
    (5-30 min deadline, re-arm) **[doc]**.
  - Idle compaction can rewrite the conversation after a long idle on the 1 h TTL
    **[doc]**; turned off in settings.json (section 7).

## 4. Models and effort

| role | model | effort | TTL | why |
|---|---|---|---|---|
| orchestrator (interactive) | Opus (settings `model: opus`) | default | 1 h | plans, judges captures, merges, talks to the owner; the context is the expensive part, so it does little raw reading |
| `triage` agent | Haiku | low | 5 m | read-only summaries and draft replies; cheap and short |
| `farm-watch` agent | Haiku | low | **1 h** | 570 s blocking polls would miss a 5 m window every time |
| `reviewer` agent | Opus | high | 5 m | one diff, one report; given the diff, never the reasoning |
| `device-runner` agent | Sonnet | medium | 5 m | device chatter (20-40 tool results) stays out of the orchestrator; returns verdict lines and capture paths |
| implementation in a worktree | Sonnet via `Agent(model: sonnet)` or `/model` | default | - | day-to-day edits and device driving; Opus when the diff touches `user_io.cpp`, `video.cpp`, `cfg_t`, scaler or savestate math (CLAUDE.md) |
| contested public claim | Fable, one short call | - | - | a hypothesis about to go public, or Opus and the evidence disagreeing |

Effort per agent is set in frontmatter (`effort: low|medium|high|xhigh|max` **[doc]**).
The split is unmeasured: compare cost and quality on one run of each before trusting it.

## 5. Reliability guards

| guard | where | what it stops |
|---|---|---|
| PreToolUse + Stop hooks (99 synthetic cases pass) | `.claude/hooks/guard-*.sh`, `test-hooks.sh` | broad kills, `gh` writes, pushes, firmware writes without an md5 marker, `make clean`/branch switch in the main checkout, credential lines in the transcript, editing a running script |
| script smoke tests (39 cases) | `.claude/scripts/test-scripts.sh` | the mister-deploy class of bug: a `set -e`/pipefail exit on a successful path; a `--dry-run` that reaches for ssh. Found two on day one: `mister-ui --dry-run` ssh'd (fixed) and `mister-build --allow-no-creds` died on an empty array under macOS bash 3.2 `set -u` (fixed) |
| verdict lines | every script | BUILD/DEPLOY/CAPTURE/HARNESS/FRESH OK or FAIL with md5s; the orchestrator quotes the line, never the log |
| known state | `mister-fresh` | the "needs a fresh launch" turn-burner: menu core, optional restart on the menu core (the only restart that arms savestates), launch booted by this firmware |
| log reads | `mister-log` | restart-aware marker/since/count/wait on `/tmp/debug.txt`, capped at 40 lines, so `wc -l`/`tail`/`grep` by hand stop |
| baseline before blame | `/device-test` step 6 | `MiSTer.pre-*` on the card |
| approvals | `approve-once`, plus an optional local `~/.config/classicui/push-guard` | any public write without the owner's exact text; a push the local check vetoes |

## 6. Files added or changed 2026-10-30

- `.claude/WORKFLOW.md` (this), `.claude/ROADMAP.md`, `.claude/DONE.md`
- `.claude/skills/wrap-up/SKILL.md`; `.claude/skills/device-test/SKILL.md` (new helpers)
- `.claude/hooks/session-start.sh`; `.claude/settings.json` (SessionStart hook, statusLine, cache TTLs)
- `.claude/scripts/mister-log`, `mister-fresh`, `mister-harness`, `statusline`, `test-scripts.sh`
- `.claude/scripts/mister-ui` (dry-run fix), `mister-build` (SS_MOUNT fix), `agents/farm-watch.md` (1 h cache)
- `CLAUDE.md` (main checkout): "Session shape" paragraph
- memory: `MEMORY.md` regrouped and shortened; old copy kept as `.bak`; no topic file changed or removed
- `support/classicui/test/{run,filmstrip,play}.sh` on the prebuilt image, `harness-image.sh` new: merged into classic-ui (5902ff5a).

## 7. Adopted 2026-10-30 (all eight proposals, approved by the owner)

1. **`idleCompaction: false`** in settings.json. Unverified what else it changes.
2. **Effort per agent:** `triage`, `farm-watch` low; `reviewer` high; `device-runner` medium.
   Measure cost on one run of each before trusting the split.
3. **`device-runner` agent** (Sonnet, Bash+Read): build, deploy, fresh-launch, capture,
   returns verdict lines and capture paths only. Not yet run on a device.
4. **Memory archive:** the SNAC/reader-core section, Astrocade and analog colour moved to
   `memory/ARCHIVE.md`, one pointer line in MEMORY.md (8.5 KB -> 7.8 KB).
5. **`.claude/rules/`**: `classicui.md` (support/classicui/**) and
   `firmware-core-coupling.md` (user_io.*, snacpad.*, sys/**); CLAUDE.md keeps a pointer.
6. **Read-only allow rules** for mister-log, `mister-ui state`, farm-poll, the two test
   scripts and `docker ps` - in settings.local.json, because they carry absolute paths.
7. **`.claude/` is committed** to classic-ui through `scripts/claude-publish <worktree>`,
   which copies agents, hooks, rules, scripts, skills, settings.json and this file after a
   leak scan (home paths, LAN addresses, e-mail addresses, and the local
   `~/.config/classicui/private-words`). The main checkout's `.claude/` stays the live copy:
   edit there, publish, commit in the worktree, merge --no-ff. Per-machine checks
   (`push-guard`, `bin/identity-check`, `private-files`) live in `~/.config/classicui/` and
   are never published. Worktrees made before this still have `.claude` symlinks; run
   `claude-publish --unlink <worktree>` before merging classic-ui into them.
8. **Stop hook `hooks/guard-claims.py`**: refuses to end a turn whose last message claims a
   deploy or a merge with no DEPLOY OK/NOOP line, device md5 or successful `git merge`
   (or merge commit in `git log`) in that turn. Never twice in a row; fails open. 11 cases
   in test-hooks.sh. Drop it if it fires on false positives.

## 8. Not verified

- `mister-fresh` and `mister-log` ran only in `--dry-run` (device off); first device
  session validates them (ROADMAP "Unverified").
- Whether hook stdout from SessionStart counts toward the cached prefix or the
  conversation layer (it is after the system prompt either way) **[unverified]**.
- The garbled in-game-menu `mister-capture` frame from 2026-10-30 is not diagnosed; the
  core path was fine.
