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

On a Mac: [docs/misc/MAC.md](docs/misc/MAC.md).

## Tests

```bat
xmake build -y
build\windows\x64\releasedbg\F4MPTests.exe
```

`xmake build` builds the tests with everything else. They cover the protocol, the session server (a
fake transport drives a real server, the test plays the players) and the parts of the plugin that
need no game. The exit code is the number of failed tests; pass a substring as the first argument to
run only the tests whose names contain it (`F4MPTests.exe Server`). `xmake run F4MPTests` works too.
`tools/soak.ps1` runs the standalone server with five bots that join, leave, crash and rejoin.
`xmake config --analyze=y` turns on the MSVC code analyser for our own sources.

The tests also build and run on Linux (`xmake f -p linux --toolchain=gcc`, g++ 14 or newer: everything but the
plugin), and `tools/coverage.sh` measures their line coverage there, on Linux or in WSL (Ubuntu 24.04: `apt install
g++-14 gcovr`, plus xmake 3.1). It builds the tests with `--coverage` into `build/coverage` (with its own xmake
configuration, so yours is left alone), runs them, and writes gcovr's reports for `common/` and `src/` to
`build/coverage/report`: a text report per file, a Cobertura `coverage.xml`, `coverage.json` (the totals) and
`coverage.md` (the total and each folder). Its exit code is the tests' own; a substring argument runs only the
matching tests. GitHub runs it for every pull request into main and the maintainer's pushes (`.github/workflows/tests.yml`): the coverage is in the
run's summary, and a pull request from this repository's branches gets a comment with it and the change against main.

Anything that touches the game (`src/game`, `src/steam`, the sync classes) can only be tested in a
game. Say in the pull request whether you ran it with two games (host and joiner) and what you saw;
"not tested in a game" is a fine answer, as long as it is said. A fix for something seen in a game
carries an `Evidence:` trailer in its commit message: what was run, and what a player saw that
shows the fix holding (`Evidence: two-game test, host in the Concord museum suit seen from the
joiner: one suit, the name tag over the wearer, an empty frame left where they got out`).

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
pre-release tagged `vX.Y.Z-rcN` once the unit tests and a two-game test pass. The same commit and
the rc's own zip (`tools/promote.ps1`) become `vX.Y.Z`, fast-forwarded to `main`, after a 90-minute
soak with no crash and no regression report; Nexus gets settled versions in batches, rarely. Details
in [docs/RELEASING.md](docs/RELEASING.md).

A pull request is merged only with the `reviewed` label, which says its code was read at its current head; the review
itself is a comment naming that commit. New commits remove the label (`.github/workflows/review-label.yml`) until they
are reviewed too.

## Binaries and new dependencies

Every pull request from outside the project gets the `needs-vetting` label by itself. The owner then runs the vetting
workflow (`.github/workflows/vet.yml`) on its head: on GitHub's throwaway machines it lists the binary files (a
compiled movie, a DLL, an archive, an image), the URLs and the sensitive changes (the build, the tools, the workflows,
submodules, git hooks, and the config that AI agents and editors load and run, such as `.claude/`, `.codex/`,
`AGENTS.md`, `CLAUDE.md` and `.vscode/`), checks every download the change adds against pinned hashes and signatures,
and builds and tests the code, before anything from the pull request is built or run on the maintainer's machines.
Only the summary of the vet's "Trusted: report" job counts, and it names the commit it vetted. The untrusted jobs'
summaries and annotations come from the pull request's own code and can say anything, and the run's report artifact is
only a convenience copy. After reading the report the owner adds `vetted`. The label workflow keeps it only while a
passed vet run exists for the pull request's current head, run from main's `vet.yml`, and then records "Vetted at
`<commit>`" in a comment. A merge checks for that comment naming the current head, not only the label. Adding the label
to any other head takes it off again, and so do new commits. An outside pull request merges only that way, whether or
not the label workflow ran. The owner's own pull requests get `needs-vetting`, and the same rule, only for a new or
changed binary, a new download URL, a change under `.github/`, to the submodules, to git hooks, to agents' or editors'
config, to build files that IDEs act on, or to `xmake.lua`'s package requirements.

An outside pull request's code is never built with `build.yml` (it refuses pull requests' refs and commits that are not
on one of this repository's branches): it is vetted with `vet.yml`.

The owner starts a vet from the `vet-runner` branch, with main's commit put there just before:
`git push --force origin origin/main:refs/heads/vet-runner`, then
`gh workflow run vet.yml --ref vet-runner -f pr=<number> -f sha=<head commit>`. The vet refuses any other branch, and
refuses when its workflow or scripts differ from main's. A cache written by the pull request's code then lands in
`vet-runner`'s scope, which no other branch reads.

Nothing committed in binary form is shipped: whatever a release holds is built from source by the project's own
workflows. Commit the sources and the script that builds them, not the build. A change that needs a new tool or
library says where it comes from and pins it: a version and a SHA-256, and the signature where one is published.

## No personal data

Commits, diffs, commit messages, logs in issues and pull request descriptions must not carry
personal data: no Steam names or Steam ids, no Windows user names (watch for absolute paths like
`C:\Users\...`), no machine names, no emails. Git history is forever and the repository is public.
A hang minidump (`F4Multiplayer-hang-*.dmp`, written only with `bHangDump`) contains file paths and
the Windows user name: it goes to the maintainer privately, never into an issue or a commit. The hang
report (`.txt`) names modules only and is fine to attach.

The same goes for the plugin's own log, which players attach to public issues. A log line names a
player as `player N` (their ID), a Steam account as `Redact::Alias(id)` (`~3fa2c1`, different every
launch) and an address through `Redact::MaskAddress`; never format a player's `.name`,
`PersonaName()`, a SteamID64, an address or a path under the user's folder into an INFO, WARN or
ERROR line. `Hud::Notify` text names players and is never logged, at any level: log what a
maintainer needs next to it, by player ID. `Redact::Scrub` runs over every line (the server's log
function and the plugin's log sink) as a net for Steam ids and IP addresses, not names;
`tests/LogPrivacyTests.cpp` and `tools/soak.ps1` check the server's log.

`tools/git-hooks` has a `pre-commit` and a `pre-push` hook that refuse a commit or push matching
patterns you keep outside the repo (`~/.f4mp-private-patterns`, one regex per line). Turn them on
with `git config core.hooksPath tools/git-hooks`. After the push is too late, so check before.

## License

The project is GPL-3.0-or-later with the Modding and Linking exceptions ([LICENSE](LICENSE),
[EXCEPTIONS](EXCEPTIONS)). By opening a pull request you agree that your change is licensed the
same way.
