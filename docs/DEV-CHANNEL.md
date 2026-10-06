# Driving the game from outside: the dev channel

Most of this mod was tested without anyone touching the keyboard. A small control channel built
into the plugin lets test scripts read the game's state and act on it. Fake players (the bot) and
a few Windows tools for screenshots and sound fill in the rest. This page explains how it works and
what it can and can't do.

## What it can do

The channel controls the game rather than the controller: it doesn't walk the character with WASD
(`tools/input.ps1` covers simple input, see below). It can do almost anything the console or the
game's own code can:

- **Move the player:** `setpos 14 x y z` teleports the character, and `console coc SanctuaryExt`
  loads a cell.
- **Run any console command:** `console tgm` (god mode), `console save f4mptest`,
  `console 000AE12F.removeallitems player` (loot a container), `console startquest 0001F25E`.
- **Read state:** `status`, `pos`, `refinfo <ref>` (position, health, cell), `count <container> <item>`,
  `quests [name]`, `actors`, `containers`, `paused` and `net status`, which prints every
  multiplayer counter: hits taken and ignored, NPCs owned and handed off, quests caught up,
  sounds played, and so on.
- **Open and close menus:** `menu PipboyMenu force`.
- **Look inside the HUD:** `gfx` reads its Flash objects, and `gfxset` changes them. The compass
  markers were found this way.
- **Make things happen:** `say <npc> <topic>` makes an NPC speak, and `playsound <sound>` plays a
  sound. `weapsound <actor>` shows which gunshot an actor's weapon uses, and `gs <name>` reads game
  settings such as the difficulty damage multipliers.

The full list is the `COMMANDS` table at the bottom of `src/DevCommands.cpp`. DESIGN.md also
describes each one.

## How it works

1. **A listener inside the game** (`src/DevChannel.cpp`). When the plugin loads, it opens a TCP
   socket on `127.0.0.1:7790` on a background thread. That's the loopback address, so only programs
   on the same PC can reach it. Each line it receives is one command, and each reply is one line.
2. **A password for every launch.** At startup the plugin makes a random token and writes it to
   `Documents\My Games\Fallout4\F4SE\F4Multiplayer_dev.token`, readable only by your Windows user.
   A connection must send `auth <token>` first or it is dropped.
3. **Commands run on the game's main thread.** Game data must not be touched from the listener's
   thread. The listener queues each command with F4SE's task interface (`AddTask`) and waits up to a
   few seconds for the result. HUD changes go one step further, to the UI thread (`AddUITask`).
4. **Commands are plain functions** (`src/DevCommands.cpp`). Each takes the rest of the line and
   returns a string. Console commands go through the game's own console (`RE::Console::ExecuteCommand`).
   Everything else calls the game's code directly through CommonLibF4: forms, actors, menus, the
   audio manager.
5. **Off in releases.** It only starts when `bDevChannel = true` is set in `F4Multiplayer.ini`. The
   shipped ini says `false`, so players never have an open port.

## The test loop

| Step | Tool |
|---|---|
| Build | `xmake build` |
| Deploy, restart Fallout 4 through F4SE, wait for the main menu, load a cell, turn on god mode | `tools/restart-game.ps1` |
| Send commands | `tools/devctl.ps1 "<command>" ["<command>" ...]` |
| Add other players | `F4MPBot.exe`: a fake player that joins the session and can stand somewhere, shoot, loot, talk, hit you, report quest stages or take over an NPC |
| Press a key or fire | `tools/input.ps1 -Click` / `-Key <code>`: real mouse and keyboard input sent to the game window (brings it to the front) |
| See the screen | `tools/screenshot.ps1` |
| Hear the game | a read-only check of the game's audio session peak level, through Windows' audio meter API |
| Find out what happened | `Documents\My Games\Fallout4\F4SE\F4Multiplayer.log` and the crash logger's `crash-*.log` |

An example: checking that loot survives a reload.

```
devctl "console 000AE12F.additem 0000000F 50"   # 50 caps in a toolbox
devctl "console save f4mptest"
F4MPBot --loot 000AE12F:0000000F:-10            # a "friend" takes 10
devctl "console 000AE12F.removeallitems player" # we take the rest
devctl "console load f4mptest"                  # like reloading after death
devctl "count 000AE12F 0000000F"                # 40: the friend's 10 are gone, ours are back
```

Each feature in the roadmap was checked like this before it was committed. Fixes found this way
include a game crash in the first try at gunshot sounds, and another when loading a save while a
friend's compass marker was showing.

## Limits

- `input.ps1` can click and press single keys but can't steer, so anything that needs real play still needs a person:
  aiming, VATS, menus that only respond to clicks, voice chat with a real microphone.
- The bot is a network client, not a second copy of the game. It can't run NPCs or see the world,
  so features that need two real games, like an NPC handed to a friend, are only half-tested
  until two people play.
- Saves made by tests go into your normal Saves folder (for example `f4mptest`).
