## 0.9.1-rc1

A test build of one fix: damage on enemies. Install it over 0.9.0. It still uses protocol 49, so it plays with 0.9.0,
but the fix only works in the games that have it: everyone should update, the host most of all.

Fixed:

- Every player's hits on an enemy count again. On 0.9.0, often only one player could hurt an enemy: the others' shots
  did nothing or their damage healed back on the next hit, and a melee hit moved the "one who can" to whoever landed
  it. Two causes: an ordinary gun hit could be taken for a grenade's or a Molotov's and given back, and a game that
  took an enemy over didn't tell the others its health, so the next report from the old game undid the damage. Now a
  gun or melee hit always counts as the hitter's, a game that takes an enemy over shares its health at once, and
  Molotov fire and grenades still hurt once, not twice.

Known issues:

- Why one enemy can be a settler in one game and a scavenger in another, and legendary enemies that are legendary in
  only one game, are still being looked at.
- The rest are as in 0.9.0.

For testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you
  restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names
  players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
