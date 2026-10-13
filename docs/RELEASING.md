# Releasing

Builds go to GitHub freely and to Nexus rarely (every Nexus upload is quarantined and needs a support
email, so Nexus gets settled versions in batches, when the maintainer decides).

## Release candidates

A version is first published as a GitHub pre-release tagged `vX.Y.Z-rcN`, built with
`tools/package.ps1 -Label rcN` (the zip is `F4Multiplayer-X.Y.Z-rcN.zip` and the plugin logs
`F4Multiplayer build rcN` at startup). Players who want the fixes early can run it and say whether it
holds.

Gate for an rc, every time:

1. the unit tests pass (`xmake build -y F4MPTests`, then run it);
2. the standing set passes once on the rig in both directions (host to joiner and joiner to host): movement
   trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades, power armor in
   motion, the cell churn with a save reload, a VM restart and rejoin, fuzz on the host. `sweep.sh`,
   or one cycle of `soak.sh`, is that set;
3. no new crash log on either machine during it.

Before the rig run, `tools/addrcheck.ps1` (Windows PowerShell, read-only, the game is not started) resolves every
Address Library ID the plugin names against the installed `version-1-11-240-0.bin` and checks the game code the
plugin relies on by hand (the call from Main::Run into the message loop, the engine's message pump the frame
check looks for, the save anchor, the HUD menu's update slot). A FAILED line means the game or the Address
Library changed, or an ID in `src/` is wrong: look at it before going further.

## Promotion to the release

The same commit is tagged `vX.Y.Z` (no label) after a multi-hour soak (`soak.sh 3`: the standing set in
a loop with periodic VM restarts) ends with no crash on either machine and no report against the rc
that points at a regression. Package it again without a label, fast-forward `main`, create the
GitHub release with `dist/RELEASE-X.Y.Z.md` as the notes.

## Notes for testers (put in every rc's notes)

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log;
  the first line of the plugin log names the build.
