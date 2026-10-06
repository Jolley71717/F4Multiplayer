# F4Multiplayer design

## Overview

```
 Player A's game                    Host (player or F4MPServer.exe)           Player B's game
 ┌────────────────────┐   UDP/ENet   ┌──────────────────────────┐   UDP/ENet   ┌────────────────────┐
 │ F4SE plugin        │─────────────▶│ Relay server (30 Hz)     │◀─────────────│ F4SE plugin        │
 │  Session           │◀─────────────│  join checks, rate limit │─────────────▶│  Session           │
 │  RemotePlayers ──▶ │              │  relays player states    │              │  RemotePlayers ──▶ │
 │  Puppets (actors)  │              └──────────────────────────┘              │  Puppets (actors)  │
 └────────────────────┘                                                         └────────────────────┘
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
