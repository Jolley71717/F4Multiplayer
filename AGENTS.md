# Agent Instructions

## Project rules (every agent, every tool)

These are the maintainer's standing wishes. They hold for Claude Code, Codex, cloud sessions, API harnesses and
subagents alike, and a task brief can narrow them but never loosen them. When a task seems to need breaking one, stop
and ask.

### Changes and merges

- Work on a branch, open a pull request against `main`, never push to `main` directly. A PR that is reviewed (below)
  and tested may be merged without asking the maintainer: keep things moving. Releases still follow the rc-and-soak
  rule below.
- Every PR gets a code review at its current head before it merges: a PR comment starting "Code review at <sha>" and
  the `reviewed` label. New commits remove the label (`.github/workflows/review-label.yml`); review again.
- Merge locally (`git merge --no-ff origin/<branch>`, then push `main`), not with `gh pr merge`.
- Say how a change was tested. "Not tested in a game" is a fine answer when it's true; never imply a game test that
  didn't happen.
- A fresh worktree needs `git submodule update --init --recursive` before it builds.
- Whoever makes a git worktree removes it once its branch is merged (`git worktree remove <path>`, then
  `git branch -d <branch>`). Each holds a ~2 GB build; leftovers once filled 112 GB. Never remove one with
  uncommitted changes, untracked source files or unmerged commits: report it instead.

### Releases (docs/RELEASING.md)

- A GitHub pre-release `vX.Y.Z-rcN` first, then a multi-hour soak of that rc's zip (the zip's DLL, not a rebuild),
  then `tools/promote.ps1 -Rc vX.Y.Z-rcN`. Never publish a release whose rc hasn't soaked.
- Release notes go to the maintainer before anything is published. Notes are for players: no issue-tracker ids or
  internal tooling.
- Nexus uploads only through `gh workflow run nexus-upload.yml -f tag=vX.Y.Z`, never by hand, and only when the
  maintainer says so. The Nexus zip never holds `F4MPServer.exe`.

### Secrets, privacy and other people

- Never read, print, log, store or ask for a secret: `NEXUSMODS_API_KEY` (a repository secret the maintainer sets),
  `APISWARM_API_KEY`, tokens, passwords.
- No real names, handles, emails, Windows usernames, Steam ids or IP addresses in commits, PRs, comments, logs or
  docs. Players in logs are `player N`.
- Never post, reply or message anyone (Nexus, GitHub issues of other projects, Discord, email) on the maintainer's
  behalf without their approval of that exact text.
- Never copy code, assets or data from a project without a GPL-compatible license. In particular, never use anything
  from Commonwealth Online (no license): comparing features is fine, reading its code to reuse is not.
- Never permanently delete the maintainer's own files outside the repo, and never change system or security settings.

### The test rig (host PC and the Hyper-V VM)

- One user at a time: take and free `rig.lock`. Back up the ini and session files first and restore them byte for
  byte after; put main's build back on both machines.
- Exercise real movement. Never teleport or inject movement to make a test pass. Console placement to reach a scene
  is fine if the report says so; injecting the host's gunfire or grenades for combat tests is fine.
- Don't steer the game by screenshots (too slow and unreliable); script input and read the plugin log.
- Never run two traces or soaks at once on the same machines.

### Cost and scale

- Routine work (merges, builds, gates, rig batches, soaks, releases, replies) runs on Sonnet. Opus is for engine
  reverse engineering, crashes, and changes that could crash the game or touch the protocol or saves. Fable only when
  the maintainer asks.
- A few agents at a time, not a fleet, unless the maintainer asks for one.

### Product constraints

- Fallout 4 only (no Fallout 76), played over Steam (not a VPN). No Vortex-specific features. The project is GPL.

### New bugs and requests go on the tracker

- Every new bug, crash or feature request, wherever it comes from (a player's Nexus or GitHub report, a rig run, a
  review, something noticed in passing), gets an issue on the maintainer's tracker (labelled with its source, for example
  `nexus-0.10.0`) the moment it is found, not at the end of the task.
- Then put it on the maintainer's tracker: set its tracker stage (0 Queued) and sync both the private tracker and the
  public status site, and move the stage as the work moves (looked into, PR, reviewed, rig-tested, merged, rc,
  shipped). The maintainer follows the work there.
- An agent that can't reach the tracker (a cloud session) lists the new bugs in its PR or hand-off so they get filed.
- Close or update the matching item when work lands; a GitHub issue that a fix covers gets a comment and is closed.

### Reporting

- Status updates are short: 2-4 lines, plain words, no tables or internals unless asked.

## Non-Interactive Shell Commands

**ALWAYS use non-interactive flags** with file operations to avoid hanging on confirmation prompts.

Shell commands like `cp`, `mv`, and `rm` may be aliased to include `-i` (interactive) mode on some systems, causing the agent to hang indefinitely waiting for y/n input.

**Use these forms instead:**
```bash
# Force overwrite without prompting
cp -f source dest           # NOT: cp source dest
mv -f source dest           # NOT: mv source dest
rm -f file                  # NOT: rm file

# For recursive operations
rm -rf directory            # NOT: rm -r directory
cp -rf source dest          # NOT: cp -r source dest
```

**Other commands that may prompt:**
- `scp` - use `-o BatchMode=yes` for non-interactive
- `ssh` - use `-o BatchMode=yes` to fail instead of prompting
- `apt-get` - use `-y` flag
- `brew` - use `HOMEBREW_NO_AUTO_UPDATE=1` env var
