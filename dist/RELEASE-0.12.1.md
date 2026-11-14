## 0.12.1

A patch for 0.12.0: the notice sound and a stuck camera after being downed. Install it over 0.12.0. (This is the same
file as the 0.12.1-rc1 test build, so seeing "rc1" in your log is normal.) It uses the same protocol (60), so it plays
with 0.12.0, and a session saved by 0.12.0 loads as it is. It is best if everyone in a session has it.

Fixed:

- A loud sound (a generator hum for one player, rushing water for another) could start with each multiplayer notice in
  the top left (a friend's XP, joined or left, connection messages) and get louder with every one until you reloaded.
  The mod asked the game to play a sound named "" with each notice, and in some setups that name matches a looping
  sound that never stops. Notices now ask for no sound, as the game's own notices do. We couldn't make the sound happen
  on our test machines, so please tell us on GitHub whether it's gone for you.
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
