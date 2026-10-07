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

The plugin is written to `build/windows/x64/releasedbg/F4Multiplayer.dll`, next to the standalone server `F4MPServer.exe` and the test bot `F4MPBot.exe`. Copy the plugin to `Data/F4SE/Plugins/`, or set `XSE_FO4_MODS_PATH` (mod manager mods folder) or `XSE_FO4_GAME_PATH` and run `xmake install`. xmake fetches ENet; CommonLibF4 is the submodule. The Steamworks SDK is not needed: the plugin calls the game's own `steam_api64.dll` through a few hand-declared functions (`src/steam/SteamApi.cpp`).

`tools/package.ps1` builds the release zip in `dist/out/` (the one on Nexus). It refuses a zip that names the build machine or its user.

Logs are written to `Documents/My Games/Fallout4/F4SE/F4Multiplayer.log`.

## Tests

```bat
xmake build F4MPTests
xmake run F4MPTests
```

Unit tests for the protocol and the session server (a fake transport drives a real server). `tools/soak.ps1` runs the standalone server with five bots that join, leave, crash and rejoin. `xmake config --analyze=y` turns on the MSVC code analyser for our sources.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). Not yet: real character faces, muzzle flash, workshop building, misc/radiant quests.

## License

GPL-3.0-or-later with the Modding and Linking exceptions; see [LICENSE](LICENSE) and [EXCEPTIONS](EXCEPTIONS).
