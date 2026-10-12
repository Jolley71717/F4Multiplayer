# Contributing

F4Multiplayer is a co-op mod for Fallout 4: an F4SE plugin (a native C++23 DLL loaded into the
game) plus a session server that runs inside the host's game or standalone. Bug reports, ideas and
pull requests are all welcome. This page says how to build, test and submit a change, and the few
rules that keep the project safe to publish.

## Where things go

- **A bug:** [open a bug report](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=bug_report.yml).
  The form asks for what the maintainer needs to reproduce it (version, host or joiner, transport,
  mod list, steps, the plugin log, a crash log).
- **An idea:** [open an idea](https://github.com/Jolley71717/F4Multiplayer/issues/new?template=idea.yml).
- **A question or help getting a session going:** the
  [Nexus page's posts](https://www.nexusmods.com/fallout4/mods/109812?tab=posts).
- **A change:** a pull request against `main`. Say what it does, how you tested it, and whether it
  touches the protocol (the template asks). Small, focused pull requests get looked at faster.

The maintainer reviews every issue and pull request.

## Building

You need Visual Studio 2022 (the "Desktop development with C++" workload), Git and
[xmake](https://xmake.io) 3.0 or newer. Clone with submodules: [CommonLibF4](https://github.com/libxse/commonlibf4)
lives in `lib/commonlibf4` and has a submodule of its own, so `--recurse-submodules` matters.

```bat
git clone --recurse-submodules https://github.com/Jolley71717/F4Multiplayer
cd F4Multiplayer
xmake config -m releasedbg
xmake build
```

The first build fetches ENet and spdlog through xmake's package manager. The plugin lands in
`build\windows\x64\releasedbg\F4Multiplayer.dll`, next to the standalone server `F4MPServer.exe`
and the test bot `F4MPBot.exe`. Copy the DLL to `Data\F4SE\Plugins\` to try it, or set
`XSE_FO4_MODS_PATH` (your mod manager's mods folder) or `XSE_FO4_GAME_PATH` and run `xmake install`.
The Steamworks SDK is not needed: the plugin calls the game's own `steam_api64.dll` through a few
hand-declared functions in `src/steam/SteamApi.cpp`.

If you already cloned without submodules: `git submodule update --init --recursive`.

## Tests

```bat
xmake build -y F4MPTests
build\windows\x64\releasedbg\F4MPTests.exe
```

The tests cover the protocol and the session server (a fake transport drives a real server) and need
no game. The exit code is the number of failed tests; pass a substring as the first argument to run
only the tests whose names contain it (`F4MPTests.exe Server`). `xmake run F4MPTests` works too.
`tools/soak.ps1` runs the standalone server with five bots that join, leave, crash and rejoin.
`xmake config --analyze=y` turns on the MSVC code analyser for our own sources.

Anything that touches the game (`src/game`, `src/steam`, the sync classes) can only be tested in a
game. Say in the pull request whether you ran it with two games (host and joiner) and what you saw;
"not tested in a game" is a fine answer, as long as it is said.

## Code style

There is no formatter configuration; match the code around you. The things that are consistent:

- **Tabs** for indentation.
- **Function parameters are prefixed `a_`** (`a_str`, `a_value`), so locals and members are told
  apart from arguments at a glance. Members have no prefix.
- **Comments say why, not what.** The code says what it does; a comment explains the reason, the
  constraint from the game or the protocol, or the bug it works around. A comment that restates the
  line below it gets removed.
- The standard library before hand-rolled helpers; `std::string_view` and `std::span` at the
  boundaries; no exceptions across the game's hooks.
- **The game is single-threaded.** `RE::` objects are only touched from the game's main thread.
  The network thread and the server thread hand work over through the existing task queues; see
  [docs/DESIGN.md](docs/DESIGN.md) before adding a thread or a lock.

## The protocol

`common/Protocol.h` is the wire format shared by the plugin, the server and the tools. Any change
to what goes over the wire (a message layout, a new or removed message type, a changed meaning of a
field, a different expectation on either side) **bumps `Protocol::VERSION`** in that file. The
server refuses a different version, which is what makes everyone update together instead of
desyncing quietly. Say in the pull request that you bumped it, or that the change does not touch
the wire. Protocol tests live in `tests/ProtocolTests.cpp`.

## Branches and releases

Work for a version lands on a `release-X.Y.Z` branch and is first published as a GitHub
pre-release tagged `vX.Y.Z-rcN` once the unit tests and a two-game test pass. The same commit
becomes `vX.Y.Z`, fast-forwarded to `main`, after a multi-hour soak with no crash and no regression
report; Nexus gets settled versions in batches, rarely. Details in [docs/RELEASING.md](docs/RELEASING.md).

## No personal data

Commits, diffs, commit messages, logs in issues and pull request descriptions must not carry
personal data: no Steam names or Steam ids, no Windows user names (watch for absolute paths like
`C:\Users\...`), no machine names, no emails. Git history is forever and the repository is public.
A hang minidump (`F4Multiplayer-hang-*.dmp`, written only with `bHangDump`) contains file paths and
the Windows user name: it goes to the maintainer privately, never into an issue or a commit. The hang
report (`.txt`) names modules only and is fine to attach.

`tools/git-hooks` has a `pre-commit` and a `pre-push` hook that refuse a commit or push matching
patterns you keep outside the repo (`~/.f4mp-private-patterns`, one regex per line). Turn them on
with `git config core.hooksPath tools/git-hooks`. After the push is too late, so check before.

## License

The project is GPL-3.0-or-later with the Modding and Linking exceptions ([LICENSE](LICENSE),
[EXCEPTIONS](EXCEPTIONS)). By opening a pull request you agree that your change is licensed the
same way.
