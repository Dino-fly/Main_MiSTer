---
name: wrap-up
description: End a task cleanly before the context is cleared - record the next roadmap steps, prune what is done out of the files the next session loads (ROADMAP.md, the feature handoff doc, MEMORY.md), stop stray background work, and say whether this is a good moment to /clear. Use at the end of every task, when the statusline says WRAP-UP SOON, or when the owner says "wrap up".
argument-hint: [one-line summary of what this task did]
---
Do these in order, editing files directly; keep each file within its budget. Nothing here is public, nothing is pushed, nothing is merged.

1. **State of the work.** For every worktree touched this session: `git -C <wt> status --short` and the branch. Uncommitted work is either committed now (only if the owner asked for commits in this session) or listed by file in the block below. Note the device firmware md5 if a build was deployed (`mister-deploy` verdict line) and whether the card runs a tested or an untested binary.

2. **Feature handoff doc** (`support/classicui/docs/*-HANDOFF.md` in the worktree, if the task has one): delete every item marked done, or marked done in this session, in full - the knowledge sections stay, the todo list holds only open items. A done item's facts that are still needed move to the knowledge section in one or two lines. Commit the doc with the code it belongs to, never on its own in the main checkout.

3. **`~/workspace/Main_MiSTer/.claude/ROADMAP.md`** (untracked, loaded at session start by the hook, budget 40 lines):
   - `## Next` - 3 to 6 numbered steps, each one line: what, the worktree/branch, the first command to run. The first step must be startable with no other reading.
   - `## Open threads` - things waiting on the owner (approvals, verdicts, questions), one line each.
   - `## Unverified` - claims from this session that no capture, harness run or build line proves yet.
   - Remove every line this task completed; append one dated line per removed item to `.claude/DONE.md` (never loaded, grows freely).

4. **Memory** (`~/.claude/projects/<this project>/memory/`): add a topic file only for a durable fact learned the hard way (a trap, a measured number, a rule the owner set), with a one-line index entry in `MEMORY.md` under the matching heading, at most 110 characters. Status and "what is next" never go into memory - that is ROADMAP.md. Never delete a memory file; if an index line is only status, shorten it to the durable part.

5. **Background work.** `docker ps --filter name=chome-harness --filter name=mister` and the session's `/tasks`: stop what this task started and no longer needs (by name, never a broad kill). Remove an agent worktree you created under `.claude/worktrees/` if its work is merged or abandoned.

6. **Print this block and stop** - no further work after it:
   ```
   WRAP-UP: <one line, from the argument if given>
   done: ...
   left uncommitted: <files or "nothing">
   next: <ROADMAP step 1, with its first command>
   unverified: <or "nothing">
   Good moment to /clear: yes - everything the next session needs is in ROADMAP.md / the handoff doc.
   (or) Not yet: <the one thing still in flight and when it ends>
   ```
