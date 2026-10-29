# Agent Instructions

## Project rules (every agent, every tool)

These are the maintainer's standing wishes. They hold for Claude Code, Codex, cloud sessions, API harnesses and
subagents alike, and a task brief can narrow them but never loosen them. When a task seems to need breaking one, stop
and ask.

### Changes and merges

- Work on a branch, open a pull request against `main`, never push to `main` directly. Merging needs the
  maintainer's approval (standing or for that PR).
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

### Reporting

- Status updates are short: 2-4 lines, plain words, no tables or internals unless asked.

This project uses **bd** (beads) for issue tracking. Run `bd prime` for full workflow context.

> **Architecture in one line:** Issues live in a local Dolt database
> (`.beads/dolt/`); cross-machine sync uses `bd dolt push/pull` (a
> git-compatible protocol), stored under `refs/dolt/data` on your git
> remote — separate from `refs/heads/*` where your code lives.
> `.beads/issues.jsonl` is a passive export, not the wire protocol.
>
> See [sync-concepts](https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md)
> for the one-screen overview and anti-patterns (don't treat JSONL as the
> source of truth; don't `bd import` during normal operation; don't
> reach for third-party Dolt hosting before trying the default).

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work atomically
bd close <id>         # Complete work
bd dolt push          # Push beads data to remote
```

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

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:46cd31e7 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   bd dolt push
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->

<!-- BEGIN BEADS CODEX SETUP: generated by bd setup codex -->
## Beads Issue Tracker

Use Beads (`bd`) for durable task tracking in repositories that include it. Use the `beads` skill at `.agents/skills/beads/SKILL.md` (project install) or `~/.agents/skills/beads/SKILL.md` (global install) for Beads workflow guidance, then use the `bd` CLI for issue operations.

### Quick Reference

```bash
bd ready                # Find available work
bd show <id>            # View issue details
bd update <id> --claim  # Claim work
bd close <id>           # Complete work
bd prime                # Refresh Beads context
```

### Rules

- Use `bd` for all task tracking; do not create markdown TODO lists.
- Run `bd prime` when Beads context is missing or stale. Codex 0.129.0+ can load Beads context automatically through native hooks; use `/hooks` to inspect or toggle them.
- Keep persistent project memory in Beads via `bd remember`; do not create ad hoc memory files.

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.
<!-- END BEADS CODEX SETUP -->
