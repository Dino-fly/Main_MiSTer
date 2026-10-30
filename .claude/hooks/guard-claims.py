#!/usr/bin/env python3
"""guard-claims.py - Stop hook: no "deployed" or "merged" claim without its evidence.

Reads the Stop hook JSON on stdin and the session transcript it names. Looks only at the
current turn (everything after the last prompt the person typed). If the final assistant
text claims a deploy or a merge, the turn's tool results must contain the matching
evidence, or the stop is refused (exit 2, reason on stderr) so the claim gets checked:

  deployed : a mister-deploy verdict (DEPLOY OK / DEPLOY NOOP) or an md5 of
             /media/fat/MiSTer read back from the device
  merged   : a `git merge` that ran this turn without CONFLICT / fatal in its result, or
             a merge commit read back from `git log --oneline`

Negated claims ("not deployed", "not merged yet") pass. Never refuses twice in a row
(stop_hook_active), and fails open on anything it cannot read - it is a reminder, not a
gate.
"""
import json
import re
import sys

CLAIM_DEPLOY = re.compile(r"\b(deployed|now running on the device)\b", re.I)
CLAIM_MERGE = re.compile(r"\bmerged\s+(into|as|to)\b", re.I)
NEGATED = re.compile(r"\b(not|never|un|without|before|once|after|if)\b[\w\s,'-]{0,20}$", re.I)
EVID_DEPLOY = re.compile(r"DEPLOY (OK|NOOP)|\b[0-9a-f]{32}\s+/media/fat/MiSTer\b")
# a merge that ran (`git [-C dir] merge ...`, not merge-base), or one read back from the log
GIT_MERGE = re.compile(r"\bgit\b[^;&|\n]*\smerge\s")
MERGE_LOG = re.compile(r"^[0-9a-f]{7,40} Merge ", re.M)
MERGE_BAD = re.compile(r"CONFLICT|fatal:|error:|not something we can merge", re.I)


def claimed(text, rx):
    for m in rx.finditer(text):
        if not NEGATED.search(text[max(0, m.start() - 40):m.start()]):
            return True
    return False


def blocks(content):
    return content if isinstance(content, list) else [{"type": "text", "text": content or ""}]


def result_text(item):
    c = item.get("content")
    if isinstance(c, list):
        return "\n".join(x.get("text", "") for x in c if isinstance(x, dict))
    return c if isinstance(c, str) else ""


def main():
    try:
        hook = json.load(sys.stdin)
    except Exception:
        return 0
    if hook.get("stop_hook_active"):
        return 0
    path = hook.get("transcript_path")
    if not path:
        return 0

    entries = []
    try:
        with open(path) as f:
            for line in f:
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                if d.get("type") in ("user", "assistant") and not d.get("isSidechain"):
                    entries.append(d)
    except OSError:
        return 0

    # The turn starts after the last prompt the person typed: a user entry that is not a
    # tool result, not meta, and not injected by a tool.
    start = 0
    for i, d in enumerate(entries):
        if d["type"] != "user" or d.get("isMeta") or d.get("sourceToolUseID"):
            continue
        items = blocks((d.get("message") or {}).get("content"))
        if not any(isinstance(x, dict) and x.get("type") == "tool_result" for x in items):
            start = i + 1
    turn = entries[start:]

    final_text, results, merges = "", [], []
    uses = {}
    for d in turn:
        for x in blocks((d.get("message") or {}).get("content")):
            if not isinstance(x, dict):
                continue
            if d["type"] == "assistant" and x.get("type") == "text":
                final_text = x.get("text", "")
            elif x.get("type") == "tool_use":
                uses[x.get("id")] = json.dumps(x.get("input", {}))
            elif x.get("type") == "tool_result":
                txt = result_text(x)
                results.append(txt)
                if GIT_MERGE.search(uses.get(x.get("tool_use_id"), "")) or MERGE_LOG.search(txt):
                    merges.append(txt)

    problems = []
    if claimed(final_text, CLAIM_DEPLOY) and not any(EVID_DEPLOY.search(r) for r in results):
        problems.append("it says something was deployed, but no DEPLOY OK/NOOP line or md5 of "
                        "/media/fat/MiSTer read back from the device appears in this turn")
    if claimed(final_text, CLAIM_MERGE) and not any(not MERGE_BAD.search(r) for r in merges):
        problems.append("it says something was merged, but no successful `git merge` ran in this turn")
    if problems:
        sys.stderr.write("guard-claims: the last message " + "; and ".join(problems) +
                         ". Run the check and quote its line, or reword the claim.\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
