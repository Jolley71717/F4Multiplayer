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
  `aim <actor>` (its pitch, the point its weapon aim turns toward, the pitch a mirrored NPC's
  owner's copy aims at, and the aim modifier's graph variables), `quests [name]`, `actors`, `containers`, `paused` and `net status`, which prints every
  multiplayer counter: hits taken and ignored, NPCs owned and handed off, quests caught up,
  sounds played, and so on.
- **Find enemies:** `enemies [radius]` lists the actors near the player that are hostile to it, or
  fighting a player, nearest first: whether the player can see each one (`los`), what it fights as a
  session player number (`targetPlayer`, the same in every game, where a friend's stand-in has another
  form ID in each), and who runs it (`owner`, `here`; a copy of an NPC another game runs fights nothing
  here, so ask that game). An enemy made at runtime (`made=true`: a random encounter, a spawned enemy)
  has another form ID in each game: `enemies like <base|any> <x> <y> [within]` finds this game's actor
  of that base at that spot. Made for a test script playing a co-op follower.
- **Open and close menus:** `menu PipboyMenu force`.
- **Change the log:** `log level debug` adds the DEBUG lines (friend scans by alias, the lines
  characters say; HUD messages name players and are never logged) until the game restarts, `log
  level info` takes them out again, and `log flush` writes out what is buffered.
- **Look inside the HUD:** `gfx` reads its Flash objects, and `gfxset` changes them. The compass
  markers were found this way. Both reach into a menu's movie from an F4SE UI task (`gfxset`
  writes to it), outside the plugin's own HUD timing: not for use during a crash soak.
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
3. **Commands run with the game's frame.** Game data must not be touched from the listener's
   thread. The listener queues each command for the next frame (`MainThread::Post`) and waits up to
   12 seconds for the result (`MAIN_THREAD_TIMEOUT`: a loading screen on a slow machine holds the game
   that long). A command it stops waiting for is marked abandoned and skipped when its turn comes, so
   it never goes off late. Queued work runs on the game's main thread, from a window message the plugin
   hooks; before the window exists, or if hooking it failed (logged as an error), `RunFrame(false)`
   runs it from F4SE's frame task instead, which is a task thread, not the main thread. A few HUD
   commands go one step further, to the UI thread (`AddUITask`).
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
| Go through workshop mode, the Pip-Boy and a conversation for an hour | `tools/menustress.sh` (see below) |
| Catch a freeze with every thread's stack | `tools/hangcatch.ps1` |
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

## Menus under stress, and hangs caught in the act

Races in menus (a workshop that deadlocked the game as it closed, a dialogue that stopped taking input, a
HUD that crashed) were found only by players: the soak walks, follows and fights. `tools/menustress.sh`
goes through the menus players race in, over and over, on one game or both at once:

```
tools/menustress.sh -m 60 -g both -n 0001CA7D    # an hour on both games, talking to Codsworth
```

Each cycle enters workshop mode (`workshop enter`), highlights and scraps the nearest junk, leaves with
Tab; opens the Pip-Boy, turns a page and closes it; puts the NPC in front of the player, talks with E,
picks options with W and leaves with Tab. Anything that doesn't open or close in time, any menu open
over two minutes, an option that takes no input and a channel that stops answering are logged as STUCK
with the game's state, put right where the channel can, and listed in `stuck.txt`. The script's header
lists the settings (each key can be swapped for another key or for `input`).

The commands it uses:

- `menus`: the menus open and for how long, and how often each opened and closed, kept from the menus'
  own open and close events. Asking the UI instead (`paused`, `GetMenuOpen`) takes a reference to the
  menu, which can change which thread destroys a closing one; the workshop menu destroyed mid-draw is
  how the game once deadlocked.
- `workshop`: workshop mode's state and the nearest workbench; `workshop enter`, `workshop exit`,
  `workshop junk` (the nearest pieces workshop mode would scrap), `workshop select <ref>` and
  `workshop scrap <ref>` (the game's own scrap function).
- `input <userEvent> [hold=<ms>]`: a button press handed straight to the game's input receivers (the
  menus' and the player's controls) and released after the hold. `key` goes through Windows,
  DirectInput and the control map first. When a key does nothing, `input` with the same user event
  tells a menu that ignores input from input that never got to it. `controls [context]` lists the
  control map's input contexts and their user events and keys.
- `dialogue menu`: the dialogue menu's own state: on the menu stack and uppermost, handling input,
  its option buttons shown, the input context it put up.

`tools/hangcatch.ps1` runs beside it on each PC (the VM needs WinDbg too). When the game's window hasn't
answered Windows for 10 s, it attaches the console debugger without taking the game over (`cdb -pv`),
writes a minidump with every thread and every thread's stack, and detaches; one catch per freeze. The
plugin's own hang watch (`iHangSeconds`) reports the main thread from inside the game; this sees every
thread from outside, even when the plugin's are stuck with the rest.

## Limits

- `input.ps1` can click and press single keys but can't steer, so anything that needs real play still needs a person:
  aiming, VATS, menus that only respond to clicks, voice chat with a real microphone.
- The bot is a network client, not a second copy of the game. It can't run NPCs or see the world,
  so features that need two real games, like an NPC handed to a friend, are only half-tested
  until two people play.
- Saves made by tests go into your normal Saves folder (for example `f4mptest`).
