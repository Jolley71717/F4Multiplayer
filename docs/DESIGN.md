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
  counts a loading screen as up for at most five minutes (`UiHold::Guard`, `src/game/UiHold.h`).
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
  entries by uniqueID), takes the settler's face template off it (faceNPC and the "use traits"
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
- Name: `ExtraDataList::SetOverrideName`, and the hooked `GetActivateText` returns true for puppets so
  the HUD shows the name when you look at them (pressing E still does nothing).

## Shared NPC AI (src/game/NpcSync.cpp)

- Every loaded, living NPC from a plugin file has one owner, whose game runs its AI. The server decides:
  - nobody owns it: the first player to claim it (they have it loaded);
  - a player's current companion goes to that player, unless it is also another player's companion
    (then the first keeps it, so two players with Dogmeat don't trade him forever; the other's game asks
    again every 15 s rather than every 3 s, and at once when he is released: `NpcClaims`, `refusals=`);
  - a player who hits or talks to an NPC takes it over, unless it is someone's companion or changed
    hands in the last 5 s.
- The owner sends its NPCs' cell/worldspace, position, heading, speed, moveMode and weapon-drawn at
  20 Hz when they're in combat or within 3000 units of any player, else 10 Hz (unreliable). The
  others register the NPC as a `Puppets::Kind::kNpc` puppet: AI off, movement copied with
  interpolation, but it can be hurt and killed. Each copy renders 100 ms in the past when its states
  arrive at most 70 ms apart on average, else 150 ms, eased 2 ms per frame (`net status`: `fast=` NPCs
  we send at 20 Hz, `delayMs=` the range over our copies). If the owner's copy is in a
  different cell or worldspace (a companion that followed its player through a door), the local copy
  is not moved there; it runs its own AI meanwhile. The same goes when the owner's copy is more than
  a cell (4096 units) away from ours (fast travel, script moves), when no state came for 3 s (the
  owner's game is paused, loading or gone; owners send nothing while a menu pauses them), and for
  our own current companion (two players can both have Dogmeat).
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
- Fix-ups: a copy that is off here while the owner's runs is switched on (`enable`), and a friend's companion
  standing near us whose copy is in a cell we have not loaded is moved here (`moveto`). Each is tried at most
  every 5 s (10 s for moving) and three times per NPC (`RetryBudget`): one a quest keeps off in our game is
  then left as it is. The tries start over once the copy is on, or here (`net status`: `gaveUp=`).
- Console commands: each one is compiled and run on the spot, so the copies taken over from their own AI are
  spread out, 8 a frame at most (the rest keep their snapshots and wait a frame), and the commands run for NPCs
  are counted (`CommandMeter`; `net status`: `cmds=` last frame/most, `busy=` frames over 32, `rereg=`,
  `released=`), with a log line for a busy frame at most every 10 s.
- An owner more than 6000 units from an NPC releases it when a friend in the same space is within
  4000 units and at most half as far (not its companion, its conversation partner or an NPC
  fighting the owner), so enemies react to the player standing next to them. The owner doesn't claim
  it back for 10 s (the friend's game claims it) and hands the same NPC off at most every 30 s.
- Remote players' stand-ins are in PlayerFaction, so enemies in the owner's world attack them. Hits on a
  stand-in by an NPC are forwarded to that player (`PlayerHit` -> damage to their health), naming the
  NPC. Hits by NPCs made at runtime are sent only for one the attacker's game shares (see Spawns), under its spawn id; the victim ignores hits
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
  a stand-in, a copy, the player or its companion), and reports `kReportSpawn {spawn id, base, cell or
  worldspace, position, heading}`. The spawn id is the NPC's own id first; one another game's spawn has is
  refused naming that game, and the next one asked for is spread over the runtime range (`CandidateId`). Spawn
  ids keep the 0xFF prefix (0xFE is light plugins' range), so every check of `IsShareableRef` still turns them
  away; only the messages about NPCs take them (`IsActorRef`), and each game looks its own NPC up under one.
- The base is what every game puts together the same: the first of the leveled data's templates that is a whole
  NPC (no template of its own), else the NPC it was placed as (a leveled one is rolled again by each game), else
  its own base when that is a plugin's. Without one it isn't shared.
- The server grants an id to the first game to ask (`actorOwners` names the owner; `spawns` holds the details),
  sends the others `kSpawned` and everyone the owner. A joiner is sent the spawns before who runs NPCs. Limits:
  32 a player at once, 128 in the session, a budget of 32 and 2 a second; past them the answer names nobody and
  the game asks again 10 s later. Nobody else can claim one; a claim to take one over notes the claimer as
  fighting it.
- The others place a copy (`CreateReferenceAtLocation`, kept out of saves like a stand-in) while their player is
  within 6000 units of where the owner's game has it, in the same space, and the owner's stand-in is within 8000
  of it; 2 a frame at most, 3 tries. NpcSync mirrors it under the spawn id as any NPC another game runs: states,
  gear, shots and health.
- A copy goes when its player leaves it 9000 units behind (placed again on coming back), when its cell unloads,
  before every save and load, and when the spawn is over while it is alive. A dead copy stays as a corpse until
  its player leaves it.
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
  when it is over. A copy somebody there gets into is theirs from then on (neither moved nor deleted). A joiner is sent
  the frames shared before it came, so a friend's suit parked before the session stands there too. `net status`:
  `framesMade=` shared/standing/our player's own (npcs), `frames: ours= reported= placed= moved= taken=` (spawns).
- Not yet: copies are not shielded from a friend's explosion (BlastShield) or watched by CopyWatch, a copy
  carries what its own game gives it until the owner's gear report dresses it, and a copy whose owner's game
  only knows the leveled NPC it was placed as can come out as another of that list's NPCs. Each game's corpse of a
  spawn carries its own loot: each player loots their own.

## Weapon fire (src/game/WeaponFire.cpp)

- Detection: every actor gets its animation graph's events through its
  `BSTEventSink<BSAnimationGraphEvent>` base at +0x38; the graph sends "WeaponFire" once per shot
  and "weaponSwing" at the hit frame of a melee swing or punch. Both count as an attack.
  We hook slot 1 of that vtable twice: the player's class (our shots) and the NPC class (shots by
  NPCs we run; stand-ins and mirrored NPCs use the same class and are filtered out).
- `ReportShot {refId, sound}` (0 = the sender) goes to the server, which accepts NPC shots only from
  the NPC's owner and relays `ShotFired {playerId, refId, sound}` unreliably (60 shots/s per player).
- Replay: "attackStart" on the stand-in's or mirrored NPC's graph plays the full firing animation
  (recoil, aim pose). Nothing is launched: damage already arrives through health sync, and real
  projectiles from stand-ins would add friendly fire and kills nobody made.
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
  shows it on every shot, to try it; the status line's `flash=` counts both.

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
  storage, power armor frames.
- Shared: ammo, chems, food, junk and other loot in containers and on corpses.

Weapons and armor are per player because a move carries only the base form: another game would get a
plain copy, and `removeitem` could take a legendary. So a friend's looted gun stays on your copy of the
corpse. (Players give each other weapons and armor by dropping them: see Giving items.) A vendor's chest is any faction's merchant container; moves while the barter menu is open, and
for a second after, are trades and never reported. A corpse is shared once the session says it died: a
change for an NPC still alive in this game waits for its kill, up to 10 minutes in the world, counted
for all of that NPC's changes together (at most 4096 changes wait; when full, one waiting for a living
NPC makes room before a friend's change for a chest not loaded yet is dropped: `pendingFull=`). A chest
waits too while this game holds it in no cell at all: one of a place we left lately stays in memory that
way (Red Rocket's desk and cabinets, seen from Diamond City), and its changes go in when we come back. A
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
each stack its own copy holds that the same take would share, and leaves weapons, armor and personal
items. It waits like any change for a holder not loaded, a holder whose inventory the game hasn't made
yet (a chest that rolls its loot as it first loads), and an NPC still alive there; a vendor's chest, a
workbench or a frame is left alone. An older build sees the player's ref as no item and leaves it.
`net status` counts `emptied=` sent/applied/stacks taken.

Picked plants (flora) are harvested in every game: TESHarvestEvent's ItemHarvested, by our player, of a
shareable ref, goes out as a pickup (kReportPickup, no new message). A receiver whose ref is flora runs
`<ref>.setharvested 1` (once: one harvested already is left), never disabling it, so it grows back on its
own schedule; a joiner or a reloaded game gets it through the world state's pickups. `flora=`
sent/applied.

Activators (buttons, switches, levers, a sink) a friend presses are pressed in our game by their
stand-in, not our player, so a script that acts on whoever pressed it (rads from a sink, a teleport)
acts on the stand-in. Without the stand-in here the press is refused (`acts=` sent/applied/refused);
the door or lock it drives arrives as its state.

Remote changes are applied only while the player is in the world (no loading screen, not at the main
menu) and to loaded references; everything else waits and is retried.

| Feature | How |
|---|---|
| Deaths | TESDeathEvent -> ReportDeath; receivers run `<ref>.kill` when loaded. Of an NPC another game runs, the server believes the runner, the host's game about one dead in its save (found dead), and a player fighting it: one who reported its health, asked to take it over, or whose game ran it until it changed hands, within `Options::deathGrace` (10 s; the takeover itself is refused for 5 s after the NPC changes hands). Our kill's death event asks to take the NPC over first (a grenade's blows name no attacker), and the host's found-dead check leaves an NPC still falling, or hit by our player lately, to its death event for up to 15 s (`NpcHold`). Others are dropped, with a log line at most every 10 s |
| Health | The game that runs an NPC reports its health (a fraction of max) after its hits and whenever it changes by anything else, at most every 250 ms (`HealthShare`: a fire, a burn, poison); receivers set their copy to it. A hit on a copy goes as the damage it did (`kReportDamage`, protocol 40: `CopyHealth::Book::TakeOurs`), which the game that runs the NPC takes off; only it applies it. Before, a copy reported its own health, which healed off the NPC whatever its game had taken since the copy last heard (a Molotov's fire). A game that comes to run an NPC (a takeover, a claim, back from a load) reports its health at once, at most 16 such reports every 250 ms (`healthShared=` sent/of them first readings): a takeover's claim leaves a frame ahead of the hit's damage, which the old runner then drops, and a host back from a death's reload runs its NPCs at its save's health. Only a throw's blow on a copy is left to the NPC's own game (`CopyHealth::OwnersToDeal`): an explosion's only while our game shares its explosions (a throw's watch: every one near our player then goes off in the friends' games), a throwable's too but never one with a weapon in hand that is no throwable, and the fire a Molotov leaves for 20 s after. In 0.9.0, as players reported it, gun hits on copies were taken for throws (how their hit events read on this runtime is not verified yet: `CopyWatch` now logs the flags), so only the game that ran an NPC hurt it, and a melee hit, which claimed it, moved that game to the hitter |
| Container loot | TESContainerChangedEvent player<->container -> resolved in the next frame on the main thread; shared moves (LootScope) -> ContainerChange; the server numbers it and echoes it to everyone, including the sender; receivers check the scope again and `removeitem`/`additem`, in order per container |
| World item pickup | TESActivateEvent (player) paired with container event old=0,new=player (its ref is 0) -> RefPickedUp; receivers disable the ref, only while it is in the world (a persistent item's ref stays itself in the inventory that took it, ours too: OwnGear kTakeLoose). One the engine names an inventory for (ExtraReferenceHandles: the local player's, a container's, an NPC's) is left there and the pickup dropped; one in no cell and no inventory (being loaded or moved) stays pending and is tried again for 30 s (OwnGear::PICKUP_WAIT), then dropped |
| The player's own gear | every inventory or equipment change the session makes for a friend's game is judged where it is made (src/game/OwnGear.h, Equipment::MayChange): stand-in dressing only on a stand-in, an NPC copy's only on a mirrored NPC, a frame emptied only if this game placed it and our player isn't in it, a pickup only off the world, loot never onto a stand-in, and none of them ever onto the local player (refused, logged, counted in `net status` as ownGear ourPlayer=). The counts start again with each session's welcome; the log takes up to 100 alarming refusals (our player, what they hold, the wrong actor, someone's own frame) and, on a budget of their own, 50 benign ones (gone, in an inventory, in no cell), so a join's catch-up can't hide the first. `inventory` (dev, read only) prints the player's stack and item counts, a checksum of every stack, the Pip-Boy (worn or not), caps and what is equipped, to compare before and after a join |
| Quest stages | poll each quest's done stages (`TESQuest::stages`, a stage's done bit; src/game/QuestStageLayout.h) twice a second for side quests (type 7), and the main story and faction quests (types 1-5, 8+, plus the type-0 faction finales Inst305, BoS302, BoS304, and the game's dialogue quests: type 0, no objectives, named Dialogue<Place> after any DLCnn prefix, src/game/QuestFilter.h; Diamond City's entrance scene opens the gate at DialogueDiamondCityEntrance 32) only with `sStory = shared` (Welcome::sharedStory); never misc, never the other type-0 quests (random encounters, companions, tutorials, dungeons). A dialogue quest has no objective to start it: it counts as started once running or with stages done, only a stage done here is past it (its stages aren't in order: a game at the gate has 400 without 32), and once it ran and stopped it is over (the rest of it from friends is dropped; starting it again would play it over); its name is the editor's, so no "caught up" or "completed" notice names it. Every newly done stage is reported, ascending, not just the current one: a stage whose script moves the quest on at once (MQ104 210) is never current. 5 s settle after load, only of quests the reporter has started. Receivers queue them (src/game/StageLog.h), only while in a session (dropped when it ends, except a lock's quests: those are set after leaving too, with the story the player's own again, so a teleport begun in the session finishes), skipping indexes the quest doesn't have, and `setstage` them in the order they came: per quest the lowest waiting stage above their own, the next once it reads done (5 s timeout), up to 4 per poll, once they have started the quest too: an objective has been shown (`BGSQuestObjective::state` not dormant). Running isn't enough: a new game runs many side quests in the background from the start. Until then the stage waits (another quest's stage may start it: MQ104 210 starts MQ105) without holding up other quests. In a session that wait is only for a quest the session doesn't share: a shared quest not started here is started by the first of its stages, once every stage that came before it is set and START_GRACE after the last one read done, and one that ran here and was stopped is never started (StageLog::Quest::kStart, Wait::kStopped; since 0.8.6). The host's game reports every done stage of the shared quests its player has started when its own server welcomes it, so a joiner is caught up to the host's story; a joiner's own progress from before joining is never reported. A quest is done with once completed and stopped (many complete at a stage and run on: MQ102 65). A stage reads done before Papyrus runs its fragment, so after a stage whose fragment waits (Utility.Wait, moving the player) nothing that came after it is set for that long plus 1 s. src/game/StageRules.h, generated from the QF_ fragments of Fallout4.esm and DLCCoast.esm (following the stages each one sets): stages that move the player (MoveTo, a door they are made to use, FadeOutGame, an InputEnableLayer) are set only when live (came in the last 30 s, not from the session state) and the friend who did it (the server stamps the reporter's id on kQuestStage; a WorldState carries none) is within 6000 units, else skipped, unless they also start another shared quest or award an achievement (MQ206 500 and 1000, the MQ206 faction 1400/1500): then after a 10 s HUD warning, outside combat; MQ206 500 and the 1400s (controls off) only once 1200 (controls on) is waiting too, and until then only their own quest waits (Verdict::kQuestWaits), not what came after it. After a lock stage is set, its quest's stages go first, without a combat hold or another warning and not held up by other quests', are kept if the session ends, and 1200 is set outright once they have done nothing (none being set, no script of theirs running on) for 90 s of unpaused play (StageLog::Queue::Lock). The release is looked for before each poll of the queue: after leaving, once 1200 is done the session's other waiting stages go, so nothing past 1200 is set in the solo game, nor left for the next session. While the game is paused (`UI::menuMode`, a loading screen) nothing is set and holds and timeouts don't run down. Stages that take caps or ordinary items from the player (MQ105 177, FFDiamondCity10 40, ...), and the dialogue quests' payments and sales (DialogueBunkerHill 210, DialogueVault81 5 and 645, DialogueDiamondCity 70, DLC03DialogueFarHarbor 90), are never set (tools/questscan.ps1 lists the stages whose own Fragment_Stage_ function calls RemoveItem, RemoveAllItems, SellItem, RemoveItemByComponent, UnequipItem, UnequipItemSlot or UnequipAll directly on Game.GetPlayer() or a PlayerRef property: of the shared quests only quest items, caps and chems; RemoveAllItems only in MQ101, the opening, and DN019JoinCult, both misc quests and never shared. It does not see such calls made through the quest's script (kmyQuest), a player alias (Alias_Player.GetRef()), a scene, a dialogue line or another script a stage starts, nor a shared stage setting a stage of another quest, so it is no proof that no shared stage takes the player's gear; src/game/StageRules.h); with `sStory = own`, side-quest stages that move a story or faction quest on (Min01 220/1500/2000, FFGoodneighbor01 100) aren't either. Caught-up stages wait during combat and 5 s after (at most 60 s), so Party doesn't take kill XP for quest XP. Never during dialogue; a scene holds them back, but one of a quest with stages waiting only for 30 s. One "Caught up" notice for many quests. Every poll in a shared-story session, a player whose MQ104 is past 210 without 210 done and whose MQ105 never started (left so by the old highest-stage-only sync, or by catching up from its session file) gets `setstage 229E6 10`, judged as MQ105's stage 0 (it moves the player): after the 10 s warning, outside combat, not in a conversation or a menu. Stage reports made while not welcomed (before the welcome, during a reconnect) are kept (the first 1024: what came first starts what came after), the story's and factions' too although Story::Shared is false while the connection is down (Session::OnDisconnected), marked so they go only if the welcome shares the story; they are sent once welcomed, at most 200 a second, oldest first (the server drops a player's events past 500 a second, all kinds together, and a stage is reported only once) (StageLog::Outbox). Ones made in no session (playing alone after leaving or stopping hosting), kept for a server the player then left for another (an invite accepted while the last host was being retried: Session::SwitchServer calls QuestSync::OnServerChanged), or kept through 5 minutes of not being welcomed (the host quit and the player plays on alone) are dropped, so a session joined later doesn't take solo progress for a friend's; loading a save drops them. Reporting doesn't settle again after a reconnect. A reconnect drops the waiting stages and what came live (OnSessionEnded): a friend's step that moves the player and was still waiting (a fight, a conversation) comes back in the WorldState as caught up, not live, and is skipped even beside that friend. Within a quest stages are set in stage order, not in the order they were done: one done after a higher one is already settled here and dropped (MQ104's 17 sets 12: 17's own script sets 12 here too). The opening's stages (MQ102 below 10, all of DialogueVault111) are never reported, baselined or set, and while any game in the session is in the opening no stage that moves the player is set, its quest waiting (see The opening); the endings' fades and slides (MQ302 850/855, MQ00 50/60, Inst308 30) count as moving the player. A fade the session caused that nothing fades back in is faded in after 20 s of play (see Fades). Each load logs whether the game awards achievements (`TESDataHandler::CheckModsLoaded`) and Steam's NEW_ACHIEVEMENT_n_0 list; new unlocks are logged after stages are set (src/game/Achievements.cpp) |
| Doors and locks | refs the player activated are watched for 60 s, and what they are linked to (a terminal's door: an entry still holding the target's form ID is looked up by it, a pointer is read guarded and taken only if the form its own ID names is itself, `LinkedRefs.h`); open/lock changes -> RefState; receivers `BGSOpenCloseForm::SetOpenState` / `REFR_LOCK::SetLocked` |

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
  another system keeps (SaveGuard's, Spawns'). Once it has come to rest
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
- A weapon or armor put in a container tells the player once a session that friends won't see it there
  and to drop it near them instead.

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
frame before the save or load are sent first (deaths first), so they are in that answer. A save with no record of this session (made
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
again. A joiner's claim on a dead NPC is dropped, so a joiner whose cell reset first sees its copy run by its
own AI until the host's game brings it back; one whose copy is still a corpse keeps the corpse (its
`AttackerHere` drops that NPC's hits). A dedicated server has no host's game, so nothing comes back there.
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
else skips this: the player is never in those places. Untried with any such mod (dist/INSTALL.md, Alternate-start mods): a start in a cell of its own is shared from its first frame, scenes and all, and a character named there gets its ID (`BGSSaveLoadManager::currentPlayerID`) outside the opening, so a host may start hosting under the unnamed ID and restart once named (CharacterBinding: kStopHosting), and a joiner who connected first stays bound to the old ID until a load makes it leave (Tracker::Check rebinds only in the opening). Only the ID's low 32 bits name the character (CharacterBinding::Character): a save's file name holds them and a letter the game loads back into the high half ('_' 0, 'M' 1 once the game has a mod's plugin), so the same character's quickload can bring another high half than the game had in memory.

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
  the last 3 s: TESHitEvent) and a friend is within 6000 units, 8 at once and 3 a second at most. What a friend's move
  put somewhere is quiet for 4 s (our physics settling it is not ours to report).
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
a session file). While any game in the session is in the opening (its own, or a friend's status flag),
no friend's stage that moves the player is set (Verdict::kOpeningWaits): only that quest waits.

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
  10 s). When they get out, the pieces come off and the copy is drawn where they got out, solid once
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
  until they get in anew. It stays hidden through saves (only its 3D is touched) and is hidden again
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
  (`HeldCopy::Look`): the same copy back is held again; otherwise it is forgotten and its record dropped at once
  (`SaveGuard::Forget`, `SaveStrays::Book::KeepForgotten`'s orphans, counted as `orphansDropped`). And before a copy
  is placed, one an earlier session left at that very spot (within 2 units: made at runtime, the same frame, enabled,
  emptied, held by no quest, nobody's to use here and not one this run placed; `HeldCopy::Leftover`) is taken over
  instead, so what 0.9.1 left in a save goes with the friend this time. The leak the rig showed (2026-10-24): a load
  does not free every copy DespawnAll had just disabled or deleted for it; one stayed in memory, nobody's once the load
  had emptied the book, the next save wrote it, and a game started afresh from that save had it back drawn and whole
  beside the copy placed for the friend, and the host shared it as its own frame. Now each reference placed before a
  load that is still there under its base, deleted or disabled (never the player's own suit), is forgotten anew and
  kept out of every save (`SaveStrays::Book::AfterLoad`'s lingering ones, `lingered` in `saveguard`). `net status` counts them: `framesUnloaded`,
  `framesBack`, `leftoversTaken` (stand-ins), `unloaded` and `back` (spawns).
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
  stand-in is held (`Puppets::Hold`: not placed at its target, no locomotion events, no do-nothing package renewal) and
  sent in with Papyrus `Actor.SnapIntoInteraction`, then, 2 s later if it is not in it, by the console's `activate`
  (plain furniture only, never a workbench or a terminal). Once in, the furniture's graph and marker place it. When
  neither takes, or no such furniture is found, it stays held where it stood as they sat down instead of following them
  onto the seat. When they get up it is stood up at once (`Actor::StopInteractingQuick`, as a mirror is) and follows
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
- Teleport: same cell or worldspace: `SetPosition`. Elsewhere: `coc <cell editor ID>` or
  `cow <worldspace editor ID> x y` (cells and worldspaces keep their editor IDs at runtime), then
  `SetPosition` once loaded. Refused in dialogue (`DialogueMenu`), a scene (`GetCurrentScene`),
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
  300 units of the stand-in for 2 s sends `Revive`. The downed player clears the flag and restores
  health; the game stands them up a few seconds later with full health, which we cut back to 30%.
  45 s, giving up or the last friend leaving: essential off (base form and the character's own flag)
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
- Session rules (`Protocol::SessionRules`, protocol 49): the host's ini (src/game/HostRules.h; F4MPServer's
  options) sets them and the server sends them with every `Welcome`, the host's own game included, so every
  game plays by the same ones until the session ends (decoded rules are clamped: bleed-out 5-600 s, help
  0.5-30 s, health 1-100 %, window up to 600 min, cooldown up to 3600 s). The downed ones (bleed-out and help
  times, health, revives per fight or per window, the cooldown after getting up, helping only out of combat)
  are kept by DownRules::Self on the downed player's side: out of revives, or in the cooldown, the player is
  not made essential, so a lethal hit kills as it would alone. A fight is over 30 s after the player's last
  frame in combat (on the ground counts as in combat). The helper checks its own combat state for
  `bReviveOutOfCombat`. The defaults are the behaviour before the rules.
- VATS in real time (src/game/RealTime.cpp, src/game/RealTimeVats.h; `bRealTimeVats`): the game slows VATS's
  aiming through `VATS::magicTimeSlowdown` (0.04 while aiming, measured on 1.11.240; the global time multiplier
  and its target follow it; Jet sets the same slowdown to 0.3), and a shot playing out through the VATS
  camera (0.001 or the shot's own). While VATS is up (its menu, or a shot playing out) a global time
  multiplier under 1 is set back to 1 each frame (`BSTimer::SetGlobalTimeMultiplier(1, true)`), unless the
  slowdown was already on the frame before VATS came up (Jet or the like). Tutorial hints: the game's
  `bShowTutorials:Interface`, which its help messages read each time, is held off in a session
  (Background::Switch) and put back after; its tutorial message boxes are not gated by it.
- Menus still pause: clearing `kPausesGame` as a menu is made (wrapping its `UIMenuEntry::create`) was tried
  and taken out. The UI counts `menuMode` from the flag after the menu has handled its show message
  (0x141a811bc) and again at close (0x141a80a49). Made without it, the Pip-Boy came up with a blank screen
  and would not close, and a container was still counted (`menuMode` 1).
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
  conversation or a scene or not. Nothing goes through the dialogue system (no Say, topic, scene,
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
- A friend's game keeps what each builder says per (builder, piece) and shows it on the copy: the graph event the
  game's own powering sends (`Powered` / `Unpowered`: a lamp's glow, a fan turning), and the lights in its 3D
  switched (`TESObjectLIGH::SwitchLight`, which the game calls for every piece: a construction light is an activator
  with a light in its model). Shown again whenever the copy is drawn anew (its cell loaded
  again, the copy made again), tried 8 times if the game doesn't take it. Not through the game's own power
  handler: that fires `PowerOnEvent`/`PowerOffEvent`, and the scripts on a copy (a workshop object's, a radio
  beacon's) would act on them as if the piece were ours.

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
session as a player would), and `wsinfo <ref>` (now with `blocked=`, `powerPiece=`, `powered=`, and for a copy
`builder=`, `theirRef=` and `theirPower=`).

## Status (2026-10-09)

Protocol VERSION 47 (28 adds who reported a quest stage to kQuestStage, 29 the sender's clock to player states, to NPC states their owner's graph's direction and a dog's gait, and kReportSpawn and kSpawned for NPCs made at runtime, 30 whether a claimed NPC is essential to kClaimActors and the frame a player left parked to their status, 31 a line's recording (info, response number and voice type) in place of its topic, 33 a mirrored NPC's gait, 34 kRename and kRenamed, and names of 32 UTF-8 characters, 128 bytes on the wire, 35 a hello's load order, kRequestLoadOrder and kLoadOrder; 47 the next release's branches together, which took 36 to 46 for their own test builds (44 was a trial bundle's): the session's plugin table and kPluginTable (36), items given by dropping them: kReportDrop, kClaimDrop, kDropRecord, kDropPlaced, kDropGone, kDropList (37), a power wire's shape on a workshop item (38), LooksMenu skins and overlays on the face (39), kReportDamage and kActorDamaged for damage over time (40), kReportObjectHold, kReportObjectRest, kObjectHeld and kObjectRested for loose objects moved by hand (41), the host controls kHostCommand, kRequestHostInfo, kRemoved and kHostInfo (42), pings of what is under the crosshair, kPing 56 and kPinged 156 in place of 18 and 120 (43), the Minutemen's settlement requests shared by their run: kReportRadiantStart, kReportRadiantStage, kRadiantStarted and kRadiantStage (45), and whether workshop pieces are powered: kReportWorkshopPower and kWorkshopPower, see "Settlement power" (46)). The sections below record each review as it was written.

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
| `kExplosion` | anyone who threw in the last 10 s | the thrower's position must be known and the explosion within 8000 units in 3D; a joiner's budget of 16, 4 a second, and all joiners' of 32, 8 a second (kept across rejoining, as for deaths) | within 6000 units of us in our cell or worldspace, and the explosion's radius in (0, 1024] (`RemoteEdits::SetOff`). It goes off beside us and our companions with friendly fire off too: it is what kills the enemies there in our game (see below) | no |
| `kActorStates` (NPCs) | the player running the actor | owner only | an NPC a friend's game runs is moved to where their game has it (from a cell we have not loaded, or after it went far) only while our copy of the friend is within 1500 units of that spot: a companion walks with them. This goes for every NPC their game runs, so a settler their game moved far from them runs its own AI here until they come; in the own story, our story's characters are not switched on by a friend's game | no |
| `kContainerChanged` | anyone | at most 30000 changes per player in the session | a settlement's workbench and our own companion are left alone (LootScope: per player, counted in `perPlayer=`), and so are vendors' chests (`vendor=`); other containers follow friends | yes |
| `kReportDrop` | any game, of an item its player dropped | the giver's welcome first; a known base and count 1..32767, mods and name within the wire's caps, the place in the world; 1024 lying in the session, 256 a player (past them refused to the giver only), one report per nonce | a copy only of an item whose base and every mod this game has, within 5000 units of our player, 2 a frame; it can't be picked up without the server's grant | yes |
| `kClaimDrop` | anyone, of an item lying about | kTake: the first claim wins, the rest are told who has it; kGiveBack: only the taker | our player takes only a copy of ours, only after the grant, as their own Take | yes |
| `kDropRecord` | each game, of its own world | at most 4 a second (the last one is answered too); another session's counts as empty; a player's own drops and takes only | | no |
| `kWorkshopItem` | anyone | caps of 20000 built and 20000 scrapped | the anchor must be a workbench (`anchorRefused`); scale clamped to [0.1, 10]; a scrap must be buildable, unscripted, not a quest or enable-parent item, in the same space and in its build area: while some of the area's volumes are not loaded, a scrap outside the loaded ones waits (`scrapRefused`, `scrapDeferred`; a waiting scrap is tried again as cells near it attach, so `scrapDeferred` can stay above 0 until a player walks there); a wire's shape must be numbers, its ends within 4096 units of its middle, its slack in [0, 2] and its thickness in (0, 16] (the item is refused whole), and a wire without one is not drawn (`unplaceable`); a moved piece of the world itself moves only the receiver's own one, and only one linked to that workbench as its workshop item, of the same object, within its cell (`ownRefused`) | yes |
| `kWorkshopPower` | anyone, of their own pieces | the event budget; at least one entry, each a reference from the runtime range and a state of 0 or 1; the sender stamped | shown only on a copy we made of that player's piece (keyed by builder and their reference), never on a piece of our own or of the world; at most 8192 pieces kept; shown without the game's power events, so no script of ours runs on it | no |
| `kReportObjectHold`, `kReportObjectRest` | the player moving the object (the first to grab it; after a let-go the thrower until it rests, or whoever grabs it next) | a plugin's reference only; a hold within 512 units of the holder, angles within 8 rad, finite; a rest in the world, in one cell or worldspace, at under 5000 units a second (none at rest); refused while another player holds it (`ObjectMovers`); 20 holds a second a player, a rest budget of 24 and 6 a second (kept across rejoining) | a loose object of ours (`Movable`: loose form type, physics body, no quest alias, nothing SaveGuard, Spawns or WorkshopSync places, not in workshop mode), within 8000 units of us in the same space; a hold drawn only within 2500 units of the holder's stand-in, a rest only within 4096 units of where our copy is; never one in our player's hands; every pose finite and in the world before it is written, velocities capped | no: a move is over in seconds, and each game's save keeps where its own copy lies |
| `kHello.appearance` | each player, of themselves | passed on as sent | placed only when it is one of the two settlers or our own `iPuppetBaseForm`; otherwise our default | no |

A friend's explosion is set off in our game whoever stands beside it, with friendly fire off too. In our
game it is the only thing that does the damage of their throw: the stand-in's own grenade is removed,
the server refuses their reports of deaths of NPCs our game runs, and no health comes for one dead in
theirs. Leaving it out near us would leave the enemies a friend's grenade
killed in their game alive and fighting in ours. So with friendly fire off a friend's grenade at your
feet still hurts you, as it always has; a forged one is bounded by the server (after a throw, within
8000 units of the thrower, 16 at once and 4 a second, and 32 at once for all joiners together). A copy of the explosion that hurts nobody but
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
- Dev commands for the party: `party [list|pick|go|ping]`, `forms <type> [text]`, `edid <form>`,
  `voice [talk|loop on|off]` (record without the key; hear yourself), `markers [name]` (map
  markers and their flags), `idles <text>` (idle animations by editor ID, event or file),
  `dialogue` (the conversation the player is in), `npcvoice <actor>` (an actor's current line), `voiceline <actor> [info response voiceType [flags [priority]]]` (the recording an actor is saying as we report it, with the engine's own voice file for it; or a recording played at an actor as a friend's line is),
  `topics <npc>` / `say <ref> <topic>` (an NPC's dialogue lines; make it say one), `named <type> <text>` (forms by name, with keywords), `menu <name>
  [hide|force]` (open or close a menu, not the dialogue or the workshop menu: `workshop enter|exit`),
  `paused`, `gfx <menu> <path> [depth]` / `gfxset <menu> <path>
  <member> <value>` (look at and change a menu's Flash objects, on the main thread between frames), `weapsound <actor>` (the sounds
  of an actor's equipped weapon), `playsound <sound> [flags] [ref]` (play a sound descriptor), `quests started` / `quests running:<type>`
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
