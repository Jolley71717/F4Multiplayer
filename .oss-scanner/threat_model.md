# F4Multiplayer: threat model for the OSS Scanner

F4Multiplayer is a co-op mod for Fallout 4: an F4SE plugin (`src/`, Windows only, loaded into the
game) that connects the players' games to each other over Steam's networking or ENet/UDP, plus an
optional dedicated relay server (`server/`), the shared protocol and server logic (`common/`) and
a fake player used for testing (`bot/`). Players are friends who chose to play together, but the
code that reads what arrives from them must not trust it: a modified client can send anything.

## What is built and scanned here

Only the game-free parts build on Linux (`.oss-scanner/Dockerfile`: Ubuntu 24.04, gcc 14, xmake
v3.1.2): `common/` (the wire protocol, the networking helpers, the relay server's logic),
`server/` (the dedicated server), `bot/` (the fake player) and `tests/` (the unit tests, which
also include the game-free headers under `src/game` and `src/net`). The plugin DLL itself needs
Fallout 4's engine and MSVC, so `xmake.lua` leaves its target out on every platform but Windows;
its engine-facing code is out of scope for this scan, but the headers it shares with the tests
are in scope. The tests pass in the image (1138, one marked as a known bug); the two that check
Windows file sharing (a file another program holds open can't be renamed) check the Linux
outcome there instead.

## Where untrusted input enters

1. **Packets from other players and from the server** (`common/Protocol.h`, `Decode`, every
   message handler in `common/Server.cpp` and `src/net/Session.cpp`'s `HandlePacket`). This is
   the main surface. A packet comes from a peer over Steam P2P or from the relay over UDP; any
   player in the session, or anyone who can reach the relay's port, can craft one. Messages carry
   lengths, counts, form ids, strings (names, dialogue lines, reject reasons) and floats.
2. **The dedicated server's port** (`server/main.cpp`, `common/Server.cpp`): a UDP service that
   accepts connections from the Internet when a host chooses to run it; a password is optional.
3. **The host's built-in relay**: the same server code, run inside the host's game, reachable by
   whoever Steam lets into the lobby (friends and invitees).
4. **Files the plugin reads**: `F4Multiplayer.ini` (`common/IniFile.h`, `src/Config.cpp`), the
   language files under `lang/` (`src/Text.h`), the session file and the F4SE co-save it writes
   and later reads back (`src/game/*Store*`, `OnGameLoaded`). These are local files, so a
   malformed one is low severity unless a peer can place content into them through the protocol
   (for example a name or a line that is later written to the session file).
5. **The developer channel** (`src/DevChannel.cpp`): a TCP listener on 127.0.0.1 only, off by
   default (`bDevChannel`), authenticated by a random token in a file under the user's
   Documents. Anything reachable only through it is low severity; a way to reach it from the
   network, or to bypass the token, is high.

## What we consider severe

- **Critical**: memory corruption (out-of-bounds read or write, use after free, integer overflow
  that drives an allocation or index) reachable from a packet sent by a peer or by a client of
  the dedicated server, with or without a password; anything that lets a packet run code in the
  game or the server.
- **High**: a packet that crashes the game or the server (null dereference, unhandled
  exception, assertion, unbounded allocation or CPU time from a count or length field, i.e. a
  remote denial of service); a way to join a passworded server without the password; a way for
  one player to impersonate another (player ids and ownership claims); content from a peer that
  reaches a file path, a console command or a script name unescaped.
- **Medium**: logic abuse within the game's rules (a peer who can kill, loot or move things it
  shouldn't be able to), information disclosure of other players' Steam ids or addresses in
  logs or messages (the plugin deliberately redacts these: `common/Redact.cpp`,
  `src/Log.cpp`), resource exhaustion that needs many packets over a long time.
- **Low**: anything that needs a malformed local file, the developer channel, or a modified
  copy of the player's own game; log spam.

## Out of scope

- Fallout 4's engine, F4SE, CommonLibF4 (`lib/`) and Steam's networking.
- Cheating by a player in their own game (the mod does not try to prevent that).
- The ENet library's own code (`enet`, pinned to a release), unless we call it wrongly.

## How we would like reports

A reproducer that builds in the Dockerfile's image and sends the packet to the server or feeds
it to `Protocol::Decode` is ideal; the unit tests in `tests/` show how messages are built
(`tests/ProtocolSamples.h`, `tests/sim/`). Patches in the repository's style (tabs, `a_`
parameter prefix, a unit test alongside) are welcome.
