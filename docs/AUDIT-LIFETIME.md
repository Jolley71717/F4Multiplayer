# Lifetime and validity audit (2026-10-10)

Scope: every place the plugin holds an engine pointer across a frame, reads an engine field whose
layout or resolution the engine controls, uses a form ID that arrived from the network, touches
engine objects off the game thread, deletes a reference, or dereferences an optional engine field.
Prompted by the Red Rocket terminal crash (a linked-ref entry read as a pointer before the engine
resolved it; fixed in 6da1cb6, 0.7.3).

Four readers covered the code: three independent read-only reviews (actor lifetime files; world
sync, weapons, workshop and the rest of `src/game`; the protocol, session, server and transports)
plus a manual pass over the puppet registry and the fixes. Line numbers are as of commit 0087695.

## Rules applied in the fixes

1. A raw engine pointer is never kept across a frame. Keep the handle or the form ID and resolve it at
   use; where a pointer is still stored for comparison, compare it, never follow it.
2. A registry keyed by address checks the form ID on every lookup; unregistering takes the ID and
   never reads the actor.
3. Every form ID from the network is looked up typed (`GetFormByID<T>`, which checks the form type)
   or followed by an `Is()`/`As<>()` check before a console verb that assumes a type.
4. Nothing is disabled or deleted while the engine walks its lists, and nothing is deleted before its
   3D is in (DeferredDelete).
5. Event sinks and hooks copy handles and IDs under the inbox lock; the reads happen in `Frame()`.
6. Memory the engine frees comes from the engine's allocator.
7. Coordinates from the network are finite and inside the world.

## Findings

Status: F = fixed (commit), O = open issue (beads id), N = checked, no change needed.

### Dangling pointers and address reuse

| Where | What | Status |
|---|---|---|
| Puppets.cpp:42, lookups at :100, :154, :349, :363, :372, :380, Unregister :332 | registry keyed by actor address; several lookups and the erase had no form-ID check | F 46dd515 (`Find` checks the ID; `Unregister(actor, id)`) |
| NpcSync.cpp:723 | equipment applied through the stored mirror pointer after its cell may have unloaded | F 46dd515 (re-resolved by ID) |
| NpcSync.cpp:167, RemotePlayers.cpp:387 | unregister by address of an actor already gone | F 46dd515 |
| RemotePlayers.cpp:43, :438-441 | face base (runtime NPC form) kept as a pointer across loads | F 46dd515 (by ID, remade when gone, reset on load) |
| RemotePlayers.cpp:657-668 | graveyard handles drained after a load | F 46dd515 (cleared on load) |
| WorkshopSync.cpp `OnGameLoaded` | copies' and graveyard handles, and the copy ID set, kept across a load: a friend's next move or scrap resolved a handle from before it (another reference now, or nothing) | F 423d527 (let go unread on load; the server sends the building again after the load and the save's copies are adopted at their spots) |
| NpcSync.cpp:127 | mirrors could target the player, a stand-in or a runtime actor | F 46dd515 |
| WeaponFire.cpp:251 | sound handle follows a stand-in's 3D root that may be deleted mid-tail | F 72646ce |
| WorldSync.cpp:537 | `LinkedTarget` returns the engine's pointer branch unverified | N (engine-owned entry; the ID branch handles the transient case) |
| RemotePlayers.cpp:265, :348, :453; WorkshopSync.cpp:163 | raw pointer taken from a temporary smart pointer, used in the same frame | N (same frame, cell holds the reference) |

### Deletes

| Where | What | Status |
|---|---|---|
| WeaponFire.cpp:511-519 | ghost projectile disabled and deleted inside the reference walk, before its 3D loads | F 4902dce |
| DeferredDelete.h:14 | 3 s fallback can delete during a long load stall | O 76z |
| RemotePlayers.cpp:601 | young spawns go to the graveyard before a save instead of being deleted | O 6ut |
| RemotePlayers.cpp:336, :385 | frame copy deleted while the stand-in is inside it | O clf |
| WorldSync.cpp:595 | picked-up item disabled without a 3D check (disable only) | N |
| WorkshopSync.cpp:249-282 | copies deleted through DeferredDelete or the graveyard | N |

### Type confusion and engine layout

| Where | What | Status |
|---|---|---|
| WorldSync.cpp:530-537 | linked-ref entries hold form IDs until resolved | F 6da1cb6 |
| WorldSync.cpp:477, :519 | open/close state read and set on actors | F 4902dce |
| WorldSync.cpp:143 | ragdoll push source and target reversed | F 4902dce |
| WorldSync.cpp:331 | loose mods accepted as inventory objects | F 4902dce |
| Face.cpp:189, :202 | engine-owned structures allocated with CRT new | F 4902dce |
| Face.cpp:47-56 | tint entries built by writing a vtable into calloc'd memory | O (noted under 6hi; needs engine constructors) |
| MapShare.cpp:16-52, DevCommands.cpp | map-marker layout assumed by cast in two places | O 6hi |
| WeaponFire.cpp:111, :149, :398 | vtable offsets assumed for the animation hooks | N (version-pinned build) |
| Equipment.cpp:45, :70; RemotePlayers.cpp:104; WorldSync.cpp:121, :396, :804; Party.cpp:109 | untyped lookups | N (each followed by a type check) |
| RemotePlayers.cpp:331 | any furniture accepted as a power armor frame | O (trust epic, 4co family) |

### Races

| Where | What | Status |
|---|---|---|
| WorkshopSync.cpp:35-42 | sinks walked `copies` without a lock | F 4902dce |
| WeaponFire.cpp:450, :456 | `watchUntil` written from hooks, read in Frame | F 4902dce (atomic) |
| WorldSync.cpp:153-312 | death, hit, container, activate sinks | N (handles and IDs under `inboxLock`; `IsPuppet` under its own lock) |
| Puppets.cpp:161-221 | update hook | N (runs in the actor update, guarded by the registry lock) |
| Session.cpp, Server.cpp, DevChannel.cpp, transports | receive thread, server thread, listener | N (no engine objects touched off the game thread) |
| Steam transport at DLL unload | Steam API called after it may be gone | O 7v3 |

### Null dereferences

| Where | What | Status |
|---|---|---|
| RemotePlayers.cpp:264, :347, :452; WorkshopSync.cpp:232; WeaponFire.cpp:548 | `TESDataHandler` unchecked | F 4902dce (`EngineRefs`) |
| QuestSync.cpp:51 | `TESDataHandler` unchecked | F 4902dce |
| WorldSync.cpp:513 | `GetLock()` re-fetched | N (state only reported when a lock exists) |
| NpcSync.cpp:110, :202; Party.cpp:614; Compass.cpp:211; Session.cpp:90 | guarded by callers | N |

### Network trust (not crashes; own epic lkt)

Explosions at any position regardless of friendly fire; activations without proximity; deaths of
any NPC; container floods; 1000-damage hits attributed to any NPC; face spam leaking forms;
workshop placement of any base anywhere; unverified actor claims; a remote hello resetting the
host's session; world-state resend rate; linear quest search; forced quest stages; lock changes
without proximity; NPC equipment growth; unsanitised HUD text. Fixed already from this list:
unbounded coordinates (261d792), health backlog (4902dce), second welcome (4902dce), swing damage
(4902dce).

Fixed since, in order: the server believes a death only from the player who runs the actor, caps hits
per hit and per second, keeps one face report in five seconds, relays an explosion only within ten
seconds of a throw and near the thrower, caps companion claims and world states (c3dc533); the client
runs an activation only from a player standing at the thing, gives an NPC an item only when it lacks
it, waits for a settlement's workbench to be loaded, and strips markup from other players' HUD text
(0f1953d); a player's container changes stop at their share of the table, a joiner is told where each
dead NPC fell, built actors are turrets and robots only (bd24f6a); a workshop piece is shown only
within 12,000 units of its workbench (7f38a5c). Left as is: the world-state resend rate (955, four a
second of about 2 MB is the host's own bandwidth to spend) and quest stages, which the host already
forces only from a player who has the quest.

## Confirmation sweep

`sweep.sh` (stage scripts): unit tests, a clean build, deploy to both games, then over Steam in both
directions: movement trace, Corvega combat with NPC tracing, terminals, loot and the safe, grenades,
power armor in motion, a three-minute cell churn with save reloads, a reconnect, and three fuzz seeds
on the host with the crash logger attached. Pass means no "DOWN", no new crash log on either machine,
and lag, error and counter results no worse than the baseline (100 to 150 ms, under 5 units; NPC height
error under 40 units; ghosts and states counters moving as before).

### Results: the scripted sweep on 0.8.1 (2026-10-10, 18:11 to 18:26, commit f5837d3) against the pre-fix baseline

One uninterrupted run of sweep.sh; every section passed on both machines. Baseline provenance: the pre-fix fuzz run is scratchpad/fuzz-before.log (13:5x); the pre-fix NPC trace is stage/trace/host-npc2.csv and vm-npc2.csv (14:04); the pre-fix movement figures are the trace.sh output of the 13:05 and 13:25 runs as printed then (those two trace files were overwritten by later sweeps).

| Check | Before the fixes (13:00 to 14:00) | Final build |
|---|---|---|
| Unit tests | 197 pass | 205 pass |
| Build warnings (full rebuild) | 2 (pre-existing) | 0 |
| Movement, VM as the host draws it | 100 ms lag, mean 3 u, max 20 u | 100 ms, mean 3 u, max 22 u |
| Movement, host as the VM draws it | 150 ms lag, mean 2 u, max 26 u | 150 ms, mean 2 u, max 21 u |
| Combat, Corvega, NPC height error | max 33 to 35 u (37 tracks) | max 3 u (17 tracks) |
| Terminals, loot and the safe, both ways | applied both ways | applied both ways, loot counters 55 = 55 |
| Grenades, both ways | one ghost caught per throw | one ghost caught per throw |
| Power armor in motion, both ways (worst frame-to-stand-in distance over a 10 s run; the frame is pulled under the stand-in once it is 48 u behind) | one snapshot only | 45 u (VM in armor) and 71 u (host in armor), both under the 48 u threshold plus a sprinting stride |
| Cell churn with save reloads | not run before | 5 min, 11 hops, 3 reloads, both games up |
| Reconnect | not run before | VM restarted and rejoined, stand-ins back |
| Fuzz on the host, crash logger attached | 1,290 packets (3 seeds), no crash | 1,290 packets (3 seeds), no crash; 10,320 packets over 24 seeds through the day, counted from the run logs |
| Crash logs | the one Red Rocket crash per machine (13:23, 13:24) | none new on either machine |

The scripted sweep earned its keep: run in sequence, the power armor section failed three times while the same check passed on its own. The cause was a friend holding a grenade from the previous section: the stand-in gear check counted it, could never pass, stripped and re-dressed the stand-in every three seconds and threw it out of its frame each time (1e0e40b, GitHub #21); a frame still attaching or standing at the stand-in is also no longer replaced (f5837d3).

### Before and after from disk (2026-10-10, 18:30 to 19:10)

To make the comparison reproducible, the pre-fix sources (commit 0087695, the last before the audit fixes) were built and deployed to both games, then the final sources (52110c3) again, and the same checks run on each. The pre-fix trace files are `stage/trace/baseline-*.csv`, the final ones `final*-*.csv`; run logs are `scratchpad/baseline-run.log` and `final-run.log`.

| Check | Pre-fix build 0087695 | Final build 52110c3 |
|---|---|---|
| Movement, VM as the host draws it | 100 ms lag, mean 3 u, max 21 u | 100 ms, mean 3 u, max 25 u (three runs: 25, 25, 25; a first run right after the restart had one 126 u sample) |
| Movement, host as the VM draws it | 150 ms, mean 2 u, max 24 u | 150 ms, mean 2 u, max 23, 27, 23 u (first run 125 ms, mean 3 u, max 31 u) |
| Combat at Corvega | not measurable: the pre-fix build has no NPC tracing (14:04 files: worst height error 35 u) | 20 NPC tracks, worst height error 22 u apart from the one NPC that teleports (42 u) |
| Terminals, loot and the safe | **both games crashed on the first terminal** (crash logs 18:33 host, 18:34 VM, WorldSync.cpp:537, the linked-ref dereference) | applied both ways, loot counters 8 = 8, both games up |
| Grenades | not reached (crashed before) | one ghost caught per throw, both ways |
| Cell churn with save reloads | not run on the pre-fix build | 10 min, 24 hops with the roles swapped halfway, 5 save reloads, both games up |
| Reconnect | not run on the pre-fix build | twice: VM restarted and rejoined, stand-ins back |
| Fuzz, crash logger attached | 1,290 packets, no crash (13:5x log) | 2,580 packets (6 seeds), no crash |
| New crash logs | the terminal crash on both machines | none |

### Like for like (2026-10-10, 19:07 to 19:39): pre-fix commit d4aee27 against the final build b386c52, same checks, same seeds

d4aee27 is the last commit before the audit fixes that has NPC tracing; its churn and reconnect use no terminal, so it runs. Logs: scratchpad/baseline2-run.log and final2-run.log; traces stage/trace/baseline2-*.csv and final-combat1..3-*.csv.

| Check | Pre-fix d4aee27 | Final build |
|---|---|---|
| Combat, NPC height error distribution over all matched samples (p50 / p95 / p99 / max) | 0 / 4 / 6 / 12 units (one run; the 14:04 pre-fix run: 0 / 4 / 11 / 638) | 0 / 2 / 6 / 37, 0 / 2 / 8 / 34, 0 / 2 / 5 / 23 (three runs). The max is a single sample at a death or a stair; the distribution is tighter after the fixes |
| Cell churn with save reloads, 10 min | 23 hops, 4 reloads, roles swapped, both games up, no crash log | 24 hops, 5 reloads, roles swapped, both games up, no crash log |
| Reconnect, twice | VM rejoined both times, stand-ins back | VM rejoined both times, stand-ins back |
| Fuzz seeds 1, 2, 3 with the crash logger | 1,290 packets, no crash (13:5x, pre-fix code plus the fuzz command) | 1,290 packets, no crash |
| New crash logs on either machine | none in this run (the pre-fix build 0087695 crashed on the first terminal at 18:33 and 18:34) | none |

### Run-to-run spread on the pre-fix commit (19:40 to 19:48), to read the single-run maxima against

Three movement runs and three combat runs on d4aee27, plus its terminal check (the terminal crash fix 6da1cb6 is in it, so the check runs). Traces: stage/trace/prefix-move1..3-*.csv, baseline2-combat2..3-*.csv; log scratchpad/baseline3-run.log.

| Check | Pre-fix d4aee27, three runs | Final build, three runs |
|---|---|---|
| Movement, VM as the host draws it: best lag, mean, max | 125 ms 2 u 27 u; 100 ms 3 u 20 u; 100 ms 3 u 23 u | 100 ms 3 u 25 u; 100 ms 3 u 25 u; 100 ms 3 u 25 u |
| Movement, host as the VM draws it | 125 ms 1 u 24 u; 125 ms 2 u 21 u; 150 ms 2 u 22 u | 150 ms 2 u 23 u; 150 ms 2 u 27 u; 150 ms 2 u 23 u |
| Combat, worst single-sample height error | 12, 36, 19 u | 37, 34, 23 u |
| Combat, p95 of all samples | 4 u | 2 u |
| Terminals, loot and the safe | applied both ways, loot counters 9 = 9, lock states applied | applied both ways, loot counters equal, lock states applied |

The movement maxima of both builds lie in the same 20 to 27 unit band; the combat maxima of both lie in the same 12 to 37 band (a single sample at a death or a stair), while the p95 is lower after the fixes. Nothing measured is worse than before.

### Power armor with the run-max metric, both builds (19:51 to 20:07)

Largest frame-to-stand-in distance over a ten-second run of running, turning and jumping (the frame is pulled under the stand-in once it is 48 units behind, so a sprinting stride can show on top).

| Direction | Pre-fix d4aee27, three runs | Final build, three runs |
|---|---|---|
| VM in armor, host watching | 28, 42, 38 units | 44, 42, 51 units |
| Host in armor, VM watching | 51, 44, 58 units | 52, 54, 53 units |

Same band on both builds, no drift over the run, no frame left behind. Pre-fix fuzz with the same three seeds on d4aee27 plus the fuzz command alone: 1,290 packets, no crash (scratchpad/baseline4-run.log), matching the final build.

### The flying NPC report, confirmed (20:10 to 20:22)

Diamond City with 39 NPCs, both ownership directions, 30 s of running around: 31 and 26 tracked actors, worst height error 15 units, nothing flagged. The report was something else: a controlled shared kill at Corvega (the VM shoots a raider it runs, the host watches the VM stand-in) moved the stand-in **276 units** on the pre-fix build d4aee27 (the 0.7.0 push with its arguments reversed, which launched the killer stand-in) and **10 units** on the final build; the corpse took the push instead (97 units). Script: scratchpad/kill-push.sh. The gunfire sound report remains not reproduced (46 of 46 shots replayed and sounded, no loop left active).

### Soaks on 0.8.2 (2026-10-10 22:44 to 2026-10-11, soak.sh, three-hour runs of the standing set, both directions)

Each cycle: movement trace, Corvega combat, terminals and loot, grenades both ways, power armor in motion both ways, a three-minute cell churn with save reloads, every third cycle a VM restart and rejoin, then a fuzz seed on the host. Stops on a crash or a game going down.

| Run | Build | Result | What it found |
|---|---|---|---|
| soak3 22:44 | 82b73d8 | 2 cycles, stopped by me | A fuzzed reject packet left a NUL in the last reject reason, which cut the host's status line short and broke every id lookup after it (ec8ce6e strips control characters from wire strings). |
| soak4 23:07 | c706e3d | 3 cycles, VM crash | The host's grenade thrown across an exterior cell seam was never reported (28a1978, GitHub #24). The VM crash was my own doing: a 2.5 s activate-hold on the VM during the churn's cell change. |
| soak5 23:42 | f74cac1 | 3 cycles, VM crash | An explosion set off for a friend was reported back as our own after our next throw (710e49a). The VM crashed 10 s after the reconnect's main-menu coc: Scaleform texture manager on the render thread, plugin not on the stack. |
| soak6 00:23 | 928d899 | running | bFastLoading=false on the VM only, to see whether the loading-screen render crashes stop. |

Measurement artefacts fixed in the scripts along the way: the power armor check picked an older, deleted frame copy standing at the same spot (now the nearest copy that still has 3D; `refinfo` shows deleted= and disabled= since 9d229ba), the `refs` listing is one line separated by semicolons, and the climb-out looks up first so the activate-hold does not target the frame or a stand-in.

Scaleform render crashes with no plugin frame appear eight times in the host's logs from 2026-10-09 (two with LoadingMenu on the stack, three with HUDMenu) and twice on the VM in these soaks, both on loading screens. The host did not crash once in the soaks. The plugin's fast loading (presentInterval 0 while LoadingMenu is open) is the suspect under test in soak6.
| soak6 00:23 | 928d899 | 1 cycle, VM "down" | bFastLoading=false on the VM made its loads frame-bound and minutes long; dev-channel commands queued behind them ran late (a292b49 skips a command whose caller gave up). Unmeasurable; fast loading restored. |
| soak7 00:40 | a292b49 | 3 cycles, VM crash | Same loading-screen signature a third time (churn hop). The only plugin code writing to the HUD is the compass marker update, a UI-thread task every frame, through TreeNode::SetVisible, which is on the crash stack. |
| soak8 01:17 | 68a1448 | 6 cycles clean, stopped by me | bCompassMarkers=false on the VM only: no crash through two VM restarts and ~90 loads, against three crashes in nine cycles with it on. |
| soak9 02:22 | f2166cd | 9 cycles, VM crash | Markers on both sides with the fix: no Scaleform crash in nine cycles with three VM restarts. The one VM crash (03:59, cycle 9, save reload) is a different signature: BSGraphics::RegisterD3DTexture on the resource-creator thread, no menu and no plugin on the stack; not seen on the host. 0.8.2 released at 03:33 on this build. |
| soak10 04:04 | f2166cd | 2 cycles, VM crash | A third signature, and ours: a physics job handling a collision with the host's stand-in crashed in Get3D at the millisecond the VM replaced that stand-in (dc597fe: disable now, mark for deletion a second later, for actors, lights and frames). The fuzz had also injected player-left and joined packets that disturbed the host's own list (5f05f7e leaves session bookkeeping out of the fuzz). |
| soak11 04:38 | f771ba1 | 15 clean cycles, then the VM texture crash | Five VM restarts, no HUD or physics crash. The one crash at cycle 16 is the Direct3D texture one again, six seconds after a save reload, no plugin frame (bead 68p, two in about sixteen reloads, VM only). 0.8.3 released on this build. |
| sweep 09:56 + soak12 10:20 | aa94e6a (0.8.4) | sweep: all sections pass; VM crash at the reconnect's main-menu load (bead 6pj, D3D texture kill list, no plugin frame, second time). soak12: 4 clean cycles incl. a restart | Save-time graveyard flush (e0af5bf), world-state bucket (73c1ed9), idle compass posts nothing (83cfe13). |
| soak12 cycle 6 | aa94e6a | VM crash at a churn load | The texture kill-list crash a third time (bead 6pj), on a cell load rather than the main menu, no plugin frame. VM only, about one per hour of churn; the host has never produced it. |

### 2026-10-11 afternoon: companions, the HUD on loading screens, and the harness

- The VM crashed in the HUD text layout (the #25 family) seconds after a friend's spoken line was written to the HUD as a load began. HUD messages now wait while a loading screen is up and for 1.5 s after (1f7cdae).
- A friend's companion that walked in from a cell we have not loaded never appeared: its copy stayed unloaded where it lived. It is now moved to where the owner's copy stands (655b395). Found by companions.sh, which recruits Dogmeat through the game's Followers function on each side in turn and checks following, the friend's view and ownership with the friend standing beside him. Both directions pass.
- Harness: god mode is set, not toggled (dev `god on`); two extra rejoins had left both players mortal and a test grenade killed the VM player mid-soak, which reloaded a save and made the host refuse its container states as 14,000 units away. Dialogue options cannot be picked by injected keys after the first exchange (bead sz2); hotkeys need an in-session key helper, not the dev channel's Alt-tapping injection.
- Two agent branches merged: the grenade shown in the stand-in's hand (engine accepts the equip and fires the attach event, the model still does not load; round 3 pending) and the F1 menu (works; the friends scan does not list the host yet).
| soak14 15:46 + soak15 16:20 | 3892d29 / 59cf6d4 (0.8.5) | 3 + 11 clean cycles, then the VM texture crash | The 0.8.5 candidate: emotes off F9, mod quests and topics kept out of the sync, HUD queue, bring-in, F1 menu, power armor pieces only in the frame. Four VM restarts, no HUD or physics crash. The one crash (18:29, cycle 12, save reload) is the Direct3D texture one again (bead 68p, fourth time, no plugin frame, never on the host). v0.8.5-rc1 published after cycle 1; promoted to v0.8.5 while it ran. Correction: the run did not end clean. It ended at 18:43 with a host crash in Scaleform (the game's UI code), after the promotion; 0.8.5 carries that as a known issue. |
