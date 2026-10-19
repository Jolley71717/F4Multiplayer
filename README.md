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

`tools/package.ps1` builds the release zip in `dist/out/` from a committed tree, after the unit tests pass. It refuses a zip that names the build machine or its user. `tools/promote.ps1` turns a release candidate that held into the release (docs/RELEASING.md).

Logs are written to `Documents/My Games/Fallout4/F4SE/F4Multiplayer.log` (the last three launches are
kept as `F4Multiplayer.1.log` to `.3.log`). They name other players as `player N` and Steam accounts by
a short alias, and mask IP addresses.

## Tests

```bat
xmake build -y
build\windows\x64\releasedbg\F4MPTests.exe
```

`xmake build` builds the unit tests with everything else: the protocol, the session server (a fake transport drives a real server) and the parts of the plugin that need no game. `tools/soak.ps1` runs the standalone server with five bots that join, leave, crash and rejoin. `tools/soak.ps1 -Abuse` floods it with made-up kills, loot, pickups and claims and a bot rejoining in a loop, and checks the limits hold without touching an honest player. `xmake config --analyze=y` turns on the MSVC code analyser for our sources.

## Bugs, ideas and pull requests

- Something broke: [report a bug](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=bug_report.yml). The form asks for the version, host or joiner, how you were connected, your mod list, the steps, and the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) plus a crash log from Buffout 4 or Addictol if the game crashed. The plugin log leaves names, Steam ids and addresses out; older builds' logs, crash logs and screenshots may not, so check those before pasting.
- Something you want: [propose an idea](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=idea.yml).
- Help getting a session going: the [Nexus page's posts](https://www.nexusmods.com/fallout4/mods/109812?tab=posts).
- A change: see [CONTRIBUTING.md](CONTRIBUTING.md) for building, tests, style, the protocol version rule and the no-personal-data rule. The maintainer reviews every pull request.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). Not yet: muzzle flash, workshop building, misc quests and radiant quests other than the Minutemen's settlement requests.

## License

GPL-3.0-or-later with the Modding and Linking exceptions; see [LICENSE](LICENSE) and [EXCEPTIONS](EXCEPTIONS).
