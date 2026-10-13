# Releasing

Builds go to GitHub freely and to Nexus rarely (every Nexus upload is quarantined and needs a support
email, so Nexus gets settled versions in batches, when the maintainer decides).

## Release candidates

A version is first published as a GitHub pre-release tagged `vX.Y.Z-rcN`. Players who want the fixes
early can run it and say whether it holds. In this order:

1. Write `dist/RELEASE-X.Y.Z-rcN.md` and commit it: the rc's commit carries its notes.
2. `tools/package.ps1 -Label rcN` on that commit. It refuses uncommitted changes to tracked files and
   notes that are not committed, builds every target, runs the unit tests (and stops if one fails),
   checks that nothing in the zip names this PC or its user, and writes to `dist/out/`:
   - `F4Multiplayer-X.Y.Z-rcN.zip`, laid out like a Data folder;
   - `F4Multiplayer-X.Y.Z-rcN.manifest.txt`: the commit, the build label, the DLL's SHA-256;
   - `symbols/F4Multiplayer-X.Y.Z-rcN-<commit>.pdb`, for reading crash logs from exactly this build.
     It stays on this PC (it is full of build paths) and is never uploaded.

   The plugin's log then says `F4Multiplayer build rcN <commit>` near its top: that is how a log
   names the build it came from. `-AllowDirty` packages uncommitted work for trying things out; such
   a build calls itself `<commit>-dirty` and can't be promoted.
3. Tag the commit the manifest names: `git tag vX.Y.Z-rcN <commit>`. The tag follows the artifact.
4. `gh release create vX.Y.Z-rcN dist/out/F4Multiplayer-X.Y.Z-rcN.zip --prerelease --title X.Y.Z-rcN
   --notes-file dist/RELEASE-X.Y.Z-rcN.md`.

Gate for an rc, every time, on the packaged zip installed on both machines (not a plain build DLL):

1. the unit tests pass (`tools/package.ps1` runs them; `xmake build` builds them with everything else);
2. the standing set passes once on the rig in both directions (host to joiner and joiner to host): movement
   trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades, power armor in
   motion, the cell churn with a save reload, a VM restart and rejoin, fuzz on the host. `sweep.sh`,
   or one cycle of `soak.sh`, is that set;
3. no new crash log on either machine during it.

## Promotion to the release

The same commit is tagged `vX.Y.Z` (no label) after a multi-hour soak (`soak.sh 3`: the standing set in
a loop with periodic VM restarts) ends with no crash on either machine and no report against the rc
that points at a regression. Package it again without a label, fast-forward `main`, create the
GitHub release with `dist/RELEASE-X.Y.Z.md` as the notes.

## Notes for testers (put in every rc's notes)

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log;
  the first lines of the plugin log name the build.
