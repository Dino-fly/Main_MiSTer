---
name: release-post
description: Prepare anything that leaves the fork publicly - release notes, a wave or firmware release, an issue/PR/discussion comment, a reply to a user. Produces a draft for the owner; an agent never posts.
---
Checklist, then stop at the draft:
1. Evidence first. State what is verified (build verdict line with md5, device capture names, harness check count versus before) and what is still a hypothesis, labelled as such in the text itself.
2. Artifacts by md5, never by filename: every file in a wave or release is compared against the build output that produced it; names are compared case-sensitively (a Linux build sees what this Mac cannot).
3. Text rules: no personal names, no home paths, no credential, no literal device address. Never tell a user to "report upstream" unless they ran the official binary. This fork opens no PRs or issues on MiSTer-devel, ever, and none on itself either.
4. Output the exact text headed `DRAFT - awaiting the owner`. Do not run `gh issue|pr|discussion|release ...` or `git push`; the hook blocks them. The owner posts, or authorises exactly one action with `~/workspace/Main_MiSTer/.claude/scripts/approve-once gh|push`.
