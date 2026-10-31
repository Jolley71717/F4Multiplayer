## 0.10.1-rc1

A test build of 0.10.1, a fix for hits that don't stick. Install it over 0.10.0. It uses a new protocol (56), so it
doesn't play with 0.10.0 or anything older: everyone in a session needs it, and a friend still on 0.10.0 can't join.
Update together. The host's session file from 0.10.0 still loads.

Fixed:

- Hits on an enemy another player's game runs now stick. Some players found that their shots took an enemy's health
  down and it jumped back, while their friend could kill it; sometimes neither of them could. Now every hit you land on
  a friend's enemy is numbered and its game answers it: applied, or refused with a reason. Your copy of the enemy keeps
  the damage until the answer comes, and a hit that is refused is taken back at once, not seconds later as a heal. A hit
  that is never answered is taken back after two minutes.
- A shot that hurts a friend's enemy but sends no hit event from the game is now counted as yours instead of being given
  back as a heal. Sometimes a player's game stops telling the mod about their hits (it was seen after a long wait at
  the "Creations are loaded" box and a load while hosting), and from then on their shots at the other player's enemies
  healed them. `net status` shows how often it happens (`copyUnseen=`), and the log warns once.
- The server sends a hit only to the game that runs the enemy. A hit sent to a game that has just handed the enemy on
  is passed on once to the new one, unless the enemy died meanwhile. The server now refuses at once a hit it cannot
  send on (the enemy is dead for the session, nobody runs it, or you run it yourself), and your game takes it back.
- A friend's hit on an enemy while its runner is in a menu, loading, or far away is held and applied when it can be,
  and your copy follows the enemy down meanwhile instead of climbing back.
- A save no longer keeps a friend's generator running. A friend's running generator was saved as running in your
  game; later, alone or in a session where the friend no longer had it, the engine sound played
  for as long as you were near, and nothing stopped it. A save now shows a friend's powered pieces unpowered while it
  writes and puts them back right after, so their lights and a machine's sound blink for the length of a save.
  Saves made by 0.10.0 or earlier that already hold the sound keep it until a session reports that piece.

Known issues:

- A save made before this build that holds a friend's generator running keeps the sound until a session reports that
  piece. A generator nobody reports any more stays running in that save.
- Every save, autosaves too, shows a friend's powered pieces unpowered for its length (see above).
- A friend's piece whose cell is unloaded at the moment of the save isn't covered.
- The dead list the session file keeps can still call an enemy dead that is alive in a new run; it was not the cause of
  this bug and is left for a later release.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Two players shooting one enemy from both sides: no health bar should jump back on either side. Trade the enemy
  between you for a minute, and try it while one of you fast-travels or sits in a menu.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
