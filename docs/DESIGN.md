# F4Multiplayer design

## Overview

```
 Player A's game                    Host (player or F4MPServer.exe)           Player B's game
 â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   UDP/ENet   â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   UDP/ENet   â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
 â”‚ F4SE plugin        â”‚â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–¶â”‚ Relay server (30 Hz)     â”‚â—€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚ F4SE plugin        â”‚
 â”‚  Session           â”‚â—€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚  join checks, rate limit â”‚â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–¶â”‚  Session           â”‚
 â”‚  RemotePlayers â”€â”€â–¶ â”‚              â”‚  relays player states    â”‚              â”‚  RemotePlayers â”€â”€â–¶ â”‚
 â”‚  Puppets (actors)  â”‚              â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜              â”‚  Puppets (actors)  â”‚
 â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜                                                         â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

- **Transport:** ENet. Channel 0 is reliable (join/leave, future events). Channel 1 is unreliable,
  sequenced (player state at 20 Hz).
- **Server:** `common/Server.cpp`, a single thread. It runs inside the host's game (`bHost = true`) or as
  `F4MPServer.exe`. Joining is checked for protocol version, max players (default 4), an optional
  password, and a load-order hash that must match the first player's.
- **Client:** `src/net/NetClient.cpp` keeps ENet on its own thread, so the connection survives loading
  screens. `src/net/Session.cpp` runs on the game's main thread every frame through an F4SE permanent
  task.
- **Remote players:** `src/game/RemotePlayers.cpp` spawns an NPC actor per remote player when the
  player is in the same interior or within about 9000 units outside. It renders 100 ms in the past
  and blends between received states. Before every save and load, all puppets are deleted so they
  never end up in a save file.
- **Puppets:** `src/game/Puppets.cpp` hooks `Actor::Update` (vtable slot 0xCF, taken from a live
  actor's vtable). For puppets it calls `UpdateNoAI` and forces position and heading. It sets
  `kMovementBlocked` and the like every frame, and re-applies the do-nothing package every 2 s.

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

## Remote player look

- Body: a settler of the player's sex (0020A578 female / 0020A57B male) unless `iMyAppearance` is set.
- Gear: the equipped armor and weapons (base forms) are sent when they change (checked every second).
  The puppet gets `removeallitems` + `additem`/`equipitem` via the console. Weapon mods and
  the player's face are not copied.
- Weapon drawn/holstered follows the `kWeaponDrawn` state flag (`DrawWeaponMagicHands`).
- Name: `ExtraDataList::SetOverrideName`, and the hooked `GetActivateText` returns true for puppets so
  the HUD shows the name when you look at them (pressing E still does nothing).

## Shared NPC AI (src/game/NpcSync.cpp)

- Every loaded, living NPC from a plugin file has one owner, assigned by the server: the first player
  to claim it. A player's current companion is always taken over by that player (force claim), and
  hitting an NPC someone else runs takes it over too (at most every 5 s per NPC) so it fights back.
- The owner sends its NPCs' position/heading/speed/moveMode/weapon-drawn at 10 Hz (unreliable). The
  others register the NPC as a `Puppets::Kind::kNpc` puppet: AI off, movement copied with 150 ms
  interpolation, but it can be hurt and killed (health and deaths sync as before).
- When the owner unloads the NPC (or leaves), it is released and the next player who has it loaded
  claims it. Mirrored NPCs are handed back to their own AI before saving.
- Remote players' stand-ins are in PlayerFaction, so enemies in the owner's world attack them. Hits on a
  stand-in by an NPC are forwarded to that player (`PlayerHit` -> damage to their health).
- Not yet: NPC attack animations and projectiles in non-owner worlds (they aim but don't fire there).

## Shared world (src/game/WorldSync.cpp)

Actors, containers and world items are identified by reference form ID, which matches across
players because the load order must match. Runtime-created references (0xFF......) are never
shared. The server keeps the session's world state and sends it to late joiners (WorldState).

| Feature | Status | How |
|---|---|---|
| Deaths | done, tested | TESDeathEvent -> ReportDeath; receivers run `<ref>.kill` when loaded |
| Health | done, tested (incoming) | TESHitEvent (cause = player) -> health as fraction of max; receivers apply via damage modifier |
| Container loot | done, tested (both directions + late join) | TESContainerChangedEvent player<->container -> ContainerChange; receivers `removeitem`/`additem` |
| World item pickup | done, tested (both directions) | TESActivateEvent (player) paired with container event old=0,new=player (its ref is 0) -> RefPickedUp; receivers disable the ref |
| Quest stages | done, tested (both directions + late join) | poll `TESQuest::currentStage` twice a second for quest types 1-5, 7+ (not misc); forward changes only, 5 s settle after load; receivers `setstage` (only if higher) |
| Doors and locks | done, tested (both directions + late join) | refs the player activated are watched for 60 s; open/lock changes -> RefState; receivers `BGSOpenCloseForm::SetOpenState` / `REFR_LOCK::SetLocked` |

## Status and next steps (2026-10-06)

Protocol VERSION 11. Release zip 0.3.0 was sent to the user (protocol 4), so it is now outdated.

Next, in order:
2. Test outgoing health with a real player hit (needs the user, or an explosion placed by the player).
8. Re-package a release and test with the friend.

## Prior art

- Skyrim Together Reborn (GPL-3.0): ownership, interpolation and animation sync design.
- FO4_Wrld (AGPL-3.0): Fallout 4-specific reverse engineering. Reference only.
- Fallout 76 (public sources): server authority for inventory and damage, and atomic item transfers.

## Testing tools

- `tools/restart-game.ps1`: deploy, relaunch and load into a cell.
- `tools/devctl.ps1`: dev channel (needs `bDevChannel = true`). Commands: `status`, `pos`, `net`,
  `echo on|off`, `spawn`, `puppet`, `setpos`, `graph`, `flags`, `console`.
- `F4MPBot.exe`: a fake player that walks in a circle.
- `F4MPServer.exe`: a standalone server.
