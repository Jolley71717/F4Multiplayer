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
3. no new crash log on either machine during it;
4. on both machines, `Select-String -Path F4Multiplayer*.log -Pattern (Get-Content ~/.f4mp-private-patterns)`
   in `Documents\My Games\Fallout4\F4SE` finds nothing from this rc's sessions, and neither do
   `-Pattern '7656\d{13}'` nor an IP address other than 127.0.0.1.

## Promotion to the release

The same commit is tagged `vX.Y.Z` (no label) after a multi-hour soak (`soak.sh 3`: the standing set in
a loop with periodic VM restarts) ends with no crash on either machine and no report against the rc
that points at a regression. Package it again without a label, fast-forward `main`, create the
GitHub release with `dist/RELEASE-X.Y.Z.md` as the notes.

## Notes for testers (put in every rc's notes)

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or
  `F4Multiplayer.1.log` if you restarted the game since) and the crash log; the first line of the
  plugin log names the build. The plugin log names players as `player N` and leaves Steam ids and
  addresses out; crash logs and screenshots can still hold names.
