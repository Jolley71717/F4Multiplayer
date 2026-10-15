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
  (default 4), an optional password and a load-order hash that must match the players already in.
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
  dbghelp, no loader lock). The report goes to the log and to `F4Multiplayer-hang-<time>.txt` next to
  it (the newest 3 kept, leftovers of an interrupted write removed at start-up); it names modules,
  never paths. A minidump (`.dmp`) is written too only with `bHangDump`: `MiniDumpWriteDump` in the
  hung process suspends every thread and then needs the loader and heap locks, so it can turn a
  freeze the game would have recovered from into a permanent one, and dbghelp may be in use by a
  crash logger. The dump holds file paths and the Windows user name; it is for sending to the
  maintainer privately, never for a public issue. The one game value the watch reads is `Main`'s
  quit flag: once the game is quitting (its shutdown reads no messages for a while) the watch stops.
  The judgement is `src/HangDetector.h`, unit tested.
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
  (kReportFace: head parts, hair colours, body-shape triangle, morph region sliders, facial bone
  transforms, tint layers, body tint); the server keeps the latest per player and tells newcomers.
  The receiver duplicates the stand-in's base NPC (a settler of the right sex), writes the look
  onto the copy (tint layers are rebuilt by vtable, pointing at the race's template entries by
  uniqueID) and spawns the stand-in from it. A changed face deletes the stand-in; it respawns with
  a fresh copy. Copies are never freed (forms can't be).
- **Remote players:** `src/game/RemotePlayers.cpp` spawns an NPC actor per remote player when the
  player is in the same interior or within about 9000 units outside. It renders a friend in the past
  and blends between received states. Each state carries the sender's clock (protocol 29), moved onto
  ours by the quickest trip of the last 64 states (src/game/Prediction.h, SenderClock), so states that came
  together after a stall are still drawn as far apart as they were sent. How far in the past (`delayMs`,
  at least 70) follows how far past the newest state our recent frames were (src/game/StateBuffer.h).
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
  NPC. Hits by NPCs made at runtime (only in the attacker's game) aren't sent; the victim ignores hits
  from an NPC that isn't loaded, alive and within 140 m in their world, and any while a menu pauses
  their game (`UI::menuMode`; `Main::freezeTime` stays false). The damage was resisted by the
  stand-in's copy of the victim's armor; the victim scales it by their own difficulty
  (`fDiffMultHPToPC*`, NPC hits only) and VATS (`fVATSPlayerMenuDamageMult` while targeting,
  `fVATSPlayerDamageMult` = 0.1 during playback). Not yet: the victim's perks, legendary armor
  effects, limb damage, radiation and stagger.
- Shots are shared too (see Weapon fire).

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
- Not yet: muzzle flash. `TaskQueueInterface::QueueWeaponFire` would bring it but only works for
  the player.

## Shared world (src/game/WorldSync.cpp)

References are identified by form ID, which matches across players because the load order must match.
Never shared: runtime-created references (0xFF......) and the player (0x14), which differ per game
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
corpse. A vendor's chest is any faction's merchant container; moves while the barter menu is open, and
for a second after, are trades and never reported. A corpse is shared once the session says it died: a
change for an NPC still alive in this game waits for its kill, up to 10 minutes in the world, counted
for all of that NPC's changes together (at most 4096 changes wait; when full, one waiting for a living
NPC makes room before a friend's change for a chest not loaded yet is dropped: `pendingFull=`). Loose
items picked up from the world are the same placed reference in every game, so only the personal ones
stay (a loose weapon is taken from the friend's world as it is). The status line counts what each end
kept: `weaponArmor=`, `vendor=`, `barter=`, `perPlayer=`, `notYetDead=`, `unsupported=` (both ends),
`personal=` (other players' personal items left alone, pickups included, as in 0.8.5), and on the
sender `personalSent=`, `holderDropped=` and `lootSent=`.

Activators (buttons, switches, levers, a sink) a friend presses are pressed in our game by their
stand-in, not our player, so a script that acts on whoever pressed it (rads from a sink, a teleport)
acts on the stand-in. Without the stand-in here the press is refused (`acts=` sent/applied/refused);
the door or lock it drives arrives as its state.

Remote changes are applied only while the player is in the world (no loading screen, not at the main
menu) and to loaded references; everything else waits and is retried.

| Feature | How |
|---|---|
| Deaths | TESDeathEvent -> ReportDeath; receivers run `<ref>.kill` when loaded. Of an NPC another game runs, the server believes the runner, the host's game about one dead in its save (found dead), and a player fighting it: one who reported its health, asked to take it over, or whose game ran it until it changed hands, within `Options::deathGrace` (10 s; the takeover itself is refused for 5 s after the NPC changes hands). Our kill's death event asks to take the NPC over first (a grenade's blows name no attacker), and the host's found-dead check leaves an NPC still falling, or hit by our player lately, to its death event for up to 15 s (`NpcHold`). Others are dropped, with a log line at most every 10 s |
| Health | TESHitEvent (cause = player) -> health as fraction of max; receivers apply via damage modifier |
| Container loot | TESContainerChangedEvent player<->container -> resolved in the next frame on the main thread; shared moves (LootScope) -> ContainerChange; the server numbers it and echoes it to everyone, including the sender; receivers check the scope again and `removeitem`/`additem`, in order per container |
| World item pickup | TESActivateEvent (player) paired with container event old=0,new=player (its ref is 0) -> RefPickedUp; receivers disable the ref |
| Quest stages | poll each quest's done stages (`TESQuest::stages`, a stage's done bit; src/game/QuestStageLayout.h) twice a second for side quests (type 7), and the main story and faction quests (types 1-5, 8+, plus the type-0 faction finales Inst305, BoS302, BoS304) only with `sStory = shared` (Welcome::sharedStory); never misc. Every newly done stage is reported, ascending, not just the current one: a stage whose script moves the quest on at once (MQ104 210) is never current. 5 s settle after load, only of quests the reporter has started. Receivers queue them (src/game/StageLog.h), only while in a session (dropped when it ends, except a lock's quests: those are set after leaving too, with the story the player's own again, so a teleport begun in the session finishes), skipping indexes the quest doesn't have, and `setstage` them in the order they came: per quest the lowest waiting stage above their own, the next once it reads done (5 s timeout), up to 4 per poll, once they have started the quest too: an objective has been shown (`BGSQuestObjective::state` not dormant). Running isn't enough: a new game runs many side quests in the background from the start. Until then the stage waits (another quest's stage may start it: MQ104 210 starts MQ105) without holding up other quests. A quest is done with once completed and stopped (many complete at a stage and run on: MQ102 65). A stage reads done before Papyrus runs its fragment, so after a stage whose fragment waits (Utility.Wait, moving the player) nothing that came after it is set for that long plus 1 s. src/game/StageRules.h, generated from the QF_ fragments of Fallout4.esm and DLCCoast.esm (following the stages each one sets): stages that move the player (MoveTo, a door they are made to use, FadeOutGame, an InputEnableLayer) are set only when live (came in the last 30 s, not from the session state) and the friend who did it (the server stamps the reporter's id on kQuestStage; a WorldState carries none) is within 6000 units, else skipped, unless they also start another shared quest or award an achievement (MQ206 500 and 1000, the MQ206 faction 1400/1500): then after a 10 s HUD warning, outside combat; MQ206 500 and the 1400s (controls off) only once 1200 (controls on) is waiting too, and until then only their own quest waits (Verdict::kQuestWaits), not what came after it. After a lock stage is set, its quest's stages go first, without a combat hold or another warning and not held up by other quests', are kept if the session ends, and 1200 is set outright once they have done nothing (none being set, no script of theirs running on) for 90 s of unpaused play (StageLog::Queue::Lock). The release is looked for before each poll of the queue: after leaving, once 1200 is done the session's other waiting stages go, so nothing past 1200 is set in the solo game, nor left for the next session. While the game is paused (`UI::menuMode`, a loading screen) nothing is set and holds and timeouts don't run down. Stages that take caps or ordinary items from the player (MQ105 177, FFDiamondCity10 40, ...) are never set; with `sStory = own`, side-quest stages that move a story or faction quest on (Min01 220/1500/2000, FFGoodneighbor01 100) aren't either. Caught-up stages wait during combat and 5 s after (at most 60 s), so Party doesn't take kill XP for quest XP. Never during dialogue; a scene holds them back, but one of a quest with stages waiting only for 30 s. One "Caught up" notice for many quests. Every poll in a shared-story session, a player whose MQ104 is past 210 without 210 done and whose MQ105 never started (left so by the old highest-stage-only sync, or by catching up from its session file) gets `setstage 229E6 10`, judged as MQ105's stage 0 (it moves the player): after the 10 s warning, outside combat, not in a conversation or a menu. Stage reports made while not welcomed (before the welcome, during a reconnect) are kept (the first 1024: what came first starts what came after), the story's and factions' too although Story::Shared is false while the connection is down (Session::OnDisconnected), marked so they go only if the welcome shares the story; they are sent once welcomed, at most 200 a second, oldest first (the server drops a player's events past 500 a second, all kinds together, and a stage is reported only once) (StageLog::Outbox). Ones made in no session (playing alone after leaving or stopping hosting), kept for a server the player then left for another (an invite accepted while the last host was being retried: Session::SwitchServer calls QuestSync::OnServerChanged), or kept through 5 minutes of not being welcomed (the host quit and the player plays on alone) are dropped, so a session joined later doesn't take solo progress for a friend's; loading a save drops them. Reporting doesn't settle again after a reconnect. A reconnect drops the waiting stages and what came live (OnSessionEnded): a friend's step that moves the player and was still waiting (a fight, a conversation) comes back in the WorldState as caught up, not live, and is skipped even beside that friend. Within a quest stages are set in stage order, not in the order they were done: one done after a higher one is already settled here and dropped (MQ104's 17 sets 12: 17's own script sets 12 here too). Each load logs whether the game awards achievements (`TESDataHandler::CheckModsLoaded`) and Steam's NEW_ACHIEVEMENT_n_0 list; new unlocks are logged after stages are set (src/game/Achievements.cpp) |
| Doors and locks | refs the player activated are watched for 60 s; open/lock changes -> RefState; receivers `BGSOpenCloseForm::SetOpenState` / `REFR_LOCK::SetLocked` |

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
else skips this: the player is never in those places.

## Party (src/game/Party.cpp, WorldClock.cpp, Hotkeys.cpp)

- Status (location, interior cell, health %, level, downed, opening, Pip-Boy light, power armor):
  sent when it changes (at most 4/s) and every 2 s; the server keeps the latest for newcomers.
  Receivers announce deaths (health 0) and put the health on the stand-in's name tag. In power
  armor it carries the frame's base form and, when every game has that frame (one a plugin placed,
  such as the museum's suit in Concord; a runtime 0xFF reference is sent as 0), the frame reference
  itself. The stand-in wears the pieces they wear from the moment the status says they are in
  power armor (`RemotePlayers.cpp`, `ArmorFrames.h`), and the receiver keeps a copy of the base
  (only a power armor frame: furniture with the engine's power armor furniture keyword) under it,
  placed at once, hidden and without collision from the moment its 3D is in, and moved along with
  the stand-in (placed again if it is in another place, over 256 units off or without 3D after
  10 s). When they get out, the pieces come off and the copy is drawn where they got out, solid once
  nobody stands within 70 units of it, and deleted 4 s later; a copy someone here gets into is left
  to them. The stand-in is never sent into the copy: activating a frame starts nothing for an NPC
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
  drawing API into `HUDMenu`'s `root.BottomCenterGroup_mc.CompassWidget_mc` on the UI thread (F4SE
  `AddUITask`, one queued at a time). The bar is 304 px for about 100 degrees (3.02 px/degree);
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
  replaced, so it never sits on the ground while its friend is up.
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

## Status (2026-10-09)

Protocol VERSION 29 (0.8.6; 28 adds who reported a quest stage to kQuestStage, 29 the sender's clock to player states, and to NPC states their owner's graph's direction and a dog's gait). The sections below record each review as it was written.

### Trust review of the 0.6.2-0.6.3 changes (2026-10-09)

What a peer can now send that it could not before, and what checks it on the way in:

- `Line.topic` (a form id with an NPC line). Server: one line per 250 ms per player on top of the event budget, decoded with the bounds-checked Reader (`AtEnd` required). Hearer: the id must resolve to a `TESTopic`, the speaker must be our own copy within 1400 units, repeats within 5 s are dropped and at most 3 lines show per 3 s; the console command is built from two integers only (`{:08X}.say {:08X}`), so no text from the wire reaches the console. Residual: a friend can make an NPC near you say any topic in the game, and topic infos may carry script fragments; that is the same trust a co-op friend already has by talking to NPCs and shooting in your world. Off unless `bExperimentalVoiceLines = true` (a line said as the hearer starts talking to that NPC can leave them held in dialogue). The old `bVoiceLines` key is ignored: the ini shipped it on from 0.6.3 to 0.8.6-rc2 and players keep their ini across updates, so reading it would have turned voicing back on for them.
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
| `kExplosion` | anyone who threw in the last 10 s | the thrower's position must be known and the explosion within 8000 units in 3D; a joiner's budget of 16, 4 a second, and all joiners' of 32, 8 a second (kept across rejoining, as for deaths) | within 6000 units of us in our cell or worldspace, and the explosion's radius in (0, 1024] (`RemoteEdits::SetOff`). It goes off beside us and our companions with friendly fire off too: it is what kills the enemies there in our game (see below) | no |
| `kActorStates` (NPCs) | the player running the actor | owner only | an NPC a friend's game runs is moved to where their game has it (from a cell we have not loaded, or after it went far) only while our copy of the friend is within 1500 units of that spot: a companion walks with them. This goes for every NPC their game runs, so a settler their game moved far from them runs its own AI here until they come; in the own story, our story's characters are not switched on by a friend's game | no |
| `kContainerChanged` | anyone | at most 30000 changes per player in the session | a settlement's workbench and our own companion are left alone (LootScope: per player, counted in `perPlayer=`), and so are vendors' chests (`vendor=`); other containers follow friends | yes |
| `kWorkshopItem` | anyone | caps of 20000 built and 20000 scrapped | the anchor must be a workbench (`anchorRefused`); scale clamped to [0.1, 10]; a scrap must be buildable, unscripted, not a quest or enable-parent item, in the same space and in its build area: while some of the area's volumes are not loaded, a scrap outside the loaded ones waits (`scrapRefused`, `scrapDeferred`; a waiting scrap is tried again as cells near it attach, so `scrapDeferred` can stay above 0 until a player walks there) | yes |
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
  `dialogue` (the conversation the player is in), `npcvoice <actor>` (an actor's current line),
  `topics <npc>` / `say <ref> <topic>` (an NPC's dialogue lines; make it say one), `named <type> <text>` (forms by name, with keywords), `menu <name>
  [hide|force]` (open or close a menu), `paused`, `gfx <menu> <path> [depth]` / `gfxset <menu> <path>
  <member> <value>` (look at and change a menu's Flash objects), `weapsound <actor>` (the sounds
  of an actor's equipped weapon), `playsound <sound> [flags] [ref]` (play a sound descriptor), `quests started` / `quests running:<type>`
  (quests the player has started; running quests of a type), `objectives <quest>` (stage and
  objectives with their state), `stages <quest>` (the stages done here and those from friends still
  waiting), `status` (cell, worldspace and location), `log level info|debug`
  (the plugin log's level until the game restarts; `sLogLevel` in the ini), `log flush`, `log path`.
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
