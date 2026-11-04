# Releasing

Builds go to GitHub freely and to Nexus rarely, as settled versions in batches, when the maintainer
decides. Until 0.7.1 every Nexus upload was quarantined and needed a support email; 0.8.7, the first
without the dedicated server's `F4MPServer.exe` (that stays on GitHub), passed the scan on its own.

## Release candidates

A version is first published as a GitHub pre-release tagged `vX.Y.Z-rcN`. Players who want the fixes
early can run it and say whether it holds. In this order:

1. Write `dist/RELEASE-X.Y.Z-rcN.md` and commit it: the rc's commit carries its notes. For a new
   version's first rc, `set_version` in `xmake.lua` is bumped to X.Y.Z in the same commit.
2. Package that commit on GitHub, with the Package workflow (`.github/workflows/package.yml`, owner only):
   `gh workflow run package.yml -f ref=<commit> -f label=rcN` (or Actions > Package > Run workflow; it runs
   only from main, the default). The ref must be on `main`; a tag `vX.Y.Z-rcN` works too, and must then match
   the label and `set_version`. On GitHub's machines it compiles the menu movies in `ui/` from source, if there
   are any, with the pinned Flex compiler under the pinned Amazon Corretto (the pins are in
   `.github/scripts/vet.py`; hashes and signature are checked first), then runs
   `tools/package.ps1 -Label rcN` on that commit with no build cache. The run's artifact
   `F4Multiplayer-X.Y.Z-rcN-<commit>` holds what `dist/out/` would; put it there:
   `gh run download <run id> -n F4Multiplayer-X.Y.Z-rcN-<commit> -D dist/out`. The soak and `promote.ps1`
   then work on exactly those files. The artifact is kept 90 days; the `.pdb` in it is needed for crash logs
   long after, so keep `dist/out/symbols/`. `-f trial=true` packages with any label and uncommitted notes, for
   trying the workflow out (`-AllowDirty`: the build calls itself `<commit>-dirty` and can't be promoted).

   `tools/package.ps1 -Label rcN` on this PC is the fallback and does the same. It refuses uncommitted
   changes to tracked files, notes that are not committed, and notes in `dist/` for a version newer than
   X.Y.Z-rcN (other than X.Y.Z's own, which are written alongside its rcs): those mean `set_version` was not
   bumped. It builds every target with Visual Studio 2022, runs the unit tests (and stops if one fails),
   checks that nothing in the zip names the machine or its user, and writes to `dist/out/`:
   - `F4Multiplayer-X.Y.Z-rcN.zip`, laid out like a Data folder;
   - `F4Multiplayer-X.Y.Z-rcN.manifest.txt`: the commit, the build label, the DLL's SHA-256;
   - `symbols/F4Multiplayer-X.Y.Z-rcN-<commit>.pdb`, for reading crash logs from exactly this build.
     It never goes on a release (it is full of build paths).

   The menu movies (`ui/<Name>/<Name>.as`) go into the zip's `Interface/F4Multiplayer/`, compiled from
   source: `-Movies <folder>` names the folder they were compiled into (the workflow does that); without it
   they are read from `dist/movies/`, where `tools/flash/build.ps1` builds them, and a movie git tracks
   is refused. With no `ui/`, there are none. The workflows clear the build dates mxmlc writes into a movie
   (its compile time and its Metadata date), so the same sources make the same bytes: the manifest's
   `movies_sha256` line can be checked against a `vet.yml` report on that commit. A movie built on this PC
   keeps its dates and hashes differently.

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
   trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades, a friend's plain bullet and laser beam launched inert (the `trails` step), a workshop piece, the host's
   workbench shared with a take across a supply line, container loot, a door, an item dropped and picked up, a
   player downed and helped up, power armor in motion, the cell churn with a save reload, a VM restart and rejoin,
   fuzz on the host;
3. no new crash log on either machine during it;
4. on both machines, `Select-String -Path F4Multiplayer*.log -Pattern (Get-Content ~/.f4mp-private-patterns)`
   in `Documents\My Games\Fallout4\F4SE` finds nothing from this rc's sessions, and neither do
   `-Pattern '7656\d{13}'` nor an IP address other than 127.0.0.1.

`tools/rig/smoke.sh` is steps 2 to 4 with a verdict, in about 30 minutes: the standing set above, each number checked
against a threshold (movement lag and error, NPC tracks, health agreement, grenade counts, power armor, the safe,
reconnect, `perf`), the plugin log's new `[E]` lines and assertion text, the privacy scan of the new lines, and the crash
logs; its exit code is the number of failed checks. A check the rig could not make (the two machines' clocks could not
line a trace up: the VM's clock offset could not be read over ssh, or a clock was set between a reading and the trace) is
a rig fault, listed as RIG and never a failure of the mod: a soak
that made and passed that check in another round still passes, and one that never made it ends `VERDICT: RIG` with a
non-zero exit, to be run again. A set-up walk that never arrives, or a press seen never to reach the game's window, is a
rig fault the same way. Before each loop step a state guard (`tools/rig/state.sh`) checks both players: out of power
armor, up, nothing open, a normal camera, god mode on. It puts a wrong state right the way a player would (keys, the
pad's buttons, a friend's walk to help a downed player up) and lists each fix as HEAL; getting a player out of power armor
by the console, when no press does, is the one fix that is not a player's action, and it says so. A state only the mod
could have caused is never healed quietly: a player found down through god mode or dead, or an E that reached the game
and left a player in power armor, is a FAIL, and a message box found open is a WARN. A step that met a rig fault has its
results from that fault on set aside (listed as VOID with what they were), the guard runs again and the step runs once
more, so its checks are still made. A heal or a re-run never erases evidence of a failure of the mod: what the step
recorded before the fault stands, a clock fault sets nothing else aside, a check that fails in both attempts stays a
FAIL, and a PASS verdict names any failures it set aside. A press or a walk counts as the rig's only on a shown rig
cause (the press never reached the game, a menu was open, the other player stood in the way). A state the guard can't
put right, or a rig fault in both attempts, is a RIGFAULT: those checks were not made, and the run ends `VERDICT: RIG`,
never PASS. So a FAIL is the mod's. The steps for what players do together (bench, loot, doors, drops,
downed) walk the players with the dev channel's virtual pad (the walking is real movement, never a position set) and press
its buttons for the takes, the door and the Pip-Boy; entering workshop mode, building and dropping an item use dev commands
(`workshop enter`, `storage build`, `dropitem`), not a player's keys; the console only
places them at Sanctuary (Concord for the loot), stocks the items, makes Sanctuary and Abernathy Farm the host's for
the supply line, and injects the damage that downs a player. Each asserts the two games agree: the workbench counts
after each build (nothing taken twice), the container's copy after a take, the door's state, the drop listed and placed
for the other player and gone from both once taken, and the downed player helped up with the rules' health, a normal
camera and a working Pip-Boy. The bench step needs `bSharedStorage = true` in the host's `F4Multiplayer.ini` (back the
ini and the session files up with `backup.sh` first: the containers of a settlement the host owns stay its storage in
the session file); without it, it fails saying so. The questactors step (a shared quest's created creatures paired
across games: Gun Run's mirelurks at Salem, then a silent quest's, both scenes reached by console placement and walked
into) needs `sQuests = safe` there too, and fails saying so without it. `tools/rig/hits.sh` is the health check: both players shoot the
same NPCs, and the two games' health must agree, never rise, and survive a pause menu. Both need the rig held
(`tools/rig/guard.sh`) and leave no test-placed workshop pieces in the save (`tools/rig/cleanup.sh`).
`tools/rig/backup.sh` saves and restores the ini and session files on both machines. Nothing about a person or a
machine is in the repository: the rig's own folder holds `rig.conf` (see `tools/rig/env.sh`).

Before the rig run, `tools/addrcheck.ps1` (Windows PowerShell, read-only, the game is not started) resolves every
Address Library ID the plugin names against the installed `version-1-11-240-0.bin` and checks the game code the
plugin relies on by hand (the call from Main::Run into the message loop, the engine's message pump the frame
check looks for, the save anchor, the HUD menu's update slot). A FAILED line means the game or the Address
Library changed, or an ID in `src/` is wrong: look at it before going further.

## Promotion to the release

The release is the rc that held, not a new build. The soak of the rc's zip is a 90-minute run after the gate passes:
`SMOKE_ZIP=dist/out/F4Multiplayer-X.Y.Z-rcN.zip tools/rig/smoke.sh --soak 90`. It runs the whole standing set's loop
(movement through the cell churn, every step with its checks, both directions where a step has two) round after round,
25 to 30 minutes a round (three in 90 minutes), until the 90 minutes are up, then the end steps once: the VM's
game is restarted and rejoins, fuzz, the cleanup of test pieces, and the end checks of the crash logs, the plugin logs'
new errors and the privacy scan. A failure names its round. The working set's growth from the first combat to each
churn's end is printed and warned on, not failed. The soak used to be cell churn alone (and before 0.11.0 a 3-hour loop
that asserted only that both games were alive); looping the set exercises what players do over the same time. When it
exits 0 with no crash on either machine and no report against the rc points at a regression, the soak's verdict goes
in `dist/out/soak-verdict.txt`:

```
commit=<the rc's commit, as in its manifest>
dll_sha256=<the SHA-256 of the DLL that was soaked>
verdict=PASS
```

(other lines, such as the start, the end, the run's output folder and crash logs, are kept but not read). Then:

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
   entry) and merge it to `main`, or leave it out for no changelog. The workflow reads it from `main` first and
   from the tag only when `main` doesn't have it, so a changelog corrected after the rc still counts. Then
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
