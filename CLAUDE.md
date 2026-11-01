# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

## Project rules

The maintainer's standing rules for every agent are in AGENTS.md ("Project rules"). Read them before any change,
merge, release, rig test or message to anyone.

@AGENTS.md

## Build & Test

See CONTRIBUTING.md. In short:

```bash
xmake config -m releasedbg
xmake build -y
build/windows/x64/releasedbg/F4MPTests.exe   # exit code = failed tests; a substring runs only matching tests
```

Anything that touches the game (`src/game`, `src/steam`, the sync classes) is only tested in a game.

## Architecture Overview

See docs/DESIGN.md (how each system syncs and why) and docs/PLAN.md.

Maintainer-local instructions, if present, are in CLAUDE.local.md (not committed).
