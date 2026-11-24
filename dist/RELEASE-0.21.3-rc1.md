## 0.21.3-rc1

A test build of 0.21.3. It uses the same protocol as 0.20.0 to 0.21.2 (84), so it can play with them. Install it over 0.21.2.

Fixed:

- **Leaving a friend's game no longer empties your own containers into your inventory.** A container of your own that stood on the spot of something the host built (often one from a mod, or one placed from the console) was taken for the mod's copy of the host's piece. It was then removed and everything in it was put in your pockets, most often when you left the session. Now only the mod's own copies are ever replaced, and your containers keep what you put in them. The player who joins needs 0.21.3 for this fix.

Notes for testers: install a crash logger (Buffout 4 or Addictol) before testing. Logs and crash reports go to the GitHub bug form, with your game's `F4SE\F4Multiplayer.log` attached.
