# F4Multiplayer

Multiplayer mod for Fallout 4, built as an F4SE plugin using [CommonLibF4](https://github.com/libxse/commonlibf4).

**Status:** alpha. Up to 4 players share one world: players see each other (gear, weapon, name), NPC AI is shared between games, enemies can fight any player, and deaths, loot, pickups, doors, locks and quest progress are synced. See [INSTALL](dist/INSTALL.md) to play and [DESIGN](docs/DESIGN.md) for how it works.

## Requirements

- Fallout 4 **1.11.240** (Steam)
- [F4SE](https://f4se.silverlock.org/) 0.7.9
- [Address Library for F4SE Plugins](https://www.nexusmods.com/fallout4/mods/47327) (1.11.240 version)

## Building

Needs Visual Studio 2022 (Desktop development with C++), Git, and [xmake](https://xmake.io) 3.0+.

```bat
git clone --recurse-submodules https://github.com/Jolley71717/F4Multiplayer
cd F4Multiplayer
xmake config -m releasedbg
xmake build
```

The plugin is written to `build/windows/x64/releasedbg/F4Multiplayer.dll`. Copy it to `Data/F4SE/Plugins/`, or set `XSE_FO4_MODS_PATH` (mod manager mods folder) or `XSE_FO4_GAME_PATH` and run `xmake install`.

Logs are written to `Documents/My Games/Fallout4/F4SE/F4Multiplayer.log`.

## Roadmap

1. ~~Plugin loads and reads player state~~
2. ~~Players see each other move (server, puppets, interpolation)~~
3. ~~Walk/run/stop animations, per-player appearance choice~~
4. ~~Jumping, sneaking, weapons drawn, equipment~~ (real face/hair still to do)
5. ~~Names when looking at players~~
6. ~~Shared enemies: NPC AI ownership, enemy hits on any player~~ (NPC firing animations for non-owners still to do)
7. ~~World sync: deaths, health, containers, pickups, doors, locks, quests~~
8. Workshop building, real character faces, PvP option

## License

GPL-3.0-or-later with the Modding and Linking exceptions; see [LICENSE](LICENSE) and [EXCEPTIONS](EXCEPTIONS).
