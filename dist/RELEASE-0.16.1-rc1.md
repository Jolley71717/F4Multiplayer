## 0.16.1-rc1

A test build of 0.16.1. Install it over 0.16.0. It uses the same protocol (63) as 0.16.0, so a friend on 0.16.0 can join.

Fixed:

- **Your friend's shots show their tracer again with an armor-piercing receiver, a recon scope, or a Bloodied or Nocturnal legendary.** Those guns showed only the muzzle flash and the sound on your screen.
- **Getting up after being downed no longer leaves the camera stuck by the ground.** Being hit again just as you got up, or dying right after a down, could hold the camera in its move back to your view until the mod put it right a moment later. It now goes straight back.
- **Rejoining away from a quest's place no longer stalls the catch-up.** A quest that can start only where it happens (the Rotten Landfill fight, the Diamond City entrance scene) held up every other quest's catch-up for about 30 s after a rejoin elsewhere. It now waits for your next catch-up, and the other quests catch up at once.
- **A random event that rolled in both games at once is cleared fully.** When both games start the same event, the later one gives way; its people who were out of sight are now removed too, so they can't turn up later beside your friend's.

Known issues:

- The people in a shared event don't make their gestures in your friend's game (hands up, cowering, sitting).
- Diamond City's synth accusation still plays out in each game on its own.
- If the host reloads a save from before an NPC died while you were somewhere else, the NPC can stay dead in your game until its area resets.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
