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
  owner's copy aims at, and the aim modifier's graph variables), `quests [name]`, `actors`, `containers`, `paframes [radius]` (each power armor frame near the player, and whether the game would let the player into it, and why not: tilted, or its seat still taken), `paused`, `timescale` (the game's time multiplier and timer, VATS, the menus' pause count, the UI's timer and the game hour), `menuclock on [seconds] | off` (logs those clocks, Main's flags and how far the nearest NPC moved, each frame for 2 s or the given seconds after any menu opens or closes: what stops the world under a menu, F4Multiplayer-tnl5), `rules` (the session's rules from the welcome, and the down state) and `net status`, which prints every
  multiplayer counter: hits taken and ignored, NPCs owned and handed off, quests caught up,
  sounds played, and so on. `mods` says what is known of the host's mods (`mods ask` asks again), and
  `mods forms [formHex]` shows our load order against the session's plugin table, what was skipped
  either way, and a form ID in both numberings.
- **Find enemies:** `enemies [radius]` lists the actors near the player that are hostile to it, or
  fighting a player, nearest first: whether the player can see each one (`los`), what it fights as a
  session player number (`targetPlayer`, the same in every game, where a friend's stand-in has another
  form ID in each), and who runs it (`owner`, `here`; a copy of an NPC another game runs fights nothing
  here, so ask that game). An enemy made at runtime (`made=true`: a random encounter, a spawned enemy)
  has another form ID in each game: `enemies like <base|any> <x> <y> [within]` finds this game's actor
  of that base at that spot. Made for a test script playing a co-op follower.
- **Start a fight:** `spawnenemies <base> <n> <radius>` sets n NPCs of an NPC base form (`findnpc`
  finds one) evenly round the player at that radius (256 to 4000 units; 128 indoors), the first straight
  ahead, all facing the player and a little above the ground where each stands (outdoors, found with the
  engine's own pick; indoors the player's floor), so they drop onto it. At most 16 are placed (more
  asked are capped); a unique character or a friend's stand-in is refused. The answer is `placed=<n>
  asked=<n> base=<base> '<name>' radius=<r> ids=<ref>,<ref>...`, and `remove <ref>` deletes one again.
  They are made at runtime like a random encounter's raiders, so in a session the hostile ones are shared
  as one is (`made=true` in `enemies`). Indoors a wide ring can put them inside a wall: use a small radius.
- **Moved objects:** `objects` prints what is shared of loose objects moved by hand (the `objects:` part of `net status`), `objects grab` what the player holds (its reference, the grab type, its distance, and whether it is shared), and `objects probe <ref>` a reference's place and angles beside its 3D transform (translation, rotation matrix and XYZ Euler angles) and its physics bodies, to compare two games' copies.
- **Destructible objects:** `destruct` prints what is shared of cars, barrels and the like (the `destruct:` part of `net status`), `destruct near [radius]` the destructible objects around the player (3000 units unless given: each one's health and maximum, stages passed, destroyed, and its stages as percent with `x` an explosion, `m` a replacement model, `d` gone), and `destruct probe <ref>` one, with whether it is watched or waits to load.
- **Shared storage:** `storage` prints the `storage=` part of `net status` (owners known, the container viewed, moves on their way, whether the game is still settling after a load or a rejoin (`settling=`: no storage menu opens meanwhile, `settlingClosed=` counts those closed), the own moves its world lacks (`lacks=` kept, `lacking=` found, `lacksDropped=` past 4096), settles sent and the moves they sent late (`settles=`, `late=`), what is set aside, views, refusals, unanswered, unsent and overflowed moves, takes corrected to a shortfall, shortfalls, listings; `ownRefused=` must stay 0). The server logs "storage undid N and did again M of player P's moves" for each settle that put the true copy right. `storage keeper <ref>` says who keeps a container (ours, a friend's, or not shared storage: nobody's yet is ordinary loot) and whether this game calls it storage. `storage view <ref>` and `storage close` do what the container menu's show and close do, without the menu. `storage take <ref> <item> [count]` and `storage put <ref> <item> [count]` move items between the player and the container the way the menu does, so the move is reported as a player's; `storage count <ref> <item>` reads both counts.
- **Gifts through a container:** `gifts` prints the `gifts=` part of `net status` (the host's `bContainerGifts` on or off, the container viewed, our copies shown in it, takes waiting for their answer, puts waiting for their offer, the gifts lying in the session, and the counters: `given=`, `kept=`, `lateOffers=`, `refused=`, `notGiven=`, `taken=`, `received=`, `takenFirst=`, `givenBack=`, `giveFailed=`, `copyMissing=`, `unplaceable=`, `dupRisk=`; `ownRefused=` must stay 0). `gifts list` names each gift lying in the session (its base, mods, condition, name, container and giver, and whether our copy shows and our take waits). `gifts view <ref>` and `gifts close` do what the container menu's show and close do, without the menu (a view so opened stays until `gifts close`). `gifts take <ref> <item> [count]` and `gifts put <ref> <item> [count]` move items between the player and the container the way the menu does, so the move is the player's; `gifts count <ref> <item>` reads both counts.
- **Open and close menus:** `menu PipboyMenu force`.
- **Change the log:** `log level debug` adds the DEBUG lines (friend scans by alias, the lines
  characters say; HUD messages name players and are never logged) until the game restarts, `log
  level info` takes them out again, and `log flush` writes out what is buffered.
- **Look inside the HUD:** `gfx` reads its Flash objects, and `gfxset` changes them. The compass
  markers were found this way. Both reach into a menu's movie on the main thread between frames, like
  every other command (`gfxset` writes to it), outside the plugin's own HUD timing: not for use during
  a crash soak. `menu` opens and closes menus by message, but not the dialogue or the workshop menu: a
  conversation ends on its own, and workshop mode is `workshop enter` and `workshop exit`.
- **The icons over friends in a menu** (off unless the ini has `bMenuIcons = true`; DESIGN.md, Menus):
  `menuicons` projects each friend's spot over the head as the icon would (where on the screen, how far,
  shown or not) and says what was drawn (`shown`, `made`, `writes`, `hides`). `menuicons layer <path>
  <class>` makes the icons' layer again under another object of the HUD movie (`none` puts them straight
  in it), and `menuicons opt scale|readd|remake|canvas 0|1` changes how they are written: scaled instead
  of drawn at their size, added to their parent again after each move, made anew on each move, or all
  drawn into the parent's own graphics. `net status` names each friend's menu kind and how often their
  stand-in raised and lowered its Pip-Boy (`menu= pipboyRaises= lowers=`).
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
   runs it from F4SE's frame task instead, which is a task thread, not the main thread. No command
   goes to an F4SE UI task: those run on a job thread beside the main thread's drawing, and a menu
   looked up there (as `gfx` once did) could decide which thread destroys a closing menu.
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
| Go through workshop mode, the Pip-Boy and a conversation for an hour | `tools/menustress.sh`, or `tools/stress.ps1` from PowerShell (see below) |
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
tools\stress.ps1 -Minutes 60 -Games both         # the same from PowerShell (through Git for Windows' bash)
```

Each cycle enters workshop mode (`workshop enter`), highlights and scraps the nearest junk, leaves with
Tab; opens the Pip-Boy, turns a page and closes it; puts the NPC in front of the player, talks with E,
picks options with W and leaves with Tab. Anything that doesn't open or close in time, any menu open
over two minutes, an option that takes no input and a channel that stops answering are logged as STUCK
with the game's state, put right where the channel can, and listed in `stuck.txt`. The script's header
lists the settings (each key can be swapped for another key or for `input`).

A hung game is caught by the loop itself. The channel answers even then, with `error: timed out waiting
for game thread` after 12 s: the game thread ran none of its commands, either frozen or drawing on with no
frame of ours. That is logged as HANG with the plugin's own hang watch and frame watch lines from the
host's log (where the main thread is), and on the host `tools/hangcatch.ps1 -Now` writes every thread's
stack and a minidump into `<outDir>/hangs`. The loop then waits for the game to run commands again and
puts it back in play. `HANGCATCH=none` leaves the debugger out.

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
- `key`, `mouse` and `click` send through Windows, which hands input to whatever window is in front. They
  send only with the game in front (bringing it there first, a few tries) and drop the input otherwise,
  rather than typing it into a console or the window someone at the PC is using. Their answer ends in
  `front=yes`, or `front=no (<window> is in front)` as the command came in; `focus` asks for the front and
  answers the same, with the tally of what became of the input so far (`input: sent= broughtFront=
  dropped=` and the last one dropped). The loop checks `focus` before each pick: a pick that can't reach
  the game is logged as such, not as an option the menu ignored. On the VM a scheduled task (a screenshot,
  `input.ps1`) takes the front for a moment; a press sent then went nowhere, and on 2026-10-11 that was
  read as a dialogue that no longer took input.

`tools/hangcatch.ps1` can also run beside it on its own, as on the VM (which needs WinDbg too). When the game's window hasn't
answered Windows for 10 s, it attaches the console debugger without taking the game over (`cdb -pv`),
prints the texture wait's counters, writes a full-memory dump (the stacks-only minidump with `-Mini`, or when the drive
has less than `-MinFreeGB` free; the newest `-KeepDumps` are kept) and every thread's stack, and detaches; one catch per
freeze. The
plugin's own hang watch (`iHangSeconds`) reports the main thread from inside the game; this sees every
thread from outside, even when the plugin's are stuck with the rest.

## The texture wait behind a move

Every move of the player (a `coc`, a door, fast travel) ends in the engine's texture wait, on a job thread, while the
main thread pumps window messages for it; the host's hang of 2026-10-17 was that wait never ending
(docs/HANG-20261017.md). Right before the game starts a move (Main::OnIdle's call to PositionPlayerJob::Run, whoever
asked for the move), the plugin hands the streamer any texture requests the engine's per-frame walk has counted and not
yet handed over, which is what the wait would otherwise wait on forever (logged as `Texture wait: before the move,
handed the streamer ...`). `texwait` looks at it, from the channel's own thread, so it answers while the main thread is
stuck:

- `texwait` (or `texwait status`): the counters the wait loops on (`pending`, `done`, `total`), the update's stage, its
  lock (owner thread and count), how many requests the main thread's batch holds not handed over, whether the
  hand-over before a move is on, and how many moves it has handed requests over before.
- `texwait sample <s>`: reads that once a millisecond for up to 60 s and counts the moments a move would have hung in
  (pending set with requests owed).
- `texwait strand [<console command>]` (on the main thread): leaves the state the hang started from on purpose: one
  real request counted into the main thread's batch and not handed over, with `pending` set, as the walk leaves it after
  the streamer's pool ran out. A command given after it (`texwait strand coc SanctuaryExt`) runs right after, in the
  same call. The move hangs unless the hand-over before it runs. For ten seconds after, the log follows what the hang
  turns on (`Texture wait: watch +N ms: ...`: the counters, the stage, the lock's owner, whether the main thread's batch
  owes anything and whether the main thread is in the pump).
- `texwait flush off` / `on`: turns that hand-over off (to show the hang) and on again.
- `texwait unstick`: when the wait is stuck (its lock's owner in the wait, the counters saying go on), clears `pending`,
  so the wait ends the way it does after its loop; for getting a game stuck on purpose back.

With the main thread in the pump, the frame watch's report (`Frame watch: no game work has run ...`) adds the same
counters, which thread holds the lock and whether it is in the wait, and how many requests the main thread's batch,
set aside by the wait, never handed over. `tools/hangcatch.ps1` prints them too, before it writes a full-memory dump.

## Havok's angle loop and a body gone NaN

The host's hang of 2026-10-11 was a job thread spinning for ever in Havok's constraint setup, on a ragdoll whose
bodies had gone NaN (docs/HANG-20261011.md). The loop now ends on a NaN and counts it, and the main thread finds the
reference whose 3D is not finite and, when nothing else has a stake in it, disables and enables it again (logged as
`Havok NaN: ...`). `havoknan` looks at it:

- `havoknan` (or `havoknan status`), from the channel's thread (it answers while the main thread is stuck): how many
  of the loop's two copies have the detour, the NaN angles cut in all and per copy, how many looks the main thread
  made (and how many deep ones), how many deep looks in a row put nothing right (and whether it has stopped making
  them, until another space, a load or a new burst of cuts), how many references it reset, how many are waiting for
  their enable, and the last one found.
- `havoknan scan [roots]` (on the main thread): one look now, at every node of every loaded reference's 3D (`roots`:
  the root nodes only, as the 5-second look does).
- `havoknan poison <refHex> [dynamic]` (on the main thread): the dump's state made on purpose: a NaN rotation is
  written straight into every physics body under the reference's 3D (the game's own SetTransform turns a NaN down)
  and into its root node, and its physics is woken; `dynamic` first makes its bodies dynamic. On a ragdoll (a corpse,
  or the skeleton `000BDD9A` near Sanctuary: `console player.moveto 000BDD9A` first), the next step's constraint setup
  reaches the loop with a NaN: the game as it was hangs there; with the detour the cut is counted, and within a
  second the reference is reset (an actor, a persistent or quest reference, or anything of the session's is only
  logged). The reply says how many bodies read back NaN.

## Limits

- `input.ps1` can click and press single keys but can't steer, so anything that needs real play still needs a person:
  aiming, VATS, menus that only respond to clicks, voice chat with a real microphone.
- The bot is a network client, not a second copy of the game. It can't run NPCs or see the world,
  so features that need two real games, like an NPC handed to a friend, are only half-tested
  until two people play.
- Saves made by tests go into your normal Saves folder (for example `f4mptest`).
