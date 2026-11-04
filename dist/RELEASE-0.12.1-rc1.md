## 0.12.1-rc1

A test build of 0.12.1. Install it over 0.12.0. It is a patch: it uses the same protocol (60), so it plays with 0.12.0,
and a session saved by 0.12.0 loads as it is. It is best if everyone in a session has it.

Fixed:

- A loud sound (a generator hum for one player, rushing water for another) could start with each multiplayer notice in
  the top left (a friend's XP, joined or left, connection messages) and get louder with every one until you reloaded.
  The mod asked the game to play a sound named "" with each notice, and in some setups that name matches a looping
  sound that never stops. Notices now ask for no sound, as the game's own notices do. We couldn't make the sound happen
  on our test machines, so please tell us whether it's gone for you.
- The camera could stay stuck (often underground) after a friend helped you up, when you were downed again right as
  you stood up, and the Pip-Boy wouldn't open until you loaded a save. The camera now finds its way back to you.

Known issues:

- Supply lines aren't shared yet: the host's supply lines feed only the host's builds.
- The robot workbench isn't shared yet.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or
  food in your workbench: they count in the numbers only.
- If a friend also owns the host's settlement in their own save, the host's pieces count in both games' production
  there.
- Players whose mods change the leveled lists differently can still see different enemies.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Worth trying: playing with a friend joining and leaving and earning XP near you (listen for any sound that starts
  with a notice), and being downed again right after a friend helps you up.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
