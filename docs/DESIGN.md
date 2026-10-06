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
 |  WorldSync       |           |   states (30 Hz)       |          |  WorldSync       |
 |  QuestSync       |           | keeps the session's    |          |  QuestSync       |
 |  Puppets         |           |   world state          |          |  Puppets         |
 +------------------+           +------------------------+          +------------------+
```

- **Transports:** the server talks to players through `ServerTransport` (`common/Transport.h`), the
  client through `ClientConnection` (`src/net/ClientConnection.h`). Reliable messages are ordered;
  unreliable ones carry player state (20 Hz) and NPC states (10 Hz).
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
- **Client:** `src/net/NetClient.cpp` keeps ENet (including DNS lookups) on its own thread, so the
  connection survives loading screens. `src/net/Session.cpp` runs on the game's main thread every
  frame through an F4SE permanent task.
- **Remote players:** `src/game/RemotePlayers.cpp` spawns an NPC actor per remote player when the
  player is in the same interior or within about 9000 units outside. It renders 100 ms in the past
  and blends between received states. Before every save and load, all puppets are deleted so they
  never end up in a save file.
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
  Unique NPCs (companions, quest characters) are refused as appearances; their scripts would come along.
- Gear: the equipped armor and weapons (base forms) are sent when they change (checked every second).
  The puppet gets `removeallitems` + `additem`/`equipitem` via the console. Weapon mods and
  the player's face are not copied.
- Weapon drawn/holstered follows the `kWeaponDrawn` state flag (`DrawWeaponMagicHands`).
- Name: `ExtraDataList::SetOverrideName`, and the hooked `GetActivateText` returns true for puppets so
  the HUD shows the name when you look at them (pressing E still does nothing).

## Shared NPC AI (src/game/NpcSync.cpp)

- Every loaded, living NPC from a plugin file has one owner, whose game runs its AI. The server decides:
  - nobody owns it: the first player to claim it (they have it loaded);
  - a player's current companion goes to that player, unless it is also another player's companion
    (then the first keeps it, so two players with Dogmeat don't trade him forever);
  - a player who hits or talks to an NPC takes it over, unless it is someone's companion or changed
    hands in the last 5 s.
- The owner sends its NPCs' cell/worldspace, position, heading, speed, moveMode and weapon-drawn at
  10 Hz (unreliable). The others register the NPC as a `Puppets::Kind::kNpc` puppet: AI off, movement
  copied with 150 ms interpolation, but it can be hurt and killed. If the owner's copy is in a
  different cell or worldspace (a companion that followed its player through a door), the local copy
  is not moved there; it runs its own AI meanwhile.
- When the owner unloads the NPC (or leaves), it is released and the next player who has it loaded
  claims it. Mirrored NPCs are handed back to their own AI before saving. New players get the current
  owner list on join.
- Remote players' stand-ins are in PlayerFaction, so enemies in the owner's world attack them. Hits on a
  stand-in by an NPC are forwarded to that player (`PlayerHit` -> damage to their health).
- Shots are shared too (see Weapon fire).

## Weapon fire (src/game/WeaponFire.cpp)

- Detection: every actor gets its animation graph's events through its
  `BSTEventSink<BSAnimationGraphEvent>` base at +0x38; the graph sends "WeaponFire" once per shot.
  We hook slot 1 of that vtable twice: the player's class (our shots) and the NPC class (shots by
  NPCs we run; stand-ins and mirrored NPCs use the same class and are filtered out).
- `ReportShot {refId}` (0 = the sender) goes to the server, which accepts NPC shots only from the
  NPC's owner and relays `ShotFired {playerId, refId}` unreliably (60 shots/s per player).
- Replay: "attackStart" on the stand-in's or mirrored NPC's graph plays the full firing animation
  (recoil, aim pose). Nothing is launched: damage already arrives through health sync, and real
  projectiles from stand-ins would add friendly fire and kills nobody made.
- A stand-in drawn before its weapon is equipped stays empty-handed with the "drawn" flag set and
  rejects attacks, so after every equipment change it holsters and draws again.
- Not yet: gunshot sound and muzzle flash. The weapon's own attack sound fields are empty for
  vanilla guns (the sound is resolved elsewhere when firing), and
  `TaskQueueInterface::QueueWeaponFire` (which would bring both) only works for the player.

## Shared world (src/game/WorldSync.cpp)

References are identified by form ID, which matches across players because the load order must match.
Never shared: runtime-created references (0xFF......) and the player (0x14), which differ per game
(`Protocol::IsShareableRef`, checked by the server and again by every receiver). Receivers also check
form types before acting: kills only for actors, loot only between containers/NPCs and inventory
items, pickups only for loose inventory items that don't fill a quest alias.

Remote changes are applied only while the player is in the world (no loading screen, not at the main
menu) and to loaded references; everything else waits and is retried.

| Feature | How |
|---|---|
| Deaths | TESDeathEvent -> ReportDeath; receivers run `<ref>.kill` when loaded |
| Health | TESHitEvent (cause = player) -> health as fraction of max; receivers apply via damage modifier |
| Container loot | TESContainerChangedEvent player<->container -> ContainerChange; the server numbers it and echoes it to everyone, including the sender; receivers `removeitem`/`additem`, in order per container |
| World item pickup | TESActivateEvent (player) paired with container event old=0,new=player (its ref is 0) -> RefPickedUp; receivers disable the ref |
| Quest stages | poll `TESQuest::currentStage` twice a second for quest types 1-5, 7+ (not misc); forward changes only, 5 s settle after load; receivers `setstage` (only if higher) |
| Doors and locks | refs the player activated are watched for 60 s; open/lock changes -> RefState; receivers `BGSOpenCloseForm::SetOpenState` / `REFR_LOCK::SetLocked` |

### Catching up (join, reconnect, loading a save)

The server keeps the session's world state and sends it in chunks (`WorldState`) on join and when a
player asks again (`RequestWorldState`). Kills, pickups, doors and quest stages can be applied twice
safely. Container changes can't (that would duplicate items), so:

- each session has a random ID, and container changes are numbered in session order;
- each player tracks which numbers its world contains (`containerNext`, plus the changes still waiting
  for their container to load) and stores that in the save's F4SE co-save;
- Hello and RequestWorldState say what the world has, and the server sends only the rest.

After loading a save (including the reload after dying) the client forgets what it knew, reads the
save's record and asks for the session's changes again.

## Status (2026-10-06)

Protocol VERSION 13.

## Prior art

- Skyrim Together Reborn (GPL-3.0): ownership, interpolation and animation sync design.
- FO4_Wrld (AGPL-3.0): Fallout 4-specific reverse engineering. Reference only.
- Fallout 76 (public sources): server authority for inventory and damage, and atomic item transfers.

## Testing tools

- `tools/restart-game.ps1`: deploy, relaunch and load into a cell.
- `tools/devctl.ps1`: dev channel (needs `bDevChannel = true`). `help` lists the commands.
- `F4MPBot.exe`: a fake player that walks in a circle and can report kills, loot, pickups, doors,
  quest stages and hits, take over an NPC (`--own`), shoot (`--shoot <weapon>`, its own or the
  NPC's), and resume a session (`--session`, `--from`).
- `tools/input.ps1`: real mouse clicks and key presses in the game window (e.g. to fire).
- `tools/screenshot.ps1`: captures the screen for visual checks.
- `F4MPServer.exe`: a standalone server.
