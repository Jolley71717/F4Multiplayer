# Releasing

Builds go to GitHub freely and to Nexus rarely, as settled versions in batches, when the maintainer
decides. Until 0.7.1 every Nexus upload was quarantined and needed a support email; 0.8.7, the first
without the dedicated server's `F4MPServer.exe` (that stays on GitHub), passed the scan on its own.

## Release candidates

A version is first published as a GitHub pre-release tagged `vX.Y.Z-rcN`. Players who want the fixes
early can run it and say whether it holds. In this order:

1. Write `dist/RELEASE-X.Y.Z-rcN.md` and commit it: the rc's commit carries its notes. For a new
   version's first rc, `set_version` in `xmake.lua` is bumped to X.Y.Z in the same commit.
2. `tools/package.ps1 -Label rcN` on that commit. It refuses uncommitted changes to tracked files,
   notes that are not committed, and notes in `dist/` for a version newer than X.Y.Z-rcN (other than
   X.Y.Z's own, which are written alongside its rcs): those mean `set_version` was not bumped. It
   builds every target with Visual Studio 2022, runs the unit tests (and stops if one fails),
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

`tools/rig/smoke.sh` is steps 2 to 4 with a verdict: the standing set above, each number checked against a threshold
(movement lag and error, NPC tracks, health agreement, grenade counts, power armor, the safe, reconnect, `perf`), the
plugin log's new `[E]` lines and assertion text, the privacy scan of the new lines, and the crash logs; its exit code is
the number of failed checks. `tools/rig/hits.sh` is the health check: both players shoot the same NPCs, and the two
games' health must agree, never rise, and survive a pause menu. Both need the rig held (`tools/rig/guard.sh`) and leave
no test-placed workshop pieces in the save (`tools/rig/cleanup.sh`). `tools/rig/backup.sh` saves and restores the ini and
session files on both machines. Nothing about a person or a machine is in the repository: the rig's own folder holds
`rig.conf` (see `tools/rig/env.sh`).

Before the rig run, `tools/addrcheck.ps1` (Windows PowerShell, read-only, the game is not started) resolves every
Address Library ID the plugin names against the installed `version-1-11-240-0.bin` and checks the game code the
plugin relies on by hand (the call from Main::Run into the message loop, the engine's message pump the frame
check looks for, the save anchor, the HUD menu's update slot). A FAILED line means the game or the Address
Library changed, or an ID in `src/` is wrong: look at it before going further.

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
   its log still names `rcN`). Both zips go on the release. Commit those notes to the branch afterwards,
   and fast-forward `main`.
3. Nexus, only when the maintainer decides: write the changelog in `dist/NEXUS-X.Y.Z.txt` (one line per
   entry) and commit it before tagging, or leave it out for no changelog. Then
   `gh workflow run nexus-upload.yml -f tag=vX.Y.Z` (or Actions > Upload to Nexus > Run workflow).
   The workflow takes `F4Multiplayer-X.Y.Z-nexus.zip` from the GitHub release, refuses one holding
   `F4MPServer.exe`, and uploads it as the new version of the main file through Nexus's own
   `Nexus-Mods/upload-action`: the old version archived, the mod's version set to match. It needs the
   repository secret `NEXUSMODS_API_KEY`, the maintainer's personal API key from
   https://www.nexusmods.com/settings/api-keys. Nexus still scans the upload before it can be downloaded.

## Notes for testers (put in every rc's notes)

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or
  `F4Multiplayer.1.log` if you restarted the game since) and the crash log; the first lines of the
  plugin log name the build. The plugin log names players as `player N` and leaves Steam ids and
  addresses out; crash logs and screenshots can still hold names.
