# Building and testing on a Mac

The project is built and released on Windows. On a Mac (Apple silicon) the server and the bot build
natively, the plugin and the tests build for Windows with clang-cl, and the game and the tests run
under CrossOver. Releases are still built with MSVC on Windows; a pull request tested with a clang-cl
build says so.

A fresh clone needs its submodules first (CommonLibF4 among them):

```sh
git submodule update --init --recursive
```

Tested with Homebrew's xmake 3.1.2, LLVM and lld 23.1.4, and xwin 0.10.0.

## Server and bot

With Homebrew's LLVM: Apple's own compiler lacks parts of C++23 the server uses before macOS 26.

```sh
brew install xmake llvm
LLVM=$(brew --prefix llvm)
xmake f -m releasedbg --toolchain=llvm --sdk=$LLVM --cxflags=-D_LIBCPP_DISABLE_AVAILABILITY \
    --ldflags="-L$LLVM/lib/c++ -Wl,-rpath,$LLVM/lib/c++"
xmake build -y F4MPCommon F4MPServer F4MPBot
```

## The plugin and the tests

clang-cl against the Microsoft C++ runtime and Windows SDK that
[xwin](https://github.com/Jake-Shadle/xwin) downloads (running it accepts Microsoft's license for
them); `tools/xwin.lua` holds the toolchain. `XMAKE_CONFIGDIR` keeps this configuration apart from
the native one:

```sh
brew install lld xwin
xwin --accept-license --arch x86_64 --sdk-version 10.0.26100 --crt-version 14.44.17.14 splat --output ~/.xwin
export XMAKE_CONFIGDIR=$PWD/.xmake/win
xmake f -p windows -a x64 -m releasedbg --toolchain=xwin
xmake build -y F4Multiplayer F4MPTests
```

Both land in `build/windows/x64/releasedbg/`, as on Windows. The tests run under a bottle's Wine
(below), against Microsoft's C++ library as on Windows. `tools/crossovermac.sh test` runs them; as on Windows, the exit code is the number
failed and a name runs only the tests whose names hold it.

## The game under CrossOver

`tools/crossovermac.sh` does what `deploy.ps1`, `restart-game.ps1` and `devctl.ps1` do on Windows,
with `XSE_FO4_GAME_PATH` set to the game folder in the bottle:

```sh
tools/crossovermac.sh restart               # deploy, start through Steam, load SanctuaryExt
tools/crossovermac.sh dev status pos
```

It starts the game through Steam, so Steam's overlay is in it. For Steam's Play to start F4SE, rename
`Fallout4Launcher.exe` in the game folder to `Fallout4Launcher.exe.bak` and copy `f4se_loader.exe`
to `Fallout4Launcher.exe` (a game update or a file check puts the launcher back).

## Two games on one Mac

A second bottle, cloned from the first with Steam exited: `cp -c` makes an APFS clone, which takes
almost no disk space. Give it its own `"BottleID"` in `cxbottle.conf` (`uuidgen`), as CrossOver's
Duplicate does. A bottle's `Documents` is a link to the Mac's own, so the clone needs a folder of its
own, without the player id: sharing one, both games are the same player (the server drops each for
the other) and write one log.

```sh
cd ~/Library/Application\ Support/CrossOver/Bottles
cp -cRp Steam Steam-2
D=Steam-2/drive_c/users/crossover
rm "$D/Documents" && mkdir -p "$D/Documents/My Games"
cp -cRp ~/Documents/My\ Games/Fallout4 "$D/Documents/My Games/"
rm "$D/Documents/My Games/Fallout4/F4SE/F4Multiplayer_player.id"
```

One Steam account plays in one place at a time, so the clone's Steam goes offline (or uses another
account that owns the game), and the games connect without Steam (INSTALL.md, "Without Steam"): both
`sTransport = enet`, the host's `bHost = true`, the joiner's `sServerAddress = 127.0.0.1`. The joiner
starts where its own save is; F7 takes it to the host. In the clone's `F4Multiplayer.ini`, another
`iDevChannelPort` and `sPlayerName`; drive it with `F4MP_DEV_PORT` to match:

```sh
XSE_FO4_GAME_PATH=<the game folder in Steam-2> F4MP_DEV_PORT=7791 tools/crossovermac.sh restart
```

`restart` closes only its own bottle's game. The game in the background keeps running while it is in
a session (`bRunInBackground`), but out of one it pauses, and a joiner that lost the host connects
again only once its window is in front: `bAlwaysActive=1` under `[General]` in both bottles' `Fallout4Prefs.ini` keeps both
running. `sIntroSequence=` under `[General]` in `Fallout4Custom.ini` skips the intro video.
