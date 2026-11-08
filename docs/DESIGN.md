# F4Multiplayer design

## Overview

```
 Player A's game                Session server                    Player B's game
 (F4SE plugin)                  (in the host's game, or           (F4SE plugin)
                                 F4MPServer.exe)
 +------------------+           +------------------------+          +------------------+
 | Session          | --------> | join checks, rate      | <------- | Session          |
 |  RemotePlayers   | <-------- |   limits               | -------> |  RemotePlayers   |
 |  NpcSync         |           | relays player and NPC  |          |  NpcSync         |
 |  WorldSync       |           |   states (60 Hz tick)  |          |  WorldSync       |
 |  QuestSync       |           | keeps the session's    |          |  QuestSync       |
 |  Puppets         |           |   world state          |          |  Puppets         |
 +------------------+           +------------------------+          +------------------+
```

- **Transports:** the server talks to players through `ServerTransport` (`common/Transport.h`), the
  client through `ClientConnection` (`src/net/ClientConnection.h`). Reliable messages are ordered;
  unreliable ones carry player state (30 Hz) and NPC states (20 Hz for NPCs in combat or within 3000
  units of any player, 10 Hz for the rest). The server relays new player states every 16 ms.
  - ENet (UDP, `common/EnetTransport.cpp`) is always on. With Steam it listens on 127.0.0.1 only, for
    the host's own game.
  - Steam (`src/steam/`): `ISteamNetworkingMessages` through Valve's relays, addressed by Steam ID
    ("steam:<id>"). Data and control (1-byte keepalive/goodbye) use separate channels per
    direction (0/2 to the server, 1/3 to players). The first keepalive opens a connection, silence
    for 20 s closes it.
- **Steam:** the flat API is resolved at runtime from the game's own `steam_api64.dll` (no SDK files
  in the repo, nothing extra shipped). The host keeps a friends-only lobby and sets rich presence
  `connect`, so friends get "Invite to Game" / "Join Game". Accepting either fires
  `GameLobbyJoinRequested` / `GameRichPresenceJoinRequested`; the player joins the lobby and connects
  to its owner. The host accepts Steam sessions only from lobby members and Steam friends. Callbacks
  are registered with `SteamAPI_RegisterCallback` and dispatched by the game's own
  `SteamAPI_RunCallbacks`.
- **Server:** `common/Server.cpp`, one thread. Joining is checked for protocol version, max players
  (default 4), an optional password and, for a hello without a plugin list, a load-order hash that must match the
  session's (with lists, any load order gets in: see Form IDs across load orders).
  Connections that don't say Hello within 5 s are dropped. Each player has rate budgets (60 states/s,
  60 NPC state batches/s, 500 events/s).
  What a player may cost the others over a whole run is kept apart from their connection, by Steam ID
  (over UDP, by the identity their game says hello with), so leaving and rejoining doesn't renew it: a
  bucket of three world states refilled one per 10 s (past it a hello, or the world state after a load,
  waits for the refill rather than being refused), shared XP, face changes. Each player but the host has
  a share of each session table (15000 kills, pickups, doors and quest stages, 30000 container changes,
  10000 built objects; `Server::Options`; a fresh session starts them again); the log says once when a
  share is used up and when a table reaches 80% and 100%, and each game's HUD says so once a session
  at 80%.
- **Client:** `src/net/NetClient.cpp` keeps ENet (including DNS lookups) on its own thread, so the
  connection survives loading screens. `src/net/Session.cpp` runs on the game's main thread every
  frame through an F4SE permanent task.
- **Hang watch:** `src/HangWatch.cpp`, one thread. Once a second it asks Windows whether the game's
  window is "not responding" (`IsHungAppWindow`: the main thread hasn't read its messages for 5 s;
  nothing is sent to it, and a minimised game waiting for messages counts as answering). A stall of
  `iHangSeconds` (15; four times that during a save or a load, from F4SE's message before it to the
  one after, and on a loading screen; a stall seen in one of those keeps that limit and is reported
  as one, even if the save ends before the game answers) is logged once with a short report, and the
  recovery is logged too. For the report the watch suspends the main thread just long enough to read its instruction
  pointer and copy the top of its stack (system calls only: the main thread may be stopped holding
  the heap or loader lock), then resumes it and names the instruction and the return addresses it
  finds on the copy as module+offset, from `VirtualQuery` and the image's mapped file name (no
  dbghelp, no loader lock). It then times every thread of the game over half a second
  (`GetThreadTimes` on a Toolhelp thread snapshot) and looks the same way at up to four other threads
  that kept a processor at least half busy meanwhile: a main thread stuck in a wait is waiting for one
  of them (a host froze on 2026-10-11 with the main thread waiting for the physics step while a job
  thread went round an endless loop in Havok on a ragdoll whose bodies had become NaN, which only a
  full dump showed). The report says too how busy the main thread itself was, so a loop of its own
  reads differently from a wait. The report goes to the log and to `F4Multiplayer-hang-<time>.txt` next to
  it (the newest 3 kept, leftovers of an interrupted write removed at start-up); it names modules,
  never paths. A minidump (`.dmp`) is written too only with `bHangDump`: `MiniDumpWriteDump` in the
  hung process suspends every thread and then needs the loader and heap locks, so it can turn a
  freeze the game would have recovered from into a permanent one, and dbghelp may be in use by a
  crash logger. The dump holds file paths and the Windows user name; it is for sending to the
  maintainer privately, never for a public issue. The one game value the watch reads is `Main`'s
  quit flag: once the game is quitting (its shutdown reads no messages for a while) the watch stops.
  The judgement is `src/HangDetector.h`, unit tested.
- **Frame watch:** `src/main.cpp` (`RunFrameWatch`), one thread, the hang watch's complement: the
  main thread answers its window, yet no frame of ours runs. The frame chain (the F4SE task posts a
  frame message once a drawn frame, the window procedure takes it on the main thread, a frame's game
  work runs unless a gate stops it: a frame of ours already under way, the engine's own message pump,
  a loading screen, a save or load) stamps when each link last ran and how the last frame message
  ended. Once a second the watch reads the stamps: when the game is alive (it draws, or the task
  runs) and no game work has run for 15 s (120 s on a loading screen or during a save or load, which
  hold frames by design), it logs which link stopped and why, with where the main thread is (the hang
  report's look at it), again every 60 s while it lasts, and once more when game work runs again. A
  host's game once ran a quarter of an hour that way after a `coc`, its main thread spinning in the
  engine's own message pump (`PositionPlayerJob::Run`, where every frame message is put off), with
  nothing in the log. Two stops it re-arms itself, once a second: the F4SE task not running (it posts
  the frame message in its place) and the window procedure never getting the message (it hooks the
  game's window again should the engine have made a new one). The judgement is
  `src/game/FrameWatch.h`, unit tested. The loading-screen gate, the one gate with no end of its own,
  counts a loading screen as up for at most five minutes (`UiHold::Guard`, `src/game/UiHold.h`). A
  notice goes to the HUD (`SendHUDMessage::ShowHUDMessage`) with no sound name, a null one, as the game's
  own notices (Papyrus `Debug.Notification`, the XP and quest lines) send it. The HUD hands the name to
  `UIUtils::PlayMenuSound` as the notice shows, which looks a sound up by a hash of its editor ID: a null
  name is never looked up, but `""` was (hash 0), and matched any sound record whose editor ID was set
  empty in the player's load order. That sound played on the player and was never stopped: a looping one
  (a generator's hum, rushing water) stacked with every notice until a load (two players' reports, 0.11.2).
- **Log:** `src/Log.cpp`. CommonLib's `F4SE::Init` opens `F4Multiplayer.log` and empties it; just
  before, `LogRotation::Rotate` (`src/LogRotation.h`, unit tested on real files) moves the last three
  launches' logs to `.1.log`..`.3.log` with `std::filesystem::rename` and error codes, so nothing can
  throw inside the `noexcept` Init (CommonLib's rotating sink throws on a failed rename). A file
  another program holds is not kept, and the log says so. Right after Init a sink wrapping
  CommonLib's runs every line through `Redact::Scrub` (`common/Redact.h`): a SteamID64 becomes a
  per-launch alias (`~` and six hex digits of a salted hash), an IPv4 address loses its last part.
  The server's log function does the same. That is the net; the lines themselves name players by
  ID and accounts by alias, and HUD messages (which name players) are not logged at all. `sLogLevel`
  (info or debug) sets the level; flushing is CommonLib's (every INFO line).
- **Faces:** `src/game/Face.cpp`. Each player sends their character's look once per session
  (kReportFace: head parts, hair colours, the complexion texture set, body-shape triangle, morph
  region sliders, morph sliders by ID, facial bone transforms, tint layers, body tint; the
  complexion and the keyed morph sliders since protocol 32); the server keeps the latest per player
  and tells newcomers. The receiver duplicates the stand-in's base NPC (a settler of the right sex),
  writes the look onto the copy (tint layers are rebuilt by vtable, pointing at the race's template
  entries by uniqueID; a layer whose class isn't its template's, by the template's vtable, is left out:
  FaceHeads::TintFits), takes the settler's face template off it (faceNPC and the "use traits"
  flag: with those the engine loads the settler's baked FaceGen files as the head and looks at
  nothing else) and spawns the stand-in from it. The copy is a runtime form with no FaceGen files.
  The engine asks one function, UsesPreprocessedHead (ID 2209308, +6DF3A0), whether an NPC's head is
  a prebuilt FaceGen nif or is built at runtime from its parts; it answers "runtime" only for the
  player, the chargen NPCs, the spouse and NPCs whose face a save has changed, and "prebuilt" for a
  form with no file, so for our copy it went looking for a nif there is none of and attached no head.
  `Face::Install` (at plugin load) points the six calls to it (in the head task, TESNPC's head part
  update, the FaceGen head data queue, DoOnQueueMe and the head build, twice) at our own check,
  which answers "runtime" for our copies (by address and form ID, in `FaceHeads::Copies`, lock-free
  since the loader threads ask) and passes every other NPC on to the engine's. All six must be
  calls to it as 1.11.240 has them (another plugin's patch at one, or another game version: none is
  patched, and every copy gets the settler's head). `src/game/FaceHeads.h` (game-free, tested) holds
  those rules and which pieces a copy gets; `tools/addrcheck.ps1` checks the six sites. With that,
  `bFriendFaces` is on by default (live 2026-10-18: both directions, a cell change, a save reload, a
  rebuild and a face changed in play), and the dev's `facemask` picks the pieces for an experiment.
  A changed face deletes the stand-in; it respawns with a fresh copy. Copies are never freed (forms
  can't be), and at most 512 a session get their own head (then the settler's).
  `src/game/FaceWatch.h` (game-free, tested) watches each stand-in placed with its own head: once
  its 3D is loaded with no face node on it for 3 s, the engine is asked to build its 3D again
  (`Actor::Reset3D`, what LooksMenu does after editing an NPC's face), once; with still no head 3 s
  later, or with our own player's face node gone 2 s while the stand-in is loaded (seen once live:
  the copy and the player seemed to share one runtime face), that friend's own head is given up
  for the session and their stand-in is placed again with the settler's face. Our own head still
  gone 2 s after that stand-in went is asked back once (`Reset3D` with the head and face flags).
  With LooksMenu (f4ee.dll) installed, the face also carries its body sliders (protocol 29; at most
  128, names up to 64 characters, values within +-2): `src/game/LooksMenu.cpp` reads the player's
  through LooksMenu's Papyrus script BodyGen (GetMorphs, then GetMorph under no keyword, both answered
  on the VM's threads; again every 15 s with no menu open) and the face waits for the first read.
  A receiver with LooksMenu sets them on each new stand-in (SetMorph; UpdateMorphs once every set is
  answered, if its 3D was built meanwhile) and removes them (RemoveAllMorphs) when the stand-in goes,
  since LooksMenu keeps them by form ID and the next runtime reference may get that ID. Without
  LooksMenu on either side nothing is called (`src/game/BodyMorphs.h` holds the rules, tested).
  Since protocol 39 the face carries LooksMenu's overlays too (tattoos, face and body paint; at most
  64, template IDs up to 128 characters, placement within +-10, tint and alpha a byte each as LooksMenu
  saves them): read in the same round as the slider names with its Papyrus script Overlays (GetAll,
  each an `Overlays#Entry` struct), added to each new stand-in (Add, one struct per overlay; Add
  refuses a template from an overlay mod the receiver lacks), laid on with Update once every add is
  answered, and removed with RemoveAll when the stand-in goes (`src/game/LooksOverlays.h`, tested).
  A LooksMenu skin override needs no LooksMenu call: LooksMenu writes it onto the base NPC (its skin
  armor, face texture set and head parts), so `Face::Read` carries the skin armor beside the head
  parts and complexion it already carried, and the copy gets it (with the body, so with or without
  the friend's own head). LooksMenu's sculpting is the vanilla face data, carried since protocol 32.
  Not carried: F4EE's node transforms, which it offers no script or plugin call for. None of this
  has been tried live: the rig has no LooksMenu (the calls follow F4EE's published source).
  Dev command `looks <playerId>` tells what was set on that friend's stand-in (sliders, overlays
  added and refused), its skin armor, and asks LooksMenu what it holds for it (GetMorphs, GetAll;
  answered into the log). Saves: the removal when a stand-in goes is itself a queued call, and a
  save takes every stand-in out just before it is written, so LooksMenu's own co-save data (written
  in the same save) could keep a friend's looks under the stand-in's runtime form ID, which a later
  reference in that save may get. So `BodyMorphs::StandIns` lists each ID until its RemoveAll calls
  are all answered, and the co-save record `LOOK` v1 (`src/game/LooksSave.h`, at most 512 runtime
  IDs, tested) holds those and every stand-in still being set as a save is written. The record is
  read on the load's thread and kept under a lock (`LooksSave::Loaded`), and listed only in
  `LooksMenu::OnGameLoaded`, after the removals queued before the load are dropped (frames can run
  during a load that outlasts the hold). After that, `LooksMenu::Frame` looks the listed IDs up once a second; a reference with one that is not a
  stand-in of ours gets RemoveAll (no script object made for it for its first 5 passes), then
  UpdateMorphs and Update, and the ID leaves the list once answered. A friend's stand-in placed under
  a listed ID (the likeliest reference to get one after a load) has RemoveAll queued first and its own
  sliders and overlays set only once that is answered. Then whatever its own looks leave out (no
  sliders, or no overlays) is rebuilt at once, since its 3D may have been built wearing the old ones. An ID with no reference yet stays listed, into
  the next save too. Without LooksMenu no record is written. Not yet checked against F4EE's source:
  whether its load drops entries for forms missing then (which would make most of this moot).
- **Remote players:** `src/game/RemotePlayers.cpp` spawns an NPC actor per remote player when the
  player is in the same interior or within about 9000 units outside. It renders a friend in the past
  and blends between received states. Each state carries the sender's clock (protocol 29), moved onto
  ours by the quickest trip of the last 64 states (src/game/Prediction.h, SenderClock), so states that came
  together after a stall are still drawn as far apart as they were sent. How far in the past (`delayMs`,
  at least 70, at most 250) follows the furthest past the newest state our frames of the last three
  seconds were (src/game/StateBuffer.h). When the next state is late anyway, the drawn time waits at
  the newest one, so the stand-in stands and then goes on from there rather than leaping ahead.
  `net status` shows the average gap between a friend's states
  (`gapMs`, about 33) and `held=a/b`: frames, out of b with the friend moving, where the next state
  was late and the stand-in stood still. More than a few percent means the delay is too short for
  that connection. `offSpot=a/b worst=w` counts frames, out of b with the friend standing still
  (their state says speed 0: one held still only by a late state is not counted), where the
  stand-in was found more than 2 units from where it was put (w the furthest): anything but 0
  means something in the engine is moving it, such as a power armor frame copy. Before every save
  and load, all puppets are deleted so they never end up in a save file.
- **Puppets:** `src/game/Puppets.cpp` hooks `Actor::Update` (vtable slot 0xCF, taken from a live
  actor's vtable). For puppets it calls `UpdateNoAI` and forces position and heading. It sets
  `kMovementBlocked` and the like every frame (restoring only the bits it set on release), and
  re-applies the do-nothing package every 2 s. Puppets are keyed by pointer but checked against their
  form ID before use, so a freed actor's address is never treated as a puppet.

## Findings (reverse engineering, runtime 1.11.240)

- `TESObjectREFR::data.location` is current for the player while moving. The 3D root's `world`
  transform is not usable right after loading.
- References created with `TESDataHandler::CreateReferenceAtLocation` need `clearStillLoadingFlag`,
  or their 3D is never shown.
- NPC AI fights forced positions. You need the Update hook, `kMovementBlocked` and
  `InitiateDoNothingPackage` together for an actor to stand still.
- `IVirtualMachine` declares overloaded virtuals. MSVC orders overloads in reverse, so calling
  `CreateObject`/`DispatchMethodCall` through CommonLibF4 hits the wrong slot (crash). Runtime-created
  actors also have no bound "Actor" script object, so Papyrus calls on them fail.
- The Settler base (0024A037) randomizes looks per spawn. Magnolia (0002268A) is consistent.
  Curie's 001846CB is her robot form.
- Loading screens are tied to presentation. Setting `presentInterval = 0` while `LoadingMenu` is open
  cuts load times substantially.
- `ModActorValue(kDamage, health, -x)` on the player kills them at 0 health (normal death and reload).
- `TESQuest::SetStage` doesn't start a stopped quest; the console `setstage` does.
- A hit queues the victim's reaction to its attacker (2231180, +CD6F70), run later by the task queue (2229323, at
  +0x2DAA) from two handles; a disabled reference's handle still resolves. The reaction checks the attacker's AI process but
  hands the victim's to `AIProcess::EnterCombat` unchecked, and `TESObjectREFR::Disable` deletes an actor's process at once:
  a stand-in replaced in a fight crashed the game there. `CombatReactions` (src/game/CombatReaction.h) hooks the task's call
  and skips a victim that is disabled, deleted or has no process (`reactionsSkipped=` in `net status`).
- The `ActorState` constructor (140CEB250) clears only bits 0-7 and 9-19 of its second flags word: bits 21-31 keep
  whatever the memory held. On some runs the player starts with `inSyncAnim` (bit 21) on, and while it is on,
  `PlayerControls::PerformInputProcessing` (140DDBE50) hands no input to the player's handlers: no walking,
  looking or shooting, while menus work. A coc or new game keeps it, and a save made then keeps it too.
  `SyncAnimMark` (src/game/SyncAnimMark.cpp, StuckSyncAnim.h) clears it once at each game's start, only with
  no synced animation running at all and the player in no scene, furniture or dialogue, alive and not down.

## Remote player look

- Body: a settler of the player's sex (0020A578 female / 0020A57B male) unless `iMyAppearance` is set.
  Only the two settlers, or the receiver's own `iPuppetBaseForm`, are placed: any other NPC (unique or
  not) would bring its scripts and factions along, so it is shown as the receiver's default.
- Gear: the equipped armor and weapons (base forms) are sent when they change (checked every second),
  with the mods on each equipped copy that has a `BGSObjectInstanceExtra` (OMOD form, attach index,
  rank, and the extra's item index; protocol 29). The puppet gets `removeallitems` + `equipitem` via
  the console; each item is given built with the owner's mods (a new `BGSObjectInstanceExtra` with
  `AddMod`, its `ExtraInstanceData` from `TESBoundObject::ApplyMods`, then
  `TESObjectREFR::AddInventoryItem`), or, with no mods on record, the item's default combination
  (`BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra`, useDefault): the console's `additem`
  rolled the object template for the receiver's level (a stock 10mm shown with a long barrel). A
  change of mods alone replaces the stand-in's copy (`StandInGear::ChooseCopy`). Items with neither
  mods nor templates still go in by `additem`.
- Weapon drawn/holstered follows the `kWeaponDrawn` state flag (`DrawWeaponMagicHands`).
- Sneaking and moving (`kSneaking`): the weapon-away sneak (MTBehavior's `MTSneakIdleLocomotion_SM`) sets off
  from its sneak idle only on `moveStartAnimated` (its `moveStart` waits on a condition that never holds), so a
  sneaking stand-in's start sends `moveStartAnimated` then `moveStart` and the graph in use takes its own
  (`MoveSync::NamesOf`, tested in SneakMoveTests). The resend every `MoveSync::RESYNC` is `moveStart` alone:
  `moveStartAnimated` is a wildcard there and would replay the set-off clip. The weapon-drawn sneak walks or runs by
  `iSyncSneakWalkRun` and `sneakWalkStart`/`sneakRunStart`; `iIsInSneak` is set every frame as
  `Actor::SetSneaking` would, so a graph rebuilt or back from a jump enters its sneaking states.
- Name: `ExtraDataList::SetOverrideName`, and the hooked `GetActivateText` returns true for puppets so
  the HUD shows the name when you look at them (pressing E still does nothing).

## Shared NPC AI (src/game/NpcSync.cpp)

- Every loaded, living NPC from a plugin file has one owner, whose game runs its AI. The server decides:
  - nobody owns it: the first player to claim it (they have it loaded);
  - a player's current companion goes to that player, unless it is also another player's companion
    (then the first keeps it, so two players with Dogmeat don't trade him forever; the other's game asks
    again every 15 s rather than every 3 s, and at once when he is released: `NpcClaims`, `refusals=`);
  - a player who hits or talks to an NPC takes it over, unless it is someone's companion, or it changed hands or its
    runner claimed it again in the last 5 s (`Server::Options::takeoverCooldown`). The runner's game claims it again
    with each of its player's hits on it (2 s apart at most) as it does every 2 s while they talk to it, so two players
    shooting one raider leave it with whoever hit it first until they stop hitting it, instead of trading it every 5 s
    with its AI starting over in the taker's game each time; the other's hits reach it as damage, as any non-runner's.
    What a hit asks for is `NpcClaims::Book::OnHit`. A hit on a spawn (an NPC a friend's game made at runtime, below)
    asks nothing the server grants: what was hit is a copy placed from the spawn's template, no NPC of the friend's
    game to hand over, and the server leaves a spawn with the game that made it for good. The claim still goes, as
    often as a takeover would, so the server notes the hitter fighting it and believes their kill; their hits reach
    it as damage. The game logs once per spawn why the copy stays theirs (`spawnFights=`).
  - a player in workshop mode takes over the settlers of that workbench (`SettlerCommand`: those linked to it by
    `WorkshopItemKeyword`; four a round, every 2 s, each at most every 5 s) and claims again the ones they run, which
    are not handed off meanwhile (`commanding=`, `settlerTakeovers=`). Giving a settler a job runs on the settler's
    own AI in the commanding player's game, and a mirrored settler has none. Settlers made at runtime (most
    recruits) are not shared, so every game runs its own. A job is the commanding game's own state (the crop's or
    post's owner, the settler's worker flags in Papyrus) and is not sent: the friend's game sees the settler move, not
    its job, and keeps the jobs its own player gave. On the rig (2026-11-01, Sanctuary, Codsworth as the shared settler)
    the host and the joiner each gave him a crop through workshop mode, the joiner's game taking him over first
    (`settlerTakeovers=1`).
- The owner sends its NPCs' cell/worldspace, position, heading, speed, moveMode and weapon-drawn at
  20 Hz when they're in combat or within 3000 units of any player, else 10 Hz (unreliable). The
  others register the NPC as a `Puppets::Kind::kNpc` puppet: AI off, movement copied with
  interpolation, but it can be hurt and killed. Each copy renders 100 ms in the past when its states
  arrive at most 70 ms apart on average, else 150 ms, eased 2 ms per frame (`net status`: `fast=` NPCs
  we send at 20 Hz, `delayMs=` the range over our copies). Each state says when its owner's game sampled it
  (`ActorState::sentMs`, the owner's clock, as a player's states do), and the copy draws the states as far apart as
  they were sampled, however they arrive (`MirrorClock.h`, game-free and tested: `Prediction::SenderClock`, started
  over when the NPC changes hands). Stamped by their arrival alone, the states of an NPC whose owner halved its rate
  (it went past 3000 units from everyone, or its fight ended) were stamped up to 130 ms early for two seconds while
  the arrival clock learned the new gap, so the copy stood at each state and stepped on, and 125 ms late when the rate
  doubled again, so the copy fell that much further behind and then caught up; a state with no `sentMs` takes the
  arrival clock as before. If the owner's copy is in a
  different cell or worldspace (a companion that followed its player through a door), the local copy
  is not moved there; it runs its own AI meanwhile. The same goes when the owner's copy is more than
  a cell (4096 units) away from ours (fast travel, script moves), when no state came for 3 s (the
  owner's game is paused, loading or gone; owners send nothing while a menu pauses them), and for
  our own current companion (two players can both have Dogmeat). A copy let go to its own AI walks its own way, and
  is put back where the owner has it when it is mirrored again: `net status` counts the fallbacks by reason
  (`fallbackWhy=` elsewhere/silent/far, `NpcPlace::Lost`), those while the owner's status named a menu that stops
  their game (`fallbacksPaused=`: their world stood still, so the copy's walk was for nothing), and the jumps of a
  body's length or more when mirrored again (`snapBacks=`, `snapMax=`), with the first few and every 50th in the log.
  Silent while the owner's status names such a menu (the Pip-Boy, the pause menu, a container, a terminal: their
  world stands still), a mirrored copy is instead held still where their last state put it, mirrored as before, for up
  to 60 s of silence (`NpcPlace::WhileSilent`, game-free and tested; `holds=` held/let go after all, `holdMax=`), and
  walks on from there when their states come back: no walk of its own, no jump. Not one in a fight here (our player
  hit it lately, it fights our player, or the engine has it in combat): a held copy can't fight back, so that one runs
  its own AI as before and jumps a little when the owner comes back. The copies a friend's game can take on are handed
  over by the owner's game after 5 s of the menu anyway (`PauseHandOff`); the hold is for the ones it keeps.
  Every change of hands restarts the NPC's AI in the game that takes it (from where that game's copy stood, a delay
  behind) and makes a copy of the game that had it, which jumps to the new runner's: `hands=` counts the changes and
  those within 10 s of the last (`NpcHands.h`, game-free and tested; a hit takes an NPC over, 5 s apart at most, and
  two players shooting one traded it every 5 s until the runner's own hits kept it), with the quick ones logged. `ownedBlocked=` is NPCs we run found with
  their movement blocked (a script's doing, or a copy's flag left on one; logged once each), `fightStandIns=` how many
  of ours fight a friend's stand-in, which can't be hurt or pushed aside. `mirror <ref>` ends with the engine's AI
  facts for any NPC (`ai:` process level, current package and its type, combat target, movement blocked, sit state).
- When the owner unloads the NPC (or leaves), it is released and the next player who has it loaded
  claims it. One that dies is kept until its death is reported (up to 15 s, `NpcHold`), and one our player hit
  in the last 10 s is not handed to a nearer friend. Mirrored NPCs are handed back to their own AI before saving.
  New players get the current
  owner list on join.
- Gear: each game rolls NPC gear on its own, so the owner reports what its NPCs wear when it changes
  (`ReportNpcEquipment`, owner only) and the server keeps the last report for newcomers. A copy is dressed
  from its own inventory only (`GearPlan`): it equips what it carries of the owner's items, unless that
  would take off something it wears that the owner's copy does not, and nothing is ever added or
  unequipped (the NPC is a real one of that game and goes into its saves: a settler the host dressed keeps
  the host's armor, and a corpse holds only its own roll). An NPC in a power armor frame is left alone, and
  power armor pieces are never put on a body. Where its roll lacks the owner's item it wears its own; the
  one thing it is given is a grenade or mine to hold for a replayed throw, which the throw spends.
- Roll (`NpcRoll.h`, game-free and tested; `Rolls.cpp`; protocol 50): a leveled NPC (a bloatfly, a ghoul, a raider
  placed from a leveled list) is put together by each game on its own as it first loads, its list and its legendary
  chance rolled at that game's player's level, so one game had a legendary black bloatfly where the other had a plain
  one. The engine keeps the roll on the reference (`ExtraLeveledCreature`: the NPC it was placed as and one NPC per
  template slot, traits to keywords). The owner reports it (`kReportNpcIdentity`: the slots, its level, legendary or
  not, and for a spawn its factions) when it changes, and the server keeps the first runner's as the session's roll: a
  later runner's report is ignored while it stands, and it goes when the NPC dies and when nobody runs it; when that
  player leaves while someone else runs the NPC, it stays, as that runner's. A newcomer is told the run NPCs' rolls. The others write the owner's roll onto their copy while they
  mirror it (its `ExtraLeveledCreature` slots) and give it the owner's level, only when theirs was placed as the same
  NPC and each of the roll's NPCs is one their lists can give (`NpcRoll::Walk` over lists, templates and legendary
  templates); a roll that can't be written is tried 3 times, 5 s apart, then left as their game has it. The engine puts
  the NPC together from those slots when the copy loads, so the roll is the owner's from the copy's next load on (a cell
  left and come back to, a fast travel, a save loaded): on the rig, written ghouls were the host's once the cell loaded
  again, while `CopyFromTemplateForms`, a disable and enable and `Reset3D` left the NPC already made as it was, so a loaded copy is never rebuilt (that risks the game mid-fight): only the roll is written for its next load. Until
  then only the level is the owner's. A plugin's NPC whose base is a plugin's NPC (not leveled) is every game's own,
  and its factions are its game's story (a recruited settler), never set from another game. `net status`: `rolls=`
  sent/known/written/kept/factions set; `mirror <ref>` shows ours and theirs.
- Shared rolls (protocol 57, docs/SHARED-ROLLS.md; `SharedRoll.h`, `RollHook.cpp`): the roll above came after every
  game had built its own, so it took only at the next load. Now every game builds the NPC as the session's roll the
  first time: a hook in the engine's leveled-actor resolver gives it a friend's spawn's roll as we place our copy, the
  session's roll of a plugin's NPC (kept by the server for the session's life, in the session file, sent to a joiner),
  or the session's pick, seeded by the session and the reference and read at the session's NPC level (the host's
  `sNpcLevel`: average by default, highest, host or own) and with the session's legendary chance multiplier
  (`sLegendaryDifficulty`: the average of the games' difficulty multipliers by default, host or own). The report above is now the check (`rollsDiffer=`, which should stay at 0).
- Hits on a copy whose roll is not the owner's yet: what our hit took is reported as a fraction of the copy's maximum
  and the owner takes that fraction of its own, so the shots it takes to kill are as many as the shooter's copy needs,
  dealt through the copy's damage resistance. A copy rolled tougher than the owner's NPC (a Stalker's 30 DR against a
  Roamer's 15, a Guardian against a Watcher) takes less from each shot, and so does the NPC; one rolled weaker, more.
- A copy's health is always its owner's: what it gains with no report from the owner's game is taken back once it has
  stood 100 ms (`CopyHealth::Book::Check`, `copyGains=` times/health), as a drop with no hit of ours is given back. A
  copy rolled legendary here and plain there mutated at half health and healed to full, which stood and was never
  sent: the player who hit it saw their hit heal it while the NPC stayed at half. A gain under 1 % of the maximum is
  left until it adds up; a new maximum (the owner's level set on the copy) keeps the share of it the copy stood at.
  What still differs until the next load: the copy's name, its mutation's look and its damage resistance.
- A drop on a copy with no hit event of ours, in the second after our player's shot or swing aimed at it (the nearest copy
  the view is on, `CopyHealth::NearestAimed`), is the shot's: sent to the owner's game as any hit's damage, not given back
  (`CopyHealth::SHOT_REACH`, `copyUnseen=`; the first logs a warning). Not with another hit on the copy in the 10 s
  before (its effect may still tick) or a blow of ours left to the owner around it. On the rig a host's shot took 24.6
  off a friend's raider and no hit event for it reached the mod: it was given back, the raider never lost a point in
  the game that ran it, and only the friend could kill it. A game whose hit events stop (another mod can stop them
  before ours) made every NPC a friend runs one its player couldn't kill.
- Fix-ups: a copy that is off here while the owner's runs is switched on (`enable`), and a friend's companion
  standing near us whose copy is in a cell we have not loaded is moved here (`moveto`). Each is tried at most
  every 5 s (10 s for moving) and three times per NPC (`RetryBudget`): one a quest keeps off in our game is
  then left as it is. The tries start over once the copy is on, or here (`net status`: `gaveUp=`).
  One an enable parent switches is switched on through the parent at the top of its chain (`EnableChain.h`,
  game-free and tested; `EnableParents.h`): the engine's own `enable` returns at once for a reference with an
  enable parent ("... has enable state parent ... will be ignored"), so up to 0.10.1 such a copy stayed off for
  good. Gun Run (DN083_Barney, a misc quest, so each game runs its own) enables Salem's mirelurks, their ambush
  spots and eggs through one marker (MirelurkQuestEnabler, 000C3D6F) at its stage 30, in the game of the player who
  got there; a friend fought a mirelurk the other player could not see anywhere (a Nexus report on 0.10.1). Never a
  parent with everyone on their own story (one marker often switches a whole scene, characters of our own story
  among them), nor one the opposite way (that parent would have to go off). The friend's NPC running proves only
  that their chain is on, not that ours is off because our game is behind: a plugin only our game has may hide a
  vanilla NPC behind a marker that stays off, or our story may be ahead and a quest of ours switched the marker off
  again. So a parent is switched on only when every link and parent comes from the game's own files (Fallout4.esm
  and the official add-ons, no other plugin changing them) the table below was made from, no script of the game's
  own ever turns a parent of the chain off, the top is off as its plugin has it, no parent is a dead actor or deleted, and the chain's parents
  switch at most 64 references between them (Gun Run's marker: 48). The scripts that turn parents off are read from
  the game's files once and kept in a table (`DisabledParents.h`, written by `tools/questscan.ps1
  -DisabledParents`: quest stage fragments, quest, alias, reference, base object, topic, scene, package, perk and
  terminal scripts, calls followed a few deep, each judged with the property values it has where it is attached;
  aliases forced to a reference or filled with a forced location's references of a ref type, collection aliases
  turned off with DisableAll): 677 of the 1624 enable parents of Fallout4.esm, Automatron, Far Harbor and Nuka-World,
  323 of them off in their plugin. Half the quest scene markers are off in their plugin, turned on at one stage and off at a later one: the
  airport's Brotherhood (BoSAirportGuards, 0019C046: BoSEnable's stage 10 on, its stage 200 and Inst307Fight's
  stage 10 off) would come back for good, 11 soldiers and a scribe, in the game of a joiner past it the first time
  the host's game ran one. Those are left to our own story (`kStoryTurnsOff`). Gun Run's marker is only ever turned
  on. Whether the top is off as its plugin has it: the references loaded with the game's data are listed once at
  kGameDataReady, before any save, with which of them were off. One that was off then is as its plugin has it; one
  that was on then (every enable parent in Fallout4.esm is persistent, so loaded with the data) is refused whatever
  its change flags say, as our game switched it off. One not loaded with the data, a temporary one, is switched on
  only if our save records no change of its flags. A save's change of a reference's flags
  (`BGSSaveLoadGame::GetChangeFlags`, CHANGE_FORM_FLAGS) can't say whether our game switched it: the engine records
  it for a reference it only loads (`TESObjectREFR::Load` sets an in-game flag, which records it whether anything
  changed or not), and a fresh game had it on Salem's marker at Gun Run's stage 5, before any script touches it, so
  the first rig run refused every switch-on. What the table can't see is switched on all the same: a marker turned
  off through a variable set at run time, a struct, a collection element reached by index, a conditioned alias or
  one filled from a location that isn't forced, or by the console; one our story never needed to touch (our quest
  took another branch); an NPC a mod only the friend has freed of its parent; a base-game marker that a script of an
  add-on the table wasn't made from turns off. The table names the files it was made from (`COVERED`), and a parent
  from any other official file (the workshop add-ons, Vault-Tec's among them, weren't installed where it was made) is
  never switched on (`kNotCovered`). The
  log line says what was switched on or why nothing, with that count, the top's change flags and its state in its
  plugin; `switchedOn=` counts tries/those through a parent. The switch-on before a friend's kill (WorldSync) goes
  the same way, and `switchedOnToKill=` counts only the ones that switched something on.
- Console commands: each one is compiled and run on the spot, so the copies taken over from their own AI are
  spread out, 8 a frame at most (the rest keep their snapshots and wait a frame), and the commands run for NPCs
  are counted (`CommandMeter`; `net status`: `cmds=` last frame/most, `busy=` frames over 32, `rereg=`,
  `released=`), with a log line for a busy frame at most every 10 s.
- An owner more than 6000 units from an NPC releases it when a friend in the same space is within
  4000 units and at most half as far (not its companion, its conversation partner or an NPC
  fighting the owner), so enemies react to the player standing next to them. The owner doesn't claim
  it back for 10 s (the friend's game claims it) and hands the same NPC off at most every 30 s.
- A friendly NPC goes to a friend much sooner (`FriendlyHandOff.h`, game-free and tested): a copy runs no AI, so a
  friend beside an NPC the other game runs saw it walk but never greet them, turn to them or play its idles. Not
  hostile to the owner or (by the friend's stand-in) the friend, not in combat, not the owner's companion,
  conversation partner, one holding them, a settler they command or one they fought lately; the owner more than
  2000 units off (or in another space), a friend whose game runs within 800 units and at most a third as far. The
  facts must hold for 3 s, and the owner must have run it for 20 s, so a friend walking past or a just-taken NPC
  moves nothing; it comes back only when the owner is within 800 and the friend beyond 2000. Through the same
  release as above. One that comes back to us unclaimed (the friend's game could not take it) is not offered to them
  again for 5 min. `net status`: `friendlyHandOffs=` handed/came back; the log: "handing ... to a player standing
  next to it".
- A game stopped by a menu (`GameStop::Now`: `UI::menuMode` above 0, the one place the plugin asks it) for 5 s
  (`PauseHandOff.h`, game-free and tested) offers to run nothing new, NPCs or frames, and releases the NPCs it runs
  that a friend can take on: a friend in the same space within 6000 units of it, whose own status names no menu
  that stops their game (Pip-Boy, pause menu, container or trade, terminal). Through the same release as above (not
  claimed back for 10 s, not let go again for 30 s); the friend's game claims it as nobody's. Kept: its companion,
  its conversation partner or one holding it, settlers it commands, one its player fought within 10 s (`NpcHold`;
  one dying is no candidate and is kept by the hold too), its spawns and frames. An enemy fighting the stopped
  player is let go once the fight's 10 s are over, so it fights on in the friend's world. After the menu closes
  its game copies them from the friend's (`NpcPlace::Follows`), and a hit takes one back as any takeover does. A
  quick look at the Pip-Boy (under 5 s) changes nothing; a loading screen ends the count. `net status`:
  `pauseHandOffs=`, `pausedFor=` (seconds the current stop has lasted).
- Remote players' stand-ins are in PlayerFaction, so enemies in the owner's world attack them. Hits on a
  stand-in by an NPC are forwarded to that player (`PlayerHit` -> damage to their health), naming the
  NPC. An explosion's hit is not, when the explosion goes off in their game too (one reported, a trap's,
  a death blast: `RemoteEdits::PassStandInHit`, `standInBlowsThere=`): it hurts them there, and forwarded as well it
  hurt them twice. Hits by NPCs made at runtime are sent only for one the attacker's game shares (see Spawns), under its spawn id; the victim ignores hits
  from an NPC that isn't loaded, alive and within 140 m in their world, and any while a menu pauses
  their game (`UI::menuMode`; `Main::freezeTime` stays false). The damage was resisted by the
  stand-in's copy of the victim's armor; the victim scales it by their own difficulty
  (`fDiffMultHPToPC*`, NPC hits only) and VATS (`fVATSPlayerMenuDamageMult` while targeting,
  `fVATSPlayerDamageMult` = 0.1 during playback). Not yet: the victim's perks, legendary armor
  effects, limb damage, radiation and stagger.
- Shots are shared too (see Weapon fire).
- Power armor frames standing in the world are shared the same way (`FrameShare.h`, game-free and tested), so a
  friend's suit stands on their rack in your game too, parked before the session or not. A frame is furniture
  (`TESFurniture` with the engine's power armor keyword), not an actor: the owner's game finds the ones standing in
  its loaded cells by a walk of their references every 2 s, claims them through the same claims (a plugin's frame
  reference is the same in every game; one made at runtime is shared as a spawn instead, see Spawns), keeps one it runs while
  its reference is still there (worn by its player, or in a cell since unloaded), and sends its cell/worldspace,
  position and heading as an `ActorState` once a second and at once when it moved a unit or turned a hundredth
  of a turn. Who runs one: the last player to get into or out of it in this session (a use takes it over as a hit
  takes over an NPC; the frame a player had left last as the session began counts as used), else the host's game,
  whose world is the session's, for any standing in its loaded cells; a joiner's game never offers one it has not
  used. The others move their own reference of the frame to where the owner's game has it, when that spot is in
  the area their engine draws: in place when it is loaded within 256 units, else through the console (`moveto`
  and `setpos`, three tries 10 s apart, `RetryBudget`); never one that is disabled there (their player wears it),
  fills a quest alias of theirs (the museum's suit before Concord is done), or has an actor of theirs in it.
  Nothing is mirrored (a frame has no AI) and its pieces are its own game's (loot keeps frames per player). A
  synced frame is not written off as the receiver's own suit for having been moved, or for their player's earlier
  use of it (`ArmorFrames::OriginalFacts::synced`). `net status`: `frames=` run/standing, `frameStates=`,
  `framesMoved=` in place/through the console, `frameGaveUp=`; the `mirror <ref>` dev command describes one.

## Spawns (src/game/Spawns.cpp, src/game/SpawnBook.h)

NPCs a game makes at runtime (a random encounter's raiders, an ambush, a settlement's attackers) have ids
(0xFF......) that are that game's own: in another game the same id names something else, or nothing. Playing,
the host saw mongrels where the joiner saw ghouls, and the joiner's follower fought an enemy only the joiner's
game had. So each one is shared as a spawn, run by the game that made it for good:

- The owner's NpcSync scan finds one of its runtime NPCs loaded, alive, in combat or hostile to its player (never
  a stand-in, a copy, the player or its companion), or a random encounter's actor calm or not (Encounters, below),
  and reports `kReportSpawn {spawn id, base, cell or worldspace, position, heading, quest, alias, kind}`. The spawn id is the NPC's own id first; one another game's spawn has is
  refused naming that game, and the next one asked for is spread over the runtime range (`CandidateId`). Spawn
  ids keep the 0xFF prefix (0xFE is light plugins' range), so every check of `IsShareableRef` still turns them
  away; only the messages about NPCs take them (`IsActorRef`), and each game looks its own NPC up under one.
- The base is what every game puts together the same: the first of the leveled data's templates that is a whole
  NPC (no template of its own), else the NPC it was placed as (a leveled one is rolled again by each game), else
  its own base when that is a plugin's. Without one it isn't shared. A copy is placed from the NPC the owner's game
  placed it as when that is a leveled NPC in ours too, so our engine rolls it and the owner's roll is then made ours
  (Shared NPC AI: Roll); it waits up to 2 s for the roll (`NpcRoll::IDENTITY_WAIT`), else it is placed from the base,
  and a copy placed before its roll came stays as it was placed (never swapped in front of a player). A spawn's copy keeps
  its own roll (our engine's name, legendary and damage resistance) until its next load, as any loaded copy does (Shared
  NPC AI: Roll): the roll is written for that load, never put together under it, and a spawn rarely lives that long. The copy is put in the owner's copy's
  factions and taken out of the rest (a random encounter's scavengers and a settlement's settlers get theirs from
  their quest at runtime: a copy of one template's NPC had that NPC's own, a friendly settler in one game, a hostile
  scavenger in the other); never our player's or our companion's faction.
- The server grants an id to the first game to ask (`actorOwners` names the owner; `spawns` holds the details),
  sends the others `kSpawned` and everyone the owner. A joiner is sent the spawns before who runs NPCs. Limits:
  32 a player at once, 128 in the session, a budget of 32 and 2 a second; past them the answer names nobody and
  the game asks again 10 s later. Nobody else can claim one; a claim to take one over notes the claimer as
  fighting it, and the claimer's game logs once per spawn that its copy stays the owner's (Shared NPC AI, takeovers).
- The others place a copy (`CreateReferenceAtLocation`, kept out of saves like a stand-in) while their player is
  within 6000 units of where the owner's game has it, in the same space, and the owner's stand-in is within 8000
  of it; 2 a frame at most, 3 tries. NpcSync mirrors it under the spawn id as any NPC another game runs: states,
  gear, shots and health. None while a runtime NPC of our own stands within 128 units of where their game has it
  (`SpawnBook::NPC_SAME_SPOT`, `Facts::oursThere`: alive, no copy or stand-in, not our companion, of the spawn's base
  or hostile to our player): two players crossing one random encounter's trigger each rolled the encounter in their
  own game at the same markers, and each game shared its NPCs, so every raider stood twice in each game, its own and
  the copy of the friend's, on top of each other (a Nexus report on 0.10.0). Ours stands for theirs for the spawn's
  life (`Spawns::OwnMadeNpcAt`, looked for as a copy is about to be placed and once a second around a placed copy,
  which goes when one of ours appears beside it: our game rolled the encounter after the friend's), so each game
  fights its own, as before spawns were shared; the friend's hits on theirs reach nobody here, as no copy is loaded.
  `net status`: `npcOursThere=` withheld/stood down; the no-copy log names the reason.
- A copy goes when its player leaves it 9000 units behind (placed again on coming back), when its cell unloads,
  before every save and load, and when the spawn is over while it is alive. A dead copy stays as a corpse until
  its player leaves it.
- Either game shares: a joiner's random encounter reaches the host as the host's reaches the joiner (nothing in the
  scan, the base pick or the placing looks at the race, the keywords or who hosts). When a friend does not see one,
  the log says why: our side logs a hostile or fighting runtime NPC it cannot report (`Spawns: ... is hostile to our
  player or fighting, but not shared`, with its race, its base and the templates the pick found none of) and counts it
  (`net status`, `spawns: ... notShared=`); the other side logs a spawn it holds no copy of for a lasting reason (`has no
  copy here`: its base is a unique NPC, placing failed, its owner's stand-in not heard or far) and counts them
  (`unplaced=base/far/owner`).
- A plugin's NPC our game has no reference of goes the same way (`Spawns::Absent`, `SpawnBook::AbsentBase`): a reference
  its plugin stores as temporary is in a game only while its cell is loaded there, and a quest can take one far from its
  cell. The random encounter RETravelKMK04 moves its Mr Gutsy (REMisterGutsy01REF, 0002F61D, a temporary reference in
  the interior REHoldingCell) onto the road beside its player; that player's game claims and runs it as any plugin NPC,
  but a friend's game beside them has no such reference (its holding cell is not loaded there), so every state of it
  came to nothing: no Mr Gutsy and no fight (a Nexus report on 0.9.2). Now a state of a plugin NPC our game lacks books
  it as a friend's spawn under the reference's own id, placed from the NPC the session's roll of it was put together
  from (the roll's first whole template, else the NPC it was placed as; one that is not leveled has no roll and gets
  no copy, said once in the log), placed, mirrored, hurt and killed as a spawn's copy is (NpcSync's ids for it name our
  copy). Our hits on it ask no takeover (the server would hand us an NPC our game cannot run); they reach it as damage.
  The copy goes when nobody runs it or another game takes it (that game's states book it afresh), and when our game
  comes to have the reference (its cell loaded here: ours is mirrored as any). `net status`: `absent=` booked/no base.
- It is over when it dies, when its owner's game lets it go (unloaded) and when its owner leaves: the server
  forgets it and names nobody as its owner, which ends it in every game. Its death is believed from its owner
  and from a player fighting their copy of it (health reported, or a takeover claim, within 10 s), never kept
  with the session's dead, and killed in the owner's game too. Its hits on a stand-in count only from its
  owner's game.
- Power armor frames a game made at runtime (0xFF......, placed by a script, a quest or an encounter rather than a plugin)
  are shared the same way (`FrameShare::SharesMade`, `SpawnBook::Theirs::DecideFrame`): the owner's frame walk (NpcSync,
  every 2 s) finds its player's own (got into or out of in the session, or left last as it began) wherever its world
  still has them, and, in the host's game only, any standing loaded (a joiner's world makes its own frames, and its
  stranger's would stand beside the host's own), and reports each as a spawn whose base is the frame's furniture; its state
  goes as a pre-placed frame's does (once a second, at once when it moved). Never one somebody is in (the engine disables
  a worn frame; the wearer's stand-in shows it), nor a copy the session placed (a friend's stand-in's, parked or shared
  frame). One that stops being shared (worn, gone, a stranger's left in a cell since unloaded) is let go, which ends
  it everywhere; it is reported again, under the same id once the book's 10 s wait is over, where it stands next. At
  most 12 frames of a game's 32 spawns, its player's own first. The others place an empty copy of the frame (as the copy a
  stand-in gets into: each game's pieces are its own loot) wherever its owner is, while the spot is in the area their
  engine draws, as a friend's parked frame is: moved in place when the owner's game has it a unit or a hundredth of a
  turn off, placed again when it is 256 units off or in another place, gone out of sight, before saves and loads, and
  when it is over. A copy somebody there gets into is theirs from then on (neither moved nor deleted). None while a frame
  of our own of that base stands within 32 units of where the owner's game has theirs (`FrameFacts::oursThere`: two
  saves with a shared past hold the same frames), and one placed goes then. A joiner is sent
  the frames shared before it came, so a friend's suit parked before the session stands there too. Never a copy an older
  version's workshop sync left in the save (`FrameShare::Ghost`: the keyword the workshop copies carry; it is cleared
  instead, see Party's power armor). `net status`:
  `framesMade=` shared/standing/our player's own, `ghostsCleared=` (npcs), `frames: ours= reported= placed= moved= taken=
  oursThere=` (spawns).
- A friend may borrow a frame another game made and parked (`FrameLoans.cpp`; the rules in `FrameLoan.h`, game-free and
  tested; the server's side in `Server.cpp`). Their press of E on its copy asks the owner's game for it (`kBorrowFrame`;
  the server keeps one loan per frame and per borrower and relays the ask as `kLoanAsked`), which says yes while its
  `bLendPowerArmor` is on and the frame stands there, loaded, with nobody in it, and sends its pieces and one fusion core
  (`kLendFrame`, at most 16, each with its mods, condition and name). The borrower's game puts copies of them, each named
  as the owner's, into its copy (held as it is while lent: `SpawnBook::FrameFacts::lent`), and the next press gets
  their player in. While they wear it their status names it by its spawn id and every game hides its own reference
  of it (the frame hide); the owner's game neither shares it nor tells it parked. While lent, the owner's frame is never
  cleared as an older version's copy (`FrameShare::Ghost`) and stands for no friend's frame at its spot
  (`FrameShare::StandsFor`), and the borrower's copy is kept whatever of their own stands where the owner's game has it
  (no `oursThere` while lent). As they get out (2 s settled, and out of power armor) the copies are gathered from the
  frame and the player and removed (one whose name did not hold is taken from the frame by its base, never a core of the
  player's own; each not found by its name is logged with what the frame and the player hold of its base), what else of the player's own the
  frame holds goes back to them (their cores, as many as they lost to it; their own pieces, put on from the Pip-Boy),
  their copy goes (found by its handle or, the engine having made it anew with its cell, by its id; one not found is put
  back under the save guard by its id and forgotten), and `kReturnFrame` says where it stands and each piece's condition:
  the owner's game wears its pieces down to that (never up), keeps a piece the borrower's game did not find as it is
  (a "not found" is only a find that failed, never word that the borrower kept it: taken out on it, every worn kit was
  lost at its return), and moves its frame there once the spot is in sight (`NpcSync::MoveOwnFrame`; the server keeps the
  spot within 1024 units of the borrower's last state, or it is that state). What came back waits for as long as the
  frame has no inventory here or the owner's player is in it. Every change to an inventory goes through `OwnGear`
  (`kLoan`), and Drops never gives a lent copy away. The core is the vanilla fusion core by its id, never other ammunition
  stored in the frame. A frame not got into within 60 s, or before a save, goes back unworn; an offer the server never
  confirms is the owner's again after 15 s, and a confirmation that comes later is taken up all the same (the kit as the
  server kept it, each piece found again by base and condition). A loan ends without a return when
  either player leaves, the spawn ends, or the borrower loads a save (`kFrameReturned` with `ended`; an ask not yet
  answered ends the same way, so the owner's game forgets it): the frame stays where it stood, and a borrower still in
  it keeps it until they get out: the copy is theirs while they wear it (all the way in, only the engine's worn frame
  names it, `FriendFrame::Ours`), never buried from under them as its owner's game shares it again or the session ends,
  and it goes with the copies once they are out. A save made in a borrowed frame keeps it and notes it in the co-save's `LOAN` record;
  loaded again, the player is sent out (again until they are), the copies go, and their own pieces and cores in the
  frame go back to them. Since the owner keeps every piece, a lent copy the borrower of a worn loan had put somewhere (a
  chest, a workbench, a vendor, the floor), on neither them nor the frame as the loan ended (by name, or from the frame
  by base and mods), is forfeit, one forfeit for each copy left behind, whether the loan ended with a return, without
  one, or with the clean-up after loading a save made in the frame (`FrameLoan::MAX_FORFEITS`: at most 64, the oldest
  dropped, kept with the save in the `LOAN` record, version 2; version 1 records are read with none). Drops and Gifts
  refuse it as any lent copy, and once the borrower has it on them again (taken back out, bought back) one item of its
  base and very name is removed for each forfeit, which is then spent (the same frame lent again names its copies the
  same, so never more of a name is taken than copies of it were left behind), at most once a second, only with no loan
  under way, the player alive, seated in nothing and out of power armor for 2 s, no inventory menu or loading screen
  up, and one log line for each piece removed. The by-base taking at a loan's end reads the frame alone and the forfeit
  sweep the player alone, by name, so the two never act on one piece. A reload on either side after a return is not
  re-settled: the owner loading a save made before the return has their frame as it was lent (any wear the return
  brought is undone), and the borrower's forfeits are those of the save they load. Both are the ordinary "an old save
  undoes a trade" of any move of items between worlds; Drops and Gifts re-settle from a record of each world's, and a
  loan keeps none on the owner's side. Not yet: a loan does not carry over a load, a piece no other game can name (one
  with a runtime mod) is not lent, and a forfeited piece left in a container for good, sold and not bought back, left on
  the floor, or put on a frame of the borrower's own is not caught (only what the borrower carries is looked at), nor
  one whose name did not hold (the sweep knows it by its name).
- *A quest alias's creatures (src/game/QuestActors.h, protocol 61).* A quest whose alias creates its creature at runtime
  creates one in every game that runs the quest: Gun Run (DN083_Barney) creates Salem's three mirelurks (its aliases 26,
  38 and 41, of the unique DN083_StartMirelurk). Each game's is its own runtime NPC, so a friend's was shared as a spawn
  of an NPC our game never made: no copy (its base is unique), and ours stood beside it idle, never hurt by what the
  friend shot, its quest never told of its death (stage 10 waits for all three). Now the game that shares one first
  (hostile or fighting, as any spawn: stage 5 takes them out of their intro faction) names its quest and alias with it,
  and a friend's game pairs its own creature from that alias with the spawn instead of placing a copy: `Theirs::Placed`
  books it as our copy, so NpcSync mirrors it from its owner's states, the owner's roll and factions are made its, a hit
  on it is a copy's (answered by the owner), and its death there kills ours (our alias's OnDeath counts it, so stage 10
  is reached in each game). It is never deleted, never tracked as a placed copy and never kept out of a save: before a
  save or a load, at the session's end, when our player leaves it past KEEP_RANGE, when it unloads, and when the
  owner's game lets a living one go (they left), it is only unpaired (its mirror dropped, its AI its own again), and
  ours then shares it itself if it is hostile. Unpairing also puts back the roll, level and factions NpcSync made ours
  from the owner's (read as ours was paired, made so again with Rolls::Apply: `restored=`), so a save keeps our game's
  own. Paired only while the spawn is alive, our player within PLACE_RANGE of it, ours alive, loaded and within
  PAIR_RANGE (4096) of theirs, and nobody's yet (granted to us, a puppet or a copy); while ours is in the alias but not
  to be paired, no copy is placed beside it. While our player hasn't started the quest, a friend's creature gets no copy
  here (its base is unique: BaseOk), as before. Our game does not report a creature whose alias a friend's live spawn
  names (`twinsHeld=`), and the server refuses such a report from another game (naming nobody, logged). Guarded so no
  story is touched: only a quest this session shares (`QuestSync::SharesHere`: a side quest, or with the host's
  `sQuests = safe` a SAFE or SILENT one: its stages go between the games, so its creatures' deaths must too; Gun Run
  was the first, the SAFE misc quests and SILENT's objective-less ones such as DN119Fight and DN045 follow), and of
  those only one that is running here and runs once or starts with the game (kRunOnce or kStartsEnabled: never started
  twice, as a second run's alias would be the first's on the wire), is of type none, misc or side (a story's or a
  faction's creatures stay each game's own even with a shared story), no radiant and not PER_GAME;
  only an alias filled by "create reference"; and only a creature (its race lacks "allow PC dialogue": set on
  HumanRace, GhoulRace and DogmeatRace, clear on MirelurkRace), so Gun Run's Barney (alias 25), whose scenes are each
  game's, stays each game's own. A vertibird (VertibirdRace, also without "allow PC dialogue") is never paired either:
  the player sits in it, and pairing would mirror it to the owner's bird while its own flight scene still moves it, so
  a seated player could be dropped. Gun Run's stages are shared only with the host's `sQuests = safe` (DN083_Barney is
  in QuestFilter SAFE), so with `sQuests = story` its mirelurks are each game's own again. `net status`: spawns questActors: paired= pairs= unpairs= keyed= twinsHeld= restored=.
- *Random encounters shared calm (src/game/Encounters.h, protocol 63).* The game that rolled a random encounter owns it,
  and its actors are shared as spawns from the moment they stand, calm or not: a man held at gunpoint and called a
  synth (RESceneLC01, whose accusor only aims), travellers, a trader, a settler asking for help. Up to 0.13.2 a
  runtime NPC was shared only once in combat or hostile to its player, so a calm scene stood in one game alone (a
  Nexus report on 0.13.2). An encounter is a misc quest (type 6) that a story manager event starts (`TESQuest::eventID`,
  the record's ENAM, a word of four capitals such as the triggers' `SCPT`; a quest no event starts holds no such word
  there, whatever the game puts in its place: `Encounters::IsStoryEvent`; and a story manager quest node lists it,
  `BGSStoryManagerQuestNode::children`, read once: `Spawns::StoryStarts`) and whose editor id, after an add-on's `DLCnn`/`DLCnn_` prefix, is `RE`
  and a capital or an underscore (`RESceneLC01`, `RETravelKMK01`, `DLC03RESceneKMK01`; not `ReconScopeQuest`, nor
  `REParent` and the checkpoint bookkeeping, which are of other types, nor `REFactionShared` and the treasure hunts'
  reward quests, misc but started by no event): Fallout4.esm has 236 RE-named quests, 207 of them encounters, every
  actor of theirs created by an alias of the quest as it starts (780 created NPC bases, 13 of them unique), so one of
  our runtime NPCs whose alias array holds a "create reference" alias of a running encounter quest is ours to share
  (`Spawns::EncounterOf`; an alias that found an NPC of ours rather than making it, a settler, names nothing).
  The spawn names the encounter quest and the alias with its kind (`Protocol::SpawnKind::kEncounter`), which the
  server tells from a paired quest actor's (`kQuestActor`, protocol 61): an encounter quest is repeatable and every
  game's run of it is its own, so two games running `RETravelKMK01` at two ends of the map both share theirs, and
  the twin refusal is for quest actors alone. A friend's encounter spawn gets a copy as any spawn does, placed,
  mirrored (position, heading, gait, weapon drawn and aim pitch, gear, health), shot and killed with it (the accused
  killed by the quest's script dies in the owner's game, the death is the session's); its base may be a unique NPC
  (the named characters encounters create exist here only through our own run of that encounter), and it is never
  paired as a quest actor. Its lines travel too: Conversations reports a line of an NPC our game shares as a spawn
  under its spawn id (`Line::speaker`, `IsActorRef`), and the hearers resolve it to their copy (`Spawns::LocalOf`)
  and play the recording there as for any NPC a friend runs; the recording is found by the line's info id and its
  plugin even when the engine has no form for the info here (a quest's dialogue is in memory as that quest needs it:
  RESceneLC01's accusor came as text alone on the rig, his files in the archives all the while); a line that comes
  before the copy of its speaker stands is held up to 2 s for it; a creature's line (a race the player can't talk to)
  is shared only when the speaker's own game has its recording (`LineShare`, game-free and tested): a growl names a
  sound descriptor and no voice file, an eyebot's broadcast or Liberty Prime's line is recorded and goes. Each game rolls encounters for its own player (the
  `RETrigger*` references send a story event as the player enters them; `REScript` stops the quest itself on
  `REParent`'s cleanup once the player is far, which deletes its created actors), so two players crossing one trigger
  together usually roll two different encounters at the same markers, sometimes the same one. So where a friend's
  encounter already runs, ours is stopped (`stopquest`, the quest's own stop, as RadiantSync stops a radiant), its
  actors are removed (`Spawns::RemoveActor`: every loaded one the quest's aliases name, alive or dead, disabled and
  deleted as our copies go and never offered; the quest's own stop deletes them only as `REScript`'s cleanup runs with
  the player far, and stopped at the spot it left a travelling group's dogs alive and a corpse standing, which were
  then shared as a fight's spawns beside the encounter that had won) and our copies of theirs are placed: one
  encounter per spot (`Encounters::Book`, game-free and tested). Who was first
  is the server's order: every spawn message that arrives (a friend's `kSpawned`, our own grant in `kActorOwners`)
  is numbered as it comes, the same order in every game since the server sends them in the order it took them. A
  friend's live encounter spawn whose spot (where it stood as it was shared) is within `COLLIDE_RANGE` (3000 units) of
  where one of our run's actors first stood, of a run that started within `STOP_WINDOW` (20 s, from its first actor
  seen in the scan), whose number is below our run's first grant (or our run has none yet), means theirs was first:
  ours is booked for a stop as its actor is seen, before it is offered, and stopped from Spawns' frame, outside the
  actor scan; both games decide alike, so exactly one stops. An encounter's spot is its markers, not where its actors
  walk to: a travelling merchant two cells on still holds the trigger he was rolled at. Never while our player is in a
  conversation or a scene, nor before the world's runs are settled (`Encounters::Holds`, from `Spawns::PlayerInTalk`:
  a stop removes every actor of the run, the speaker among them, whichever the scan is looking at; ours is judged
  again once they are done, while the window lasts). Past the window ours stays (its player is in it), `OwnMadeNpcAt`
  keeps covering the same-encounter case, and the encounter quests already running on the session's first in-world
  frame, or the first after a load, are settled, never stopped (`Spawns::SettleRunning` lists them from the data
  handler, `Book::Settle`; every run seen after is fresh): they began before the book could know when, and a
  long-running encounter is not stopped partway for a friend's that came to stand beside it. A run is kept until its
  quest stops running here (checked once a second, `OursOver`), not for a time, and its actors still standing and
  shared are then shared on plain: reported again under their ids with no quest, which the server takes and sends
  again to the others (`kSpawned`), whose copies stay as they are (`Theirs::Rekind`); so no game counts an encounter
  as running at their spot (a radstag of an ended encounter once made a friend's game stop five new runs near it). A
  stopped quest is left alone for `STOPPED_FOR` (60 s) while the engine winds it down; its actors seen meanwhile are
  not offered, and removed while the quest does not run (running again, it is a fresh roll, whose actors are its
  own). A receiver takes a friend's word for the kind only when the quest named is an encounter in its own files
  (`kindsVetoed=`; else the spawn is plain here: the twin rule and the unique bases as for any), and a named character's
  copy is withheld while our own game runs that encounter. Calm actors take at most `MAX_CALM` (16) of our 32 spawns, so
  a fight's raiders are never refused for a crowd of bystanders (`calmHeld=`; one that turns hostile or joins a fight
  goes as any fight's spawn); our companion is never one. The server relays a spawn's line from the game that shares it
  alone. `net status`: `encounters: runs= shared= calm= calmHeld= received= stopped= removed= over= sharedOn= theirsOver=
  kindsVetoed=`.
- Not yet (random encounters): two encounters both already running as the world came up (a load, a session's start)
  are both settled and run side by side until one ends on its own, their actors shared both ways; idles are not mirrored (a copy's AI is off, so hands up, cowering, sitting and the
  pointed gun's pose are not played; `AIProcess::PlayIdle` would, given a hook on the owner's `SetupSpecialIdle`);
  only the owner can take part in the encounter's dialogue (the friend sees and hears it; talking to a copy gives
  the NPC's plain greeting); XP and karma are the owner's. Diamond City's accusation (FFDiamondCity12: Kyle and
  Riley are placed unique NPCs, two guards created, started by MQ104's stage fragment in every game with a shared
  story, so its scenes run in each game over that game's own copies, killed by script at its 25 and 30) is another
  mechanism, a scene of placed actors a shared stage starts everywhere, and is not covered here.
- Not yet (a quest alias's creatures): a death while the two aren't paired is not carried. If the owner kills theirs
  while ours is unloaded or past KEEP_RANGE, ours lives on, is shared back once hostile, and the owner's game (its own
  dead) places a live copy of it: that fight is fought again. Paired, ours takes the owner's roll and factions until it
  is unpaired; a save made while paired is made after the unpairing (DiscardCopies), so it holds ours.
- Not yet: copies are not shielded from a friend's explosion (BlastShield) or watched by CopyWatch, a copy
  carries what its own game gives it until the owner's gear report dresses it, a roll's new abilities (its spells
  slot) may take effect only once the copy loads again, and the factions an alias of the owner's quest gives are not
  read. Each game's corpse of a
  spawn carries its own loot: each player loots their own.

## Quest terminals (src/game/TerminalRules.h, TerminalSync.cpp)

- Some quests keep what their terminals set in their script's properties, not in a stage: at Easy City Downs (DN035,
  Fallout4.esm) the race's terminals call DN035QuestScript's SetRaceState, SetRaceTargeting, SetRaceCombatInhibitors,
  SetRaceReserves and RaceSelfDestruct, each with the player's choice and True (the player directed it); each sets its
  property and, a quarter second on, UpdateRobots sets the race's factions (the robots allied with or set against the
  player, the raiders and each other), wakes the reserves, and the self-destruct (stage 35) counts down and blows the
  robots up. The robots are the plugin's, mirrored across games, but each game's factions said whom they fought: set on
  the player at one player's terminal, they stood idle in the friend's game. TerminalRules::ROWS lists those five
  properties; Protocol::TERMINAL_VALUES has each one's values and whether it is set once.
- Reading: four times a second, while the quest runs here, TerminalSync reads each row's property off the quest's bound
  script (no call into the VM). A change while our player has a terminal open (or for 2 s after it closed: the fragment's
  call lands a moment later) is ours: sent as kReportTerminalChoice. A first reading (after the quest starts or a load)
  is a baseline, and what no terminal set (a stage, the self-destruct's countdown) is never sent; a row that happens once
  (the reserves, the self-destruct) is sent as 1 when it goes from 0.
- The server checks a choice's value against its row's (DecodeTerminalChoice), passes it on to the others as its
  player's (kTerminalChoice) and keeps each row's latest with its maker and time. A joiner is told, after the welcome,
  each row's latest that is not set once (the reserves and the self-destruct are live only), is at most 10 minutes old
  (CHOICE_LIFE) and was made by a player still in the session, as that player's, with its age
  (`TerminalChoice::ageSeconds`), so the joiner's wait for it ends when the server's does. Not saved: each game's save keeps
  what its own terminals were set to.
- Applying: a friend's choice is made here by calling the row's setter with the value and True, as the terminal does
  (its announcement plays here too), only while the race is on here: the quest has run here 5 s, its stage 10 is done
  and neither 35 (the robots blew up) nor 255 (its end) is, and our player is within 8192 units of the friend who made
  it (the same cell or worldspace: at the Downs with them). Stage 10 is a start-up stage (INDX flag 0x02 in
  Fallout4.esm), run as the quest starts whether a race is on or not, so the rows set once (the reserves, the
  self-destruct) also wait for RaceState to be 0 here: 0 from the quest's start and after Start Race, 1 after Stop Race,
  so they are not made into a game whose race a player stopped. Our player must be at no terminal, and ours must differ
  (a once row only if it has not happened here). A choice that waits keeps the latest of its row and is dropped after 10
  minutes, or once the race is over here (`dropped=`). So a game far away, a later first visit, or an old save whose
  race never ran gets nothing, and the self-destruct's stage 35 is set only in a game at that race. What a friend's
  choice set here is not sent back: for 3 s (EXPECT_LIFE) a reading of that value is its call landing; past that, a call
  the script refused no longer keeps our own choice of that value from being sent. After a load, a friend's latest of a
  row (never our own: the loaded save's are its own, and never a once row) is made again under the same rules. Only with
  `bSyncQuests`. `net status`: terminals: sent= heard= made= failed= dropped=.
- Not yet (quest terminals): only DN035's terminals; another quest whose terminals set its script's state needs its
  rows. The route terminals (SetRaceRoute) have no row. Two players at terminals at once, setting the same row to
  different values, each get the other's once they step away, so the two games end up swapped; the server stamps no
  order.

## Weapon fire (src/game/WeaponFire.cpp)

- Detection: every actor gets its animation graph's events through its
  `BSTEventSink<BSAnimationGraphEvent>` base at +0x38; the graph sends "WeaponFire" once per shot
  and "weaponSwing" at the hit frame of a melee swing or punch. Both count as an attack. A gun's bash
  sends "weaponSwing" too; with a gun in hand it is not reported (`AttackKind::ReportsAsShot`): a
  receiver replays any attack of a friend holding a gun as a shot, so each bash sounded as a gunshot.
  We hook slot 1 of that vtable twice: the player's class (our shots) and the NPC class (shots by
  NPCs we run; stand-ins and mirrored NPCs use the same class and are filtered out).
- `ReportShot {refId, sound}` (0 = the sender) goes to the server, which accepts NPC shots only from
  the NPC's owner and relays `ShotFired {playerId, refId, sound}` unreliably (60 shots/s per player).
- Replay: "attackStart" on the stand-in's or mirrored NPC's graph plays the full firing animation
  (recoil, aim pose). Nothing may be launched: damage already arrives through health sync, and real
  projectiles from stand-ins would add friendly fire and kills nobody made. The animation's own
  "WeaponFire" did launch one from a stand-in, though (seen on the rig: its round hit our player on
  every replayed shot, whichever way it faced, so each of a friend's shots showed the other player a
  hit's flash, blood and damage marker). A stand-in's "WeaponFire" within 1.5 s of a replayed shot is
  kept from the engine (`AttackKind::HoldsOwnFire`, `firesHeld=` in the status), as a replayed
  throw's release is. The engine then draws no flash of its own for it either, so a stand-in's
  replayed shot gets MuzzleFlash's. A shot that would go off (a missile, a shell, an explosive round) is the
  exception, on a stand-in and on a copy alike: it leaves the gun, launched inert (`AttackKind::ShotReplay`,
  `InertShots`, `firesInert=`; the stand-in's gun is given a few rounds first, `ammoGiven=`), so friends see it fly;
  so does a stand-in's plain bullet or beam while `bFriendShotTrails` is on (default; dev `trails on|off`): launched from an
  inert copy of its record, with no damage of its own, aimed where its friend aimed (`InertShot::Swaps`, `Aims`; `trails=`),
  launched so friends can see a tracer or a laser's beam fly (a stand-in's laser beam was seen drawn on the rig, as bright as our own; `Projectile::Launch` sets a projectile's tracer flag, 0x10, on every Nth launch of an actor's by the gun's tracer frequency and the shooter's process counter, as for our player and an NPC, and a beam without it gets no 3D: the laser is 1 in 1, the minigun 1 in 4, the 10mm 0, which draws no tracer for anyone; `trailTracers=` counts the trails made tracers, the rig step asks every laser trail to be one; the launch data's own tracer only forces one, dev `trails tracer always`; dev `launchlog` writes out a launch and its projectile); a copy's plain shot stays the engine's, and with the key off or no inert
  launch it is held as before (`trailsHeld=`). So is one from a gun with any enchantment (a legendary's Wounding, Freezing, Poisoner's: should an inert trail ever register a hit, its bleed or poison would land on top of the friend's reported damage) and one of a hitscan record (flag 0x1: its hit may be settled inside `Launch`, before the made projectile's damage is zeroed; none of the game's own guns has it) (`AttackKind::ShotReplay`, `IsPlainTrail`; `trailsHeldGun=`, the first logged). On the rig none of its impacts on our player was counted as a hit and none changed its health, but a zero-damage impact could still show blood or a hit marker on
  whoever it lands on (`puppetHitsOnUs=` counts those on our player, `puppetHits=` a stand-in's hits on any actor, our player, a settler, a companion or an NPC; the rig's trails step asks both to stay 0, aimed at the watcher too; none is relayed).
  `trails off` restores the replay's decision exactly (held, the hold armed, no copy made), but not the hook: a stray `WeaponFire` of a stand-in's plain bullet or beam that the hold misses (more than 1.5 s after the replay) is still swapped inert by `Projectile::Launch`'s hook (`InertShot::Swaps` doesn't read the toggle), its copy made there off the main thread, where 0.13.1 fired the engine's record owner-aimed. That is the safer of the two (no damage of its own either way), so it is left so;
  "Who sets off an explosion" below. A stand-in's graph can refuse "attackStart" while it says an animation drives
  it (`bAnimationDriven`) with the stand-in in no furniture and no frame (seen on the rig right after a join: every
  replayed shot sounded and flashed, none animated, nothing left the gun, for minutes); nothing of a stand-in's is
  animation-driven there (we place it), so on such a refusal the flag is cleared and the replay asked once more
  (`animDrivenCleared=`).
- A stand-in drawn before its weapon is equipped stays empty-handed with the "drawn" flag set and
  rejects attacks, so after every equipment change it holsters and draws again.
- Gunshot: the weapon's own attack sound fields are empty for vanilla guns. The game picks the
  sound when the weapon is equipped (by its keywords, which its mods add: a suppressor or an
  automatic receiver changes it) and keeps it in the equipped item's
  `EquippedWeaponData::attackSoundData`, a keyword sound mapping. The shooter's game sends that
  mapping's form ID with every shot (`sound`): a stand-in's copy of the gun is a new one of the same
  base weapon without the shooter's mods, so the mapping its own game picks can be another
  receiver's (an automatic one's looping fire for a semi-automatic pistol) or miss the suppressor.
  A replayed shot plays the shooter's mapping's descriptor, or the stand-in's own when the shooter
  sent none we know, through `BSAudioManager` as a 3D sound following the shooter. The status line's
  `theirs=` counts shots sounded with the shooter's mapping, `kssm=` is the last one played (with
  `(loop)` for a looping, automatic sound); `weapsound <actor>` shows a gun's mapping in either game.
  (Papyrus `Sound.Play` crashed the VM when called from here.)
- Muzzle flash (src/game/MuzzleFlash.cpp): the engine shows it by itself. At the first replayed shot it
  makes its own flash object on the stand-in's `EquippedWeaponData` (`muzzleFlash`: a clone of the projectile's
  muzzle flash model under the gun's ProjectileNode, and a point light) and shows it for a few frames whenever
  the graph takes "attackStart", like a real shot; no projectile is needed (checked on the rig for pistols,
  shotguns, the laser rifle, the SMG and the minigun, both ways). `muzzle watch <actor>` samples that object
  every frame. A replay the graph refuses gets no animation and so no flash: for it the flash model is applied
  at the muzzle as a temporary art effect (`ApplyArtObject` with a runtime art object of form ID 0, so a save
  that catches one loads it as nothing), forced to full fade every frame (the clone nested in the actor's 3D
  stays at fade 0 otherwise) and ended at the projectile's flash time. MuzzleFlashBook keeps one flash per
  shooter (an automatic gun's next shot keeps it up), 50 ms between two, and 12 at once. `muzzle mode always`
  shows it on every shot, to try it; the status line's `flash=` counts both, and `takenDown=` the flashes of ours taken down.

### Who sets off an explosion (WeaponFire.cpp, RemoteEdits.h, CopyWatch.cpp)

One rule for every explosion: **it goes off once in every game, and each game deals its blows on its own player and
the NPCs it runs.** A blow on a copy (an NPC another game runs) is given back, since the NPC's own game deals it. A
stand-in's or a copy's shot that may go off is launched with no blast of its own (it flies, and goes off nowhere), and
a stand-in never throws a grenade: what its player set off comes as their game's report. An explosion is decided by who
set it off first (the explosion's owner), then by
its base: a trap or a destructible object claims only its own. An explosion a game can't be sure the others get is not
shared at all. That game deals every blow itself, a copy's too, reported as its hit's damage, like a gunshot's.

| Kind | Goes off in | Reported as `kExplosion` | Its blow on a copy |
|---|---|---|---|
| Thrown grenade or Molotov (our player's, or an NPC our game runs, our companion among them, one made here at runtime too) | the thrower's game; once in each other game, from the report. A replayed throw launches nothing (`HoldsReplayedRelease`) | yes, when `ReportGate` passes it; else ours alone | the NPC's own game's when reported, else the thrower's. A Molotov's fire is the NPC's game's for 20 s after its thrower's watch, if every blast of that thrower's watch was reported (`ThrowWatches`: each thrower's own) |
| Missile, mini nuke, grenade launcher's shell, cannonball, explosive bullet, a beam with a blast, a legendary Explosive round (`ShotGoesOff`: the projectile names an explosion or has the explosion flag, or one of the gun's enchantments has an effect that does, which none of the game's legendaries has; the test the shooter's game watches by) | the shooter's game; once in each other game, from the report. A stand-in's or a mirrored NPC's shot flies but goes off nowhere: it is launched from an inert copy of its projectile record (`InertShots`, `AttackKind::ShotReplay`, `firesInert=`, `inert=` launches swapped). The engine makes every projectile in `Projectile::Launch` (Address Library 2236958, +E421E0), whose entry is hooked: a stand-in's or a copy's launch of a record that goes off gets a runtime copy of the record (a runtime form ID, in no save; never freed, one per record, at most 128 of them, the last 32 kept for records that go off so friends' calibers and laser variants never crowd out a missile's: the same motion, speed, model, light and sounds; no explosion, no proximity or timer for one, no force, nothing to pin, pick up or switch off, `InertShot::InertFlags`) before the projectile class's constructor reads it, so nothing of the projectile exists that could go off, a point-blank missile and a beam included; the projectile's own explosion and damage are cleared as well once it is made. Its blast comes as the shooter's report, at the reported spot; the inert shot ends where it hits, with its impact effect and no blast. Its base is a form of this run alone, and the game saves projectile references, so every inert shot is kept a handle to and ended (disabled, deleted, its record dropped) before every save, when it is older than any flight (12 s: a lobbed shell at rest with no timer never ends on its own) and when its cell is no longer attached (`inertLive=`, `inertEnded=`; `InertShot::Stale`); before a load each is handed to the load instead (`SaveGuard::HandOver`, as a stand-in is: its record given back so the load frees it, or let go of now when it takes none; ended, it would stay in memory through the load, which frees only what has a record; `inertHanded=`). A 65th in flight at once is not launched (as the hold), so none goes untracked. A relayed explosion placed from a record that spawns projectiles (a MIRV pack's eight children, a Sentry Bot's shoulder cluster) spawns them here with no shooter: for 2 s after it is placed an unowned launch of that spawn record is swapped too (`inertSpawned=`; `InertShot::SwapsSpawned`), as the shooter's game reports the children's blasts; it is told before the explosion is placed, as the engine launches them inside the placing (on the rig a MIRV pack's five children went off twice before). The engine aims an NPC's launch at whoever it looks at (a stand-in's missile went straight at our player, whichever way its friend aimed), so a swapped launch is given its angles from the puppet's own heading and its owner's pitch (`Puppets::AimOf`, `InertShot::LaunchAngles`): it flies where the friend fired. Every other launch of a stand-in's (a flamer's or a cryolator's stream, a harpoon, a railway spike: the engine's own record, fired as before) is given the same aim, with no homing target or auto-aim left, so none goes at our player (`InertShot::Aims`, `standInsAimed=`). Where the entry isn't as 1.11.240 has it (another plugin's hook), nothing is hooked and such shots are held, as before (`firesHeld=`). One that would go off by an enchantment alone is held (there is no record to take a blast from). A stand-in's plain bullet or beam is launched the same way (`bFriendShotTrails`, `trails=`; damage zeroed, owner-aimed, so friends see the tracer or the laser; held when the key is off), where it used to be held (it hit our player); a copy's is the engine's, as before | yes, by its owner (`Explosion::GetActorOwner`: our player or an NPC of ours, seen by the 8 s watch a shot that may go off arms, `shotsWatched=`), when `ReportGate` passes it | as a throw's |
| Trap (a mine, a grenade bouquet, a tripwire's grenades) | every game whose copy of the trap is armed sets its own off (`kReportTrap`). Only one with no owner near the trap is the trap's: our grenade beside a mine is ours | never | the NPC's own game's (`Fate::kEachOwn`) |
| Car or barrel (a destructible object's own stage, by its explosion's base) | every game passes the stage once, with its blast (`kDestructions` with `explode`) | never | the NPC's own game's (`Fate::kEachOwn`) |
| An NPC's own, one another game runs | that NPC's game; once in each other game, from that game's report, wherever a player could be hit. Our copy's throw and explosive shot are held | by the NPC's own game only | the NPC's own game's (our copy's hits are nobody's) |
| A robot's or a turret's death blast, told by its explosion's base alone (`RemoteEdits::DEATH_BLASTS`, from the game data: the self-destruct its race's script sets off, a Sentry Bot's, an Assaultron's, a Robobrain's, a Mr. Handy's or Gutsy's, a Protectron's, an Eyebot's, a turret's, a Vertibird's, a Fog Crawler's, and the limbs its body part data blows apart, with no damage of their own) | every game's copy sets its own off as it dies (the NPC's game's death report kills the copies; on the rig a Sentry Bot's whole cascade went off in both games, the copy's a moment later) | never (`deathBlasts=`) | the NPC's own game's (`Fate::kEachOwn`) |
| A suicider's nuke (its script's `FatManExplosion` with no owner; a Fat Man's, the same base, names its shooter and is a missile's case) | every game's copy sets its own off: on the rig, armed and swinging at a friend's stand-in, it went off in both games, the copy's replayed swing setting off its own, and named no owner. Killed outright (a console kill) it went off in neither | never (`deathBlasts=`) | the NPC's own game's (`Fate::kEachOwn`) |
| A stand-in's | never here: its shots that may go off and its throws are held. One that goes off anyway is not reported (`standInExplosions=`) | never | nobody's: given back |

- Sure to arrive: our game sends a report only when `RemoteEdits::ReportGate` says the server passes it on: within 7000
  units of the nearest player, ours or a friend in our space (the server's 8000 less a state's age), a radius in
  (0, 1024], and a budget of 8 at once, then 3 a second (the server's 16 and 4, less two seconds of a burst bunching up
  on the way). An NPC's missile far from its own game's player but beside a friend is reported, as only the friend's
  game can deal the friend's blow: the scan looks around each friend in our space too (within 2024 units of them,
  only an explosion an actor of ours set off: `nearFriends=`), and the server passes one near the reporter or any
  other player in that space. Reports
  go reliably both ways. The server keeps no window after a shot or a throw. All joiners' explosions together have room
  for every seat's own budget at once (what another game spends can't be seen from ours), so only a client that rejoins
  as someone new runs out.
- Set off where its NPCs are: a reported explosion goes off within 6000 units of our player, or beside an NPC our game
  runs (our companion among them) within its radius and 1024 units, wherever our player is (`explosionsForOurs=`). Our
  copies of NPCs other games run that its damage could kill are made ghosts while it goes off (BlastShield; the rest
  take it and are given it back: `ghostsSpared=`). Every ghost we set is written down until the engine reads the copy as
  no ghost again, in the save's co-save too (GhostLedger, `ghostLedger=set/cleared/stuck/unstuck/fromSaves/open`,
  the `ghosts` dev command): it comes off whenever the copy is next loaded, whoever runs the NPC, with setghost 0 sent
  again at a growing gap while it stays, sixteen times at most in a run. A ghost an older save left (a plugin's NPC the
  session shares, its flag on, its base record no ghost, no quest holding it in an alias, hostile to our player, not
  ours now) is taken off once per actor per load, in a session (`ghostsRepaired=`). A copy is ghosted only when the
  explosion's record says what it deals and that could kill it (`Lethal`); one with an object effect on its record
  says nothing of its spell's damage, so every copy in reach is ghosted as before.
- Never from a fight we can't see: a report names the NPC that set it off (`source`, 0 for a player's), and a game with
  no copy of it sets nothing off (seen on the rig: a host-only Nuka-World NPC's blast went off beside a player without
  the DLC). One of a plugin that game lacks is dropped as it comes in (FormTranslate); a spawn it never placed is
  counted (`explosionsNoSource=`). A spawn's blast in the moment before its copy is placed is lost the same way.
- The objects' own: `explode` goes with a destructible object's stage whoever reports it, with no window, and a budget
  of its own of 64 at once, then 64 a second per player, far past any fight. A stage comes once in the session, near its
  reporter. A trap's spring is relayed from 16000 units.
- Which explosion dealt a blow: the one its hit names (`HitData::sourceRef`, `blastsNamed=`), else the nearest that
  reached it (`blastsNearest=`). Two that reached it and fared differently, with neither named, can't be told apart:
  the blow is taken as ours (`blastsAmbiguous=`), dealt here and reported.
- Known limits:
  - An NPC whose game is in a loading screen when the report comes takes no blow from it. A copy more than 1024 units
    from where its NPC stands in its own game can be hit by a blast the NPC's game sets off out of its reach.
    An NPC our game made at runtime and doesn't share (a companion aside) hits copies for nobody (as before).
  - A connection that stalls for more than about 2.7 s with the gate at its limit (an explosive minigun firing on) has
    its resent reports arrive in one go, past the server's burst of 16: the excess is dropped, and its blows on copies,
    given back, are dealt nowhere. A client that rejoins under new identities on ENet can drain the joiners' pool.
  - A modified client can set off every destructible object near the others within the stage budget (64, then 64 a
    second): the objects' own explosions are trusted to whoever passes the stage.
  - The engine gives a freed explosion's id to the next one, and a hit that names an id is matched to the oldest
    explosion of the last 3 s with it: when two share an id that fast and fared differently, the blow goes by the older.
  - A stand-in's or a copy's inert shot flies the engine's own path from here, not the shooter's: at range it lands
    beside the reported blast, and a point-blank one can leave the gun a few frames after the blast went off (the
    blast's report is reliable and the shot's replay is a frame or more behind it). It hurts nobody and sets off
    nothing, but its impact is a real one: a direct hit on our player shows the hit's blood, and what the impact's
    own effect does (a mine shot at goes off here as the engine has it) is each game's own.
  - A record that goes off whose shot isn't the gun's `GunProjectile` (the record the replay made the copy for) gets
    its copy made in the launch itself, on whatever thread the engine launches from (`inertOffMain=`); when no copy
    can be made (32 at once), nothing is launched (`inertRefused=`), as the hold.
  - An inert shot's direct hit still carries the gun's own on-hit effects (its enchantments' and its ammunition's), with
    no damage: none of the game's launchers or explosive guns has one, a modded gun may. Its impact is the engine's own,
    with no blast and no damage, so a mine in its path has nothing to go off by (a direct inert hit on a mine was not
    reached on the rig); a mine beside the friend's relayed blast goes off there, as beside a grenade's, and is shared as
    sprung (the trap row).
  - A living robot's limb blown off goes off in its runner's game alone (it has no damage of its own). A Sentry Bot's
    `RobotModExplosion` pieces (no damage) go the normal way, as a turret's shots share that base: in a throw's
    watch one can show twice.
  - An explosion with no owner that our game keeps during a throw's watch unshares every thrower's watch, so a
    reported Molotov's fire is then dealt from our copies too (a rare double fire).
  - The gate takes a friend's space and place from their last relayed state, the server from its fresher one: an NPC's
    blast beside a friend who went through a door within about a second passes our gate and is dropped by the server,
    its blows on copies dealt nowhere.
  - A client can have an explosion relayed beside any player in its space without being near them, within the
    explosion budget and each receiver's own checks (its reported position was its own word anyway).
- `net status`: `explosions=reported/set off`, `explosionsKept=` (ours, not sent), `othersExplosions=`,
  `shotsWatched=watched/checked`, `nearFriends=`, `deathBlasts=`, `standInBlowsThere=` (an explosion's hits on a friend's stand-in not passed on), `firesInert=` (a stand-in's or a copy's shots that would go off, let fly), `inert=swapped/launches` with `inertRefused=`, `inertCopies=`, `inertOffMain=`, `inertLive=`, `inertEnded=stale/atSaveOrLoad`, `inertDropped=` and `inertSpawned=` (every launch the engine made, those made inert, those refused for want of a copy, the copies, those off the main thread, the inert shots in flight now, those ended as stale or unloaded and those ended before a save or load, those the engine deleted itself, and a relayed explosion's children swapped), the blast counters above; the log names each explosion's owner and
  what became of it, and each hit on a copy names its source.

To check live: in both directions, a missile, a mini nuke, a shell, a cannonball and an Explosive round at range and point blank (seen in flight in the friend's game, `inert=` up by one there, `explosions=` up by one on each side, each game's own counters of damage once), a grenade and then a missile within 8 s, a missile into a car, a companion blowing a
car up, a grenade beside a mine, an NPC's missile at a player far from the NPC's game's player, and a Sentry Bot killed by a missile (its death blast
goes off once in each game, `deathBlasts=` up on both sides and nothing reported). Each goes off once in
each game (`explosions=` up by one on each side, `standInExplosions=0`), each NPC it hits loses its health once, the
same in both games.

## Form IDs across load orders (common/FormIds.h, common/FormTranslate.h)

Everything shared is named by form ID, and a form ID's top byte is its plugin's place in the load
order of the game that loaded it: among the full plugins (`00` to `FD`), or, after `FE`, among the
light ones (12 bits of place, 12 of form). Two games with other mods, or the same mods in another
order, give one mod's NPC two IDs. Until protocol 35 the server let in only a joiner whose load
order hashed like the session's. Since protocol 36 every form ID on the wire is in **the session's
numbering** instead, and each game translates at its edge.

- **The session's plugin table** (`kPluginTable`, `Protocol::PluginTable`) is the host's load order
  (a dedicated server's first arrival's), as hellos lay one out: the full plugins,
  `Protocol::LIGHT_PLUGINS`, the light ones. A joiner's plugins the table lacks are added after the
  rest of their kind (`FormIds::Extend`), so an ID the table gave stays what it was for everyone
  already in, and for the saved session. The server sends the table to each joiner before its
  welcome (after the `kLoadOrder` list a differing joiner is sent), and to everyone when a joiner
  makes it grow. It is capped by what IDs can name (254 full, 4096 light) and by the wire's list
  (`MAX_PLUGINS`): past that a joiner's plugin has no ID in the session.
- **The mapper** (`FormIds::Mapper`) pairs our load order (the list our hello sent) with the table,
  by plugin file name in any case. It works per plugin, so a reorder, a light plugin in another
  light place, and a plugin light in one game and full in the other (its forms past `FFF` have no
  light ID) all map. 0 and runtime forms (`FF`, which includes spawn ids) are nobody's plugin's
  and pass as they are. With the same plugins in the same places, or before the table has come, it
  is the identity, and nothing is decoded at all.
- **The edge** is `Session::SendOut` and `Session::Receive` (src/net/Session.cpp): every packet we
  send goes through `FormTranslate::Translate(kToSession)` and every packet we get through
  `kToLocal`, before `HandlePacket` reads it. So the rest of the game, the stand-ins, NpcSync,
  WorldSync, the ledgers, the dev commands, sees only its own IDs, and the server only the
  session's: the server translates nothing. The one exception is the hello, which goes out before
  the table is known: the server maps its one form ID (the appearance) from the hello's own list.
  The dev channel's `fuzz` (`InjectPacket`) is read in our own numbering, untranslated.
- **Every message type is in `Translate`'s switch**, and a type added to the enum without a case
  stops the build (C4062 made an error there; gcc's `-Wswitch`), so a new message cannot carry raw
  IDs by mistake. A test sends one of every message to the session's numbering through a mapper that
  moves every plugin, and back, and gets the same bytes (tests/FormTranslateTests.cpp).
- **What has no ID on the other side is skipped, never guessed.** A message about such a form is
  dropped (a death, a door, a shot by a mod's NPC, a friend standing in a mod's cell); an entry of a
  list is left out (one NPC of a batch of states, an item someone wears, a head part); a detail
  becomes 0, which every message reads as none (a gun's sound, the weather, a hair colour, a line's
  recording, which leaves it text only); a numbered container change keeps its number and is
  emptied (`Protocol::IsEmpty`), so the ledger's count stays the server's while it applies nothing.
  The log names each plugin once ("Session: forms: X is not in our game; what comes from it is
  skipped here"), the session line counts what was skipped either way (`skippedOut`, `skippedIn`),
  and `mods forms [formHex]` on the dev channel shows the table and an ID in both numberings.
- **Joining.** A joiner whose load order differs is let in, sent the session's list first so its
  game can name what differs (src/net/ModDiff.h): only the plugins one game has matter now (missing,
  extra); a plugin in another place is listed as nothing to change. A hello without a plugin list (a
  bot, a test tool), or a session whose first player sent none, still needs the session's hash: with
  no lists there is nothing to translate by.
- **The saved session** (SessionFile layout 33, 32 in the plugin table's own test builds) keeps the table as its first packet, so its IDs keep
  their meaning whatever the host loads next: a host who reorders or adds mods carries on with the same
  world (the new plugins are added to the table). A file of layout 29 or older, or of 30 or 31 (drops or wires alone), has no table: its IDs
  are the load order its hash names, and only a host with that one carries on (or the one older
  versions hashed without the texture pack); another starts fresh, as before.
- **What it does not do.** It cannot share what one game lacks: a mod's NPC only the host has is
  not there for a friend without the mod, and its fights, loot and quest stages are skipped for
  them. It compares names, not versions: two versions of one plugin share IDs as one plugin. Records a
  plugin *changes* in another plugin (an override of a vanilla NPC's gear) translate as that vanilla
  form, so they are shared as each game has it. Messages added by branches built before protocol 36
  need their case in `Translate` (the build says which).

## Shared world (src/game/WorldSync.cpp)

References are identified by form ID, in the session's numbering (see Form IDs across load orders above).
Never shared: runtime-created references (0xFF......) and the player (0x14), which differ per game
(runtime NPCs are shared as spawns under ids of their own: see Spawns)
(`Protocol::IsShareableRef`, checked by the server and again by every receiver). Receivers also check
form types before acting: kills only for actors, loot only between containers/NPCs and inventory
items, pickups only for loose inventory items that don't fill a quest alias.

Which loot is the session's is one rule, `LootScope::Of` (src/game/LootScope.h, game-free and tested),
run by the sender before reporting a move and by every receiver before applying one, so what an older
build reports anyway stays out. WorldSync runs it through `LootSync` (src/game/LootSync.h, game-free):
the sender's report, the receiver's check, and the changes that wait for a container or a death. The
tests play two and three games through it against a real server (tests/LootGame.h).

- Per player: keys, paper notes, holotapes, bobbleheads, magazines (`FeaturedItem`), quest items (a
  stack held in a quest alias), all weapons and armor, vendors and caps, what companions and settlers
  carry (any living NPC's inventory: trading with a companion, pickpocketing), settlement workbench
  storage (with the host's `bSharedStorage` on, a workbench of a settlement the host owns has one true copy instead:
  below), power armor frames.
- Shared: ammo, chems, food, junk and other loot in containers and on corpses.

Weapons and armor are per player because a move carries only the base form: another game would get a
plain copy, and `removeitem` could take a legendary. So a friend's looted gun stays on your copy of the
corpse. (Players give each other weapons and armor by dropping them: see Giving items; with the host's `bContainerGifts`, also by putting them in a container: see Giving through a container.) A vendor's chest is any faction's merchant container; moves while the barter menu is open, and
for a second after, are trades and never reported. A corpse is shared once the session says it died: a
change for an NPC still alive in this game waits for its kill, up to 10 minutes in the world, counted
for all of that NPC's changes together (at most 4096 changes wait; when full, one waiting for a living
NPC makes room before a friend's change for a chest not loaded yet is dropped: `pendingFull=`). A chest
waits too while this game holds it in no cell at all: one of a place we left lately stays in memory that
way (Red Rocket's desk and cabinets, seen from Diamond City), and its changes go in when we come back. A
take waits too while this game holds the chest in a cell but hasn't made its inventory yet (a place not
loaded, or still loading): it would find nothing to take, and the roll made as the place loads would still
hold what the friend took. So does a put, in order with the takes: going in first, it would make the inventory
ahead of the roll, and the takes and the emptying after it would go into that instead. A chest of a
place our game held this way makes its roll as our player opens it, so opening it counts as its load: what
waits for it goes in on the next frame after the menu opens (`Loot::Opened`, `openApplied=`), not when the 2 s
sweep comes to it, which left the whole roll in the menu for up to a second. A
change to a chest our player has open goes in at once: the container menu shows the new contents, and
taking what a friend took gets nothing (tried both ways at Red Rocket, 2026-10-18). Loose
items picked up from the world are the same placed reference in every game, so only the personal ones
stay (a loose weapon is taken from the friend's world as it is). The status line counts what each end
kept: `weaponArmor=`, `vendor=`, `barter=`, `perPlayer=`, `notYetDead=`, `unsupported=` (both ends),
`personal=` (other players' personal items left alone, pickups included, as in 0.8.5), and on the
sender `personalSent=`, `holderDropped=` and `lootSent=`.

Each game rolls a chest's and an NPC's leveled loot on its own (the same corpse and the same chest held
different things in the two games of the rig), so a friend taking what their copy held left ours
holding its own roll. A take that leaves a holder with nothing the session shares is followed by an
emptied marker (`Protocol::CONTAINER_EMPTIED`: the player's ref as the item, count -1, an ordinary
numbered change, so it is caught up on after a load or a join like any other; one also follows a weapon, a key or a note taken from a holder that then holds nothing the session shares, once, though that take itself is not reported): every receiver takes out
what its own copy has left of each item another player took from the holder since it was last emptied there (its roll
had more of it; the takes themselves went in before), and leaves the rest: what only its own roll put there, an item
only its own mods add, what its player put in where the session didn't see it (alone, before a join), and weapons,
armor and personal items. Taking every item thus leaves a friend's copy as taking all but one does, less the leftovers
of what was taken (a Nexus report, 2026-10-26: Take All cleared the friend's own loot; until 0.10.0 the marker took
every shared stack of the receiver's roll). Its own player's takes don't count: they say nothing of what the friend's
copy held. The items taken are kept in memory only, as the session's puts are: after a load they come back with the
catch-up of the takes, and a marker whose takes went in before the save leaves everything. It waits like any change for a holder not loaded, a holder whose inventory the game hasn't made
yet (a chest that rolls its loot as it first loads), and an NPC still alive there; a vendor's chest, a
workbench or a frame is left alone. Storage is too: what the player has made their own, a container they own or
whose cell they own (a home bought), a settlement's workbench or a piece built there, or a container in the build area
of a settlement whose workbench they own (`LootScope::IsStorage`). Not a chest that only never respawns (a dungeon's
one-off loot) nor one in a settlement nobody owns yet (the Sanctuary houses at the start): those are rolls of loot. Each game
judges its own copy: none is sent for storage, and one that comes for it (from a game where it isn't storage yet, or
from an older build) is not applied, so a friend's stash there stays. What the session put into the holder (a friend's
put at the same moment, which the server may order either side of the marker) stays too, as many of each as the
session put less what was taken of them since; a take is counted against the holder's own roll first, so a friend
who took their copy's roll of the same item before the put reached them doesn't use the put up. Once a holder's loot
respawns (it holds more of something than when it was emptied) it may be told empty again. An older build sees the player's ref as no item and leaves it.
`net status` counts `emptied=` sent/applied/stacks taken and `emptiedKept=`, the items a marker left for the session's puts.
Storage skips only the marker: puts and takes there go between the games as anywhere else, as in 0.9.1, so a friend's take
from their copy takes as many of the item from ours, whether ours got them from the session or not. That is still so with
the host's `bSharedStorage` off (the default); on, storage has one true copy (below).

**Shared storage (0.10.0, the host's `bSharedStorage`, off by default; welcome flag bit 32).** A home's or a settlement's
container has one true copy: the host's game's. The server keeps what each such container holds (`common/StorageBook.h`)
and checks every friend's move there against it; the games' side is `src/game/StorageView.h` (game-free) and
`src/game/Storage.cpp` (the engine). All of it is tested without the game: `tests/StorageBookTests.cpp`,
`tests/StorageViewTests.cpp`, and both ends through the real server in `tests/StorageGameTests.cpp` (LootGame.h), which
counts every item across the host's chest and the players' inventories in each scenario.

- *Who keeps a container.* Only the hosting game (the server's local player) claims, by listing a container
  (`kReportStorage` 68: its shared stacks, LootScope's ammo, aid and junk, and the ledger's next number it is as of). Its
  scan lists, every 2 s and at most 16 at a time, the loaded storage containers within 8192 units of its player that are
  not a vendor's chest, nor a workbench of a settlement the host doesn't own (owned by the player or the player's
  faction), once each since the session began, its world last loaded or it last asked the
  server for changes. Nothing is listed while its ledger is still asking (the listing would miss changes). The server
  adds the friends' changes numbered after the listing's number, and tells everyone the owner (`kStorageOwners` 169).
  The design had the first claim win; the host alone is simpler and safer: a friend's game is never anybody's truth, a
  listing from another game is ignored, and a host whose ID changed (a reconnect) takes its old entries over.
- *Stale.* The host's hello, the world request it sends after a load (`WorldRequest::reason` 1) and its leaving make all
  its listings stale: the friends viewing one are told it closed (`kStorageRefused`, item 0, `kClosed`), and every move
  there is refused until the host's game lists it again. A gap asked again (reason 2) stales nothing: its world is the
  same, and nothing is settled again. After a load or a hello the host's scan lists what it listed before wherever it is,
  not only within its radius (`Side::ToRelist`), so friends at a settlement the host isn't standing in are not shut out
  until it walks back; a container its game doesn't hold in memory stays stale until the host comes near. The owners are saved with the session and come back stale. They are read and written whatever the rule: a run
  with it off keeps them as they were and never asks the book (no container has a keeper; views and shortfalls are
  ignored), so turning it on again finds each container claimed since it was, and the claim's take-back (below) never
  runs a second time over moves made while the host kept it. The session file is written as layout 38 (the owners and
  the moves the true copy undid) only when there are any, so a session that never used the rule stays readable by
  0.9.x; one that did stays at 38, and an older build can't read it, the rule off or on. Layout 37 was this branch's test builds' and is set aside: start the
  first 0.10.0 session from a fresh file.
- *A friend's view.* As a friend's container menu shows a container another game keeps (the container menu's show,
  Equipment's hook), the shared stacks their copy held are set aside: taken out of the copy and kept in memory, never
  given to the player (`setAside=`), and `kViewStorage` (69) open is sent. The answer (`kStorageContents` 168: the
  stacks and the number they are as of) is shown in the copy, with the player's own moves still on their way added.
  Others' changes numbered from there go into the copy while the view lasts; before it, after it and in a copy nobody
  views, none do. At the close (the menu's close event, a save or a load, a disconnect, the session's end) the moves
  made in the menu go out first, then the session's stacks leave the copy, what was set aside goes back into it, and
  `kViewStorage` close is sent. A save is made after the close, so no save holds the session's stacks in the copy or
  misses the set-aside ones. The player's own stash in a container the host keeps is thus out of reach for the
  session (it is there again once the rule is off or they play alone): the price of never letting both it and the
  host's stacks reach the player. Weapons, armor, keys and notes never take part: they stay in each player's copy.
  When the server closes the view (stale, the host gone, the view budget), the game sends the menu's pending moves,
  takes the session's stacks out of the copy at once, asks the menu to close, and shows a notice; the set-aside
  stacks come back as it closes.
- *Moves.* A friend's take or put at a kept container goes out as any loot, and the server refuses it (`kStorageRefused`
  170, never numbered) when there is no view, the container is stale, the take is more than it holds, or the host's game
  can't hold the item (`FormIds::Mapper` with the host's plugins, cached per player until the session's plugin table
  changes or they say hello again); one accepted but then dropped by the server's own limits (a full log, the player's share) is refused too.
  The friend's game undoes a refused move: a take comes back out of the player (what they no longer hold is counted,
  `usedUp=`: the move was never numbered, so the chest still has it), a put goes back to them, and the view asks for the contents again so the copy
  is redrawn (once per answer: no second ask while one is out). Two friends taking the last stack: the first numbered
  wins. The player change goes through OwnGear's `kStorageUndo`, which only `StorageView::Client` can make
  (`PlayerChange` has a private constructor).
- *Quest aliases.* A shared item the friend's copy holds a stack of that fills a quest alias (`Engine::Aliased`, read at
  the open) stays the player's own for the whole view: it isn't set aside, the host's stack of it isn't shown, and
  every move of it there (part of the alias stack or all of it) stays in their copy and isn't reported
  (`storageKept`). Reading the alias after each move instead would let a take of the host's plain stack beside the
  alias one pass as local (a dupe), or a take of the whole alias stack pass as the host's (a loss to the host).
- *The true copy is put right, never the player.* A friend can always move items out of their own inventory (to a
  workbench, a companion, the floor) before anything taken back from them lands; they can never touch the host's
  chest without the server. So after a load or a rejoin, a friend's own move their world lacks is undone in the true
  copy, by the server, and their inventory is left as their save has it. Three earlier rounds took the move back
  from, or gave it again to, the loading player and then forfeited what they lacked; each left a window (a delayed
  catch-up, a disconnect, a timeout's give-back) in which they could hide the items first, and the reviews duped
  through every one.
- *Settling.* Every world state a friend's game gets (a hello, a load's or a gap's `kRequestWorldState`) is followed,
  with the rule on, by `kStorageCaughtUp` (171: which of their requests it answers; the server counts them, the
  game too, the hello being 1). Once the last request's marker has come and the ledger waits for no earlier change,
  the game sends `kStorageSettle` (71): the ledger's next number, its own numbered moves at storage others keep that
  its world lacks (`lacking=`, `lacks=` kept; at most 4096, `lacksFrom` saying where the list is whole from,
  `lacksDropped=` past it), and the moves it still has on their way that the session never numbered (`late=`).
  While the ledger waits, the game tries the last marker again each frame, so a gap it gives up on settles too (no
  further world state would come); a gap request held back in the opening isn't counted. A gap asked again is no reason to settle again (the server keeps the game settled, and the game only waits for a settle after a hello or a load). Lacks numbered past where
  the log now ends (a server whose host quit before saving them rewound it) are forgotten at the settle: those
  numbers will be other moves.
  Until the settle comes the server shows the player no storage (a view is answered closed) and refuses every move
  of theirs at storage others keep (`kClosed`), and their game won't open such a container either
  (`settlingClosed=`, the menu closes with the notice): no view and no move until their moves are settled. A world
  request or a hello also closes the player's view. The server takes one settle per request, only for the last
  (a marker for an older one is ignored), and only once that world state has gone (not while it is owed).
- *What the server does with a settle.* In the log up to the settle's number, from `lacksFrom` on: each of the
  sender's own moves at a container another game keeps that their world lacks is undone once in the true copy
  (`StorageBook::Settle`, numbered as a change by nobody, player 0, which the host's game applies as any friend's
  move and every viewer's copy follows): a put becomes a take, no more than the book holds while the listing is
  fresh (all of it while stale: the host's world applies what is there); a take becomes a put. Only the sender's
  own moves: never the host's, another friend's, nobody's, or one at a container nobody keeps. No move numbered before
  the container was claimed is undone by a settle: the claim put those right (below). Each move
  is undone once until a later settle says the player's world has it after all (a newer save loaded): then the
  undoing is undone, the opposite of what it did. Changes into the true copy go first, so a put then a take of the
  same items, both lacking, never comes up short. The moves undone are kept with the session (`kStorageReversals`
  172, layout 38), so none is undone twice across a restart. The server logs "storage undid N and did again M of
  player P's moves" with the run's total, and "held N fewer" when the true copy had less than a change out of it
  needed. Each change counts against the settler's own share of the container log, as their moves do (30000 by
  default; the host's game has none): flipping their moves between two saves can't fill the log for everyone. A settle
  is all or none: its changes are planned first (before what the book holds caps them, so the count is never short),
  and if they don't all fit in the share (or the log) none is made and the player stays unsettled (no view, no move
  at storage others keep) until a later settle can; the server logs "can't all be put right (N changes". A settle
  cut part way could put a take's items back and leave the matching put's in, in the chest and the player's world at
  once. A late move goes through `takeContainerChange` as a move made at the
  container the player viewed last (this run): it needs no view, a take is numbered whatever the book holds (it was
  made against the true copy as shown; the host's world finds a shortfall, below) and a put must be holdable, and
  either goes through while the listing is stale. A late move anywhere else is refused (`kNotViewing`).
- *Unanswered moves.* Every move at a friend's storage is remembered until its number or its refusal comes (at most
  `MAX_STORAGE_MOVES_UNANSWERED`, 2 x 1024 + 256: a Take All of the largest chest twice over). One that can't be
  remembered, past that, isn't sent and is undone at once (`overflow=`); one the session's numbering can't carry
  (Session::SendOut drops it) is undone at once (`unsent=`). One with no answer in 30 s is only counted
  (`unanswered=`): nothing is given back or taken for want of an answer, because a number may still come. As the
  connection goes, every one still out stays on its way; the rejoin's catch-up numbers those the server had, and
  the settle sends the others late. A refusal still undoes a move (it was never numbered), and a `kShort` still takes
  back what the chest lacked: both answer the move itself, before it is anybody's.
- *Loads (no dupe by reload).* After a load, the catch-up brings the friend's own numbered moves made after the save's
  ledger point (ContainerLedger's own changes, the ones `ownSkipped` counts). Those at a friend's storage go to
  `StorageView::Side::Own` in order: one the loaded world has (a move still on its way from it, kept with the save) is
  matched and left; one it lacks is added to the settle's list (`lacking=`); nothing is done on the player. The
  co-save record `STOR` (v2) holds the moves the save was made with still on their way and the moves its world
  lacks, so a save made in the older world, loaded later, settles the same. A save from another session drops both.
  So a quickload neither duplicates (put, load: the chest gives the put up, the loaded player has the items) nor
  loses (take, load: the chest has the take back). This is what the original design proposed (the server undoing
  the moves against the book and the host's container), with the friend's game naming them: only it knows what its
  loaded world has.
- *What it can't cover.* Another friend takes from the chest in the window before a put is undone, so the true copy
  holds less than the put (the rest stays in the first player's world as well; the server logs "held N fewer");
  a late take the host's chest held less of whose taker moved the items away (the short takes back what they still
  hold); a save made between a move's echo and its short (the save keeps the whole take; rare, and the short's
  correction in the log still bounds what a later settle puts back); a friend whose moves never reached the server
  as the connection went who never rejoins that session (those numbered stand; a take never numbered stays in their
  world and the host's chest alike, a put never numbered is in neither, and a new session drops them); a restart of
  the server between such a move and the settle that sends it late (the last view is forgotten, so the late move is
  refused: a put goes back to the player, a take is taken back from them, and what they moved away meanwhile stays
  theirs as well); and lacking moves past 4096 since the last load (the oldest are forgotten and never undone). Also:
  - The redo side of the first gap: between a take's undo and its redo another player takes the items put back, so
    the redo takes out less ("held N fewer"); the reloading friend keeps the items and the other player has them too.
  - A short that comes after a settle undid the take (the book showed more than the host's world held, the host was
    away, and the friend loaded a save from before the take): the undo put back the whole take, the short corrects
    only the log, so the chest ends with what was missing as well. It needs an unreported change in the host's world.
  - Settle changes and claim take-backs are numbered as changes by nobody, so a restart, which counts each player's
    share from the log by who made each change, doesn't count them: each run gives a player their whole share of
    undoing and doing again. Honest players never come near it; a modified game could fill the session's log over a
    few restarts, which stops loot sharing for that session. Keeping a count per player would need the session file
    to hold it.
  - A gap the ledger gave up on (WorldSync's "gave up on container changes") hides the moves in it, so none of them
    is called lacking: the settle after it does again any of the player's undone moves in that range whether their
    world has them or not (a take done again that their world lacks loses it, a put a dupe). It needs a skipped gap
    holding one of their own undone moves.
  - A claim's take-back counts as a friend's every put by a player other than the host's game as it is now. A host
    whose identity changed (its identity file reset, a new PC) has its own earlier puts taken out of its world. And a
    numbered put the host's world never got (dropped from a full queue, or undone by an unreported change) is taken
    back all the same, out of the host's own units of that item where the listing holds them.
  - A late take refused because the player viewed another container since (an unanswered take at one, a view of
    another, then the settle): it is taken back from the player as any refusal, and what they moved away stays.
    Getting there needs a move past the budgets or a disconnect timed between two views.
  - A friend who put items into a container nobody keeps yet, and another friend whose copy took them in as loot
    and took them out (or kept them): the claim takes back only what the friends put in net, so the two worlds keep
    what loot sharing gave each, as with any shared container before the rule.
- *Budgets.* The server checks a friend's moves at a kept container (late ones too) against their own budget (a burst
  of 2304, 500 a second) before the storage check, so the check and its refusals cost nothing past it; refusals have
  a budget of the same size, and a move past both gets no answer (it stays on its way in their game, and the next
  settle sends it late). Views have a budget of 16, 8 a second; a view past it is answered closed. A settle names at
  most 4096 lacking moves and 2304 late ones. What a settle puts right counts against the settler's share of the
  container log, and what a claim takes back against the share of the friend who put most of it. A host's listing is at most 1024 stacks.
- *Shortfalls.* The host's game applies friends' takes as loot. One its container held fewer of than taken (its world
  changed unreported: a script, a load the server didn't know of yet) goes back as `kReportStorageShort` (70), the
  container is listed again, and the server sends the taker a `kShort` refusal for what was missing and corrects the
  take in its log (and the session file) to what was there, so a later settle puts back only that much: a friend
  who loads a save from before a short take has the chest get back what it gave, not what they asked for. A load that
  kept the take unanswered (the save's `STOR`) matches the corrected number to it and takes the difference back from
  the player (`corrected=`; only a move kept with a save is matched to a smaller number, so an older take never pairs
  with a newer one). A take by nobody (a settle's) the host's chest held less of stays in the log as numbered: there
  is nobody to give the rest back to, and the server logs "held N fewer". A container whose
  inventory the host's game hasn't made yet (never loaded there) is neither listed nor applied to (it waits).
- *Storage nobody keeps yet* (the host hasn't been near it since the session began) is ordinary loot while the rule is
  on, as with the rule off: moves there go between the copies. A friend's move at a container in the moment the host
  claims it can still reach the host's copy as ordinary loot (the claim race); the listing is as of a ledger number,
  so it counts in what was numbered before it. What friends put in it before the claim comes back out of the host's copy
  at the claim (`StorageBook::BeforeClaim`, numbered as takes by nobody right after the owner is told and before
  anyone views it): for each item, what the friends' moves there added net, no more than everyone's moves there
  added (the host's own takes, or others', took some) nor than the listing holds. Those items are still in the
  friends' own copies (their stash, set aside while they view the host's), or back in a world they loaded from before
  the put, so the host's copy must not hold them too: a friend who put, quickloaded and put again before the host
  came near could otherwise take them all out after it (the fourth review's loop). A put and a take of the same items
  before the claim cancel out, so nothing of the host's is taken. No move before the claim (the claim's `since`) is
  undone by a settle afterwards; the friend's game doesn't call one lacking. The server logs "claimed: N items
  friends put in it before go back out".
- *The host's workbench (protocol 60).* One pile of building materials at the host's settlements: a workbench of a
  settlement the host owns is listed like any storage (LootScope's `sharedBench`: a workbench is per player unless
  shared storage keeps it), so whatever either player scraps or stores there is there for both, and a build or a
  craft by either uses it up once. Where the engine reads components (1.11.240, by RE): workshop mode builds its list
  in a temporary container of its own (`Workshop::RefillSharedWorkshopContainer`, 2194977: empties it and copies in
  the workbench, the benches its supply lines reach and the player) as it starts and after each build or scrap; a
  crafting menu's gather (`WorkbenchMenuBase::UpdateOptimizedAutoBuildInv`, 2224955) fills `sharedContainerRef` the
  same way from the station's workbench (`GetWorkshopContainer`). A build takes each component with
  `Workshop::RemoveItemFromWorkshopContainers` (2195117: a real `RemoveItem` from the workbench, then the supply
  benches, then the player), a craft from `sharedContainers` (2224960), and a scrap or a build's leftovers are added
  to the workbench (`ScrapReference`, 2195125). Every one is an ordinary inventory change at the real workbench, so
  the view is the container menu's: nothing is linked to a workbench, placed on a grid or given to a workshop
  script, and no copy is anything but the workbench's own inventory.
  - A friend's game opens a view of the host's workbench as its workshop mode starts there (`WorkshopModeEvent`) or a
    crafting menu opens beside it (ExamineMenu, CookingMenu, PowerArmorModMenu, and Automatron's RobotModMenu, an
    ExamineMenu by RTTI with the same gather, since protocol 62: the first workbench among the menu's
    `sharedContainers`, looked for 2 s), and closes it as they end. Its stash's shared stacks are set aside, the true
    copy is shown in their place, and after each change to the copy the engine's own list is filled again (the
    refill in workshop mode, the gather on the main thread for a crafting menu), so the list is never stale. The
    engine's takes and puts at a watched workbench (not the player's own transfers, which are loot already, nor a
    friend's applied change) go out as moves while the view shows it, checked by the server as any. A view the
    server closes (the host away, stale after its load) ends at once with a notice: workshop mode and a crafting
    menu are never closed under the player; they build from their own stash meanwhile. A view refused is asked again
    after 5 s, doubling to 60 s. A save or a load ends the view and it opens again after.
  - A refused take (the host's workbench lacked it: a race, or the copy was stale) is paid by the friend: first from
    their own stash at that workbench (set aside for the view: `stashPaid=`), as their game's build would have used it,
    then from their inventory; what neither holds is counted (`usedUp=`) and the build stands that much short of paid
    (a build can't be undone). A refused put is given to the player.
  - A friend's change at a watched workbench is made in our copy by Storage itself (`Storage::ApplyAtBench`, the same
    guarded change as the view's own), never by a console `additem`/`removeitem`: the console's change reached
    ContainerWatcher outside the mark that tells it from the engine's, so each game reported the other's change back
    as its own and the two fed each other (found on the rig: a put grew without end, a take ran down to a refusal).
  - Out of a session no Frame() runs, so a disconnect fills workshop mode's or the crafting menu's list again there and
    then (the view's end puts our own stash back in the copy).
  - The host's game reports its own engine and script changes at a workbench it keeps (its builds, its scraps, the
    day's production) as the keeper's own moves; it flushes its own pending loot before each listing so a move sent
    after it can't count twice.
  - A friend's own game's changes there are never the session's: what its scripts add or take (the day's food and
    water production, workshop scripts: the game's Papyrus `AddItem` and `RemoveItem` functors are hooked, vtables
    1563821 and 579181, slot 1 checked before it is written, to mark the thread) and what another container moves into
    it are kept local (`StorageView::Client::Local`): an add goes into the set-aside stash, a take is put back and
    paid from it (`localKept=`, `localUnpaid=` for a take the stash lacked). So each game's production stays out of
    the true copy, and the host's is counted once, as the host's own.
  - Weapons and armor stay per player as in any storage. A workbench the host doesn't own, or hasn't been near this
    session, stays per player. Tested without the game in `tests/StorageBenchTests.cpp`.
  - *The host's supply lines (protocol 62, src/game/SupplyLines.h).* Every one of the engine's gathers reaches a
    settlement's linked workbenches through `Workshop::AddOtherContainers` (2195029, `(list, location, excludePlayer)`):
    `BGSLocation::CollectLinkedLocations` (2199664) walks the settlement location's links of the caravan keyword
    (global 4797310; every location reachable, its own left out), each linked settlement's workbench is added from the
    workshop list (`GetWorkshopContainer`, 2195187), then the player unless excluded. It is called by
    `Workshop::CollectLinkedWorkshopContainers` (2195116, +0x178: workshop mode's list, a build's takes, the resource
    counts) and by the crafting menus' gather (2224943, +0x426). A settlement's location is the engine's
    `Workshop::FindLocation`: the workbench's current location's ancestor with keyword 4797293
    (`FindLocationWithKeyword`, 2199627). Those links are each save's own, so a friend's game used to find its own
    world's supply benches, holding its own stash. Now the host's game, every 10 s, asks its own `AddOtherContainers`
    which workbenches its lines reach from each workbench it keeps, keeps those of settlements it owns (listing any
    not listed yet, from afar: a workbench is persistent), and tells the session what changed (kReportBenchLinks 79,
    in its engine's order, the order of the engine's workshop list, which its own builds take in; at most 64; the server keeps a table only from the workbench's keeper, relays it as kBenchLinks 183 and
    sends it with the welcome after the owners, and forgets a keeper's lines, telling everyone, as its world goes
    stale or it leaves). A friend's game viewing the host's workbench views the workbenches its lines link too (each
    a view like any: its own stash set aside, the true copy shown; one a friend's game keeps or nobody keeps is left
    out). The server sends a player a burst of 48 views' contents, then 8 a second (and the session a burst of 192,
    then 32 a second), and answers one past that closed, so the first 32 links are viewed, enough for a network players
    build (the rest stay the friend's own, as their world's lines are), one opening every 250 ms, and one the server closes is tried again quietly (no notice while the workbench's own view stays) after
    5 s, doubling to 60 s until its contents come (SupplyLines::Views); and both calls to `AddOtherContainers` are hooked (one 14-byte stub, the call bytes checked first): for that
    settlement's location the list after the workbench is the host's linked workbenches in the host's order, then the
    friend's own world's other linked workbenches (their own stash, as before), then the player. So a build takes from
    them in the host's order, each take a move at that workbench the server checks; a refused one is paid from the
    friend's stash there. A linked settlement is mostly far from both players, its workbench not loaded in either game
    (LootSync's `Loaded` says no: seemingly in no cell), and every change at a workbench shared storage keeps goes in there and then all the
    same (`World::FromAfar`, Storage::WatchesBench): the host's game takes a friend's take out of its pile at once, as
    its engine's own supply-line builds and the day's production change it from afar, and the friend's view shows the
    host's changes there as they come. Before, they waited for the workbench to load, which a supply line never needs:
    the server's book had the take while the host's pile still held it, so it could be used twice (the rig). The
    host's own workshop mode or crafting menu, open at a workbench its lines link others to, is gathered again after a
    friend's change at any of those, far or near (SupplyLines::HostListChanged: its own `AddOtherContainers` asked at
    most once a frame, only when such a change came). Before, only a change at the workbench itself refilled it, so
    after a friend's take at a far link the host's list still counted what was gone (the rig: 21 shown, 18 left), and
    a host build from it took fewer components than it cost (`RemoveItemFromWorkshopContainers` takes what is there). A
    container menu opened meanwhile ends every view first. The host away closes every view and
    forgets the lines, so the friend builds with their own. `storage bench` lists the supply views (`supply=`) and, on
    the host, its own lines (`ourLine=`); `storage status` counts `supply=`, `linkedHeard=`, `linkedSent=`,
    `linkReports=`, `supplyGathers=`.
- `net status` and the dev channel's `storage` show the counts (`inFlight=`, `settling=` 1 until settled, `lacks=` kept,
  `setAside=`, `views=`, `shown=`, `refusedTakes=`, `refusedPuts=`, `shortTakes=`, `unanswered=`, `unsent=`,
  `overflow=`, `lacking=`, `lacksDropped=`, `settles=`, `late=`, `settlingClosed=`, `corrected=`, `takenBack=`,
  `usedUp=`, `givenBack=`, `closedByHost=`, `unmatched=`, `listed=`, `shorts=`, `listPutOff=`, `ownRefused=` which
  must stay 0; `bench=` the workbench viewed and how, `benches=` those known, `localKept=`, `localUnpaid=`,
  `stashPaid=`). Each kind of notice shows at most once every 2 s. The server's "storage undid" line carries the run's
  total.

Picked plants (flora) are harvested in every game: TESHarvestEvent's ItemHarvested, by our player, of a
shareable ref, goes out as a pickup (kReportPickup, no new message). A receiver whose ref is flora runs
`<ref>.setharvested 1` (once: one harvested already is left), never disabling it, so it grows back on its
own schedule; a joiner or a reloaded game gets it through the world state's pickups. `flora=`
sent/applied.

Activators (buttons, switches, levers, a sink) a friend presses are pressed in our game by their
stand-in, not our player, so a script that acts on whoever pressed it (rads from a sink, a teleport)
acts on the stand-in. Without the stand-in here the press is refused (`acts=` sent/applied/refused);
the door or lock it drives arrives as its state.

Which open states are shared (src/game/DoorShare.h, SharedGates.h, ScriptDoors.h; docs/script-doors-survey.md):
- a plain door a player opens, and a door whose scripts are all on `DoorShare::QUIET_SCRIPTS` (ChainedDoor's
  53 chained doors, DefaultRefOnActivate, DefaultRefEnableLinkedRefOnActivate...: scripts that act only on
  activation or on the lock, which the engine's `SetOpenState` in a friend's game doesn't raise, and never
  move the door themselves); `DoorShare::NEVER` keeps Mass Fusion's out;
- a plain door an NPC our game runs opens near our player (TESActivateEvent names the NPC; `NpcSync::RunsLocally`,
  not a load door, within 1200 of our player, else the friend refuses the report): watched for 30 s, so the
  friend's door opens and closes as their copy of the NPC, whose AI is off, walks through. `npcDoors=` counts them;
- a two-state gate (an activator run by Default2StateActivator, or by one of `SharedGates::CHILDREN` that extend it:
  Med-Tek's lab doors, Jamaica Plain's treasure gates), with nothing beside the script but `SharedGates::SIBLINGS`
  (scripts that act on activation only): set in a friend's game by calling that script's own `SetOpenNoWait`;
- a plain door a script opens, closes, locks or unlocks (a quest stage that unlocks the way, a trigger the player
  walks into, a script shutting a door behind them): every loaded plain door is read every 500 ms
  (`DoorShare::POLL_INTERVAL`, WorldSync's `PollDoor`), whoever changed it, by a sweep that reads 32 a frame
  (`DoorShare::POLL_PER_FRAME`; `doorPollUs=` last/most per frame) from 2 s after the door loads
  (`DoorShare::LOAD_SETTLE`). The loaded doors are the game's load event's (TESObjectLoadedEvent, which names few of
  them: on the rig three of nineteen in Corvega's plant, none in Sanctuary on coming back) and those of a walk of the
  loaded cells' reference lists (the interior, or the grid's attached cells and the worldspace's persistent cell,
  where the load doors are), 256 references a frame with a second's rest between passes (`DoorShare::Walk`,
  `FIND_PER_FRAME`, `FIND_REST`, WorldSync's `FindDoors`; `doorsFound=` found by the walk / passes), so a door in a
  cell loaded later is on the list within about a second. Its first read, and its first after it loads again, is where it stands and is not
  reported (the save's doors would fill the world state); a change is, once our player is within 1200 of it
  (`DoorShare::REPORT_REACH`; a gate's `SharedGates::REPORT_REACH`), else kept until they are (`DoorShare::OnLook`;
  `polledDoors=` put on watch / loaded, `reportsHeld=`), and no reference is reported more than 8 times at once and
  then once every 2 s (`DoorShare::Pace`, `reportsPaced=`: a door a script flips over and over, or two games'
  scripts fighting over it). A friend's state taken is what our watcher holds as said, with ours for what it leaves
  out and no open state where we read none (`DoorShare::Heard`), so nothing goes back;
- a door only a quest stage opens (Vault 114's gear door): the stage is shared (ScriptDoors).
Not shared: elevators and their cars, load elevators, automatic doors, doors whose script answers OnOpen (an ambush,
a stage), blocks activation or opens the door itself, the Mechanist's lair doors (their controls' state is each
game's), the track system's doors, Pulowski shelters, and the story's (Vault 111, Mass Fusion).

World traps (src/game/Traps.cpp, rules in src/game/TrapRules.h; protocol 50) are spent in every game: a
mine (a placed grenade projectile, PGRE, whose projectile explodes), a tripwire (script TrapTripwire), a
tension trigger on a door or a container (TrapTrigTension), a trap proper (TrapBase and its children: a
grenade bouquet, a gun trap, an IED, a flamethrower, an electric arc, a radiation trap) and a Far Harbor
bear trap (DLC03:TrapBear). A pressure plate (TrapTrigBase) isn't: it sets itself again. Neither is a
trap made at runtime (a mine a raider threw, a trap built in a settlement), which no other game has.
There is no baseball pitcher in the base game or its DLCs (it is Contraptions Workshop's, a
workshop-built trap, runtime). Each game watches the traps loaded around its player four times a second
(found once a second within 12000 units): a scripted trap by its script's state (`Object::currentState`,
each listed script looked up by name with `FindBoundObject`, which finds one that extends it too: walking
the bound scripts with `ForEachBoundObject` showed one script of an activator that has two, which one
changing from run to run; an activator with none of them is looked at again once a second until it has
been loaded 10 s, `TrapRules::ClassFinal`), a mine
by whether it is still there (deleted, disabled or freed while its cell is attached: gone). Only a trap
seen armed, once it has settled (loaded, its player in the world, 2 s: a Far Harbor bear trap placed shut
reads open until its OnLoad shuts it), is reported: going off, disarmed, or gone (a mine our player activated in the last 3 s was
picked up: kTaken; a bear trap that snapped and then went was taken). Each kind's states, read from the
scripts on 1.11.240 (tools: the pex dump of `Fallout4 - Misc.ba2` and `DLCCoast - Main.ba2`):

| Kind | Armed | Going | Fired | Disarmed |
|---|---|---|---|---|
| tripwire | Active | Busy | Inactive | Disarmed |
| tension trigger | Waiting | Busy | triggered | Disarmed |
| trap (TrapBase) | Idle | | warn, TrapFired | Disarm |
| bear trap | Open | | Busy, Closed | Disarming |

The server keeps the first report of each trap (a take still replaces a spring or a disarm:
`Protocol::TrapSupersedes`) from a player within 16000 units of it for a spring, 1024 for a disarm or a take (done
standing at it), in the same space and where the session first had it, passes it on at once (kTraps, live),
and sends the list (kTraps, not live) to whoever joins or loads, before the world state; it is in the
session file (layout 34). A receiver does a state to its trap only while that is armed and has been
loaded, and its player in the world, 2 s (the script's OnLoad first): set off as it went (a mine's
explosion placed as a friend's is, its copies of NPCs other games run made ghosts; a tripwire's
`Trigger()`, which sets off what it is linked to; a tension trigger's `triggered`; a trap's `warn`; a bear
trap's `Disarming`, which snaps on nobody) when the report came in the last 3 s and names where ours is
(within 512 units); otherwise spent quietly through its script (Inactive, Busy and destroyed, TrapFired
and destroyed, Disarming; a mine disabled). A disarm is done as the trap's disarmed look, never its
disarm function, which gives the disarm's reward and XP to whoever's game runs it. A take disables it.
What that does in the receiver's game is in the session already, so it isn't reported back. A mine's warning
(2026-11-01, a Nexus report of mines going off at once for a joiner, without the beep): the engine starts a mine's
countdown when an actor comes within its proximity (times `fMineExteriorRadiusMult` outdoors), at the record's timer
plus `fMinesDelayMin` for our player (1.5 + 0.2 s for a frag mine, beeping all along) and at `fMinesDelayMin` alone (0.2 s)
for anyone else, a friend's stand-in among them. Measured on the rig with the probe below, both ways: the walker's mine
started its 1.7 s; the friend's copy started 0.2 s later, as the stand-in arrived, ran 0.2 s and went off 1.3 s before
the walker's, and its report set the walker's off 0.5 s into the warning: each player heard a blip and the blast came at
once in both games. So a mine whose countdown starts here with a friend's stand-in in its reach (and no other actor but
our player) is given the walker's countdown, less what it ran (`TrapRules::MineCountdown`, looked at each frame, as 0.2 s
is a few frames; `countdowns=`): on the rig both games then beeped the whole 1.7 s and went off within 0.1 to 0.2 s of
each other (whichever came first of our own countdown and the walker's report), in both directions. One an NPC may have
started (a raider, a copy of a friend's NPC, a companion in reach) keeps the engine's, as in every game. A trap's
activation is no longer pressed by the presser's stand-in (`acts=` refused): that disarmed our copy and
gave our player the XP. For 8 s after a trap went off in our game, explosions within 1500 units of it,
and any at a mine we watch, are the trap's (`Traps::Claims`): WeaponFire's explosion watch doesn't
report them as our player's, or they would go off twice. While a trap set off for a friend's goes off,
our copies near it stay ghosts (BlastShield, renewed each look). A mine picked up was not shared before:
the pickup pairing matches the activated reference's base (the mine's projectile) with the item added
(its weapon), which never match. `traps=` watched/known, `sent=`, `got=`, `live=`, `quiet=`,
`disarmed=`, `taken=`, `blasts=` (explosions left to the trap), `countdowns=` (mines a stand-in set counting, given the
walker's countdown); the dev channel's `traps` lists the watched ones, `traps <ref>` says how one was classified, and
`traps probe on|off` logs each watched mine's countdown, countdown sound and the actors near it each frame while it counts.

The Prydwen (src/game/ScriptDoors.h, with Vault 114's gear door; 2026-10-27, a Nexus report on 0.9.2 of a friend who
didn't see it arrive): its arrival is the story's. Out of Time's 130 (set when "Sanctuary" starts, Preston's talk after
the museum fight) enables a flyover effect (MQ201FXPrydFlyoverRef 0019537C, persistent, full LOD) whose script flies
the airship across the Commonwealth to Boston Airport over some minutes and at the end (or when it unloads, the player
waits or travels) sets BoSEnable's 15, which enables the airship moored at the airport (the marker BoSPrydwen
00184FD9, whose enable-parent children are the airship's parts), then disables itself. With `sStory = shared` a friend's game sets Out of Time's 130 too and plays its own flyover once its
player is outdoors; with `own` it doesn't (the stage is the story's: it also sends Preston's group off); either way a
game's moored airship appeared only when its own flight ended. So BoSEnable's 15 is shared through the session's quest
stages like the vault door's (reported by the game whose flight ends first, or by the host's at its welcome if moored
already; kept by the server for late joiners): BoSEnable is an internal start-game-enabled quest QuestSync never
shares, 15 only enables the marker, and a persistent marker is enabled from anywhere, so the friend's stage is set at
once, with no wait for loading (`Door::loaded` false), unless that game's own flyover is in the air (the effect 0019537C
enabled, with its 3D: `Door::effect`, `Look::busy`): then it waits, logged once, as the effect's script sets 15 itself at
the end of the flight or when the player waits, travels or goes indoors, and set sooner the moored airship would stand at
the airport while the flying one is still crossing (both players leaving Concord together). A friend whose story is behind sees the airship moored before
their own Brotherhood quests, as the game orders it anyway (the flyover follows Concord, long before Shadow of Steel,
which takes the airship as there; nothing in the game reads BoSEnable's 15 or the marker: the only conditions on
BoSEnable are Liberty Prime's scribes' packages, for its 20 to 40); their own flight later sets 15 again, which
BoSEnable (no repeated stages) ignores. The flyover effect itself isn't shared: its script plays once a game and never resets,
so enabled again after its flight it would hang in full LOD over the airport.

Remote changes are applied only while the player is in the world (no loading screen, not at the main
menu) and to loaded references; everything else waits and is retried.

| Feature | How |
|---|---|
| Deaths | TESDeathEvent -> ReportDeath; receivers run `<ref>.kill` when loaded. Of an NPC another game runs, the server believes the runner, the host's game about one dead in its save (found dead), and a player fighting it: one who reported its health, asked to take it over, or whose game ran it until it changed hands, within `Options::deathGrace` (10 s; the takeover itself is refused for `Options::takeoverCooldown`, 5 s, after the NPC changes hands or its runner claims it again with a hit). Our kill's death event asks to take the NPC over first (a grenade's blows name no attacker), and the host's found-dead check leaves an NPC still falling, or hit by our player lately, to its death event for up to 15 s (`NpcHold`). Others are dropped, with a log line at most every 10 s |
| Health | The game that runs an NPC reports its health (a fraction of max) after its hits and whenever it changes by anything else, at most every 250 ms (`HealthShare`: a fire, a burn, poison); receivers set their copy to it. A hit on a copy goes as the damage it did (`kReportDamage`, protocol 40: `CopyHealth::Book::TakeOurs`), which the game that runs the NPC takes off; only it applies it. Since protocol 56 (docs/HEALTH-SYNC.md) each such damage is numbered and the server sends it to the runner alone, by its own table; the runner answers it (`DamageAnswer`: applied, or refused and why; the server answers itself for one the session has dead, nobody runs, or the sender runs), and the hitter's copy keeps the drop until the answer comes: a report raises a copy only to the runner's word less the hits still unanswered (`CopyHealth::Book::InFlight`), however long, a refused hit is given back at once and logged (`copyAnswers=`), and one never answered after two minutes as lost (`copyLost=`; the server answers for a runner that leaves). The blow that kills a copy goes as damage too (`CopyHealth::Book::Fell`, `copyFalls=` booked/sent): at least all the copy stood at, so the NPC dies in its own game. A copy only down (essential down, bleeding out) is no fall, and one killed here for another game's word (the session's console kill) none of ours. Only living copies were checked before, so a killing blow sent nothing, and the copy's own death report, at the end of its fall, came too late for the server to believe once the fall outlasted `deathGrace` (a Mr. Gutsy killed by the host's companion fell for 10 s and stood on at 6 health in the game that ran it). Up to 0.10.0 a hit was written off a second after it was sent, so one dropped anywhere on the way was healed back by the runner's next report. Before, a copy reported its own health, which healed off the NPC whatever its game had taken since the copy last heard (a Molotov's fire). A game that comes to run an NPC (a takeover, a claim, back from a load) reports its health at once, at most 16 such reports every 250 ms (`healthShared=` sent/of them first readings): a takeover's claim leaves a frame ahead of the hit's damage, which the old runner then drops, and a host back from a death's reload runs its NPCs at its save's health. The server keeps the last report of each NPC hurt in the session and sends it with the world state to a game that joins or loads (protocol 57, `NpcHealths`, see Catching up). Only a throw's blow on a copy is left to the NPC's own game (`CopyHealth::OwnersToDeal`): an explosion's exactly when that explosion goes off in the NPC's own game too: our game decides once per explosion what becomes of it (`RemoteEdits::BlastLog`: reported by a throw's watch or a destructible object's, a destructible object's own that goes with its wreck, a world trap's that every game sets off, or ours alone), and a hit judges by the explosion that reaches it (`WeaponFire::BlastAt`, deciding one the 100 ms scan hasn't looked at yet) and the owner's player's distance from it by the receiver's own rule (`RemoteEdits::DealtThere`, `copyBlasts=` there/ours). Before, the hit went by the watch alone: a missile that hit a copy over 6000 units from our player, never reported, was dealt in neither game and healed back by the owner's next report, and at the watch's edge the hit and the scan could disagree. A throwable's too but never one with a weapon in hand that is no throwable, and the fire a Molotov leaves for 20 s after. In 0.9.0, as players reported it, gun hits on copies were taken for throws (how their hit events read on this runtime is not verified yet: `CopyWatch` now logs the flags), so only the game that ran an NPC hurt it, and a melee hit, which claimed it, moved that game to the hitter |
| Container loot | TESContainerChangedEvent player<->container -> resolved in the next frame on the main thread; shared moves (LootScope) -> ContainerChange; the server numbers it and echoes it to everyone, including the sender; receivers check the scope again and `removeitem`/`additem`, in order per container |
| World item pickup | TESActivateEvent (player) paired with container event old=0,new=player (its ref is 0) -> RefPickedUp; receivers disable the ref, only while it is in the world (a persistent item's ref stays itself in the inventory that took it, ours too: OwnGear kTakeLoose). One the engine names an inventory for (ExtraReferenceHandles: the local player's, a container's, an NPC's) is left there and the pickup dropped; one in no cell and no inventory (being loaded or moved) stays pending and is tried again for 30 s (OwnGear::PICKUP_WAIT), then dropped |
| The player's own gear | every inventory or equipment change the session makes for a friend's game is judged where it is made (src/game/OwnGear.h, Equipment::MayChange): stand-in dressing only on a stand-in, an NPC copy's only on a mirrored NPC, a frame emptied only if this game placed it and our player isn't in it, a pickup only off the world, loot never onto a stand-in, and none of them ever onto the local player (refused, logged, counted in `net status` as ownGear ourPlayer=). The counts start again with each session's welcome; the log takes up to 100 alarming refusals (our player, what they hold, the wrong actor, someone's own frame) and, on a budget of their own, 50 benign ones (gone, in an inventory, in no cell), so a join's catch-up can't hide the first. `inventory` (dev, read only) prints the player's stack and item counts, a checksum of every stack, the Pip-Boy (worn or not), caps and what is equipped, to compare before and after a join |
| Quest stages | poll each quest's done stages (`TESQuest::stages`, a stage's done bit; src/game/QuestStageLayout.h) twice a second for side quests (type 7), and the main story and faction quests (types 1-5, 8+, plus the type-0 faction finales Inst305, BoS302, BoS304, and the game's dialogue quests: type 0, no objectives, named Dialogue<Place> after any DLCnn prefix, src/game/QuestFilter.h; Diamond City's entrance scene opens the gate at DialogueDiamondCityEntrance 32) only with `sStory = shared` (Welcome::sharedStory; the value is read in any case, `Shared` too, and the log says which story the host shares: Config's "loaded" line and the server's "listening" line); never misc, never the other type-0 quests (random encounters, companions, tutorials, dungeons), unless the host sets `sQuests = safe` (default `story`; only with `sStory = shared`, whose gate those quests pass as the story's; the welcome's `SessionRules::quests`, protocol 58; F4MPServer's `--safe-quests`): then the misc and type-0 quests with objectives that src/game/QuestFilter.h SAFE holds are shared too, 98 (Fallout4.esm 46, Far Harbor 38, Automatron 1, Nuka-World 13: Diamond City's and Goodneighbor's errands, the quest givers' pointers, Far Harbor's own quests, Nuka-World's DLC04MQ03 and DLC04MS01), generated by tools/questscan.ps1 -Safe from the game's files by a rule: a quest of the game's masters with objectives, no radiant (RadiantRules.h), no PER_GAME and no companion's by its name, run once or started with the game, every alias fixed (forced, unique, another quest's, filled by its script, or made at or found from one of those; none from the story manager's event or by its conditions), and no stage that, in its own fragment, a script function it calls (its quest's script through kmyQuest, a parent's, a script a property or local is typed with) or a stage it sets or starts of a quest no friend's game is sent the stages of: not shared by its type, or a radiant or PER_GAME quest (walked as -Moves walks them, and a call through a variable typed with the script of one quest is that quest's; a shared quest's stage reaches a friend's game as the story's anyway), names a companion's affinity (an argument named *Affinity* or CA_*, a call named *Affinity*), calls SetCompanion, DismissCompanion, AddToPlayerFaction, SetAvailableToBeCompanion, SetPlayerTeammate, AllowCompanion, DisallowCompanion, ClearCompanion, ReleaseCompanion or Dogmeat's, or sets a stage of a companion's quest, calls SetEnemy, SetAlly or SetPlayerEnemy or puts an actor in or out of a PlayerEnemy or PlayerAlly faction, calls SetOwnedByPlayer, calls RemoveAllItems or UnequipAll on the player, sends a story event (SendStoryEvent: the story manager starts quests from it with each game's own picks), or starts a quest each game runs its own way, by Start or by setting a stage of one the game doesn't start itself: a radiant, PER_GAME, a repeatable quest or one with an alias each game fills (a friend's game would start it there, unshared: DN054 starts the repeatable DN054Radio, DN123 a Skylanes assault, BoSM04Postquest DN053, DLC04DialogueRaiderBossOperators the PER_GAME DLC04RaidCoerce, all four left out so); a fixed, run-once quest it starts runs in the friend's game as the friend plays it, its started stages walked as any (it does not see a scene a stage starts, a timer or event a script waits for, a call through a variable whose type has no script, a quest matched by no property name, or anything else a script changes: MQ00MamaMurphy 700 kills her, a death every game shares anyway); less QuestFilter::DENIED (MQ101, DogmeatQuest, HC_Manager, CZ_Arena, Nuka-World's park bookkeeping: DLC04PowerTracker, DLC04ParkPopulationHandler, DLC04CollectVassalIncomeMiscQuest, its raiders' kickout DLC04RaiderKickout and its outpost count DLC04MQ04_RaiderOutpostTracker; the rule leaves out the kickout, MQ101 and DogmeatQuest by itself, so DENIED is a backstop for them). So the rule leaves out CZ and FFGoodneighbor07 (an ally faction, SetEnemy; Cait and Hancock made recruitable), DLC04MQ05 (DLC04GangSetup turns the gangs on the player and takes their settlements), MS07cPostQuest (a stage of Nick's companion quest), DLC04DialogueHubologists (rads and the extraction chair on the player, their donations and rank bonuses), DLC04DialogueHarvey and DLC04SafariAdventureQuest (each starts a quest each game runs its own way). By the maintainer's choice, the quests with objectives whose stages turn a faction hostile or friendly, make the player a faction's enemy or give or take a settlement are shared with the story when nothing else leaves them out (tools/questscan.cs ALLOW_OUTCOMES, 10: DN036 and DN049; Far Harbor's FarHarborS01, S02 and FF01, Acadia_Institute, ADV017 and CoA_DialogueNucleus; Nuka-World's DialogueSettlement and KiddieKingdomMain): one player's outcome is both players', and with sQuests = story each keeps their own. Their stages that make the player an enemy (SetPlayerEnemy, SetEnemy on PlayerFaction, a PlayerEnemy faction) are StageRules rows kMoves, as a stage that moves the player: set only with the friend who did it near, skipped for a far one (none is a bridge); the objective-less ones (the kick-outs, DN117's settlement) are not shared yet. Their stages that move the player or take from them are StageRules rows as the story's (tools/questscan.ps1 -Safe -Rules: DLC04MQ03's flag poles, DLC04MS01's Nuka-Cola Quartz, Diamond City's caps), and those that set another quest's are in SETS_ANOTHER (-Safe -SetsOther); an item one gives is given in each game, as for every shared quest; and none of their stages sets, through its fragment and the functions it calls, a stage that pays (a kPays row): -Safe walks them, and every other shared quest's stages, and StageRules::SHARED_REACHES_PAYS is 0 (MS13MarowskiRevenge 380 sets its own 400, which pays only if 380 is not done). Every game watches them whatever its session's sQuests, so what was done is known when one that shares them begins; a report of one is sent only into a session with both (StageLog::Outbox's safe mark). With them, the quests the rule passes but for their objectives (QuestFilter::SILENT, 106: Fallout4.esm 70, Far Harbor 19, Automatron 5, Nuka-World 12; a place's own quest such as DN035 or DN142, the Castle's waves DN029*, the Prydwen's arrival BoSEnable, the add-ons' fights; the change-location hooks that send a Minutemen request's story event, DN121_ChangeLocation and DN151_ChangeLocation, or start the repeatable DN017 or DN053 are left out by the same rule), shared as a dialogue quest is (QuestSync's Unordered: started here once running or with stages done, a stage past only once done here, dropped once it ran and stopped, never named in a notice, no "completed" report), less those tools/questscan.cs denies by name (its DENY_PATTERNS, each with a reason: the game's systems, achievements, patches and tutorials, items and holotapes it hands out, the player's own state, debug quests, ambient chatter and scenes, companions', bookkeeping other quests use, random encounters, workshops) or by hand (its DENY_SILENT, 29, each with why, read in their fragments and the scripts these call: teleports of the player or their companion (RRAct3Pickup, InstituteTeleportEffects, DN136_Attack, DNPrime_BoS304, MQ302Post), a companion's affinity or greeting, the player's Brotherhood rank or expulsion, factions' hostility to the player (Nuka-World's Galactic Zone, Vault 118's Pearl), gear or tokens of the player's own (Kent's costume upgrades, the Gauntlet, the arcade), the memory lounger, caravans, the Minutemen's bookkeeping and DN151's tracker, Goodneighbor's gate guards whose each game's timer ends the event where its player isn't), those whose stage stops the quest below a stage that does more (MS07Intro: its 350 stops it below its 400, which starts Nick's case files; a friend catching up is sent the host's stages done before the session lowest first, as the game keeps no order of them, so the 350 would stop it there first), and those with no stages; their stages ScriptDoors works (DN142 50, BoSEnable 15) stay its own with either setting and QuestSync never reports them. A quest's own actors made at a marker (a "created" alias) are made in each game, as before. Their rows in StageRules (a few waits, DLC03MQ02Fight's quickstart, Grandchester's pre-war money and ticket, kPays) and SETS_ANOTHER (76 more: MS11's that start the Castle's waves, MQ106 900's BoSEnable 10) come from the same scans, and the payment walk covers them: SHARED_REACHES_PAYS stays 0. What their stages give (a fight's prize, a reward's caps) both players get, as a shared quest's reward, and a companion reacts to what one does (a script on the quest, CA_QuestStage_Bump, which the scan does not read), as to the story's and SAFE's. A catch-up sets each quest's stages lowest first, so a stage that branches on another being done (DN009_MQ205's GetStageDone) sees them in that order, as a dialogue quest's do. And never the side quests the story manager starts with targets each game picks itself (src/game/QuestFilter.h PER_GAME, from tools/questscan.ps1 -PerGame: a shared-type quest with objectives started by a story manager event or quest node, with an alias filled by its conditions or from the event; 21 besides RadiantRules' radiants: Far Harbor's DLC03WorkshopRadiantOwned01 and 03, Automatron's nine DLC01MQPostQuestRadiant*, Nuka-World's four DLC04Raid* (RaidCoerce takes the raided settlement from the player) and six DLC04_RQ_* (StealCache and DefendCache make their attackers and defenders at runtime and roll the enemy gang in each game; KillRivalBoss and CollarRivalBoss wait for a check that the player took the raiders' path, tested on a raider save: DLC04_RQ_Manager runs in every save with Nuka-World); four it listed before, DN121 and DN151 and DLC03WorkshopRadiantOwned02 and 04, whose aliases are the game's own, are RadiantRules' radiants now, shared by their run and found by their file and form ID there, so an older game ignores a run of them that a newer one shares), whose stages a friend's game set here started them with this game's own targets (up to 0.11.0): each player's own, a friend's stage of one dropped with a log line once per quest (`not setting ... the story manager picks its targets in each game`), `quest` dev commands calling it "radiant (each player's own)". A dialogue quest has no objective to start it: it counts as started once running or with stages done, only a stage done here is past it (its stages aren't in order: a game at the gate has 400 without 32), and once it ran and stopped it is over (the rest of it from friends is dropped; starting it again would play it over); its name is the editor's, so no "caught up" or "completed" notice names it. A quest's done stages are read out again only when their digest (how many, and a mixed sum of their indexes, read off the quest without a list: StageLog::Digest) changed, or a stage was set on it here since, not at each poll of every quest watched. Every newly done stage is reported, ascending, not just the current one: a stage whose script moves the quest on at once (MQ104 210) is never current. 5 s settle after load, only of quests the reporter has started. Receivers queue them (src/game/StageLog.h), only while in a session (dropped when it ends, except a lock's quests: those are set after leaving too, with the story the player's own again, so a teleport begun in the session finishes), skipping indexes the quest doesn't have, and `setstage` them in the order they came: per quest the lowest waiting stage above their own, the next once it reads done (5 s timeout), up to 4 per poll, once they have started the quest too: an objective has been shown (`BGSQuestObjective::state` not dormant). Running isn't enough: a new game runs many side quests in the background from the start. Until then the stage waits (another quest's stage may start it: MQ104 210 starts MQ105) without holding up other quests. In a session that wait is only for a quest the session doesn't share: a shared quest not started here is started by the first of its stages, once every stage that came before it is set and START_GRACE after the last one read done, and one that ran here and was stopped is never started (StageLog::Quest::kStart, Wait::kStopped; since 0.8.6). The host's game reports every done stage of the shared quests its player has started when its own server welcomes it, so a joiner is caught up to the host's story; a joiner's own progress from before joining is never reported. A quest is done with once completed and stopped (many complete at a stage and run on: MQ102 65). A stage reads done before Papyrus runs its fragment, so after a stage whose fragment waits (Utility.Wait, moving the player) nothing that came after it is set for that long plus 1 s. src/game/StageRules.h, generated from the QF_ fragments of Fallout4.esm, DLCCoast.esm, DLCRobot.esm and DLCNukaWorld.esm (following the stages each one sets; Automatron's and Nuka-World's rows are tools/questscan.ps1 -Rules's: DLC04MQ01 1000 walks the player through the Nuka-World station's stairwell door, the quickstarts move them, their stage 0s, none a start-up stage, left out as MQ203's): stages that move the player (MoveTo, a door they are made to use, FadeOutGame, an InputEnableLayer) are set only when live (came in the last 30 s, not from the session state) and the friend who did it (the server stamps the reporter's id on kQuestStage; a WorldState carries none) is within 6000 units, else skipped, unless they also start another shared quest or award an achievement (MQ206 500 and 1000, the MQ206 faction 1400/1500): then after a 10 s HUD warning, outside combat; MQ206 500 and the 1400s (controls off) only once 1200 (controls on) is waiting too, and until then only their own quest waits (Verdict::kQuestWaits), not what came after it. After a lock stage is set, its quest's stages go first, without a combat hold or another warning and not held up by other quests', are kept if the session ends, and 1200 is set outright once they have done nothing (none being set, no script of theirs running on) for 90 s of unpaused play (StageLog::Queue::Lock). The release is looked for before each poll of the queue: after leaving, once 1200 is done the session's other waiting stages go, so nothing past 1200 is set in the solo game, nor left for the next session. While the game is paused (`UI::menuMode`, a loading screen) nothing is set and holds and timeouts don't run down. Stages that take caps or ordinary items from the player (MQ105 177, FFDiamondCity10 40, ...), and the dialogue quests' payments and sales (DialogueBunkerHill 210, DialogueVault81 5 and 645, DialogueDiamondCity 70, DLC03DialogueFarHarbor 90), the pre-war stages that set MQ101's (MQ101TVStation 150/200, MQ101SanctuaryHills 91/500: its start-up stage empties the inventory), Nuka-World's raids' caps (DLC04RaidChaseOff 90, DLC04RaidCoerce 90) and DLC04MS02 500 (unequips and takes the space suits), are never set (tools/questscan.ps1 lists the stages whose own Fragment_Stage_ function calls RemoveItem, RemoveAllItems, SellItem, RemoveItemByComponent, UnequipItem, UnequipItemSlot or UnequipAll directly on Game.GetPlayer() or a PlayerRef property: of the shared quests only quest items, caps and chems; RemoveAllItems only in MQ101, the opening, and DN019JoinCult, both misc quests and never shared. It does not see such calls made through the quest's script (kmyQuest), a player alias (Alias_Player.GetRef()), a scene, a dialogue line or another script a stage starts, nor a shared stage setting a stage of another quest, so it is no proof that no shared stage takes the player's gear; src/game/StageRules.h); with `sStory = own`, side-quest stages that move a story or faction quest on (Min01 220/1500/2000, FFGoodneighbor01 100) aren't either. A friend's purchase of Home Plate (DialogueDiamondCity 64 and 300, Geneva's and the mayor's, and FFDiamondCity02 10 itself, which takes the price, ServiceCostBuyHouse, and gives the key, the house and its doors' owner and map marker) is set with that price at 0 (StageRules kFree and PRICES: the global is 0 from just before the stage is set until the key is in the inventory, or 5 minutes of play (a lagging Papyrus queue can run the script late), then its own value again, and its own value before a save, when a load begins and when a main menu opens (StageRules::PriceWaiver, game-free and tested: a waiver is never dropped without its price put back), so no save holds it at 0, and a second price is not waived beside the first; one already bought here is set as it is): every player in the session has the house, only the buyer pays (up to 0.13.1 every game in the session paid the price again). Caught-up stages wait during combat and 5 s after (at most 60 s), so Party doesn't take kill XP for quest XP. Never during dialogue; the player's own scene holds them back, but one of a quest with stages waiting only for 30 s. Not a scene a friend's stage started here: one of a quest a friend's stage was set on here in the last 30 s, or of the quest of such a scene that ended in the last 30 s, and not first seen with the dialogue menu open (the player started it by talking), is the session's for as long as the player is in it, a dropped connection or a load too (src/game/SceneHold.h, `QuestSync::SessionScene`; the shared quests' stage fragments start 502 scenes of their own quest and 18 of another's, those the player's own here). Codsworth's talk (DialogueCodsworthPostWar 50) starts his search of Sanctuary's houses, the scene MQ102_CodsworthSearch01 with Codsworth and the player as its actors, which only Out of Time's 40 ends (it sets his 100, whose script stops the scene): held back by that scene, the friend's stages never came, and the host had to follow Codsworth through the search themselves (up to 0.9.2). Its people aren't claimed for it either (`Dialogue::Holds`: Codsworth stays the friend's, mirrored here), and it doesn't refuse a teleport to a friend. A menu a friend's stage opens here by itself is closed again (src/game/CatchUpMenus.h): a stage that makes an NPC our companion (FollowersScript.SetCompanion, Getting a Clue's 520 with Nick) dismisses ours with the settlement list (Workshop_CaravanMenu), and one that gives our player a FeaturedItem (the Broadsider at MS11 900, Hole in the Wall's cure at its 800) shows the engine's featured-item inspection (ExamineMenu with VignetteMenu, which only Enter closed); either paused the game and the catch-up until the player answered it. Each is closed the way it takes. The inspection gets a hide message, which takes the vignette along. The list is a script's question (DismissCompanion waits on OpenWorkshopSettlementMenuEx, a latent call), and only its own answer ends the wait: hidden, it left that script suspended for good, in the save too (the new companion never joined, the old one never left), so it is never hidden (MenuMessages refuses it) but sent its own cancel, a Tab press and release handed to that menu alone (src/game/MenuInput.h: its ShouldHandleEvent and OnButtonEvent, which hand the swf's ProcessUserEvent the Cancel), as the player's Tab closes it: the swap finishes, the new companion follows and the dismissed one goes home (Escape doesn't answer it: the pause menu opens over it). Such a menu is the stage's when it opens within 30 s of a friend's stage set here and our player activated nothing (TESActivateEvent) and had no menu of their own up (a conversation, workshop mode, the Pip-Boy, a container, a terminal, a workbench, the console or the pause menu) since, nor shortly before: 5 s for the inspection, which the engine opens on the spot, 30 s for the list, which the player's own scripts (a dismissal in a conversation, a supply line) open as late as a stage's. A vignette is judged only once the inspection under it is gone, and one over an inspection left to the player is theirs too. It is closed once it has been up 0.5 s, and one still up 3 s later, or whose close couldn't be sent (the list taking no input), is left to the player (`net status`: menusSent=, menusCancelled= (lists gone after their cancel), menusHidden=, menusGaveUp=, menusLeft=; each close is logged with the stage before it and how long before it our player last acted). One "Caught up" notice for many quests. Every poll in a shared-story session, a player whose MQ104 is past 210 without 210 done and whose MQ105 never started (left so by the old highest-stage-only sync, or by catching up from its session file) gets `setstage 229E6 10`, judged as MQ105's stage 0 (it moves the player): after the 10 s warning, outside combat, not in a conversation or a menu. Stage reports made while not welcomed (before the welcome, during a reconnect) are kept (the first 1024: what came first starts what came after), the story's and factions' too although Story::Shared is false while the connection is down (Session::OnDisconnected), marked so they go only if the welcome shares the story; they are sent once welcomed, at most 200 a second, oldest first (the server drops a player's events past 500 a second, all kinds together, and a stage is reported only once) (StageLog::Outbox). Ones made in no session (playing alone after leaving or stopping hosting), kept for a server the player then left for another (an invite accepted while the last host was being retried: Session::SwitchServer calls QuestSync::OnServerChanged), or kept through 5 minutes of not being welcomed (the host quit and the player plays on alone) are dropped, so a session joined later doesn't take solo progress for a friend's; loading a save drops them. Reporting doesn't settle again after a reconnect. A reconnect drops the waiting stages and what came live (OnSessionEnded): a friend's step that moves the player and was still waiting (a fight, a conversation) comes back in the WorldState as caught up, not live, and is skipped even beside that friend. Within a quest stages are set in stage order, not in the order they were done: one done after a higher one is already settled here and dropped (MQ104's 17 sets 12: 17's own script sets 12 here too). Different quests' stages go side by side, except a stage whose script sets a stage of another quest (StageRules::SETS_ANOTHER, from tools/questscan.ps1 -SetsOther: DialogueCodsworthPostWar 50 and 200 set MQ102 30 and 50, MQ102 40 sets DialogueCodsworthPostWar 100): it waits until nothing that came before it is still to be set or running on (as a quest's start does), else it moves the other quest past its stages still waiting and those are dropped as behind it; 0.9.1 lost a friend's Codsworth search (MQ102 20-25 and 36-40) that way when it was caught up all at once. Not a lock's quests. The opening's stages (MQ102 below 10, all of DialogueVault111) are never reported, baselined or set, and while this game is in the opening no stage that moves the player is set, its quest waiting (see The opening; a friend still in it holds nothing in the others' games); the endings' fades and slides (MQ302 850/855, MQ00 50/60, Inst308 30) count as moving the player. So does a stage whose script moves the player only through its quest's own script or a stage of another quest it sets or starts, shared or not (tools/questscan.ps1 -Moves follows both, and a branch on a literal argument one way): Dangerous Minds' lounger (MQ202 300) sets MQ203 10, whose MQ203Script.StartQuest fades out and moves the player into Kellogg's memory, and MQ203 1000 moves them back to the lounger; caught up from the session they sent a joiner into the memory (up to 0.10.1). The same for the Memory Den's own memory (FFGoodneighbor01 40), DiMA's memories (DLC03MQ04_01 1, DLC03MQ04_05 350), Vault 118's date (DLC03_V118_Quest 548), MS02 17 and MS19 60 (the controls frozen) and the opening's MQ101TVStation 150/200 and MQ101SanctuaryHills 91/500 (they set stages of MQ101, whose start-up stage would start the pre-war opening again). MQ203's and MQ204's stage 0 are left out: debug stages, never set in a game played through, by which every stage that starts their quest here is judged, so with them The Glowing Sea would never be caught up. A fade the session caused that nothing fades back in is faded in after 20 s of play (see Fades). Each load logs whether the game awards achievements (`TESDataHandler::CheckModsLoaded`) and Steam's NEW_ACHIEVEMENT_n_0 list; new unlocks are logged after stages are set (src/game/Achievements.cpp) |
| Doors and locks | refs the player activated are watched for 5 min (a door an NPC our game runs opened, 30 s; every loaded plain door, quietly, while it stays loaded), and what they are linked to (a terminal's door: an entry still holding the target's form ID is looked up by it, a pointer is read guarded and taken only if the form its own ID names is itself, `LinkedRefs.h`); open/lock changes -> RefState; receivers `BGSOpenCloseForm::SetOpenState` / `REFR_LOCK::SetLocked` |

### Giving items (src/game/Drops.cpp, src/game/DropBook.h, common/DropStore.h)

Container loot can't hand a friend an item: a weapon or armor put in a chest stays in the giver's game
(above), workbench storage is each player's, and what is shared there moves by base form and count with
no one deciding who got it, so two players taking at once both get it. A dropped item is a reference
the giver's game made at runtime (0xFF......), which no other game has, so it was never reported at all.
So players give each other items by dropping them, and the server decides who has each one:

- The giver drops an item (the Pip-Boy's Drop). The container event (old = the player, new = nothing)
  names the reference it became, or, with no reference named and the Pip-Boy or the console open, it is
  the newest runtime reference of that item near the player not seen before, only when the player's count of
  that base fell by just what the events said left since the frame before and no equip event of it came
  (food eaten or a stim used in the Pip-Boy is one: `DropBook::MayScan`, `notScanned=`), and never a reference
  another system keeps (SaveGuard's, Spawns'). An item the player picked up from where a plugin placed it
  goes back into the world as that same reference (the engine keeps it with the item), not a runtime one:
  it is the drop, whether the event names it or the scan finds it, only if the player picked it up in this
  session (WorldSync's pickup report) and hasn't dropped it since (`DropBook::MayBeOurs`, `DropBook::PlacedPickups`,
  noted only while welcomed, forgotten on a load and when the session ends; `droppedPlaced=`). One picked up
  before the session is not given: a friend's world never heard of that pickup and still has the item where
  the plugin placed it. Up to 0.11.0 only runtime references were looked at, so such a drop was never given.
  On the offer it makes way by OwnGear `kOwnDrop` (a reference made here, or a plugin's with that note;
  the copies' `kDropCopy` stays limited to references this game made) and is disabled rather than deleted
  (`DropBook::HowItMakesWay`): its record is the plugin's, and a load would put a deleted or unsaved one
  back where the plugin placed it.
  Such an item dropped and not swapped (not given, refused, or dropped outside a session) lies in the dropper's world as
  the reference the session keeps as picked up, so their next welcome or load would have taken it away as a friend's
  pickup, for good (up to 0.12.0). The server keeps who picked up each item (layout 43 of its session file) and sends no
  player their own pickups in a world state: the picker's game has the item already, in an inventory or lying where they
  dropped it. A save of theirs from before the pickup has it where the plugin placed it, theirs to pick up again (a
  second pickup report of it changes nothing); two characters of one PC share a player ID, so the other character's
  world keeps it too. A file before 43 knows no picker: its pickups go to everyone, as before. Only the first picker
  is kept: two players who pick up the same item before either hears of the other's pickup, and the second drops it
  un-swapped, still lose it there (rare). And a picker who gave the item away and loads a save from before the pickup
  has it again, as with container changes (`ownSkipped`): one more copy.
  Once it has come to rest
  (`DropBook::Settled`: at least 1.5 s, then still for 0.5 s, at most 5 s) it is reported (`kReportDrop`)
  with a random number of the giver's game (the nonce), its mods and template combination (read as
  Equipment reads worn gear, so a legendary effect travels as the mod it is), a power armor piece's
  condition, a name the player gave it, and where it lies.
- Not given, staying on the giver's ground in their game only (said once a session): keys, notes,
  holotapes, books, bobbleheads, magazines (everyone collects their own), quest items, an item whose base
  or a mod was made at runtime (no other game has it: `DropBook::Judge`), more than one reference holds.
  Caps can't be dropped in Fallout 4: they still go through a container (shared as before).
- The server numbers it and offers it to the giver's game alone (`kDropPlaced`), or refuses it past the
  session's share (1024 lying about or offered, 256 a player): then the giver alone hears it with no
  number, and keeps the item as an ordinary one. On the offer the giver's game swaps its item for a copy
  like everyone's, records the swap (the nonce: `made`) and says so (`kClaimDrop` kPlaced); only then does
  the server tell the other games, so nobody can take an item still in the giver's world. It tells the
  giver's game too: until that comes the drop is unconfirmed (`DropBook::Unconfirmed`, `unconfirmed=`), and
  a giver who leaves the session first (the link dropped with their kPlaced, whose offer the server forgets
  as they leave) gets the item back in their world as an ordinary one, where it lay or else in their
  inventory, and their record no longer says it left (`restored=`). A giver who
  picked the item up again before the offer came says kKept, and it never lay anywhere. An offer is never
  taken, listed or saved; one not answered goes when the giver leaves, and the giver's next record
  settles one a load overtook (its nonce in `made`: laid down; not: gone). Each game places its copy where the item
  lies while its player is within 5000 units (removed past 7000): the same base, built with the item's
  mods as Equipment builds a stand-in's (or the base's default combination for a plain one), the count,
  condition and name. A game that lacks the base or one of its mods places none (an item with a mod
  this game hasn't would be another item).
- A copy is placed disabled; its activation is blocked by script (`ObjectReference.BlockActivation`) and
  it is shown once the VM says the call has run (its stack callback); one not blocked within 5 s is
  removed and placed again (`blockLate=`). Pressing Take on it only asks the server
  (`kClaimDrop` kTake; one claim per item until answered, again after 5 s). The server grants the first
  claim and tells everyone who took it (`kDropGone`): the taker's game picks its copy up as the player's
  own take (`ActivateRef` with the default processing only, which a block doesn't stop) and checks that
  the copy left the world and the count of its base rose by the drop's count (`DropBook::JudgeTake`),
  and SaveGuard lets go of the copy's id at once (`SaveGuard::Release`: the game hands it out again, often to
  the next item of that base the player drops, which would otherwise read as another system's and not be given);
  every other game removes its copy. A later claim is answered with who has it. A take the game couldn't
  finish in four tries is given back (`kGiveBack`) and lies there again for everyone; its copy goes and
  a fresh one is placed. Taking your own item back is the same claim. Nothing is ever added to the local
  player's inventory on a friend's behalf (OwnGear `kDropCopy`: only references this game made, loose in
  the world); one thing is taken from it: a copy picked up past its block (`unblocked=`) whose claim
  someone else won goes out of the player's inventory again (the stack with its mods and name;
  `takenBack=`, else `dupRisk=`).
- Reports and claims are not counted against the server's event budget (a dropped one would leave an
  item in two inventories or none); the list answering a record goes after a welcome and then at most
  every 10 s (up to ~385 KB).
- Copies are never saved: they go before every save and load (with the stand-ins) and are placed again.
  What a world holds of the transfers is the co-save record `DROP` (v1, `DropBook::Record`): per session
  (the last 8), the nonces of the drops whose item left it and the drop numbers it took in. It goes to
  the server after every welcome and after a load that stays in the session (`kDropRecord`, answered at
  most 4 a second), and the server makes the session agree with it (`DropStore::Reconcile`): what the world
  holds (an item it took, or its own drop whose nonce it lacks: a save from before the drop) is that
  player's and lies nowhere; what the book had them hold but the world lacks (a save from before the take,
  or after a drop it had withdrawn) is laid down again for everyone. Each load is judged on its own, so
  loading the wrong save first and the right one after leaves the book as the right one says.
  A loaded save without an entry for the session it is played in gets a fresh one, so the transfers
  made after the load are recorded. The answer is the list of what lies about (`kDropList`). A record of another session counts as
  empty. What lies about and who took what is kept in the session file (layout 33; 30 in the drops' own test builds), so it survives the
  host's server restarting.
- Whose world (0.10.0): only one character's saves hold what its world gave and took. Each world has a key of
  its own (`CharacterBinding::World`), kept in its saves (co-save record `CHAR` v1, 8 bytes, beside `DROP` so an
  older build reads that as before) and said last in its `kDropRecord` (`DropRecord::character`; a drop with none, from a session
  file from before, is any character's). The engine's character ID can't be it: every coc character from the main menu has 0, and a
  new game has the last character's until named. A new game gets a fresh key; a save its own, or, from before
  keys, one made from its character's ID (all that character's older saves agree); a world nothing loaded or
  started (coc from the main menu) a fresh one when first asked. The server keeps on each drop the characters
  whose worlds it left and went into (`ownerCharacter`, `takerCharacter`, never sent), and a record of another
  character's world lays down nothing an older character took, takes back nothing it gave (its drops lie on) and
  leaves its gifts' offers waiting (`DropStore::SameCharacter`). So a player who starts over doesn't get back
  what their old character took (found on the rig: a host restarting as a fresh character, 4 items became 6),
  while that character's own save from before a take still does. The characters are saved with the session
  (layout 40, once it holds a gift or any drop whose characters are known: a loose drop's taker forgotten with a
  restart would have their new character lay it down again); a drop a 39 file brought has none (as before, any
  character).
- A weapon or armor put in a container tells the player once a session that friends won't see it there
  and to drop it near them instead (not for a put in a gift's place with the host's `bContainerGifts` on: see Giving
  through a container).
- Workshop mode can scrap (or store) a loose item lying in a settlement, copies included: the player gets its
  components and the game takes the reference away. That is our world consuming the item, so it is our player's
  take (`DropBook::Scraps`; no new message). Only the workshop's destroyed event (`Drops::OnScrapped`) makes it a
  scrap: a copy merely found gone, in workshop mode or not, is taken for lost and placed again
  (`DropBook::JudgeCopy`). Its drop is claimed (`kClaimDrop` kTake), recorded as taken at once (a link lost before the
  answer is settled by the next record), never given back to our world as we leave, and not placed again; the claim
  is never refused for want of room. Granted, every friend's copy goes and no list or saved session lays it down
  again (`scrapped=`). Lost to a claim that came first, the item is that friend's, our record forgets it
  (`Record::Untook`), and what the scrap put in the workbench is taken back out of it (`scrapLost=`,
  `scrapTakenBack=`): the things that came into that workbench from nothing while workshop mode was up, within
  0.5 s of the event, each given to the nearest scrap of it in time so our player's own scraps beside it keep theirs
  (`DropBook::ScrapYields`). With nothing seen coming in, our player keeps the components (`scrapShort=`). Our own
  drop scrapped before the server offered it is gone, not swapped for a copy (`kKept`). Neither kind is reported as
  a workshop piece of ours.

Left open, counted in the log and `net status` (`drops:`): a giver who reloads a save from before a drop
a friend already took has the item again (the server can't take it back from the friend's inventory:
`dupRisk` on the server's log); a giver whose kPlaced reached the server but who left before hearing it
laid down gets the item back too, and has it twice only if a friend took it in that moment, before the
giver's next record withdraws it (`dupRisk` again); items lying
about are the host's session's, so they are gone if the host starts another session or loses the session
file; a host crash between a drop and the session file's next write (it is written at once after every
change) loses it. A world that never played the session (a new game in the same character's slot, as the
rig's `coc` from the main menu makes) has no record of it, so it is taken to hold its player's earlier
drops: those are withdrawn. The server trusts a game's record of its own world, as it does its container
record.

### Giving through a container (0.10.0, the host's `bContainerGifts`, off by default; welcome flag bit 64)

(src/game/GiftBook.h, src/game/Gifts.cpp; the server's side is the drops' own, common/DropStore.h.) With the host's
`bContainerGifts` on, a weapon or armor a player puts in a container is given, as a dropped item is: it is a drop whose
place is that container (`DroppedItem::holder`, a session reference, count 1), and the server does for it what it does
for any drop (offered to the giver alone, laid down on their kPlaced, the first claim granted, the record after each
welcome and load making the session agree). Each game shows the gifts lying in a container while its player has that
container's menu open, and only then.

- **Where, and what.** A gift's place is a container (`LootScope::Holder::kContainer`, a session reference): not a
  settlement's workbench (each player's own), a merchant's chest or a companion, an actor or a power armor frame.
  Only weapons and armor, each given on its own, and never a quest item or one with a mod made at runtime
  (`DropBook::Judge`, with the drops' other refusals), nor one with more mods than the wire carries: those stay in
  the giver's copy of the container, said once a session (`gifts.not_given`). Items of one class (base, mods sorted,
  the player's own name for it, condition to the percent: `GiftBook::Class`) are the same thing; a copy and the
  player's own item of its class are told apart by count. Shared storage never carries weapons or armor (LootScope's
  `kWeaponArmor`), and the container loot never shares them, so a gift never meets their copies: with
  `bSharedStorage` on, a gun put in the host's chest is given just the same as with it off.
- **Giving.** WorldSync's event watch posts the player's moves between their inventory and a container
  (`Gifts::OnMoved`, ahead of the barter filter, so a take in a trade's last second isn't lost): only those at the
  container the view watches are kept, summed by container and item (`GiftBook::Inbox`), so a Take All of any number
  of kinds loses none (0.10.0's first cut kept 256 moves a frame, and a gift's take past them stayed unclaimed). A
  change the open container then shows (one more of a class than we knew of, with a put of its base waiting:
  `SIGNAL_KEEP`, 1 s) is reported (`kReportDrop` with its holder and a nonce); a put whose event comes late or never
  isn't (it can't be told from a script's), and stays the player's own. Nothing is read off the container before the
  inbox of moves is (every entry point drains it first). On the offer the item leaves the giver's world: taken out of
  the container, or, while the menu still shows it, left there as the giver's copy of the gift (nothing moves on
  screen; it goes as the menu closes). Then the record says it left (`made`), and kPlaced goes. A take of the
  player's own of a class whose put still waits for its offer takes that put back (latest first): its offer is kKept,
  so a like item the container held before is never given in its place. An item no longer there, or a container not
  loaded, is kKept too: it never lay anywhere. An offer that comes past 60 s (`PUT_FORGET`) or after a load is kKept
  too, unless the record says the item left. A refused one (`bContainerGifts` off on the server, the session's share
  full) stays in the giver's container.
- **No give-back on a link.** Unlike a loose drop's, a gift's offer survives its giver leaving and is saved in the
  session file (takerId `DROP_OFFERED`): the giver's game gives nothing back when the link goes, so only its next
  record says whether the item left its world (laid down, or the offer gone). Nothing anywhere is given back or taken
  again on a timeout alone.
- **Taking.** Fewer of a class in the container than we knew of went to the player as far as their take's events say,
  or, an event lost, as far as their own count of that class rose since the last frame (`unsignaled=`): those take a
  copy first (so it is claimed), then the player's own. A copy gone anywhere else (another mod's script handing it
  to a settler) is counted (`copyMissing=`) and that gift isn't shown again in this game until a load or a rejoin (the view's
  memory of it goes with the session's), so such a script makes one item of each gift a load or a rejoin, not one a
  frame; across saves and rejoins they add up. The taken copy is held back: taken out of the player's
  inventory at once (OwnGear `kGiftTake`, only ever our player and only for their own take), claimed (`kTake`, asked
  again every 5 s until answered, never settled by a timeout), and added to their inventory only on the grant (with
  `Took` in the record); a lost claim needs nothing undone (`gifts.taken_first`). So the side the player can't hide an
  item from (the take held back) is the one put right, never an item already in their inventory. Taking one's own
  gift back is the same claim. A grant to a take the world no longer makes (after a load) is given back
  (`kGiveBack`), and so is one that can't go into the inventory (`giveFailed=`).
- **Copies.** One item of each gift lying in the viewed container (at most 64 at once, and never more than can be
  claimed: 128 with the takes waiting), with its mods, condition and the player's name (`Drops::AddOneTo`, as the
  drops' copies are made); none of a gift whose base or mods this game lacks (`unplaceable=`), nor of one that isn't a
  single weapon or armor of this game's with plugins' mods (`GiftBook::Receivable`, `notGear=`): the server can't tell
  a gun from a box of ammo, so a modified game's "gift" of anything else would be an item no view sees, left in every
  friend's copy for good. An add that fails after putting something in all the same has it taken out again
  (`addUndone=`), a copy's and a grant's alike; a grant's then lies there again. Stacks are read one inventory entry
  per base, as the engine keeps them and as a stack's index to take one from counts them (`Drops::StacksOf`). Copies go as the menu
  closes (or is found gone for 1 s), as their gift goes, before every save and load and as the session ends: no save
  holds one. OwnGear `kGiftCopy` lets them into a container's inventory only. The engine's changes made here are kept
  out of WorldSync's watch (`Gifts::Applying`).
- **Drawn at once.** The container menu draws its two lists as it opens and after a transfer it makes itself (or as
  the player switches between them), never for an item put in or taken out from outside it: a copy added just after
  it opened (as every copy is) and a take granted or held back while it was open showed only at the player's next
  key press (rig, #180). Every add and remove Gifts makes asks the open menu to draw both lists again
  (`Equipment::RedrawContainerMenu`): the menu's own code, on the UI's thread, calls its own `UpdateList` for each
  side, as the engine does after a transfer, from whichever comes first of its frame (its vtable's AdvanceMovie,
  hooked beside its messages) and an update message posted to it through the UI's queue (the way the plugin closes
  the menu), which lands in its message hook; at most once a request. Not while the menu's quantity box is up (the
  transfer it ends in names its item by its place in the list, which a redraw can change): the request waits for the
  box to close. The vanilla movie keeps the box's clip visible and fades it to alpha 0 when it closes, so the box is up
  only while the clip is visible with an alpha above 0 (a guard on `visible` alone held a request forever on a rig).
  It fails open: a box that is missing or can't be read is no box, and the lists are drawn. A request is dropped as a menu shows or hides: it isn't owed to the next. `net status` counts each
  step (`redraws=` and the rest of `DescribeContainerRedraw`), and the log has the first few of each.
- **Reloads.** A taker's save from before the take lacks the item: their record lays it down again. A giver's save from
  before the put has it in the container: their record withdraws the gift. A giver's save from after the put lacks
  it: it lies there again. Only that character's saves: a player who starts over as a new character (or a coc one
  from the main menu) gets back none of the gifts the old one took, and the old one's gifts lie on (Giving items,
  "Whose world").
- **Saved.** The session file takes layout 40 (39 in 0.10.0's test builds, without the drops' characters) while the server remembers any gift (a holder on a drop): one lying
  there, offered, or taken, as the server remembers the last 4096 takes (a taken gift keeps its holder: a give-back
  lays it down in its container again), or any drop whose characters it knows (every drop made since keys, loose
  ones too: see "Whose world"). So a session in which one gun was ever given or dropped is written as 40 until that many
  takes later; one where none was is written in the layout an older build reads (38 with shared storage's owners,
  else 36). An older build can't read a 40 file; 0.10.0 reads every older layout: 39's drops with no characters, those before 39 as loose ones.
- **The record's cap.** Gifts share the drops' record (`MAX_DROP_RECORD`, 4096 `made` and 4096 `taken` a session,
  the oldest forgotten), and fill it sooner: every gift given is a `made`, every take (one's own take-backs too) a
  `taken`. Past that, the record misreads its oldest: a gift given that early and never taken is withdrawn from the
  session by the giver's next record (lost), and an early take of one's own back is laid down again (twice). The
  loose drops have the same limit; left as it is for 0.10.0 (thousands of transfers in one session), and said here.
- **Off.** With the host's rule off a container no gift lies in isn't watched: no view, no reads of its inventory,
  and the event watch keeps no move (Gifts' view filter), so the setting off costs nothing.
- The "stays in your game only" hint is not said for a put in a gift's place with the rule on.
- `net status` says `gifts=` (on or off, the container viewed, copies, claims, reports waiting and the counters; `ownRefused=`
  must stay 0). The dev channel's `gifts` (`status`, `list`, `view <ref>`, `close`, `count`, `take`, `put`) drives it
  without the menu.

Tested without the game: `tests/GiftTests.cpp` drives GiftBook::Client and the drops' record (GiftGame.h) against the
real server and counts every item of each class across every world and what lies in the session, through a give and a
take, the giver's own take back, a take back before the offer (with a like item already in the container too), two
friends taking at once, a reload on either side, links that go before the grant, the claim, the kPlaced or the offer,
a late claim answered twice, a grant after a load, an offer past 60 s, others' and replayed confirms, keeps, takes and
give-backs, a forged gift of no weapon or armor, the rule off (no reads at all), the host's server stopping, the giver
away, places and items that aren't given, saves with a copy shown, a power armor piece's condition, and Take All beside
the player's own like gun. Its fake engine misbehaves as a real one might: moves whose events are lost (a Take All)
or come late, an item whose class changes as the copy is made or as it moves, adds that fail (before or after putting
the item in), and a script moving a copy out with no event; each test checks the counters of what went wrong
(`copyMissing`, `unheld`, `unsignaled`, `giveFailed`, `takenBack`, `dupRisk`, `overflow`, `notGear`, `addUndone`).
With shared storage on, the game's own storage and loot code (LootGame.h: StorageView::Client and LootSync::Loot) run
beside the gifts on the same packets, menus and moves, and are seen to move and send no weapon while the chest's
stimpaks stay the host's one true copy. The inbox is tested on its own. `tests/DropStoreTests.cpp` and
`tests/SessionFileTests.cpp` cover the saved offer and the layouts.

Left open, behind the setting (and in the install guide): a giver's save from before the put loaded after a friend
took the gift has it again, as with a dropped item (the server logs it: `dupRisk`); a gift waits in the session for
its giver's record when their link went between the offer and their kPlaced (nobody can take it until they come
back); a container that is the same reference in two games but holds a different thing (a mod changing it) shows the
gift anyway. A copy another mod's script moves out of the container is no claim: it goes as missing (`copyMissing=`),
the item stays where the script put it, and the gift still lies there for the others (one item more, at most one a
gift and a load or a rejoin; across many they add up. Keeping the memory across a rejoin to the same session is
left for later). A copy whose class changes as the player takes it (the engine dropping its name, say) can't be held
back (`unheld=`): granted, it stays with them and nothing is added; lost to a friend who was first, both have it, and
the game counts it (`dupRisk=`). Both are rig checks. The protocol needs nothing past 55: gifts add no message type,
only `DroppedItem::holder` on kReportDrop, kDropPlaced and kDropList (listed in 55's line), so a 0.10.0 build from
before this change passes the hello and can't read those messages: both rig games run the same build.

### Catching up (join, reconnect, loading a save)

The server keeps the session's world state and sends it in chunks (`WorldState`) on join and when a
player asks again (`RequestWorldState`). Kills, pickups, doors and quest stages can be applied twice
safely. Container changes can't (that would duplicate items), so:

- each session has a random ID, and container changes are numbered in session order;
- each player tracks which numbers its world contains (`containerNext`, plus the changes still waiting
  for their container to load) and stores that in the save's F4SE co-save (record `CONT`, v1);
- Hello and RequestWorldState say what the world has, and the server sends only the rest.

The receiving end is `ContainerLedger` (src/game/ContainerLedger.h, game-free and tested). Changes are
applied strictly in number order, whether they come live or in a `WorldState`: one that comes before an
earlier one waits for it (`gap=`, at most 8192; past that the newest is dropped, and `containerNext`
never jumps), a number already applied is dropped (`dupChanges=`). A gap with nothing arriving is asked
for again after 15 s, then 30 s, then every 60 s (`rerequests=`); a refused request (the server allows
3, plus one every 10 s) is covered by the next one. Should the server's log be shorter than the save
says (a host that crashed restarts from its last session file, written every 15 s), its answer to our
request starts below what we asked for and has no changes: the ledger goes back to the log's end, drops
what waited past it and logs "the server's log is shorter than our save" (`rewound=`). Only an answer to
a request we sent can do that.

After loading a save (including the reload after dying) the client forgets what it knew, reads the
save's record and asks for the session's changes again. The deaths and the player's moves from the
frame before the save or load are sent first (deaths first), so they are in that answer. So are the workshop reports still
waiting in their line (`WorkshopSync::TakeFromWorldBefore`; see "Whose world"). A save with no record of this session (made
alone, or in another session) takes this one from its first change, so the saves made from it on carry
one. Container changes carry who made them; the player's own are never replayed. Those made before the
loaded world (the save is from before them) are counted in `ownSkipped=`: replaying them would empty
the container without giving the loot back, or duplicate what they put in. Each report is remembered
with the world it was made in (counted by loads), so a move made just before a load counts whether its
echo arrives live after the load or in the catch-up. One known way to duplicate remains: put something in a
chest after saving, a friend takes it, reload: the reloaded game still carries it and the friend has it
too. `dupRisk=` counts it (our skipped deposit, then a friend's take of the same item from the same
container) until own changes are replayed as moves. A player who reconnects gets their old player ID back: Hello carries `identity`, a random
number kept in `Documents\My Games\Fallout4\F4SE\F4Multiplayer_player.id`, and the server remembers
which ID each identity had during its run (not while the old connection is still open; the client
also remembers every ID it had in the session).

The health of the NPCs hurt in the session comes with the world state too (protocol 57, no new message:
`kActorHealth`, as live reports; `common/NpcHealths.h`). A game that runs an NPC reports its health only when
it changes (`HealthShare`), and the server used to pass each report on and forget it, so a game that joined,
rejoined after a crash or loaded a save had its copies of hurt NPCs at its own save's health (or a fresh
cell's) until the NPC's health next moved: the raiders were back at full health in that game, and a copy its
save had lower than the NPC could be killed early. The server now keeps the last report of each NPC from the
game that runs it, as it keeps what each wears, and sends it with every world state (on joining, after a load)
to every game but that one, whose own reading is the truth there; a copy not loaded yet takes it when it loads
(`pendingHealth`). Only NPCs hurt here: a first reading at full health (every NPC a game comes to run) is not
kept, one healed back to full is (a joiner's save may have it hurt). An NPC leaves the book when it dies, when
nobody runs it any more (released, or its game left: the next runner reports its own reading, and a cell reset
may have healed it), and with the session; at most 256 (under the client's 512 waiting reports), the oldest
healed one first. Kept server-side rather than having each runner report again when someone joins: the server
already knows who joins and who runs what, a load needs the same catch-up and tells only the server, and the
runners send nothing more (no burst of reports in every runner's game each time someone joins). A spawn's health
is not kept: a joiner places its copy some frames after hearing of it, and a report before then has nothing to
set (a spawn the joiner meets mid-fight stands at full health until its next report).

Until the session's world state has come after a welcome or a load (or 15 s have passed), the client
offers to run no NPC, and never one the session has dead (`WorldSync::MayClaim`, `SessionDeaths`): its game
can have it alive from a save older than the kill.

### Deaths that end (cell resets)

A killed NPC is dead in the session, but the game brings a cleared cell's NPCs back after days away. A
game that had an NPC dead here (it saw it die or its corpse load; a console kill that left an NPC essential in
that game standing doesn't count), learned of the death at least 10 s ago and then loads it alive takes that
as a respawn and forgets it was dead (`revived=` in `net status`), so it may offer to run it again. Only the
host's game is believed about it: its kUnowned claim on a dead NPC, at least `Options::aliveAgainGrace` (10 s)
after its last hello or world request and after the death, takes the NPC off the dead list (and out of the saved session) and gives
it to the host's game ("alive again in the host's world" in the server log); it can fight and hurt players
again. A joiner's game that has a dead NPC alive (its cell reset first, or its save is older) is told the death
again when it offers to run it, hits it or reports its hits on a stand-in, once it has gone on about the NPC for
`Options::aliveAgainGrace` (10 s: the host's game, walking into the reset camp beside it, is given that long to
bring the NPC back for everyone first) and then every `Options::remindDeadEvery` (5 s per NPC; "dropped: the
session has it dead" in the server log), and kills its copy (`WorldSync::TakeDeath`,
`killsRetried=`), so every game has it dead until the host's game brings it back; before, those reports were
dropped without a word and the joiner fought a copy alive only there. The host's game is never told again: its
offer is the revival. Whenever the server names a game that runs an NPC, every other game forgets that NPC's
death (`SessionDeaths::Ledger::OnOwned`, `corrected=`), whether its copy is alive or still a corpse (the
corpse lies until its own cell resets); a death a game reported itself in the last 3 s is kept, since the
grant crossed the report. Hits by that NPC on a stand-in in the runner's game reach the player even while their
copy is a corpse, on the other side of a door, not loaded or of a plugin they lack (`PlayerHits`: taken when
the reporting friend is within 20000 units, `hits: unseen=`; before, `AttackerHere` dropped every such hit and
the enemy could not hurt them at all). A dedicated server has no host's game, so nothing comes back there.
Nor does anything while the host's last world request went unanswered (refused for asking too often after a
burst of loads): its game may have the NPC alive only in the save it loaded, so the server waits for a request
it answers (the next load) before believing it again.

### The opening (src/game/Story.cpp)

From a new game until "Exit Vault 111" (Out of Time, objective 1) is done, while the player is in the
pre-war world (`SanctuaryHillsWorld`) or Vault 111, the session leaves their world alone: its scenes
need their NPCs, doors and time of day as the game set them. Their game claims and copies no NPCs
(it releases any it ran), applies no world changes, quest stages, time or weather (they wait and are
applied once the player is out), takes no damage from friends' NPCs, gets no shared XP or map
markers, and reports nothing. They can't teleport, and nobody can teleport to them (status flag
`opening`). They still see friends who are in the same place. A mod that starts the game somewhere
else is never in those places.

A new character with no name yet counts as the opening too, wherever it is (`Story::Opening::kUnnamed`; log
`Story: in the opening (a new character with no name yet)`). The game gives a character its ID
(`BGSSaveLoadManager::currentPlayerID`) when its name is confirmed; until then a new game still has the last
character's. `CharacterBinding::Tracker` keeps it: since a New Game (`OnLoad` with a_newGame), with no load
since, and the ID still the one it had then, the character is unnamed (`Unnamed`). A change of ID that `Check`
sees with no load in between is the name being confirmed (frames are held from `kPreLoadGame` until
`Session::OnGameLoaded` has run, so a load's new ID always reaches `OnLoad` first): a joined game is bound to
the named character from then on, anywhere (log `Session: the new character is named: X -> Y`), so its own
saves load without leaving. Before, only a name confirmed in the opening was taken, and a friend who joined
before naming in an alternate start's own room was made to leave on the next load. Held as unnamed, an
alternate start (a room of the mod's own, or the Commonwealth) is left alone until named, and a host doesn't
start hosting (`host.err_opening_unnamed`): hosting opens the loaded ID's session file, which would have been
the last character's, taken in whole by the new world and written into with its kills and loot. After two
minutes held as unnamed (`kUnnamed`) outside the pre-war world and Vault 111 (`Story::InVaultPlaces`: a vanilla
start is unnamed there until the Vault-Tec form, and the mirror takes most players longer), a player set to host or join is told once per new game that the session
waits for the name, and that a save made before it has the last character's ID and would play in that
character's session (`session.unnamed_waits`). The notice offers no load as a way out: a start that never asks
for a name stays held until a save of it is loaded, and that save carries the old ID, which nothing can tell
apart, so the host would then host it under the last character's session file. Whether the game's own
character edit gives such a character a new ID is untried. Teleporting unnamed says so
(`party.cant_teleport_unnamed`), not Vault 111. A new game in Vault 111's opening of the game's own start (Out
of Time running: `Story::InVaultStart`; not the pre-war world alone, where some alternate starts make the
character at its mirror and move the player on unnamed, and an unnamed vanilla start is held there as unnamed
until the cryo pod) is not held as unnamed
(`Tracker::OnVaultOpening`): the vault holds it as it always did and lets it go when the player leaves, so a
Vault 111 start is held no longer than before even if the game gave the new ID before the new-game message
(naming would then change nothing). An alternate start that walks into the vault's places doesn't count (Out of
Time never starts there, so `Story` sees them as the opening's): unnamed, it stays held for its name there
(`Story::Opening::kUnnamed`, not `kVault`) and after it leaves. The rule is `Story::Decide`, free of the game
and tested on its own. A load's start (`kPreLoadGame`,
`Session::OnBeforeLoad`) ends the unnamed state too: should its end never come (frames go on after
SaveLoadHold's 60 s limit with no `OnLoad`), the save's ID is not taken for a name outside the opening. The dev channel is exempt:
the rig's character from `coc` at the main menu never gets a name, and its session files are its own
(`dev.`). Untried with any alternate-start mod (dist/INSTALL.md, Alternate-start mods). Only the ID's low 32 bits name the character (CharacterBinding::Character): a save's file name holds them and a letter the game loads back into the high half ('_' 0, 'M' 1 once the game has a mod's plugin), so the same character's quickload can bring another high half than the game had in memory.

## Moved objects (src/game/MovedObjects.cpp, ObjectShare.h, common/ObjectMovers.h)

A loose object a player grabs with the use key (a broom, a bottle, a tire: a plugin's reference of a junk, moveable
static, aid, ammo, book, weapon, armor, note, key or ingredient form, loaded with a body physics moves) is shared while
held, when let go of or thrown, and where it comes to rest. Never: actors and corpses, quest items (in a quest alias),
references made at runtime (dropped items, spawns, workshop builds: all 0xFF......), what SaveGuard keeps out of saves,
power armor frames, and nothing in workshop mode (WorkshopSync's).

- Held: our game polls `PlayerCharacter::grabbedObject` each frame. While it moves in the player's hands, its pose goes
  15 a second (twice a second while still) as `kReportObjectHold`, unreliable, **in the holder's frame**: an offset to
  their right, ahead and up, and its rotation turned back by the holder's heading (as a whole: a reference's angles are
  R = Rz(z) Ry(y) Rx(x), rows its axes, and carried round by a heading d it is R Rz(d), so a tilted object's x and y
  angles change as it turns; its angles are read from its 3D, which the reference lags behind while held). The
  friend's game makes its copy keyframed and collide with nothing (gravity and its own physics leave it alone, and it
  can't shove the stand-in) and draws it each frame in front of its own stand-in of the
  holder (the stand-in's smoothed place and heading), eased towards the newest hold (time constant 60 ms). So it stays
  in the stand-in's hands as they walk and turn, however far the stand-in is drawn from the holder's own place.
- Let go (dropped, or let go of while turning or walking, which flings it: the game has no throw button for a
  held object): 80 ms after the release the holder's game sends `kReportObjectRest` kLetGo, reliable, with where it left
  their hands (world place, cell or worldspace) and its launch velocity measured over those 80 ms. The friend's copy is
  physics again, put there and given that velocity, so it flies as it did. Once it lay still for 600 ms (or after
  10 s), the holder's game sends kRest with where it lies; the friend's copy is put there if its own physics settled it
  more than 1 unit or 0.02 rad away.
- One mover at a time (`ObjectMovers::Book`): the first grab wins. A friend's hold of an object someone else holds is
  refused and answered with the holder's last hold; that game turns its copy off and on again (the engine drops a grab
  whose 3D goes) and says nothing of it until its player lets go. After a let-go the rest is the thrower's to report
  for 12 s of flight and 3 s after it rested, but a friend may grab it meanwhile (handing it over). A holder who stops
  sending for 2 s, or leaves, holds it no more.
- Knocked about: four times a second our game looks at up to 256 loose objects within 1500 units. One that moved with no
  hand on it and came to rest is reported (kRest) when our player moved it (stood within 160 units of it, or hit it in
  the last 3 s: TESHitEvent), 8 at once and 3 a second at most, whether or not a friend is near (the session keeps it,
  below). What a friend's move put somewhere is quiet for 4 s (our physics settling it is not ours to report). At rest
  means it moved under a unit (and turned under 0.02 rad) from where it last moved for 0.6 s, or 15 s went by. One of
  ours that moves again within 10 s of that rest (a ball rolling on after it slowed, or still rolling when we gave up)
  is still ours and is reported again where it truly rests (`KnockWatch`'s follow-on: the same scan, nothing more looked
  at), unless a friend's move put it there, someone picked it up, or a friend's blast reached it. Rests that only the
  follow-on claimed stop after 8 in a row (`FOLLOW_ON_MAX`: a thing that swings for hours is not reported for ever), and
  the follow-on is gone once the scan has not seen the object for 3 s (a loading screen, a cell that unloaded).
  A blast flings a can far past its own reach (its radius and 128 units) and past our player's near range in the moments
  between two looks, so a blast reaches an object by where it lay before it moved as well as by where the scan first
  finds it moving, and that claim, made once when it starts moving, is held until it rests. An object something of ours
  set going that is still moving (or at rest and owed, or in its follow-on) is looked at wherever it has flown, past the 1500 units (`KnockWatch::Tracking`, at
  most 64 beyond the scan's own range a scan), until it is said or its 15 s, owed or follow-on time is up.
- A friend standing by it changes nothing: each game reports what its own player, NPCs and blasts moved and what rolled
  on from its own rests, and a rest two games claim (both players stood by the can one kicked; a friend stood in the
  pile a grenade threw about) is settled by the server: the first rest holds for 3 s (`ObjectMovers::SETTLED`), a second
  player's rest of the object within it is refused and answered with the first, to the refused game alone, so that
  game's copy goes where the session has it instead of staying where its own physics left it. A copy a friend's rest put
  down in our player's own body (or in their stand-in's, where their player is not) is pushed out by our physics to a
  place their game knows nothing of: if our player, an NPC of ours or a blast of ours is by it as it moves on from their
  place, it is ours again, whatever blast of theirs still reaches it (their throw of it ended with their rest), and is
  reported where it lies once the 4 s quiet after their place is over, past the server's 3 s, so their rest and ours
  never cross on the way, at most one such rest per object goes back per quiet and `ECHO_MAX` (3) per `ECHO_WINDOW` (120 s), so two games whose physics never agree on where it lies stop trading it. A copy that only settles on its own
  after their place, with nothing of ours by it, stays where it lies, unreported.
- Later (`ObjectMovers::Places`): the server keeps where each object came to rest last (a kRest it relayed; up to 2048,
  the one moved longest ago forgotten first; a pickup ends it; a joiner's game may add 512 new objects a run,
  `maxObjectPlacesPerPlayer`, never more whatever `--max-share` says, so made-up ones can't push out the host's), saves it with the session (layout 36, as kObjectRested
  packets) and sends it with the world state after a join or a load, as `kObjectRested` naming nobody (playerId 0). A
  game puts each where it says once the object is loaded in that space and the place is within 8000 units of its player
  (WorldSync's retry, as references load and as the player changes cell), however far from where its save had it, but
  not while anyone moves it; a friend's live rest for an object it doesn't
  have loaded waits the same way (at most 4096 waiting; a flood of those drops the rests waiting of the friend with the
  most, the flooder, never the catch-ups or anyone else's). While a place waits, our copy still lies where our save had
  it, so a friend's hold, let-go or rest of that object is judged from the waiting place as well as from our copy
  (`ObjectShare::FromWhereItLies`: their move starts where the session has it), and a move our game doesn't apply leaves
  the place waiting, or their rest when it is too far even from the place (`StillWaiting`; a hold ends the wait once it is drawn here). So a friend who loads the cell later, and every save loaded later, has the object
  where it was left. No new message (an older game takes a catch-up as a rest, if the object is loaded then). A known
  limit: a reference moved this way keeps its parent cell, as one a friend carried across a cell border does.
- Knocked about by NPCs and blasts: the same scan also reports an object that came to rest after an NPC our game runs
  stood within 200 units of it (`ObjectShare::Answers`: the session knows the NPC and the server made our player its
  owner; a copy of a friend's NPC, one nobody runs yet and a stand-in are never ours), or after an explosion of ours
  reached it (within its radius + 128, for 4 s after our scan first saw it). An explosion is ours when our player or an
  NPC we run caused it (`Explosion::GetActorOwner`); one our game set off for a friend's throw (WeaponFire's copy), a
  friend's stand-in's or another game's NPC's is theirs, and an object a blast of theirs reaches is never reported here
  whatever else moved it, so exactly one game reports each object (a trap's explosion, with no cause, nobody's). Same
  budget as every knock (8 at once, 3 a second), and a blast reports at most 64 objects; the rests the budget has no room
  for (a blast's, or any other of ours: one rolling on by the follow-on) wait up to 24 s for it, and one that waits is
  still ours if another can knocks it on meanwhile. The cap was 16: a grenade in a cluttered room throws 20 to 40
  objects, the ones nearest it fly furthest and rest last, so on the rig every throw said 16 to 18 rests and the pile's
  own cans, past the cap, lay hundreds of units apart in the two games. `knocksDropped=` counts rests of ours never said
  (past a blast's cap, or still waiting after 24 s). No new message: kReportObjectRest as before (`objects` in
  `net status` adds `blasts=seen/noted`).
  A known limit: an explosion with no cause of ours, a trap's (a mine, a tripwire's grenades) or a destructible
  object's own (a car going up, a barrel), goes off in every game (TrapRules, `kDestructions`) but knocks nothing for
  anyone to report, so what it throws about lands where each game's physics puts it, and can lie apart in each.
- Bandwidth: holds 15 a second of one object while a player holds it (about 40 bytes each); rests a few a second.
  Server: 20 holds a second a player, and a rest budget of 24, 6 a second, kept across rejoining.
- HavokGuard's rules hold: every pose written is finite and in the world (`ObjectShare::Writable`), velocities are
  capped at 5000 units a second, and physics is only touched on the main thread. Before a save or a load, and when the
  session ends, every copy held still for a friend is physics again.
- `net status` has an `objects:` part (holds sent/applied, let-gos, rests, knocks, refused grabs, bad poses); the dev
  channel's `objects` prints it, `objects grab` what our player holds, `objects probe <ref>` a reference's place,
  angles, 3D transform and bodies.

The opening's own quest stages are every player's, never shared (StageRules::IsOpening): MQ102 below 10
(10 is set by the trigger at the top of the vault's lift, 12 s after it fades the screen back in) and
all of DialogueVault111, a dialogue quest whose stages aren't in order, so each game that went through
the vault has a different set of them. Set from a friend's game once the player was out, its 40
activates the vault's cryopod with the player and its 98 and 100 start the mirror and lift scenes, whose
script fades the screen to black and leaves it so until the trigger at the top fades it in. They are not
reported, not part of the host's baseline, and dropped if they come anyway (an older version's report,
a session file). While this game is in the opening, no friend's stage that moves the player is set
(Verdict::kOpeningWaits): only that quest waits. A friend still in it holds nothing here: with three or more in the
session, the others go on side by side, their stages that move the player set as ever, and the friend's own game
holds them while they are in it.

### Fades (src/game/FadeWatch.cpp, FadeGuard.h)

The game's quest scripts fade the screen out and leave it so (`Game.FadeOutGame` with `abStayFaded`) until
a later stage, a trigger or a timer of theirs fades it back in; the plugin itself never fades. A stage
the session sets from a friend's game, or a teleport to a friend, can start such a fade where nothing will
end it. So for 15 s of play after the session sets a stage here (`setstage`, MQ105's repair, a lock's
release) or moves the player, a fade that comes up (FaderMenu open, from OpenMenus) is the session's, as
is one already up then; one that stays up 20 s of play is faded back in
(`Game.FadeOutGame(false, true, 0, 1, false)`), once more 20 s later if that didn't take, then left.
Play leaves out loading screens, the pause menu, the Pip-Boy, the console and the opening (the vault's
lift holds the screen black on purpose), and a frame counts at most 250 ms. A fade the session had no
hand in is logged after 30 s of play and left to the game; one up 5 s or more is logged when it goes.
Dev command `fade` (state), `fade in`, `fade stuck` (fades out to stay, as the session's, to test it).

## Destructible objects (src/game/Destructibles.cpp, DestructionRules.h)

A car, an explosive barrel, a fuel tank or a gas pump a player shoots is as wrecked in every game, and goes off once
in each (protocol 50, with world traps and NPC identity; 52 in its own test builds). Before, a car shot in one game blew up
there only: the friend's stayed whole, and could blow up again when they shot it.

The engine keeps an object's damage as its health (ExtraObjectHealth on the reference, none while it was never
damaged), and works out from it which of its base's destruction stages it has passed (DEST: a station wagon has 500
health; at 80% it burns, at 75% a first blast, `CarExplosion`, at 70% it burns harder, at 50% its big one,
`CarNukeLargeVehicles` with a 900 radius, where it becomes the hulk and counts destroyed, at 0% the last model). Each
stage comes once, when the health first falls past it: its explosion, debris, model and self-damage (a burning car
loses health on its own, about 10 s from first fire to the big blast). An explosive barrel has 1 health and one stage
that blows it up and takes it away. The engine's rules (read from GetCurrentDestructionStage and the damage function):
the percent is the health over the maximum rounded up; the stages are in order from most health down; passed are those
before the first whose percent is below the object's.

- Ours: an object our player hit (TESHitEvent, not an actor) is watched ten times a second for 30 s after its last
  change (64 at most), and a scan once a second of the destructible objects within 4096 units finds any whose stage
  moved since the scan before (an explosion's, an NPC's shot, burning down). Each stage passed that the session didn't
  know of goes out as `kReportDestruction` (reliable): the health, destroyed, and `explode` when a stage passed since
  sets off an explosion. An object first seen by a scan is taken as it is: damage from our own save before the session
  isn't shared. A deleted reference is left out, but for one the engine deleted as it destroyed it: an explosive
  barrel's last stage disables it and marks it deleted and destroyed at once (seen on the rig), so leaving out every
  deleted reference lost the very stage to report, and the shooter's explosion watch reported its blast as a plain
  explosion. A deleted reference is also read when a stage it passed takes it away
  (`DestructionRules::DeletedByDestruction`), should the engine not mark that one destroyed; in Fallout4.esm only a
  trap and a hatch have such a stage. The gas canister (a MISC, "GasCanister") is not one of them: its explosion stage is
  flagged cap damage, destroy and ignore external damage, at 15% of 40 health, like a car's, so why it is not shared is
  open. A hit on something not shared logs why and the base's form type and name (`hit on ... is not shared: ...`, once
  for each base and reason, a hundred at most; `destruct probe <ref>` says it too).
- The server keeps the lowest health of each and destroyed for good (a wreck is never mended), relays only news (a
  report another game's already took further is dropped), and saves the table with the session (layout 34). A report
  names where its object stands (its cell or worldspace and position; a relay and the session file don't), and is
  taken only from a player whose position is known, in the same space and within 16000 units of where the session
  first had the object (of the report, for one it hasn't placed yet): else a report wrecks, and sets off, any car at
  anyone. `explode`
  goes with the stage whoever reports it, with no window after a shot and no budget (Weapon fire: Who sets off an
  explosion): each game sets its own off with the stage and deals its blows on the NPCs it runs, so a stage passed on
  without it was a blow dealt nowhere. Each game's copy of a burning car burns down on its own: on the rig the friend's
  copy, set burning by the replayed first blast, passed the big stage a moment before the shooter's, and its report is
  the one that goes out. Each player has a share of the table (`maxWorldEntriesPerPlayer`), as for doors.
- A friend's (`kDestructions`): our copy takes the lower health (ExtraDataList::SetHealth), destroyed (the form flag, as
  TESForm::SetDestroyed sets it), and both are marked for the save. What the stages it passes now do in the engine is
  done to it: the last replacement model passed goes in (ExtraModelSwap, the old model's saved physics dropped, and the
  3D off and on 100 ms later so it loads with the hulk and its damage stage); otherwise the model's damage stage for the
  health (the engine's own, as Load3D does); a stage that takes it away disables it. With `explode`, the explosion of
  each stage our copy passes now goes off at the object through `WeaponFire::ApplyExplosion` (range and radius checks,
  BlastShield). A stage our copy had passed went off here already (our own shot, an explosion of ours), so it never goes
  off twice. Whichever game's copy passes a stage first reports it; the other's takes it and doesn't pass it again. A catch-up
  (joining, loading a save, a restarted session) names nobody and is the wreck alone. An object not in our world waits
  (up to 20000) until its reference loads (WorldSync's retry).
- The explosion of a destructible object's own stage is never reported as `kReportExplosion` too: WeaponFire's watch
  asks `Destructibles::ClaimsExplosion`, which finds a plugin's destructible object within 1024 units that has passed a
  stage with that explosion (or the one its explosion places), and watches that object so its wreck, which sets the
  blast off in the others' games, is reported. Before that the shooter's game would have sent the car's explosion apart
  from its state, and the friend's whole car would have caught it and gone off on its own as well. Its blows on copies
  are each NPC's own game's to deal whenever it blows (`Fate::kEachOwn`, `CopyHealth::OwnersToDeal`). Our hit on a destructible
  object arms a watch of its own (so a mine we shoot is shared), and so does a report of ours with `explode`, for 8 s
  and only within the farthest its stages' explosions reach (`RemoteEdits::AreaWatches`): before, it was a throw's,
  and every explosion within 6000 units of our player was reported for the friends to set off.
- Explosion radii a shot sets off, read from Fallout4.esm: a car's first blast 128 and its big one 900 (small and large
  vehicles alike), an explosive barrel's 350, a gas canister's 120, a Fat Man's shell 850: all under the 1024 cap.
- `net status` has a `destruct:` part (hits, watched, reported, applied, already, waiting, set off, models, gone,
  claimed, the scan's objects and time); the dev channel's `destruct near [radius]` lists the destructible objects
  around our player (health, maximum, stages passed, each stage's percent with `x` explosion, `m` model, `d` gone),
  `destruct probe <ref>` one.

To check live: shoot a car in one game until it blows: one explosion in each game, the hulk in both; reload either
game, and a friend who arrives later sees the hulk too (`destruct near` reads destroyed=true and passed=4 or 5).

## Party (src/game/Party.cpp, WorldClock.cpp, Hotkeys.cpp)

- Status (location, interior cell, health %, level, downed, opening, Pip-Boy light, power armor, menu):
  sent when it changes (at most 4/s) and every 2 s; the server keeps the latest for newcomers.
  Receivers announce deaths (health 0) and put the health on the stand-in's name tag. In power
  armor it carries the frame's base form and, when every game has that frame (one a plugin placed,
  such as the museum's suit in Concord; a runtime 0xFF reference is sent as 0), the frame reference
  itself. The frame is the furniture they sat in as the armor went on or, in a game loaded with them
  already in the armor (a save made in it, a reload, a relaunch before hosting or joining), the frame
  the engine keeps as worn (`ExtraPowerArmor`, `ArmorFrames::WornSeat`): learnt only from getting in,
  such a player was told as in power armor with no frame, and friends saw them out of their suit
  until they got out and in again. The stand-in wears the pieces they wear from the moment the status says they are in
  power armor (`RemotePlayers.cpp`, `ArmorFrames.h`), and the receiver keeps a copy of the base
  (only a power armor frame: furniture with the engine's power armor furniture keyword) under it,
  placed at once, hidden and without collision from the moment its 3D is in, and moved along with
  the stand-in (placed again if it is in another place, over 256 units off or without 3D after
  10 s). A friend in power armor who names a frame from a plugin this game lacks, or still names none a second
  into the wear, has a copy of a frame every game has placed instead, the museum
  T-45's base (`ArmorFrames::ChooseWornBase`, `STAND_IN_FRAME`): what a stand-in wears goes by the frame the
  engine has it in, and with no copy to get into it wore the clothes under their suit, out of it, until they
  got out and in again. Worn, a frame's base shows nothing of its own; one they name later takes over as any
  change of frame does. When they get out, the pieces come off and the copy is drawn where they got out, solid once
  nobody stands within 70 units of it, and deleted 4 s later; a copy someone here gets into is left
  to them. The stand-in is switched into the copy (`ArmorFrames::ChooseSwitch`), and the engine rebuilds
  it on the power armor skeleton with a fresh animation graph. Rebuilt before the power armor subgraphs
  are loaded, that graph is set up only later by the tail of the engine's own `Actor::Update`
  (`AIProcess::GetQueuedModifyInitialAnimationPose`, the subgraphs loaded, then the actor's
  `ModifyInitialAnimationStateImpl`), which a stand-in's no-AI update skipped: it glided after its friend
  with its legs still (every switch-in of a two-hour session, 2026-10-19; before 0.8.10 the switch-in ran
  the stand-in's own AI update, and they walked). `Puppets` does the same after the no-AI update. A
  stand-in whose legs still stay still while they walk is first tried in place, each way given 2 s
  (`ArmorFrames::Remedy`): its graph set up afresh whatever the engine says of its subgraphs, then the
  engine's own update for 1 s. Only then is it replaced (a try); with no try left it is kept in its frame,
  never shown out of its armor for it. The log says at each step whether the engine had the setup queued
  and the subgraphs loaded. Out of power armor, the status also carries the frame they got out of last
  (`lastUsedPowerArmor`, once the engine has it enabled in the world again; protocol 30): its base,
  its reference when every game has it, its cell or worldspace, position (to the unit) and heading.
  A pre-placed frame is shared as an NPC is besides (see Shared NPC AI): the receiver's own reference
  of it is moved to where the wearer's game has it, so it stands there within a second or two of the
  status. A frame made in the wearer's game is shared as a spawn (see Spawns): the receiver's copy of it stands
  there, and the parked copy below is for the time until it does (one not reported yet, or for up to 10 s after they
  get out of it, before it is shared again); the spawn's copy takes it over, or it goes once that copy stands within
  200 units of the spot.
  The receiver keeps a copy of it there for as long as they are out and the spot is in the area its
  engine draws, whether or not their stand-in is (`ArmorFrames::ChoosePark`): the copy left where the
  stand-in stood is moved onto it (the exit steps the wearer some 76 units off the frame), it is
  moved when the frame is, placed again when far off or never loaded, and none is placed where the
  receiver's own copy of that very pre-placed frame stands within 200 units (that one stands for it,
  and the copy left where the stand-in stood goes at once).
  The receiver's own copy of a pre-placed frame they left parked elsewhere is hidden by the same
  guards as a worn one (`ChooseParkedOriginal`), and stays hidden when they get into it again there.
  Its pieces are not shown: a copy stands empty. The stand-in is never sent into the copy: activating a frame starts nothing for an NPC
  (`TESFurniture::Activate` begins power armor use only for the player; an NPC gets in only through
  its AI package), so the stand-in sent in never got in, and pieces that waited for it left the
  friend shown without armor. The receiver's own copy of a shared frame reference is hidden (not
  drawn, no collision; the reference itself is never moved, used or deleted) while the friend wears
  theirs, only when it is a power armor frame of the base they sent, nobody there is in it, it is
  not the receiver's own suit (worn or last left), no quest of the receiver's holds it (it fills no
  quest alias: the museum's suit is the main quest's objective) and it stands within 200 units of
  where the friend was as they got in (their newest state if it came within 0.5 s of the status,
  else the next one); otherwise, or once it has been elsewhere or used in that wear, it stays shown
  until they get in anew. One another game runs (FrameShare: the wearer's, since getting in is a use)
  is hidden wherever it stands while they wear it, loaded: it is moved to where their game has it (where
  they got in), which for a friend who joined mid-wear is not where their stand-in first appeared, and
  compared with that it stood shown for the whole wear, a second suit for the one they wear. It stays hidden through saves (only its 3D is touched) and is hidden again
  at once after a load. It is drawn again once the friend is out and their copy is clear of it (when
  several friends wear their copies of it, once the last of them is), and solid again once nobody
  stands within 70 units of it.
  One copy per friend's frame (`HeldCopy.h`): every frame copy (the worn one, the one left where they got out, the
  parked one, a spawn's) is held by its handle beside the id and base it was placed under. The engine unloads a
  reference it never made persistent with its cell (the player walks or travels off): the form is freed, the handle
  stops resolving, and the record of changes stays under the cell, bringing the copy back under the same id when the
  cell loads again. Up to 0.9.1 the copy was only let go of: it came back on the player's return beside the one
  placed for the friend's frame then, and every save kept it (the 0.9.1 report: frames piling up where friends had
  parked, got out or walked in their armor in Sanctuary). Now the id is looked up again once the handle fails
  (`HeldCopy::Look`): the same copy back is held again (the same id and base, standing where it was last seen while
  held: a record dropped while the copy was still held frees its id, and a new frame of that base could be made under it);
  otherwise it is forgotten, and one whose form is gone is taken
  off the books at once and its record dropped (`SaveGuard::Forget`, `SaveStrays::Book::ForgetGone`, counted as
  `orphansDropped`): dropping the record frees the id, and the engine may hand it to the next frame it makes. A frame already
  standing where a copy is placed is never taken over: nothing tells a plain copy an earlier session left from a suit of
  the player's own (stripped, so empty, and with no owner if they never got in), so such a one stays (the console removes
  it: disable, then markfordelete); the workshop copies 0.9.1 made of frames carry the keyword and go (below). The
  leak the rig showed (2026-10-24): a load does not free every copy DespawnAll had just disabled or deleted for it; one
  stayed in memory, nobody's once the load had emptied the book, the next save wrote it, and a game started afresh from
  that save had it back drawn and whole beside the copy placed for the friend, and the host shared it as its own frame.
  Now `SaveGuard::BeforeLoad` notes each frame copy of ours just before a load (its address, extra data, ownership,
  base and spot). After the load the game counts ids on from the save's, so an old id may name a frame the loaded game
  made since (a raider's, disabled while worn); only the very form noted, at the very spot, lingers
  (`SaveStrays::Remains`), and only while it is off (disabled or deleted), nobody is in it or got out of it last (in the furniture it uses, or the frame the engine holds as
  worn, which is all there is right after a load), it
  has no owner it did not have when noted (the copy a friend's stand-in wears has the stand-in's base for its owner)
  and nothing in it. It
  is asked again each second and before each save, kept out of saves while all of that holds and left to the game for
  good once it does not (`lingered`, `lingeringDropped` and `lingering` in `saveguard`); no record is dropped for its
  id once it is gone, as the id is the loaded game's. `net status` counts the rest: `framesUnloaded`, `framesBack`
  (stand-ins), `unloaded` and `back` (spawns).
  A lingering copy was only ever disabled, never deleted: it stood on, unseen, for the rest of the run, and a save wrote
  it once it was no longer provably ours. A copy let go of to the game as someone's while it waited to be deleted was
  off the books for a load in that second: a stand-in buried in its frame still counted as someone in it
  (`RemotePlayers`' `FrameInUse` now skips anything on its way out, and `BuryFrame` puts a frame back on the books and
  buries it once). Each session places its copy at the same spot again, so these piled up, disabled, at one spot (rig,
  2026-10-27). Now, right after every load, each disabled frame made at runtime is watched (`FrameLeftovers`), and
  judged once it has stood so `FrameLeftovers::LOOK_AGAIN` (5 s, no loading screen up, still the same form). One goes
  as every copy of ours goes (`SaveGuard::Track`, `RemotePlayers::Discard`) only with proof that it is ours
  (`FrameLeftovers::Proven`: it lingers from this run's load, it carries the workshop copies' keyword, or a stand-in's
  base owns it, which the switch-in gives every copy a stand-in gets into) and nobody's: not persistent (the engine
  promotes a frame as an actor gets in, so an NPC away in their suit keeps it persistent), holds nothing (nor would its
  base fill it), no quest alias holds, nothing links or enables, nobody here is in it or left it last (every process
  list), and no owner but a stand-in's base (one our player ever used is theirs; an owner that cannot be told counts as
  someone's). A plain disabled frame with no proof stays, whatever else it lacks: it cannot be told from a frame the
  game made (as `FrameShare::Ghost` rules for a standing one), and a frame a player cares about is often one full of
  pieces. So a parked or left-behind copy of an older version's save, which no stand-in got into, stays unseen where
  it is. `saveguard` shows `frameLeftovers=` cleared/judged, `leftoversKept=` and `leftoversWatched=`; each one judged
  is logged with its facts.
  Nor is a frame a workshop piece (`WorkshopApply::Shared`): workshop mode moves a parked frame, and up to 0.9.1 the
  move (of the player's own, or of our copy of a friend's) was reported as building: each friend's game made a plain
  copy of the frame where it was set down, and the server kept it for every later session and load. Now WorkshopSync
  neither reports a frame nor places or moves anything for a report of one (an older game's, or the session file's:
  counted in `unplaceable`, or `ownRefused` for a frame of the world). Such a copy is told from a suit of the player's
  own: every workshop copy carries the keyword that keeps workshop mode off it (`BlockInteraction`, since 0.9.0, saved
  with it), and nothing else gives a frame that. So the report of one still finds what stands at its spot, and a frame
  carrying the keyword and holding nothing goes (`WorkshopApply::Placeable::kFrame`, `framesCleared`); one without it,
  or one holding pieces (fitted out by a player, or their own suit an earlier session took over: deleting it would
  lose them), is left alone. The session file is not needed for it: NpcSync's frame walk (every 2 s in a session) finds
  any such copy standing loaded (`FrameShare::Ghost`: made at runtime, the keyword, empty, nobody in it, not the frame
  our player wears or left last, with no owner (the engine gives a frame its wearer's ownership as they get in, so one
  our player ever used has one), no quest's, no copy of this run's, loaded) and buries it as a frame copy of ours
  (`SaveGuard::Track`, `RemotePlayers::Discard`: disabled, deleted, kept out of saves, handed to a load that comes first;
  `ghostsCleared` in `net status`, `paframes` shows `marked=` and `holds=`). Left standing, the host's game shared it as
  a frame of its own (`FrameShare::SharesMade` took any created frame standing loaded), and every joiner placed a copy
  of it beside the friend's real one, every session: one 0.9.1 ghost in the host's save was one extra suit in every
  other game. A ghost holding pieces is shared as the player's own, since it is theirs by then.
  Two saves with a shared past (one save, played on by both) hold the same runtime frames under the same ids at the
  same spots, and the joiner's game placed a copy of the host's beside its own: a frame of our own, of the base, no copy
  of ours and not a ghost, standing within `FrameShare::SAME_SPOT` (32 units; no two frames stand nearer) of where the
  owner's game has theirs stands for it (`SpawnBook::FrameFacts::oursThere`, `NpcSync::OwnMadeFrameAt`): no copy is
  placed, and one placed before ours stood there goes; once theirs is moved away (its owner got in and parked it
  elsewhere) a copy stands there, and ours stays ours (`oursThere=` in `net status`).
  Why they stayed (read from disk, 1.11.240): a load frees only what has a record of changes
  (`BGSSaveLoadGame::RevertAllChanges` walks the records and lets go of each created form it finds there), and DespawnAll
  drops the record of everything of ours on its way out, so none of it was freed (deleting a created reference drops it
  too, `SetDelete` calling `RemoveChanges`, and the engine gives none back to one deleted and not persistent). Nothing
  of ours keeps them: past a frame we hold handles and plain pointers, never a reference count. A friend's stand-in so
  kept could be written by a later save (0.9.0 and 0.9.1 wrote them): it came back at every load of that save in no
  cell, had its record dropped, stayed through the next load, and the second load of that save in a run crashed (on the
  VM: the process lists, `TESPackage::Revert`). Now `SaveGuard::BeforeLoad` hands to the load each of ours on its way
  out, each lingering from the last load, and each held with no record (one adrift, inside RemotePlayers'
  `ADRIFT_GRACE`; `SaveStrays::GoesToLoad`), of every kind Track is told of (stand-ins, frame copies, lights, Spawns'
  copies): one without a record is given it back (`TESForm::AddChange`), and one the engine will not give one (a created
  reference deleted and not persistent) is let go of then as the load would (the engine's own call,
  `SaveStrays::Handover`; `leftToLoad`, `recordsGiven`, `releasedAtLoad` in `saveguard`). An actor handed over is no
  puppet of ours from then on (`Puppets::Unregister`), so one the load keeps all the same reads as lingering after it,
  not as one of ours in use. A stray of ours a save brings back has its record dropped as before (no save writes it), and
  is handed to the next load while it is still the same form (`broughtBack`, `broughtBackHanded`). Such a save is still
  not safe to load again and again in one run: on the VM the third load of one crashed (`Actor::Revert` on a settler),
  and so did some first loads of it, in the save's own record, before any of this runs. A save made after loading it
  holds none (five loads of one in a run were fine). So loading one says so on the HUD, once a load
  (`save.old_leftover`: save now, then restart the game and play from the new save; `oldSaveNotices`).
  One kept all the same lingers by kind (`SaveStrays::StillOurs`: an actor that is no puppet of ours, a light, a frame as
  above), one buried before its 3D came in (never disabled: disabling it then crashes the loader) is disabled after the
  load and lingers (`SaveStrays::LingerOf`, `turnedOff`), and a graveyard or deleting entry whose handle failed with its
  form still there is still on its way out at the load (`SaveStrays::LostOnWayOut`, `lostOnWayOut` in `net status`).
  Dropping the record of a form still there never frees its id: `TESDataHandler::GetNextID` passes over an id while a
  form is under it.
  Known and left: a session ending, or the host handing over, while a companion is being sent out of a copy leaves that
  copy to the game with the companion in it, saved, and the friend's game shares the frame again when they are back (a
  second copy on the spot); and a save during a send-out writes that copy, worn by the companion (one extra suit after
  loading it). Both need the few seconds of a send-out, or one that failed (`sendOutsFailed`). Each created base of an
  actor handed over is kept in memory for the run, with no record (`basesKept`): a face copy is made again after each
  load, so a run leaks one small `TESNPC` per friend, and per NPC copy on a leveled base, at each load. When the main
  thread does not answer before a save or load in time, the load's thread does this work, the engine's release
  included, as it already disabled and deleted from there. A created actor of the game's on a generic settler base,
  deleted and in no cell, reads as a stand-in: its record is dropped (as before) and it goes to the next load, as the
  load would take it anyway.
  A friend's frame is theirs (`FriendFrame.h`, `RemotePlayers::KeepOutOfFrame`, through `Equipment::SetFrameGate`):
  every frame copy the session placed here is kept from our player, whoever used it last (a friend's parked one, the
  one their stand-in wears or got out of, and every copy of a frame their game shares), and they are told whose it is
  (`party.friends_frame`). Getting into one made it ours while the friend's stayed theirs, and their game shared it
  again after their next load: a second copy on the same spot. Three ways in are closed. Getting in: the frame use
  `InstallFrameUse` patches asks first. Transfer: the container menu shown for a copy is closed again in the same pass
  of the UI's messages, before anything can be moved into it (items left in a copy go with it): `InstallFrameGate` hooks
  its messages in its vtable, and compares the menu's target with the handles of the copies our player is kept out of,
  which the main thread hands it each frame (`Equipment::SetTransfersKeptOut`; no game object is read on the UI's
  thread). A companion ordered into one: the engine's command walks them there and gets them
  in without asking the frame, so anyone our game runs (not our player or a stand-in) found in a copy (getting in, or all the way in, when only the
  engine's ExtraPowerArmor still says which frame it is) is sent out at once
  (`Actor.SwitchToPowerArmor` with None). The copy is left as it is meanwhile, from the frame they get out (whose 3D the
  engine takes away then, before `SendOutOfFriendsFrames` sees them out), and until its new 3D is in (10 s at least, at
  most 30 s, `FriendFrame::Settling`, `RemotePlayers::BeingSentOut`): never "theirs now", nor taken for a copy that never
  loaded. Nothing buries a copy with anyone in it (`FriendFrame::ChooseLeave`; `FrameInUse` counts the frame an NPC
  wears too): buried, it left them in a frame that was gone, and a save then had them wear a frame it did not have,
  which the engine fails on as it loads that save. A save or load keeps a copy someone is being sent out of as it is,
  saved with the frame they wear (a load takes it with the old world); let go of anywhere else (the friend leaves,
  their game shares the frame again, a copy is placed anew), it stays ours, still kept from our player, until they are
  out, then goes (`RemotePlayers::WaitForSendOut`, `copiesWaited`). A load forgets who was being sent out (their ids
  name the loaded game's forms); a send-out is not asked while a load is queued, and one the VM ran after a load anyway
  is counted (`sendOutsHeld`, `sendOutsDropped`, `sendOutsAfterLoad`). A plugin's frame (each game has its own
  reference), our player's own (a copy they are in or got out of last stays usable) and a quest's are never kept from
  them. Counted as `framesKeptOut`, `othersSentOut` and `sendOutsFailed` (`net status`) and `keptOut`,
  `transfersKeptOut` and `transfersClosedAtShow` (`paframes`). Borrowing a friend's suit comes later.
- Menus (`MenuKinds.h`, `StandInPipboy.h`, `MenuIconPlace.h`, `MenuIcons.cpp`; protocol 48): the status carries the kind
  of menu the player is in (`PlayerStatus::menu`: none, the Pip-Boy, paused, looting, a terminal, crafting, talking or
  another), read from the open-menu book, never from the UI, and never what the menu shows. VATS is no menu here. The
  receiver adds "[Pip-Boy]" or "[in a menu]" to the friend's name tag. While the friend is in the Pip-Boy, the
  stand-in checks its wrist (`IdlePipboyExamineMale`/`Female`, the human graph's own idle; NPCs have no third-person
  "Pip-Boy up" pose to hold), again every 6 s, and `IdleStop` ends it when they close the Pip-Boy mid-way. Not in
  furniture, power armor, down or moving. The Pip-Boy itself (`Pipboy` 00021B3B) is worn like any other gear the
  friend's game reports: the rig showed it on the left forearm of both stand-ins.
  An icon over the stand-in's head (a Pip-Boy, two pause bars, or a bubble with three dots), drawn on the HUD from its
  own advance like the compass markers, is written but off (`bMenuIcons`, not in the shipped ini): on the rig the HUD
  showed it only in part, or where it was first drawn, while the values read back from the movie were right. The HUD
  seems to redraw only what the game itself changed: a shape made and moved from the plugin showed, if at all, only
  near the middle of the screen, whether it was in a layer of its own, straight in the root, in the
  center group or the quest markers' base, scaled or drawn at its size, added again or made anew on each move, or
  drawn into the parent's own graphics. The compass markers likely show because the game redraws the compass every frame.
  `menuicons layer` and `menuicons opt` (DEV-CHANNEL.md) are there to try the next idea.
- Furniture (`StandInSeat.h`; protocol 31): while the player sits, lies or works in furniture (a chair, couch, bench,
  stool, bed, sleeping bag, workbench, chem or cooking station, terminal: the engine's sit/sleep state says they are in
  it or getting in, and it names the furniture they occupy), the status carries it: the reference when every game has
  it, its base and where it stands (to the unit). Never a power armor frame or station, nor in power armor (the frames'
  own path above), nor a seat on an actor (a vertibird). Getting up (want-to-stand or want-to-wake) ends it, so the
  stand-in gets up as they start to. The receiver finds its own copy: the named reference if it is of that base, else the
  nearest of that base within 32 units of the spot (a settlement's furniture, which WorkshopSync copies to the same
  spot), enabled, with 3D and in the stand-in's cell or worldspace. When the friend's spot is within 256 units of it, the
  stand-in is held (`Puppets::Hold`: not placed at its target, no locomotion events, no do-nothing package renewal) and,
  once it has had its 3D and its AI process for 1 s (`StandInSeat::READY_AFTER`), sent in with Papyrus
  `Actor.SnapIntoInteraction`, then, 2 s later if it is not in it, by the console's `activate` (plain furniture only,
  never a workbench or a terminal). A round that does not take is followed by another 5 s later, three rounds in all,
  and a stand-in that is out of the seat again while they still sit goes on with the rounds left. A stand-in made
  while its friend already sat (after a load or a join) used to be sent in the frame it was spawned, with no 3D, and
  spent both ways before its head was built: it never got in (a quarter of the misses in a 5-player game's logs). Once
  in, the furniture's graph and marker place it. When none takes, or no such furniture is found, it stays held where
  it stood as they sat down instead of following them onto the seat. When they get up it is stood up at once (`Actor::StopInteractingQuick`, as a mirror is) and follows
  their states again. Dev: `seat <puppet> <furniture> [snap|activate]`, `seat <puppet> stand|hold|free`.
- Heartbeat once a second, answered by the server: round-trip time and "unstable" after 4 s without
  an answer (the transports only give up after 20 s). Main-thread stalls (loading) don't count.
- Kill feed: deaths now carry who reported them and whether that player killed it
  (`TESDeathEvent::actorKiller`).
- Kill XP: XP gained within 2 s after a player kill (or up to 5 s before its death event, which
  waits for the victim's next AI update) is held for 2.5 s, then reported unless a quest stage
  changed or a location was discovered meanwhile (that XP isn't shared: the quest reaches the others
  by itself); others get `fXpShare` of it through `player.modav experience`. Shares received are
  credited so they aren't reported again. The server allows each player 10000 shared XP per 10 s.
- Time and weather: everyone reports `Calendar::gameHour`, `gameDaysPassed` and
  `Sky::currentWeather` every 5 s; the server passes on only the host's (a player on its machine,
  else the one there longest). The game keeps GameDaysPassed at midnights passed + hour / 24, so
  setting the hour across midnight would skip the date: receivers line their hour up once, remember
  how their day count differs from the host's, and from then on move forward like waiting does
  (stopping at 23:59 and letting the game roll the date over). Ahead of the host, they only go back
  within the same day. Weather is forced only outside in the same worldspace.
- Teleport: same interior, or a friend outside in the cells next to ours (the 3 x 3 around us): `SetPosition`.
  Elsewhere, or farther across our worldspace: `coc <cell editor ID>` or `cow <worldspace editor ID> x y`
  (cells and worldspaces keep their editor IDs at runtime), then `SetPosition` once loaded (for a cow within our
  worldspace: once we stand in the friend's cell). A `SetPosition` beyond the loaded cells landed the player
  thousands of units short of the friend, in the air, and they fell and went down (src/game/TeleportRules.h,
  tested). Until the player has stood on the ground for 1 s after the move (at most 15 s), a fall does no damage
  (`bhkCharacterController::fallStartHeight` follows the player down). Refused in dialogue (`DialogueMenu`), a scene (`GetCurrentScene`; not one a
  friend's stage started here, `QuestSync::SessionScene`: the friend is playing it and their stages end it),
  furniture (sit/sleep state) and Survival (`GetDifficultyLevel`), where it could strand a quest.
- Compass (src/game/Compass.cpp): each friend in our cell or worldspace gets a shape in their color
  (blue circle, orange square, pink diamond, purple triangle by player ID), drawn with the Flash
  drawing API into `HUDMenu`'s `root.BottomCenterGroup_mc.CompassWidget_mc` from inside the HUD's
  own update (its `AdvanceMovie`, hooked; one update queued at a time, and none while a loading
  screen or another menu is up). In power armor that compass is hidden and `PowerArmorHUDMenu`, a
  menu of its own, shows its own `HUDCompassWidget` at `root.CompassWidget_mc`: the markers and
  ping rings go there too, from inside its update (hooked in its vtable from the address library,
  as that menu is made anew each time the player gets in, and never looked up). The bar is 304 px for about 100 degrees (3.02 px/degree);
  friends beyond it are pinned to the edge at half alpha. Text made in code doesn't render (the
  HUD's embedded fonts don't reach new TextFields), hence shapes; the player list names them.
- Hotkeys: `GetAsyncKeyState`, only while the game window has focus and no menu or console is open.
- Menu (src/game/Menu.cpp, F1): the engine's message box (`MessageMenuManager::Create`, four
  buttons, our `IMessageBoxCallback`) as a paged menu: host, join a Steam friend who advertises a
  lobby, who's here, and five settings written back to the ini (`common/IniFile.h`, one line
  replaced). The button press is kept under a lock and acted on in the next frame, where the next
  page is created. See docs/MENU.md.
- Friendly fire (`bFriendlyFire`, the host's setting, sent in `Welcome`): our hits on a stand-in
  become `PlayerHit{byPlayer}` like an NPC's; the server drops them when it's off. Our replayed
  shots launch nothing, so each hit is only counted in the shooter's game.
- Downed and revive (src/game/Downed.cpp): with friends in the session, the player's base form gets
  the essential flag (set directly, so it isn't recorded for the save, and cleared before every
  save). A lethal hit then puts the player in `ACTOR_LIFE_STATE::kEssentialDown`; we set
  `kNoBleedoutRecovery` so the game doesn't stand them up after ~12 s. Status carries `downed` while
  the player waits for help (not once helped and getting up, so friends help once); a friend within
  300 units of the stand-in for 2 s sends `Revive`. The downed player clears the flag, restores
  health and, on their next frame, ends the down the engine's own way (`Actor::EssentialDown(false)`,
  ID 2230033: life state, bleed-out camera and controls, the graph's bleed-out flag and the character
  controller's bleed-out collision bit, in that order), asked again every second while it doesn't take
  and only once a knock-down's ragdoll is over (3 s at most); full health from it is cut back to 30%.
  However a down ends with the player up (helped, a save loaded while down, stood up by the game, the
  last friend leaving), the get-up clears any bleed-out flag left on; with the bleed-out camera (current, or
  waiting in the camera's return stack) or the down's input layer (`Player bleedout`) still on once they are
  out of any knock-down, it ends those the way the life state's end of a down does (the camera's bleed-out
  end, ID 2248381, and the layer's release, ID 2200873), as soon as it can and once more 2 s later if that
  did not take (`DownRules::Self` finishDown, tested): stood up by a knock-down's get-up first, a player
  was left with the camera on the ground and no Pip-Boy until a load. The camera's own bleed-out start
  (ID 2248380) pushes the current state on its return stack, and its end makes a transition (the
  `kPCTransition` state) from the bleed-out camera to the state it pops: a player downed again while the
  first end's transition still ran (a rocket engine's fire, downed twice within a moment) had the
  transition state itself pushed, and the second end made a transition to itself, which never ends, the
  camera left by the ground and the Pip-Boy shut until a load. On the ground or up and not yet settled,
  a transition on the return stack is replaced by where it was going (the view the first down left), and
  a transition going to itself is sent to the last first- or third-person view (`DownRules::Camera`,
  `fixCamera`, tested; the transition's target at +0x40 is checked by tools/addrcheck.ps1). If they went down sneaking and
  are still sneaking (the stance survives a down), it stands them as they get up, and again 2 s later only
  if that did not take (`DownRules::Self` gotUp/settled, tested). A sneak the player chose is left
  alone: one who went down standing and pressed the sneak key while down, or who sneaks once up.
  One who went down sneaking and gets up standing was stood by the game's stance alone
  (`Actor::SetSneaking` sends the graph no event): the graph stayed in its sneak, and the sneak key,
  which asks the graph for the start the stance calls for, was refused until a load. The graph is told
  `sneakStop` as they get up and again 2 s later unless the stance changed since (graphSneakStop,
  tested). `controls` ends with a `sneak:` line naming what holds the key back (`SneakKey.h`, tested).
  Friends are not sent the crouch flag while the player is down or getting up, and a stand-in that
  gets up is told its sneak state again 2 s later, so it is not left crouched.
  45 s or giving up (the last friend leaving, or a dropped connection, changes nothing until then; a friend back in
  time can still help): essential off (base form and the character's own flag)
  and `player.kill`; essential stays off until they are dead or the game stands them up, and the
  kill is repeated every 2 s while they are still alive and down. The rules are in
  src/game/DownRules.h (tested). Both flags are cleared for a save; a save made while down stands
  the player up when it's loaded. A stand-in put down in our game (a hit bigger than its health) is
  replaced, so it never sits on the ground while its friend is up. A friend who is down is shown down
  (`DownRules::StandInDown`, tested; `Puppets::SetDown`): their stand-in's life state goes to
  `kEssentialDown` with `kNoBleedoutRecovery`, which plays its own graph's downed pose (in a power armor
  frame too, once its switch into the frame is done), and it is still put at their spot each update but
  told no locomotion, aim or draw, so the revive range is measured where they lie. The reference's own
  essential override is set while it lies (`ExtraDataList::OverrideEssential`; "overridden not essential"
  once up): the engine's per-actor update in ProcessLists recomputes the essential flag every frame from
  that override or the base, a settler shared with world NPCs, and stands a non-essential actor in
  `kEssentialDown` up (rig 2026-10-20: up 20 ms after each lie-down without it). Up again, its life
  state goes back to alive (the engine's get-up); still down 4 s later it is replaced, standing. Found up
  again while they are down, it is laid down again 2 s on. Laid down by us, it is not replaced as broken.
- Crawling while down (`bDownedCrawl`, the host's rule, off by default; `DownRules::Crawl`, tested;
  `Downed::CrawlFrame`): down and waiting for help, the player's own movement input
  (`PlayerControls::data.moveInputVec`, which the game fills from keys or stick through its control map)
  moves their own character in their own game: forward 40 units/s along the heading, back at half, and
  sideways turns the body 90°/s (the bleed-out camera gives no direction to go by). A frame counts for
  at most 0.1 s. Nothing moves while the game has the player up and they are being put down again (the
  engine walks them then, and a step of ours could keep the put-down from taking), nor while paused, in the air, swimming, in furniture or a scene, knocked over
  (knock state not normal), or with a friend's stand-in within help range (a crawl away would start
  their help over). Each step is checked first with the cell's physics pick: two rays along the way at
  35 and 60 units up (a wall within the step plus 20 units refuses it), and two down from 35 units up, at
  the step's end and 20 units ahead (no ground within 120 units below, a rise over 30 or a drop over 45
  refuses it; the feet go onto the ground). Only hits on the world's own solid layers count
  (`DownRules::CrawlStops`, railings and grates included): the rays start inside our own body, and
  characters, loose clutter and triggers are passed through. A step is `SetPosition` and `SetHeading`, as teleport does. The position
  goes to friends in `PlayerState` as always, and their stand-in, lying, is placed at it every update,
  so it drags itself along in the downed pose. Nothing new is saved, and a load while down stands the
  player up as before. A game from before the rule ignores the welcome's bit and does not crawl. Whether
  the game still fills `moveInputVec` during bleed-out is logged once a down ("movement input while
  down") and shown in `Describe`.
- A death loads the newest save (`src/game/AutoSaveRules.h`, tested; `AutoSave.cpp`). The teleport key's
  `coc`/`cow` is not travel to the engine, so it made none of the autosaves a load door or fast travel makes,
  and a bleed-out could take a player back ten minutes or more while the world, caught up from the session,
  looked current. Landing after a teleport into another cell or worldspace, or across the
  worldspace both players are in (farther than an exterior cell), now asks for an autosave, once the player has
  stood on the ground for 1 s, at least 3 s after the move, alive and not down (none if that doesn't come in 30 s:
  one made in the air loaded the fall again at each bleed-out, TeleportRules.h), with
  `BGSSaveLoadManager::QueueSaveLoadTask(kAutoSave)`, when the player's own `bSaveOnTravel`
  is on; with the host's `bSaveOnDown` (welcome flag bit 128, no protocol version of its own; off) going down
  does too. Neither in Survival, nor within 60 s of the last save of any kind, nor of a dead player, nor a
  travel one of a downed player: the engine makes an autosave at its next chance, maybe late (in a fight),
  so the `kAutoSave` bit it still holds in `BGSSaveLoadManager::queuedTasks` is cleared when the player goes
  down, before a bleed-out's kill, and while they are dead ("Save: the autosave the game held ... is taken
  back"); a save of ours that still begins then logs an error. The down-save is not asked for a player out of
  revives or in the cooldown (`DownRules::Self::AsksForDownSave`, tested); a death that loads the save made
  as they went down puts them back on that spot, up as if helped (a checkpoint, not a limit on dying). Going
  down, a last revive, a
  cooldown and a session without revives say on the HUD that a death loads the last save. Each save logs
  "Save: <kind> (<location>, <date>) begins (the mod's autosave after a teleport | ... | the player's or the
  game's)" and "written (N ms)", and each load "Save: loading ..." (F4SE's message data; the character's
  name, hex in the file name, is left out); a request with no save within 10 s logs a warning.
- Session rules (`Protocol::SessionRules`, protocol 49): the host's ini (src/game/HostRules.h; F4MPServer's
  options) sets them and the server sends them with every `Welcome`, the host's own game included, so every
  game plays by the same ones until the session ends (`sQuests`, the quests shared with the story, is one since protocol
  58: a byte after `sLegendaryDifficulty`'s, `QuestRule`, an unknown value read as `story`) (decoded rules are clamped: bleed-out 5-600 s, help
  0.5-30 s, health 1-100 %, window up to 600 min, cooldown up to 3600 s). The downed ones (bleed-out and help
  times, health, revives per fight or per window, the cooldown after getting up, helping only out of combat)
  are kept by DownRules::Self on the downed player's side: out of revives, or in the cooldown, the player is
  not made essential, so a lethal hit kills as it would alone. A fight is over 30 s after the player's last
  frame in combat (on the ground counts as in combat). The helper checks its own combat state for
  `bReviveOutOfCombat`. `bDownedCrawl` is the welcome's flag bit 16, added without a protocol version of
  its own (off is what a game from before it does). The defaults are the behaviour before the rules.
  `bShareGuestBuilds` (the welcome's flag bit 8, which older builds neither set nor read, so the protocol
  version is unchanged) lets a guest's game report what its player built before the session
  (`BuiltWalk::Shares`; see Shared settlements). Rules that reach `Self` while the
  player is on the ground (a welcome back into the session mid-down) wait until they are up: a down finishes
  under the rules it began with, so a shorter bleed-out or revive turned off never kills on the spot; the
  revive count of the session carries on through any change. A joiner is told rules that differ from the
  defaults once per session on the HUD ("Host's rules: revive limit 3 per fight, 30 s to bleed out";
  common/RulesSummary.h, tested), and the F1 main page shows the same line while in the session. With the
  defaults nothing is shown.
- VATS in real time (src/game/RealTime.cpp, src/game/RealTimeVats.h; `bRealTimeVats`): the game slows VATS's
  aiming through `VATS::magicTimeSlowdown` (0.04 while aiming, measured on 1.11.240; the global time multiplier
  and its target follow it; Jet sets the same slowdown to 0.3), and a shot playing out through the VATS
  camera (0.001 or the shot's own). While VATS is up (its menu, or a shot playing out) a global time
  multiplier under 1 is set back to 1 each frame (`BSTimer::SetGlobalTimeMultiplier(1, true)`), unless the
  slowdown was already on the frame before VATS came up (Jet or the like).
- VATS never targets a friend's stand-in (src/game/VatsTargets.cpp, src/game/VatsTargetRules.h; `bVatsSkipsFriends`,
  each player's own, on by default; no protocol). VATS lists, highlights and locks onto the nearest target first, which
  in a group was the stand-in of the friend in front. Every actor VATS can list or keep as a target is first asked of one
  engine function (+B16220, ID 2224729), from four calls in the VATS menu's code: the gather of potential targets from
  the process lists' high actors (+B164B0, within `fVATSMaxEngageDistance`), the check of a target reference the menu's
  other paths ask (its update, its highlights, the projectiles' gather), and the two "anything to target?" checks (the menu
  button, and the refill of an empty list, which plays `UIVATSNoTarget` with none). It already says no for a child, a
  player teammate (Actor flag 0x4000000 at +2D0: why companions are never in VATS), a ghost, the dead, a hidden 3D and a
  Stealth Boy. The four calls go through a hook (checked to call that function where 1.11.240 does, all or none
  patched) that says no for a friend's stand-in (`Puppets::IsPuppet`; a mirrored NPC stays a target) and asks the engine
  for anyone else. Engine data was looked at first and none fits: teammate also puts the stand-in in the player's team
  for the AI, perks and quest conditions; a ghost cannot be hit at all; the `VATSRestrictedTarget` keyword (what shows
  `sVATSFollowerTarget`) keeps the actor in the list, highlighted and nearest first, only refusing the attack; the form
  flag `TESForm::SetVATSTargetOverride` sets is read for destructible objects only. With only friends around VATS does not
  open. Shooting a friend without VATS is as before (friendly fire is the host's `bFriendlyFire`). `net status`:
  `vatsSkip=` on/off/unhooked, `vatsFriendsSkipped=`; the dev `vatstargets` command lists the potential targets.
- Tutorial hints: the game's
  `bShowTutorials:Interface`, which its help messages read each time, is held off in a session
  (Background::Switch) and put back after; its tutorial message boxes are not gated by it.
- Menus still pause: clearing `kPausesGame` as a menu is made (wrapping its `UIMenuEntry::create`) was tried
  and taken out. The UI counts `menuMode` from the flag after the menu has handled its show message
  (0x141a811bc) and again at close (0x141a80a49). Made without it, the Pip-Boy came up with a blank screen
  and would not close, and a container was still counted (`menuMode` 1). What a stopped game does to the
  session is asked of `GameStop::Now` alone (src/game/GameStop.h), the place a menu made to run in real time
  would have to be told apart; meanwhile the NPCs a stopped game runs go to friends near them after 5 s (see
  Shared NPC AI). `menuMode` itself is read in GameStop.cpp only (a test checks the source; `GameStop::MenuMode`
  for logs). What stops the world under a menu is not known yet (`Main::freezeTime` stays false;
  `BSTimer::disableCounter` or a `menuMode` test in the main loop are the candidates, F4Multiplayer-tnl5): the dev
  `menuclock` command (src/game/MenuClock.cpp) logs the UI's count and freeze frame, the engine's and the UI's
  timers, Main's flags, the game hour and the nearest NPC's movement each frame after a menu opens or closes, and
  tools/menumodescan.ps1 lists the game's functions that load the UI and use its +0x1E0, and the operands into
  the timer, to disassemble.
- Voice (src/game/Voice.cpp): `ISteamUser::StartVoiceRecording`/`GetVoice` (compressed) while the
  push-to-talk key is held, sent unreliably (server budget 16 KB/s per player). Receivers run
  `DecompressVoice` at 24 kHz, scale the samples by distance to the speaker's stand-in (full within
  8 m, silent beyond `fVoiceRange` or in another cell/worldspace) and queue them on one winmm
  `waveOut` device per speaker, with 80 ms of silence first when it was idle.
  `SetInGameVoiceSpeaking` tells Steam's own chat to mute the mic while we record.
- Map (src/game/MapShare.cpp): map markers are persistent references with `ExtraMapMarker`. Its
  `MapMarkerData` isn't in CommonLibF4: a TESFullName (name at +0x08), then a flags byte at +0x10
  (1 = on the map, 2 = can fast travel). We list the markers once, poll their flags every 2 s and
  report new bits. Receivers run `showmap <ref> 1` (the console records the change for the save;
  writing the byte wouldn't). The server keeps the session's markers and sends them with the world
  state (on join and after loading a save).
- Quest completion: `QUEST_DATA::flags` 0x0002. A quest that completes within 10 s of another
  player's stage was completed by them; otherwise `ReportQuestDone` names us to the others.
- Conversations (src/game/Conversations.cpp): dialogue plays in one game only. The talker re-claims
  the NPC every 2 s, so nobody takes it over mid-conversation, and if a friend runs it, their mirror
  is paused so our copy can talk. Lines are read from `HighProcessData::strVoiceSubtitle` (filled
  even with subtitles off; `voiceState` stays 0) while `IsTalking()`. The player's lines, and those of
  NPCs we run or talk to, are reported unless the speaker is in combat. Receivers show
  "Name: line" when their copy of the speaker (or the friend's stand-in) is within 20 m, isn't saying
  it already, and the same line wasn't shown in the last 5 s (at most 3 lines per 3 s).
- Friends' lines as subtitles: a line shown here goes into the game's own subtitle area, the way the game
  shows a nearby NPC's line, through the engine's `SubtitleManager` (its list of who is saying what, from
  which it shows the nearest line of the highest priority each frame, within `fMaxSubtitleDistance`, 1500
  units). Added with `SubtitleManager::AddSubtitle` (Address Library 2249542; the speaker is our copy of the
  NPC or the friend's stand-in, no INFO, normal priority, so the game names the speaker as it does its own),
  taken out with `RemoveSubtitle` (2249543, every line of that speaker; skipped while our copy is saying a
  line of its own, whose end takes them all out). It stays up as long as the recording lasts
  (src/game/SubtitleTime.h: the .fuz's XWMA packet table, `dpds`, or a .wav's data size and byte rate, read
  through `BSResource`; plus 0.5 s, at least 2.5 s, at most 30 s), or for a line with no recording as long as
  it takes to read (1.5 s + 50 ms a character, 2.5 to 12 s); whether we play the recording or not. The game's
  rules for the subtitle area, read from its code: out of a conversation a line is added and shown only with
  `bGeneralSubtitles` on; in one, only the conversation's own lines (the player's and their speaker's), with
  `bDialogueSubtitles`. So with General Subtitles off, or while the player is in a conversation
  (`MenuTopicManager::menuOpen`), a friend's line is the "Name: line" HUD notice as before. A line the engine
  no longer lists 200 ms after it was added (its speaker's own line ended and took it out) becomes that
  notice too.
- Spoken lines (0.8.7, `bSpokenLines`, on): a line goes with the recording it is (`Line.info`, the
  INFO; `Line.response`, the response number its voice file is named with; `Line.voiceType`, the
  NPC's voice type, or the default object `kPlayerVoiceMale`/`kPlayerVoiceFemale` for a player). The
  reporter takes them from the dialogue item on the speaker's process (`HighProcessData::greetTopic`)
  while its current response's text is the subtitle, or else from the last greeting's response with
  that text; a line it can't place goes as text only. The hearer builds
  `Sound\Voice\<plugin>\<voice type editor ID>\<INFO id>_<response>.fuz` (src/game/VoicePath.h; the
  info's own plugin, its id with the load-order byte 00; a shared info's (`dataInfo`) file when the
  info has none), checks the game has it (`BSResource::GetOrCreateStream`) and plays it with
  `BSAudioManager::GetSoundHandleByFile` as a 3D sound (flag 0x10, the `kDialogueOutputModel3D`
  output model) that follows the speaker's 3D: the same range and same-space test as the text, in a
  conversation or a scene or not. The flag and the extension are the engine's own: its actor voice
  calls (Address Library 2229972 and 2229973, 1.11.240) play `sound\` + the response's file through the
  same `GetSoundHandleByFile` with usage flags 0x10 (0x80 with priority 128, 0x4000 in one case), and the
  audio manager's file source skips a .fuz's `FUZE` header and lip data itself. What the engine has and a
  plain sound does not is an end: it sets the speaker's voice timer from the recording's length
  (`GetDuration`), and the line ends with it. So each friend's line's sound has a hard end
  (src/game/LineSounds.h): the recording's length plus 1 s, at least 2.5 s and at most 240 s (the game's
  longest recordings are Magnolia's songs, to 214 s; companions have lines of 35-38 s), or 15 s when the
  length can't be read. The length is read from the start of the file that plays (the .fuz, or the .wav
  or .xwm the audio manager took instead): its lip sync and the RIFF chunks before the audio
  (`SubtitleTime::HeaderBytes`), not the recording. At most 4 play at once (a fifth line
  ends the oldest); a speaker's next line ends the one before. Before this, a sound that did not end by
  itself was only ended by that speaker's next line, and one that the audio manager stopped calling
  playing was forgotten with no way to end it: a player heard a loud engine-like noise that grew with
  every friend's line. A sound still playing at its hard end, or crowded out, is logged (at most one line
  in 30 s, with how many went unlogged: "Conversations: the sound of a line from player N ... ended") and
  counted in net status
  (`sounds=`, `cutLong=`, `cutCrowded=`). Nothing goes through the dialogue system (no Say, topic, scene,
  conversation or lip sync), so nobody can be held in one. Until 0.8.6 an NPC's line was the console's
  `Say <topic>` on our copy (off by default from 0.8.6-rc4: it could leave the player held in
  dialogue); the player's own lines were never voiced.

## Text and languages (src/Text.h, src/TextEnglish.h, dist/lang)

Everything a player reads that the plugin wrote (HUD notices, the F1 menu's pages and buttons, the server's refusals
as the joining game shows them) is a key with a text, not a literal in the code: `Text::Get(key)`, or
`Text::Format(key, { { "player", name }, { "n", 3 } })` for a text with `{named}` placeholders. A placeholder the
arguments lack stays in the text as written, so a mistake in a translation shows where it is read. The English texts
are compiled in (`TextEnglish.h`) and are the fallback for every key a language file lacks; `Text::Load` (at startup,
after `Config::Load`) picks the language (`sLanguage` in `F4Multiplayer.ini`, else the game's own `sLanguage`, read
from `Fallout4_Default.ini`, `Fallout4.ini` and `Fallout4Custom.ini` in that order, else English) and reads
`Data\F4SE\Plugins\F4Multiplayer\lang\<code>.txt` (`key = text` lines, UTF-8, `#` comments, `\n` a line break).
`Text::CodeFor` maps what the game calls its languages (`ptbr`, `cn` = Simplified, `zhhant`/`tchinese` = Traditional,
`esmx` = Latin American Spanish, its own file `es-mx`) to the file codes. The files ship from `dist/lang` (tools/package.ps1, tools/deploy.ps1). The
tests (tests/TextTests.cpp) hold every English key to the literal the code showed before, every language file to the
mod's keys and each text's placeholders, `en.txt` to the built-in table, and the code to no bare English literal in a
`Hud::Notify` or a menu page. The log, the dev channel's replies and the dedicated server stay English: they are
for people debugging, and a notice's log line is written from the English text (`Text::FormatEnglish`). The
server sends its refusals in English on the wire (any server version, and a dedicated one, sends the same text);
`Text::Reject` shows the ones this client knows in the player's language, sentence by sentence for the load order
refusal, and anything else as it came. Arabic is not shipped (the reviewed file waits in a pull request): the game ships no Arabic font and its Scaleform text
fields do not shape right-to-left text. A translation's first header line says what it is: a first machine translation
(most of them), or one reviewed against the game's own terminology in that language, with the date (German, Polish, Brazilian Portuguese).

### Player names (src/net/NameChoice.h)

The name in the Hello comes from `NameChoice::Choose(sPlayerName, character, steam)`: a name in the ini is used as
written; `character` (the default in the shipped ini) is the player character's display name
(`PlayerCharacter::GetDisplayFullName`), else the Steam name, else "Vault Dweller"; an empty value (older ini files)
is the Steam name. The server keeps every letter and digit of every script (`common/Name.h`, `Name::Shown`: the
name is validated as UTF-8 byte by byte, what isn't UTF-8 is dropped, so are control characters, line separators,
the bidi overrides and other invisible characters, and `<` and `>` since the HUD's text fields read HTML; at most
`Name::MAX_CHARACTERS` = 32 characters, counted in code points, so a Cyrillic name keeps as many letters as a Latin
one; `Protocol::MAX_NAME_LENGTH` is 128 bytes on the wire, four per character; a name nothing is left of is
"Player N"). The client cleans the names it hears the same way before they reach the HUD, and its own the same way
before it sends it. The name is read when the Hello is sent, and looked at again once a second while in the session
and in the world (with the character check; not at the main menu, where the character's name may be gone): a hosting game
says hello again after every character change (hosting restarts and the game connects to its own server anew), a
joining game that loads another character leaves and its next join reads the new name, and a joining game that
connected before a new character was named in the opening (the character is named in the opening, after the
connect) said hello under the Steam name and sends `kRename` once the character's name differs from what it sent
(protocol 34). The server cleans it as a hello's name, keeps it for later joiners' welcomes, tells the others
(`kRenamed`, not the sender; two a second per player at most, on top of the event budget: one that comes sooner waits
on the player as `pendingName` and a newer one takes its place, so the last name a game sent is the one that stays
even when the network brings two together; the same name again goes nowhere) and logs "player N renamed", never the name. The hearers change the stand-in's name tag and the player
list's entry and show "{old} is now {new}".

Encoding: a name is UTF-8 all the way. Steam's persona name is UTF-8 (the Steamworks API speaks nothing else), the
game's strings are UTF-8 (its .STRINGS files and Scaleform's text, so the character name typed in the opening is
UTF-8 in `GetDisplayFullName`), `Protocol::Reader::Str` drops only bytes below 0x20 (every byte of a multi-byte
character is 0x80 or more), `Text::Format` copies bytes, and `ShowHUDMessage` and `ExtraDataList::SetOverrideName`
take a UTF-8 `const char*` into Scaleform; no step narrows to ASCII or to the Windows code page. What the HUD can
draw is another matter: the game's text fields use the fonts embedded in `Interface/fonts_<language>.swf` through
`Interface/fontconfig.txt`. Checked live on 2026-10-18 (two English games, names from `sPlayerName`): the
stand-in's name tag in an English game draws a Cyrillic name in full (in the font's capitals: "Нейт Волков" shows
as НЕЙТ ВОЛКОВ), and a CJK name as one box per character (内特·山本 as two boxes, the middle dot, two boxes); a
Chinese or Japanese game's own font should draw it. Emoji are in no game font. A player who wants CJK in an English
game can map a font with its glyphs in `fontconfig.txt` (a font mod).

## Settlement power (src/game/WorkshopPower.h, WorkshopSync.cpp)

A friend's piece in our game is a copy (`WorkshopSync`): a plain reference of the same object, deleted when
the session ends or its builder scraps it. Two things follow for power.

**No saved grid may name a copy.** A settlement's power grids (`Workshop::ExtraData::powerGrid`, saved with the
workbench) name their pieces by form ID. Workshop mode used to offer Attach Wire on a copy, and wiring our light
to a friend's generator put the copy on our grid, gave it power links and linked it to our workbench. Deleting
the copy left the grid naming a deleted reference, whose ID the game later hands to something else: the
classic corrupt-grid save. Three layers, outermost first:

1. *Out of reach.* Every copy, made or adopted from a save, gets the default object
   `WorkshopBlockInteractionKeyword`'s keyword (`TESObjectREFR::AddKeyword`, saved with it). Workshop mode's pick
   test skips a reference with it, so a copy can't be wired, stored, scrapped or moved (which also ends the free
   components from scrapping a friend's piece).
2. *Taken off.* An audit (`WorkshopPower::Audit`) looks for copies tied into our power: named by a grid of a
   workbench we have seen, with power links (`ExtraPowerLinks`), or linked to a workbench as a workshop item. It
   runs every 2 s in a session and before every save (`kPreSaveGame`, on the main thread before the game writes
   anything). One it finds is taken off through the game's own scrap path: `Workshop::ExtraData::RemoveItem`
   (the grid), `SplineUtils::DisconnectSpline` on each wire strung to it (our wires go with it, reported
   scrapped so the friend's copy of the wire goes too), and its workbench link cleared.
3. *Never deleted while tied in.* Every copy's removal goes through `WorkshopPower::Remove`: taken off first if
   it is tied in, and if the game still ties it in after that, it is switched off and kept, never deleted, so
   the grid's ID still names it and nothing else. Copies waiting in the graveyard count as copies for the
   audit.

**Our own pieces are never taken for copies.** A copy an earlier session left in the save (a crash) is adopted at
its builder's piece's spot (`FindExisting`): the same object, made at runtime, within 4 units. Two saves that share
a past (one save, played on by both) hold the same built pieces at the same spots, and the old guard (the
workshop's own extra data, which only a workbench carries) let our own pass: adopted, it was blocked from workshop
mode, unlinked from our workbench by the audit (its settler's job with it) and deleted when the session ended. A
piece linked to a workbench as its workshop item is ours and is never adopted; a friend's report of the same object
at its spot makes no copy over it (`OursAt`, `oursThere=`), and their scrap of theirs leaves ours standing. The host's game
reports what its player built before the session (`BuiltWalk`); a guest's does too only when the host sets
`bShareGuestBuilds` (a session rule, sent with the welcome; off by default, since each player keeps their own save and a
guest's solo settlement would land on top of the host's). Building done in the session is shared either way. Then a third
player can get the same piece from two friends: the second report finds the first's copy at the spot and makes none
(`WorkshopApply::TwinOf`, `twinned=`). A twin is another player's copy (one of the reporter's own is another piece of
theirs) of the same object standing just so, turned the same way (two of one object crossed at one origin are two pieces).
The report is kept with the key of the copy that stands for it (`twins=`): if that friend scraps or moves theirs, the other
friend's piece still stands in their game, and is placed then (`untwinned=`). A third player on a build before this check
still gets such a piece twice.
Friends' copies stay out of workshop mode's reach by design (above), so a friend's crop or post can't be a job for
our settlers; each player assigns jobs at their own pieces.

**A friend's crafting station reads our settlement's junk** (`CraftingWorkshop.h`). A workbench menu (chemistry,
cooking, armor, weapons) gathers what its recipes may use from the settlement the station is linked to as a workshop
item (`Workshop::GetWorkshop`: the workbench's own inventory and those its supply lines reach); with no link, only the
station's and the player's own things count. A copy is never linked (above), so crafting at a station a friend built read
only our player's inventory (a 0.10.1 report: at the host's stations only the host's own components counted). The
gather's one call to `Workshop::GetWorkshop` is patched (`WorkshopSync::InstallCrafting`, its bytes checked first): the
engine's answer stands, and a station with none that is our copy of a friend's piece reads the workbench its report
named, when that is a workbench here (each copy is noted with it as it is kept). Nothing is linked or saved. `wsstatus`
counts `craftingAtCopies=` and `craftingUnbenched=`, and the first 20 of each are logged.

**A piece its builder's world lost goes for everyone, and comes back if it has it again** (`WorkshopAudit.h`,
`WorkshopStore::BuiltBy`, `Gone` and `Tag`). The server keeps each built piece under its builder and their reference ID,
and only that builder's scrap takes it out; a game reports no scrap of a piece it lacks, and none of a copy of a
friend's. So a builder who loaded a save from before they built something (the host reloading an older save) left it in
every friend's game, sent again at every join, and nobody could scrap it (the test rig's session held six chairs that
way for days). Now the server sends each player what it keeps as their own building in the world their game plays
(`kOwnWorkshopItems`: pieces made at runtime, never the world's pieces they moved or scrapped), and their game looks for
each one (`ownLooking=` `ownFound=` `ownGone=` `ownLostLooking=` `ownBack=` in `wsstatus`). What it lacks it reports
(`kReportWorkshopGone`); the server takes those out, if what it keeps is still that object (one built since under the
same reference ID stays), off the builder's share, and tells the others to scrap their copies. Nothing else is
touched: only a builder's own game can say, so an offline player's pieces stay, and a report from one player names only
their own (the server sets its player id).

*Whose world.* The player id is the PC's (`Hello::identity`), not the character's: a friend who built with one character
and joined with another (a new game, an alt on a dedicated server) would have been asked about the first's pieces in a
world that has none of them, and every one would have gone for everyone (the rig VM's four chairs were probably that).
So each built piece keeps the character whose world built it (`WorkshopItem::character`, the server's alone, never on
the wire: the builder's `DropRecord::character` when the report came). The list goes only once the player's record after
their welcome or load has said whose world it is (the record comes after both; a world request that isn't a gap marks
the world unknown until it does), holds only that world's pieces, and a gone report is taken only for that world's.
What a player reports before their record is tagged when it comes (`Player::untagged`); a piece whose character isn't
known (an older build's, a layout 41 file's) is never asked about. So a report of the world before a load must reach the
server before the load's request for the world: the game's reports wait in a paced line (`SendPacer`, 200 a second), and
one still waiting at a load (a piece built a moment before, a blueprint) went after the request, was tagged with the
loaded world's character, looked for there, and removed for everyone (up to 0.12.0). Loading another character's save
leaves the session, which empties the line, but a world under another key the session keeps: a new game, a main-menu
`coc` world, or the same character's save from before keys (`CharacterBinding::World`). Now a load fences the line (`SendPacer::Queue::Fence`, the first
thing `WorkshopSync::OnGameLoaded` does) and the request goes behind what was fenced (`TakeFromWorldBefore`): as many as
the pacer lets go then, the rest dropped (`dropped=`, logged): for a friend the piece is missing rather than gone for
everyone, and the builder's walk, where it runs, finds it again. Only when the world changed: a load of the world the
server last heard of (`CharacterBinding::World::SameAsTold`: a death reload, a quickload, the commonest) lifts the fence
(`Unfence`) and the line goes on at its pace, since after the request its reports are tagged with that same world. Holding
the request until the line drains instead would lose nothing, but it would move the record and the rest of what follows a
load behind the backlog too.

*Reversible.* A piece reported gone is not forgotten: it is kept as lost (`WorkshopOp::kLost`, at most
`WorkshopStore::MAX_LOST`, the longest lost going first), saved with the session, and sent in the list marked lost. A lost
piece the builder's game finds standing (a later save loaded, a cell that wasn't loaded after all) it reports placed
again, and the server keeps it as built and relays it: it is back for everyone ("has ... again" in the server log).

*Sure it is gone.* "Not in our world" must mean gone, not unloaded: a built piece that isn't persistent is in memory only
while the cell it was built in is loaded, and one moved since keeps that cell (above, "keeps its parent cell"). A
reference with its ID that is something else (another object, deleted, one of our copies) is gone once a second look
says so too; a piece with no reference is gone only after three looks over 10 s while the 3x3 cells around its spot (a
wire's: around each end) have each been attached for 10 s (`CellAges`, kept from the cells the walk sees attach) and
the player stands in its workbench's build area. One in another worldspace, towns away, or of a workbench we lack (left
out of the list by FormTranslate) waits. The audit's time is that of frames in a settled world (the player in a cell,
no loading screen or load, no menu stopping the game, not the opening), not the wall clock, and nothing is looked at
otherwise. The list is forgotten at the next load, which brings it again. Rejected: the builder's full list replacing
its entries after a load. No game has its full list: what it built before the session is found as cells attach, and
only by the host (or a guest under `bShareGuestBuilds`), and the power snapshot (`Reporter::SendAll`) names powered
pieces only. Known gaps: a piece moved more than a cell from where it was built, in a build area wider than the loaded
grid, can still be taken for gone while its own cell isn't loaded; it comes back once that cell loads and the game sees
it standing. And two characters of one PC number their pieces alike: a piece the second builds under an ID the first's
piece has takes its place in the store.

**A friend's piece waits for its own cell.** Both searches walk only the attached cells
(`TES::ForEachReferenceInRange`), while a workbench, a persistent reference, is in memory from anywhere in its
worldspace. So a report is placed only once the exterior cell its piece stands in is attached (`SpotAttached`);
until then it waits under that cell. Placed earlier (as older versions did), from a town away, the copy went over the save's
copy (after a load away from the settlement, which forgets the copies' handles) or over a piece of ours, unseen:
two at one spot, flickering. Copies left doubled that way go when their piece is placed again (`LeftoverAt`,
`leftovers=`). Only a copy some session kept counts (it has the block keyword), never a loose reference of the same
object.

Rejected: stripping copies' links before every save and putting them back after. A save happens at any moment
(autosaves, quicksaves, the game's own on sleep and fast travel); a put-back would re-run grid code mid-session,
fire the game's power events into scripts, and would need wires that were never ours. Blocking the wire in the
first place keeps every grid our game builds made of our own pieces.

**Each builder's game says what is powered.** Cross-player wiring is not done: each player wires their own
pieces, and both games show the result.

- The builder's game tracks its own pieces with power (runtime-made workshop items of its workbenches,
  `PowerUtils::ItemIsPowerReceiver`, `ItemIsPowerConnection` or `ItemGeneratesPower`; no wires, turrets or
  copies), polls `Workshop::IsPowered` every 500 ms and sends what changed (`kReportWorkshopPower`, 255 entries a
  packet). It sends all of them when someone joins, after a load, and every 30 s (a late or lost packet heals).
- The server relays it as the sender's (`kWorkshopPower`, the event budget, sender stamped) and keeps nothing.
- A friend's game keeps what each builder says per (builder, piece) and shows it on the copy, as the game's own
  powering does: the graph event (`Powered` / `Unpowered`: a lamp's glow, a fan turning, a machine running) and, for
  a light (LIGH), its light switched (`TESObjectLIGH::SwitchLight`, which returns at once for any other form in
  1.11.240). Only two of the base game's light recipes are LIGH (the light bulb, the industrial light); the others
  (the construction light, the light box, the high-tech lights, the strobe, the fan light, the mirror ball) are
  activators lit by their graph alone (the generic workshop graphs). The game loads a copy's graph after its 3D, on
  its own, drops an event sent before then, and, once loaded, shows only its own powered workshop items as powered
  (a graph load's init, +529120): a copy's new graph starts unpowered. So a copy whose model names a graph (the
  NIF's `BGED`) isn't drawn until the graph is loaded (`WorkshopPower::Drawing`; waited for, no try spent), and a
  graph made anew counts as the copy drawn anew. Shown again whenever the copy is drawn anew (its cell loaded again,
  the copy made again, its graph made again), tried 8 times if the game doesn't take it (a loaded graph that refuses
  the event), and shown once more when that drawing has lasted 2 s, counted with the redraws (6 a minute). Through
  0.11.0 the show counted as done whenever the copy had a 3D, so a light already powered as its copy appeared (built
  in a lit camp, wired at once, a friend coming to a camp already lit) was shown before its graph was there and
  stayed dark. `graphWaits=` and `graphRefused=` in `wspower`; the first 32 shows of a session are logged
  ("friend's piece ... shown on: graph took" / "light switched"). Not through the game's own power handler: that
  fires `PowerOnEvent`/`PowerOffEvent`, and the scripts on a copy (a workshop object's, a radio beacon's) would act
  on them as if the piece were ours.
- A conveyor belt (Contraptions Workshop's belts) is neither a graph nor a light: it runs by the engine's conveyor
  on its reference (`BGSConveyorBelt::ConveyorBelt`, one of its init actions, `ExtraInitActions`; a surface velocity
  on the model's belt node), made running with its 3D when the model asks for one (+50A47B) or by a script's
  `ApplyConveyorBelt`. Whether it runs is set only by Papyrus (`ObjectReference.ConveyorBeltOn`, whose native nothing
  else in 1.11.240 calls; the power handler never touches a belt), so by the belt's own script on the power events,
  which a copy never gets: through 0.12.1 a friend's conveyor stayed as its own script left it, stopped while their
  game ran it (a 0.11.1 report). So a show also sets a copy's belt running or stopped as `ConveyorBeltOn` does (its
  native, +259120, no events), a save's hush too, and a belt its own script stopped or started again since our show
  (a copy is never powered for it) counts as a lapse and is shown again (`WorkshopPower::StillShows`, with the
  redraw cap). `beltsSwitched=` in `wspower` (`lightLapses=` counts the belts' lapses too), `belt=running|stopped` in
  `wsinfo`; the log's first shows say "belt set" for one with no graph. Not known without the DLC's files: what the
  belt's script does on its other events; a script that keeps setting it is answered at most 6 times a minute.

**A save never keeps a copy running.** A copy's graph keeps what it was told, and a save writes it: a friend's generator
shown "Powered" came back from the save running, with no session to tell it otherwise, and a copy can't be scrapped (the block
keyword), so its engine ran on in a game with no friend in it (found on the rig, after a Nexus report of a generator sound that
never stops: show a copy on, save, `net leave`, load: the sound is there and nothing stops it; with the copy
shown off before the save, a load is silent). So before every save (`WorkshopSync::BeforeSave`) each copy shown powered is
shown unpowered (`Mirror::Hush`: its lights and its graph), and once the save is over (kPostSaveGame: `AfterSave`, else
`HUSH_LIMIT` later) it is shown as its builder's game says again (a frozen copy too: it keeps what it showed). `hushed=` in
`wspower`. Not done: a copy a save from before this holds running (or one nobody reports any more: its piece is gone from the
friend's game) stays so in a game that has it; the session's reports show the ones it names, the others stay.

Trade-offs:

- A real shared grid (copies as real workshop items of the receiver's workbench, the game computing power on
  both sides) would let a friend's generator light our lamp. It needs copies to be our workshop items, which is
  what made the corruption possible, and two games computing the same grid can disagree (destroyed generators,
  a piece not loaded in one of them). The mirror shows exactly what the owner's game computed.
- Pieces the settlement came with keep each game's own power (only runtime-made pieces have copies).
- A piece whose builder is away shows its last state; nothing powers or unpowers it until they come back.
- Turrets and other actors are not mirrored (their copies are actors in the player's faction, not powered pieces).

Message: `kReportWorkshopPower` 62, `kWorkshopPower` 162: playerId (u32, stamped), a count (u8, at least 1), then
per entry a reference (u32, runtime range only) and its state (u8, 0 or 1). Protocol 46.

Dev: `wsgrid [bench]` (each grid's nodes, links and powered pieces, with any copy or deleted reference it names; for
one workbench, the pieces too), `wspower` (counters), `wspower block on|off` (copies pickable again, to try the
repair), `wspower audit`, `wspower copies` (each copy and whose piece it is), `wspower show <ref> on|off`,
`wswire <from> <to>` (a wire strung as the game strings one, for the rig), `net leave` / `net stophost` (end the
session as a player would), `wsinfo <ref>` (now with `blocked=`, `powerPiece=`, `powered=`, `owner=` (a crop's or
post's assigned settler shows as its owner, the settler's base NPC), and for a copy `builder=`, `theirRef=`, `graph=` (loaded, loading or none), `lit=` (a light's) and
`theirPower=`), and `scriptvars <ref> <Script> <name...>` (a bound script's state and its properties' or variables'
values, read without calling into the VM: `scriptvars <crop> WorkshopObjectScript workshopID` is -1 until the
workshop's reset has run, as it does when the player enters the settlement; until then the game says "This resource is
now assigned" yet assigns nobody, so a test that loads straight into a settlement it then owns by console runs
`cqf WorkshopParent ResetWorkshop <bench>` first).

## A friend's pieces count in your settlement (src/game/ResourceMirror.h, WorkshopResources.cpp, WorkshopSync.cpp)

Crops, water pumps, turrets and generators a friend built count in your settlement's food, water, defense and power,
as they do in theirs. Before, a friend's piece was a copy that counted only in its builder's game (a copy is linked to
no workbench: see "Settlement power"). It still is: this mirrors the numbers, never by linking a copy, putting one on a
grid or telling the workshop scripts of it.

**How the game counts.** A settlement's numbers are actor values on its workbench (`resourceFood`, `resourceWater`,
`resourceSafety`, `resourceBed`, `powerGenerated`, `powerRequired`). Found on the rig (2026-11-04, Sanctuary, `wsres`
dev commands), with three vanilla crops, a placed crop, water pump, bed and small generator linked to the workbench:

- The workbench's recount (`RecalculateResources`, run as pieces are built, scrapped, assigned or powered) sets each
  value's base to the sum of every linked piece's value, and its damage modifier to minus the sum of the pieces that
  can't produce now: food base 2.00, damage -2.00 (four unassigned crops at 0.5), water base 3.00 (the pump needs no
  worker), beds base 1.00 damage -1.00 (unassigned), power base 3.00. So it is the engine, not a script, that zeroes
  an unassigned crop: its `CanProduceForWorkshop` condition, summed over the pieces, gave exactly the workbench's
  numbers for every kind (`wsres sum`).
- It leaves the permanent and temporary modifiers alone: +5 food (permanent) and +3 water (temporary) came through
  the recount, a workshop mode visit and a piece placed and linked.
- The Pip-Boy's workshop data (`PipboyDataManager::workshopData`) and the workshop scripts (`GetValue`: the daily
  update's production, `GetTotalFoodRating`, `GetTotalWaterRating`, safety) read the value with both modifiers: the
  Pip-Boy said Food 5, Water 3, Defense 4, PowerGenerated 5 with modifiers of those sizes on. Beds are the
  exception: the Pip-Boy's Bed is the base alone (a temporary +1 left it at 1, a base +1 made it 2), and the daily
  update reads `GetBaseValue` for a settlement's beds as well as `GetValue`.
- Modifiers are written into the save: a save with +5 food on, +15 in memory, then a load: +5.

**What is sent.** Each game tracks its own pieces that count for anything (made at runtime, linked to one of its
workbenches as a workshop item, standing, no copy; `TrackOwnResources` every 2 s) and polls them once a second
(`ResourceMirror::Reporter`): each one's value of each kind if the engine's test says it produces now, else nothing,
as its own recount counts it. A change goes at once, all of them every 30 s and to a joiner, a piece whose cell isn't
loaded as last sent. A piece that is no longer one of ours (scrapped, or disabled, deleted or unlinked with no scrap
report, which a script may do) goes once as counting for nothing, so the friend's total leaves it out though its copy
stays. The reports go reliably; one the full send queue refuses goes again at the next poll as its piece counts then
(`Reporter::Unsent`), so a gone piece's one word, which no later snapshot names, is never lost. When the builder's own
world goes (a load in session, a death's reload, a new game, or the main menu opening: a `coc` from it starts a world
with no load or new-game message), every tracked piece that counted is forgotten and sent once as counting for nothing
(`Reporter::WorldGone`), and the pieces the new world has are found and sent again (no piece is looked for while the
main menu is open). A look can't tell a piece gone with its world from one whose cell isn't loaded (both name nothing),
so without it a friend's game counted a builder's pieces from before a new game until the builder left (seen on the rig,
2026-11-04). A reload of a save with the pieces in it takes them out of the friend's numbers for a moment, until they
are found again (up to 2 s once their cell is loaded). Per piece, not per workbench: a friend's game counts only the pieces it has a copy of, so a piece both saves hold (two saves with one past: no copy is
made, `OursAt`), another friend's twin of it, or a guest's building from before the session that the host's rules
don't share never counts twice or out of nothing.

**Caps.** One piece counts for at most `Protocol::MAX_RESOURCE_PIECE` of each kind: 10 food, 100 water, 50 defense, 10
beds, 200 power made, 50 power taken, over the biggest vanilla piece of each (the PRPS of Fallout4.esm and its DLCs: a
greenhouse planter's 2 food, an industrial water purifier's 40 water, a missile turret's 15 defense, a double bed's 2,
the interior fusebox's 100 power, the Institute's beam emitter's 20 power taken). A message whose value is no number,
negative or past 100000 is dropped; a value past its kind's cap is read as the cap, so the server relays it capped
and the friend's game keeps it so. A workbench holds at most `ResourceMirror::MAX_BENCH` from each friend, and from
all of them together: 500 food, 2000 water, 1000 defense, 200 beds, 2000 power made, 1000 taken, past what an honest
settlement reaches within the build limit (40 industrial purifiers make 1600 water). A value under 0.001 counts for
nothing.

**What is applied.** Each game sums what friends' games say, over the copies it has, at the workbench each copy was
reported at (`ResourceMirror::Tally`), and once a second makes what it has added to each workbench equal that sum
(`ResourceMirror::Applier`), on the value's temporary modifier (`WorkshopResources::AddToBench`; scripts' `ModValue`
writes the permanent one, the recount the base and damage). What it added it alone knows and takes off:

- before every save (`kPreSaveGame`, the first step and one of its own, `WorkshopSync::BeforeSaveResources`), put back
  after it (`kPostSaveGame`, or 5 s after the last hush if no word comes; a second save before the first is over keeps
  it off until both are). Taken off, it is all of what was added however small, so no remainder is saved. What
  couldn't come off (no such workbench then), or all that is on should the hush throw or not run, is written in that
  save's co-save record `RSRC` (`ResourceMirror::SaveRecord`, made fresh for each save, never an earlier one's), and a
  load takes what its record names off again;
- when the friend leaves (at once), and all of it when the session ends;
- at a load (all of it off before it), or a new game from the main menu (no word before it): what was added to the
  old world's workbenches is forgotten, and a friend's pieces count again as their copies are found after it. One clear
  for each, never both (`Applier::ForgetWorld`): after a load, only what the save's `RSRC` record names comes off (what
  the take-off before the load couldn't reach was the old world's, and the save sets the new one's); on a new game, what
  is still on a workbench of the same ID (an engine that didn't set it back), and no record is read. Neither takes a
  value off unless the workbench's temporary modifier still holds all of it, nor ever more than it holds, nor anything
  from a modifier at 0 or below (`ResourceMirror::TakeOffHeld`), so no clear of ours leaves the modifier below 0, raises
  it, or leaves a save a value it didn't hold. From a load's start to its new world's clear nothing goes on
  (`Applier::BeforeLoad`): the load's clear forgets what was on without looking, and a load longer than the save/load
  hold's 60 s lets frames run before it, which could otherwise have put values on the new world's workbenches that the
  clear forgot while they were still on, for the next save to keep. Should the session's own after-load work not run
  (an earlier step threw), the clear is done at the end of the load's message all the same, so the block never outlasts
  the load. The main menu ends a world too, and a `coc` from it starts the next with no message at all: when it is seen
  (open, or opened since the last look), what each workbench still holds of ours comes off as on a new game, the rest is
  forgotten, and nothing goes on while it is open (`Applier::MainMenu`). It is looked at before anything goes on, before
  a save's hush, before a load and as the session ends, so none of those takes the world before's values off a
  workbench that no longer holds them and leaves a save a negative one. It doesn't end a load's block or a save's hush.

  The limit: the new-game clear can't tell our value from another's on the same modifier. Should the engine set it back
  and something else (the engine, a quest's start-up script, another mod) put at least as much on before the new game's
  first frame, ours is taken out of theirs. Vanilla's workshop scripts write the permanent modifier, not this one, and a
  new game's settlements are not yet set up then, so it isn't expected; the rig's new-game check says whether a new
  game sets the modifier back at all (if it does, the take-off is never needed).

Not beds: a modifier on beds would show in the scripts' `GetValue` as beds the settlement doesn't have in the Pip-Boy
or in the daily update's count. They are sent and kept, and put on no workbench (`ResourceMirror::APPLIED`).

Trade-offs and limits:

- A friend's piece counts only while we have its copy (its report placed, its workbench loaded here once); copies are
  forgotten at a load and counted again once found.
- Settlers and their jobs aren't shared: a friend's crop counts in our food only when their settler works it in their
  game (their game says it produces), and our settlers can't work their crops.
- A friend's generator counts in our power numbers, not on our grid: it lights nothing of ours (see "Settlement power").
- Happiness, population and supply lines are each game's own.
- A piece whose base was made at runtime (`ReportRef` never shares one) has no copy anywhere, so it counts only in its
  builder's game. A Machinegun Turret placed with `wsreport place` was one (its templated record gives the actor a new
  base); whether one built in workshop mode is too is not checked yet.

On the rig (2026-11-04, Sanctuary, both directions, pieces placed with `wsreport place` and linked with `wslink`):
the host's water pump (3) and small generator (3) showed in the VM's game as temporary +3 water and +3 power on its
workbench and as Water 3 and PowerGenerated 3 in its Pip-Boy; the host's unassigned crop and bed (the engine says they
don't produce) added nothing; the host scrapping the pump took the VM's water to 0 within 3 s. The VM's pump and
generator did the same in the host's game (Pip-Boy Water 6, PowerGenerated 6: its own 3 and 3, and the VM's). Saves
in session on both took the values off for the save and put them back; loading those saves in session showed them
again once the copies were found, and out of session showed none (temporary 0 for every kind). The VM leaving took the
host's to 0 at once, and the VM's own to 0 with the session.

Message: `kReportWorkshopResources` 77, `kWorkshopResources` 181: playerId (u32, stamped), a count (u8, 1 to 128), then
per entry a reference (u32, runtime range only), a mask (u8, bits 0-5: food, water, defense, beds, power made, power
taken) and a float for each bit set (finite, above 0, at most 100000). An entry with mask 0 says the piece counts for
nothing now. Protocol 59.

Dev: `wsres state` (counters, and what is on each workbench), `wsres av <ref>` (a workbench's or piece's values: value,
base, permanent, temporary, damage), `wsres sum <bench>` (its pieces as its recount sums them, beside its own values),
`wsres can <ref>` (the engine's test on a piece), `wsres pip [text]` (the Pip-Boy's workshop data), `wsres mod <ref>
<value> base|perm|temp|damage <delta>`, `wsres find <text>` (buildable objects by name).

## Status (2026-10-09)

Protocol VERSION 63 (63: a spawn's kind, `Spawn::kind` a byte last on kReportSpawn and kSpawned (plain, a quest actor, a random encounter's actor), see "Random encounters shared calm" under Spawns, the twin refusal for quest actors alone, and a line's speaker may be a spawn id (`Line::speaker`, `IsActorRef`), which the server takes from the game that shares the spawn alone; 62: the host's supply lines at a friend's builds, kReportBenchLinks 79 and kBenchLinks 183 (`BenchLinks`: a workbench and the workbenches the keeper's supply lines link to it), see "The host's supply lines"; 61: a spawn names the quest alias that created it, `Spawn::quest` and `alias` last on kReportSpawn and kSpawned, see "A quest alias's creatures" under Spawns, and the server refuses another game's report of an alias a live spawn names; and a player's choices at a quest's terminals, kReportTerminalChoice 78 and kTerminalChoice 182 (`TerminalChoice`: a row of src/game/TerminalRules.h, its value and the choice's age), see "Quest terminals"; 60: with bSharedStorage the host's workbenches are viewed from friends' workshop and crafting menus, no message of its own; 59: what each player's workshop pieces count for in their settlement, kReportWorkshopResources 77 and kWorkshopResources 181 (`ResourceStates`: per piece a mask and a float for each of food, water, defense, beds, power made and power taken that isn't 0), see "A friend's pieces count in your settlement"; 58: the host's `sQuests` in the welcome, `SessionRules::quests`, a byte after `sLegendaryDifficulty`'s (an unknown value is `story`): with `sStory = shared`, `safe` shares the quests src/game/QuestFilter.h SAFE holds too, see the Quest stages row; 0.11.2's 57 is refused at the hello; 57: one roll per leveled NPC for the session, docs/SHARED-ROLLS.md: the host's `sNpcLevel` and `sLegendaryDifficulty` in the welcome, a byte each after the limits, each game's difficulty and legendary multiplier last on its status, and kSessionWorld 177 with the session's NPC level and legendary multiplier; the server keeps plugin NPCs' rolls for the session's life and counts their deaths, kRollLives 178, each death a new life and a new roll; a joiner's rolls packed, kNpcIdentities 179; and, in the same number (57 was not yet released), what the session keeps as a player's own building in the world their game plays sent to them once their record after the welcome or a load says whose world it is, kOwnWorkshopItems 180 (its lost pieces marked `WorkshopOp::kLost` 2, read only there), and their game's answer naming what its world lacks, kReportWorkshopGone 76, both laid out as kWorkshopItems, see "A piece its builder's world lost goes for everyone"; session file layout 42: each built piece's character after it and the lost pieces, `EncodeKeptWorkshopItems`, written only when a session has either, a 41 file still read; 56: a hit on a copy is a numbered request answered by the game that runs the NPC, `ActorHealth::seq` and `playerId` on kReportDamage and kActorDamaged, `DamageAnswer` on kReportDamageAnswer 75 and kDamageAnswer 176, and damage sent to the runner alone, docs/HEALTH-SYNC.md; 28 adds who reported a quest stage to kQuestStage, 29 the sender's clock to player states, to NPC states their owner's graph's direction and a dog's gait, and kReportSpawn and kSpawned for NPCs made at runtime, 30 whether a claimed NPC is essential to kClaimActors and the frame a player left parked to their status, 31 a line's recording (info, response number and voice type) in place of its topic, 33 a mirrored NPC's gait, 34 kRename and kRenamed, and names of 32 UTF-8 characters, 128 bytes on the wire, 35 a hello's load order, kRequestLoadOrder and kLoadOrder; 47 the next release's branches together, which took 36 to 46 for their own test builds (44 was a trial bundle's): the session's plugin table and kPluginTable (36), items given by dropping them: kReportDrop, kClaimDrop, kDropRecord, kDropPlaced, kDropGone, kDropList (37), a power wire's shape on a workshop item (38), LooksMenu skins and overlays on the face (39), kReportDamage and kActorDamaged for damage over time (40), kReportObjectHold, kReportObjectRest, kObjectHeld and kObjectRested for loose objects moved by hand (41), the host controls kHostCommand, kRequestHostInfo, kRemoved and kHostInfo (42), pings of what is under the crosshair, kPing 56 and kPinged 156 in place of 18 and 120 (43), the Minutemen's settlement requests shared by their run: kReportRadiantStart, kReportRadiantStage, kRadiantStarted and kRadiantStage (45), and whether workshop pieces are powered: kReportWorkshopPower and kWorkshopPower, see "Settlement power" (46); 48 the menu a player is in (PlayerStatus::menu); 49 the host's session rules in the welcome (SessionRules); 50 the next release's branches together, which took 51, 52 and 54 for their own test builds: world traps, kReportTrap 64 and kTraps 164 (51), destructible objects' damage, kReportDestruction 34 and kDestructions 137, see "Destructible objects" (52), and a leveled NPC's roll and a spawn's factions, kReportNpcIdentity 67 and kNpcIdentity 167, see "Shared NPC AI" (54), and, with no test build of its own, kPlayerReloaded 160: the others told a player loaded a save, so their games send their radiant runs going again (RadiantSync)); 55 0.10.0's changes together, one number for the release: shared storage, kReportStorage 68, kViewStorage 69, kReportStorageShort 70, kStorageSettle 71, kStorageContents 168, kStorageOwners 169 (each owner with the claim's log index), kStorageRefused 170, kStorageCaughtUp 171, kStorageReversals 172 (the session file's), welcome flag bit 32 and why a world request is sent, `WorldRequest::reason` (a load or a gap asked again), last on it; gifts through a container: the container a drop lies in, `DroppedItem::holder`, and welcome flag bit 64; kPlayerHit and kPlayerDamaged may name no attacker (0) for an NPC the sender's game can't name on the wire, where such a hit was dropped before: FormTranslate keeps it, and the victim takes it when the sender is near, see "Deaths that end"; and when an NPC state was sampled on its owner's clock, ActorState::sentMs, see "Shared NPC AI"; and whose world a drop record speaks for, `DropRecord::character`, last on it; power armor lent between players, kBorrowFrame 72, kLendFrame 73, kReturnFrame 74, kLoanAsked 173, kFrameLent 174, kFrameReturned 175 and PlayerStatus flag bit 64 (lends), see Spawns; an autosave as a player goes down, the host's `bSaveOnDown`: welcome flag bit 128, the flag byte's last). The sections below record each review as it was written.

### Trust review of the 0.6.2-0.6.3 changes (2026-10-09)

What a peer can now send that it could not before, and what checks it on the way in:

- `Line.topic` (a form id with an NPC line). Server: one line per 250 ms per player on top of the event budget, decoded with the bounds-checked Reader (`AtEnd` required). Hearer: the id must resolve to a `TESTopic`, the speaker must be our own copy within 1400 units, repeats within 5 s are dropped and at most 3 lines show per 3 s; the console command is built from two integers only (`{:08X}.say {:08X}`), so no text from the wire reaches the console. Residual: a friend can make an NPC near you say any topic in the game, and topic infos may carry script fragments; that is the same trust a co-op friend already has by talking to NPCs and shooting in your world. Off unless `bExperimentalVoiceLines = true` (a line said as the hearer starts talking to that NPC can leave them held in dialogue). The old `bVoiceLines` key is ignored: the ini shipped it on from 0.6.3 to 0.8.6-rc2 and players keep their ini across updates, so reading it would have turned voicing back on for them. Since protocol 31 (0.8.7) the topic is gone and so is the Say: a line names its recording (`Line.info`, `Line.response`, `Line.voiceType`; the decoder takes info and response only together, a voice type only with them, and none from the runtime range). The hearer plays it only for the game's own infos and voice types (Official.h), from a path it builds itself (the plugin's file name and the voice type's editor ID must each be one plain folder name, src/game/VoicePath.h) and only if the game has that file; no text from the wire reaches a path, the console or a script. Residual: a friend can make you hear any of the game's own recorded lines in any of its voices near their speaker, which is no more than talking to NPCs does.
- Hotkeys: the Alt/Ctrl/Win chord check reads the local keyboard only.
- Dev commands (`turnto`, `frames`, `say`): dev channel only, which is off by default and token-protected (docs/DEV-CHANNEL.md).
- Frame-time stats: local counters, nothing on the wire.

### Who may change your world (0.8.6-rc3)

The server is a relay with budgets; it has no copy of the world to check against. So each game also
judges what a friend's game says happened in it, by its own state. The game-free rules are in
`src/game/RemoteEdits.h` (tested in `tests/RemoteEditsTests.cpp` and, end to end through the real
server, in `tests/ServerWorldTests.cpp` under "host authority").

| Message | Who may send it | Server check | Receiver check | In the session file |
|---|---|---|---|---|
| `kActorDied` | the player running the actor (anyone for an actor nobody runs) | owner only; a known death is dropped; a joiner's budget of 60, 2 a second, and all joiners' of 120, 4 a second (the host's own game has none; refused or repeated deaths cost nothing; a joiner who leaves keeps theirs for when they come back as themselves, and one who comes back as someone new still shares the joiners') | own story: a unique or essential character dies only if the reporter was, or now is, within 8192 units in the same cell or worldspace (`DeathReports`, `KillsOurCopy`). A report from someone who seems nowhere near waits 2 s for their position, which can come after the death, then is refused for good. A death the session replays (on joining, after loading a save) names nobody, so our story's characters stay as our save has them. The body is moved only within 4096 | yes, so a death the server dropped is never replayed |
| `kReportSpawn` | any game, of an NPC it made at runtime | a free spawn id only (one taken is answered with its owner); 32 a player at once, 128 in the session, a budget of 32 and 2 a second; the base must be a plugin's form and the place in the world. Nobody but its owner may claim, release, move or fire it, or hit a stand-in with it; its death is believed from the owner and a player fighting it | a copy only near our player, its owner's stand-in beside it, of a non-unique NPC that is no stand-in's base; 2 a frame, 128 at once | no: it is in no save but its owner's |
| `kExplosion` | any game, of what its player or an NPC it runs set off, only what the server is sure to pass on (`RemoteEdits::ReportGate`) | the reporter's position must be known and the explosion within 8000 units of it in 3D, or of another player in that cell or worldspace (an NPC's blast beside a friend, far from its own game's player); a joiner's own budget of 16, 4 a second (kept across rejoining, as for deaths; no window after a shot or a throw), and all joiners' together have room for every seat's at once; relayed reliably | within 6000 units of us, or beside an NPC our game runs, in our cell or worldspace, and the explosion's radius in (0, 1024] (`RemoteEdits::SetOff`). It goes off beside us and our companions with friendly fire off too: it is what kills the enemies there in our game (see below) | no |
| `kDestructions` | any game, of a plugin's destructible object (a car, a barrel) | the reporter's position must be known and the object within 16000 units of it, in the same space (where the session first had it, once it has: a report names where its object stands, a relay doesn't); the event budget; a plugin's reference, a health in [0, 1000000]; only news (lower health, or destroyed) is kept and relayed, and kept for good; each player's share of the table; `explode` with any news, from whoever reports it (no window, and not the explosion budget: a stage comes once), within a budget of its own of 64 at once, then 64 a second per joiner; past it the stage is relayed with nothing going off | a destructible object of ours, not an actor; only a lower health than ours is taken (a wreck is never mended); an explosion only for a stage our copy passes now, through `ApplyExplosion`'s checks; a catch-up sets nothing off | yes (layout 34) |
| `kReportTrap` | a game that has the trap loaded | its position must be known and the trap (where the session has it, once it has it) within 16000 units of it, in the same space, for a spring, 1024 for a disarm or a take; the event budget; a plugin's reference, a state of 1 to 3 and a place in the world; first kept, then only a take (after a spring or a disarm); past the session's table size or the player's share, dropped | done only to a trap of ours that is armed and settled; set off as it went only within 3 s and 512 units of where theirs was, else spent quietly | yes |
| `kActorStates` (NPCs) | the player running the actor | owner only | an NPC a friend's game runs is moved to where their game has it (from a cell we have not loaded, or after it went far) only while our copy of the friend is within 1500 units of that spot: a companion walks with them. This goes for every NPC their game runs, so a settler their game moved far from them runs its own AI here until they come; in the own story, our story's characters are not switched on by a friend's game | no |
| `kNpcIdentity` | the player running the NPC | owner only; the first runner's report stands (a later runner's is ignored) until the NPC dies, nobody runs it or that player leaves; an actor id, a level of at most 1000, slots only with the NPC it was placed as, at most 32 factions, none 0 or twice | only for our copy of an NPC another game runs, while we mirror it: a roll only when ours is leveled, was placed as the same NPC and every one of the roll's NPCs is one our lists can give; factions only on our copy of a friend's spawn, never PlayerFaction or our companion's; 3 tries | no (a plugin NPC made their roll goes into our save as it) |
| `kContainerChanged` | anyone | at most 30000 changes per player in the session | a settlement's workbench and our own companion are left alone (LootScope: per player, counted in `perPlayer=`), and so are vendors' chests (`vendor=`); other containers follow friends | yes |
| `kReportStorage`, `kViewStorage`, `kReportStorageShort`, `kStorageSettle` | the hosting game (a listing, a shortfall); any game (a view, a settle) | the rule on and the event budget; a listing only from the server's local player, at most 1024 stacks, its changes added from its number on (clamped to the log); a view only of a kept container that isn't stale, at most 8 viewers each, and only from a player whose last world state is settled (else answered closed); a shortfall only for a numbered take of that container by a player (never one by nobody), answered once (and the take corrected in the log to what was there); a settle only one per world request, for the last one, once that world state has gone: it undoes in the true copy only the sender's own numbered moves at a container another game keeps, up to the log's length, each once until a later settle says their world has it (none from before the claim), a put undone no more than the book holds while fresh, numbered as a change by nobody and counted against the settler's share of the container log, all of a settle or none of it (past it the player stays unsettled); a first claim takes back out of the true copy what friends put in it before, net (no more than everyone's moves added or the listing holds); its late moves at most 2304, budgeted as moves, only at the container the player viewed last; a friend's container change at a kept container is budgeted first (2304, 500/s; refusals the same; views 16, 8/s), is refused while their settle is pending, then needs their view and must be held (and the item mappable to the host's plugins), else it is refused and never numbered | the owners only from the server; contents only for the container we view; a refusal only undoes a move of ours still on its way, or one this world reported as ordinary loot as the container was claimed (once: the report is forgotten); a `kStorageCaughtUp` only settles when it names our last request and the ledger waits for nothing; nothing is ever done on our player after a load or a rejoin | the owners and the moves undone, whatever the rule (layout 38 when there are any; owners come back stale) |
| `kReportDrop` | any game, of an item its player dropped | the giver's welcome first; a known base and count 1..32767, mods and name within the wire's caps, the place in the world; 1024 lying in the session, 256 a player (past them refused to the giver only), one report per nonce | a copy only of an item whose base and every mod this game has, within 5000 units of our player, 2 a frame; it can't be picked up without the server's grant | yes |
| `kClaimDrop` | anyone, of an item lying about | kTake: the first claim wins, the rest are told who has it; kGiveBack: only the taker | our player takes only a copy of ours, only after the grant, as their own Take; a copy scrapped in workshop mode is claimed the same way | yes |
| `kDropRecord` | each game, of its own world | at most 4 a second (the last one is answered too); another session's counts as empty; a player's own drops and takes only | | no |
| `kWorkshopItem` | anyone | caps of 20000 built and 20000 scrapped | the anchor must be a workbench (`anchorRefused`); scale clamped to [0.1, 10]; a scrap must be buildable, unscripted, not a quest or enable-parent item, in the same space and in its build area: while some of the area's volumes are not loaded, a scrap outside the loaded ones waits (`scrapRefused`, `scrapDeferred`; a waiting scrap is tried again as cells near it attach, so `scrapDeferred` can stay above 0 until a player walks there); a wire's shape must be numbers, its ends within 4096 units of its middle, its slack in [0, 2] and its thickness in (0, 16] (the item is refused whole), and a wire without one is not drawn (`unplaceable`); a moved piece of the world itself moves only the receiver's own one, and only one linked to that workbench as its workshop item, of the same object, within its cell (`ownRefused`) | yes |
| `kWorkshopResources` | anyone, of their own pieces | the event budget; 1 to 128 entries, each a reference from the runtime range, a mask of the six kinds and a finite value above 0 and at most 100000 for each (else the message is dropped), each read as at most its kind's `MAX_RESOURCE_PIECE`; the sender stamped | counted only at the workbench of a copy we made of that player's piece (keyed by builder and their reference), never for a piece of our own or of the world; at most 8192 pieces kept; each friend's sum at a workbench, and all friends' sum, at most `MAX_BENCH`; on a workbench's temporary modifier, taken off before every save and when they leave; no beds | no |
| `kWorkshopPower` | anyone, of their own pieces | the event budget; at least one entry, each a reference from the runtime range and a state of 0 or 1; the sender stamped | shown only on a copy we made of that player's piece (keyed by builder and their reference), never on a piece of our own or of the world; at most 8192 pieces kept; shown without the game's power events, so no script of ours runs on it | no |
| `kReportObjectHold`, `kReportObjectRest` | the player moving the object (the first to grab it; after a let-go the thrower until it rests, or whoever grabs it next) | a plugin's reference only; a hold within 512 units of the holder, angles within 8 rad, finite; a rest in the world, in one cell or worldspace, at under 5000 units a second (none at rest); refused while another player holds it (`ObjectMovers`); 20 holds a second a player, a rest budget of 24 and 6 a second (kept across rejoining) | a loose object of ours (`Movable`: loose form type, physics body, no quest alias, nothing SaveGuard, Spawns or WorkshopSync places, not in workshop mode), within 8000 units of us in the same space; a hold drawn only within 2500 units of the holder's stand-in, a rest only within 4096 units of where our copy is; never one in our player's hands; every pose finite and in the world before it is written, velocities capped | no: a move is over in seconds, and each game's save keeps where its own copy lies |
| `kHello.appearance` | each player, of themselves | passed on as sent | placed only when it is one of the two settlers or our own `iPuppetBaseForm`; otherwise our default | no |

A friend's explosion is set off in our game whoever stands beside it, with friendly fire off too. In our
game it is the only thing that does the damage of their throw: the stand-in's own grenade is removed,
the server refuses their reports of deaths of NPCs our game runs, and no health comes for one dead in
theirs. Leaving it out near us would leave the enemies a friend's grenade
killed in their game alive and fighting in ours. So with friendly fire off a friend's grenade at your
feet still hurts you, as it always has; a forged one is bounded by the server (within
8000 units of the reporter, 16 at once and 4 a second for each joiner). A copy of the explosion that hurts nobody but
the enemies is not done.

To check live before this ships (the tests cannot reach the game):

- A friend's grenade thrown into a melee next to the other player and their companion kills the
  enemies in both games, and the first explosions in the log read a radius of about 500 (a Fat Man's
  shell under 1024) with `explosionRefusedRadius=0`.
- Junk scrapped in one game is gone in the other, at a settlement whose build area is several
  volumes too (scripted pieces are left alone in the other game, as the host's own walk leaves them).

Not done yet: a single `Mutates(MessageType)` list the server and a CI check share, so a new message
that changes the world cannot ship without a row here.

## Prior art

- Skyrim Together Reborn (GPL-3.0): ownership, interpolation and animation sync design.
- FO4_Wrld (AGPL-3.0): Fallout 4-specific reverse engineering. Reference only.
- Fallout 76 (public sources): server authority for inventory and damage, and atomic item transfers.

## Testing tools

- `tools/restart-game.ps1`: deploy, relaunch and load into a cell.
- `tools/devctl.ps1`: dev channel (needs `bDevChannel = true`). `help` lists the commands.
- `tools/crossovermac.sh`: both of the above for the game under CrossOver on a Mac (docs/misc/MAC.md).
- Dev commands for the party: `party [list|pick|go|ping]`, `forms <type> [text]`, `edid <form>`,
  `voice [talk|loop on|off]` (record without the key; hear yourself), `markers [name]` (map
  markers and their flags), `idles <text>` (idle animations by editor ID, event or file),
  `dialogue` (the conversation the player is in), `npcvoice <actor>` (an actor's current line), `voiceline <actor> [info response voiceType [flags [priority]]]` (the recording an actor is saying as we report it, with the engine's own voice file for it; or a recording played at an actor as a friend's line is),
  `topics <npc>` / `say <ref> <topic>` (an NPC's dialogue lines; make it say one), `named <type> <text>` (forms by name, with keywords), `menu <name>
  [hide|force|cancel]` (open or close a menu, not the dialogue or the workshop menu: `workshop enter|exit`; the settlement
  list is not hidden, only cancelled: a tap of its Cancel handed to it alone),
  `paused`, `gfx <menu> <path> [depth]` / `gfxset <menu> <path>
  <member> <value>` (look at and change a menu's Flash objects, on the main thread between frames), `weapsound <actor>` (the sounds
  of an actor's equipped weapon), `playsound <sound> [flags] [ref]` (play a sound descriptor), `sounds` (every sound the audio manager holds and the reference each follows, PLAYER for one that follows
  the player: one heard wherever the player goes), `quests started` / `quests running:<type>`
  (quests the player has started; running quests of a type), `objectives <quest>` (stage and
  objectives with their state), `stages <quest>` (the stages done here and those from friends still
  waiting), `aliases <quest>` (its aliases, how each is filled and what fills it here), `global <global>` (a global's value), `enemies [radius]` / `enemies like <base|any> <x> <y>` (hostile actors near the player with
  line of sight, what they fight as a session player and who runs them; this game's actor for a spawned one
  another game listed), `spawnenemies <base> <n> <radius>` (n NPCs of a base round the player, at most 16, facing it,
  each on the ground where it stands; made at runtime, so a session shares the hostile ones as it does an ambush), `status` (cell, worldspace and location), `log level info|debug`
  (the plugin log's level until the game restarts; `sLogLevel` in the ini), `log flush`, `log path`.
- Dev commands for the menu stress loop: `menus` (the menus open and for how long, and how often each
  opened and closed, kept from their own events: no reference to a menu is taken), `input <userEvent>
  [hold=<ms>] [device=key|mouse|pad] [id=<hex>]` (a button press handed to the game's own input receivers, past
  Windows and DirectInput), `controls [context]` (the control map's input contexts and their user events),
  `workshop [enter|exit|junk|select <ref>|scrap <ref>]`, and `dialogue menu` (the dialogue menu's own state).
- `tools/menustress.sh`: workshop mode, the Pip-Boy and a conversation over and over on one game or both,
  logging whatever gets stuck (`tools/stress.ps1` runs it from PowerShell). A game thread that runs none of
  its commands for 12 s is a HANG: the loop copies the plugin's hang and frame watch lines from the log and,
  on this PC, catches the game with `tools/hangcatch.ps1 -Now`. `tools/hangcatch.ps1`: when the game stops
  answering for 10 s, a debugger writes every thread's stack and a minidump.
- `F4MPBot.exe`: a fake player that walks in a circle and can report kills, loot, pickups, doors,
  quest stages and hits, take over an NPC (`--own`), shoot (`--shoot <weapon>`, its own or the
  NPC's), report a status, ping and kill XP (`--status-health`, `--opening`, `--ping`, `--xp`), come back as the same player (`--identity`), set the session
  time (`--time`, `--weather`, `--time-step` to move it forward), complete a quest (`--quest-done`), be down or help someone up
  (`--downed`, `--revive`), hit a player as a player (`--hit-player id:damage:p`), talk (`--voice-silence`:
  valid Steam voice packets of silence) or repeat what it hears (`--voice-echo`), discover a map marker (`--marker`),
  keep claiming an NPC as if talking to it (`--talk`), report lines said by an NPC or itself (`--say`), and resume a session
  (`--session`, `--from`).
- `tools/input.ps1`: real mouse clicks and key presses in the game window (e.g. to fire).
- `tools/screenshot.ps1`: captures the screen for visual checks.
- `F4MPServer.exe`: a standalone server.
