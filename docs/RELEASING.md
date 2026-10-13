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
   names the build it came from. An untracked file under `src`, `common`, `bot` or `tests` counts as
   uncommitted (xmake would compile it in). `-AllowDirty` packages uncommitted work for trying things
   out; such a build calls itself `<commit>-dirty` and can't be promoted, and with `-Label` it needs
   `-OutDir`, so it never replaces a real rc's zip in `dist/out/`.
3. Tag the commit the manifest names: `git tag vX.Y.Z-rcN <commit>`. The tag follows the artifact.
4. `gh release create vX.Y.Z-rcN dist/out/F4Multiplayer-X.Y.Z-rcN.zip --prerelease --title X.Y.Z-rcN
   --notes-file dist/RELEASE-X.Y.Z-rcN.md`.

Gate for an rc, every time, on the packaged zip installed on both machines (not a plain build DLL):

1. the unit tests pass (`tools/package.ps1` runs them; `xmake build` builds them with everything else);
2. the standing set passes once on the rig in both directions (host to joiner and joiner to host): movement
   trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades, power armor in
   motion, the cell churn with a save reload, a VM restart and rejoin, fuzz on the host. `sweep.sh`,
   or one cycle of `soak.sh`, is that set;
3. no new crash log on either machine during it;
4. on both machines, `Select-String -Path F4Multiplayer*.log -Pattern (Get-Content ~/.f4mp-private-patterns)`
   in `Documents\My Games\Fallout4\F4SE` finds nothing from this rc's sessions, and neither do
   `-Pattern '7656\d{13}'` nor an IP address other than 127.0.0.1.

## Promotion to the release

The release is the rc that held, not a new build. After a multi-hour soak of the rc's zip (`soak.sh 3`:
the standing set in a loop with periodic VM restarts) ends with no crash on either machine and no
report against the rc points at a regression, the soak's verdict goes in `dist/out/soak-verdict.txt`:

```
commit=<the rc's commit, as in its manifest>
dll_sha256=<the SHA-256 of the DLL that was soaked>
verdict=PASS
```

(other lines, such as the start, end, cycles and crash logs, are kept but not read). Then:

1. `tools/promote.ps1 -Rc vX.Y.Z-rcN`. It refuses unless the rc's zip matches its manifest, the tag
   `vX.Y.Z-rcN` is the manifest's commit, the rc was not packaged with `-AllowDirty`, and the verdict
   is a PASS for that commit and that DLL. It writes `F4Multiplayer-X.Y.Z.zip` (and `-nexus.zip`,
   without the dedicated server) holding the rc's files byte for byte, plus their manifest and the
   `.pdb` under the release's name. Only `promote.ps1` makes a release zip; skipping the soak means
   publishing by hand, as a deliberate and visible choice.
2. Run the commands it prints: tag the rc's commit `vX.Y.Z`, push the tag, and create the GitHub
   release with `dist/RELEASE-X.Y.Z.md` as the notes (say there that the build is identical to rcN:
   its log still names `rcN`). Commit those notes to the branch afterwards, and fast-forward `main`.

## Notes for testers (put in every rc's notes)

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or
  `F4Multiplayer.1.log` if you restarted the game since) and the crash log; the first lines of the
  plugin log name the build. The plugin log names players as `player N` and leaves Steam ids and
  addresses out; crash logs and screenshots can still hold names.
