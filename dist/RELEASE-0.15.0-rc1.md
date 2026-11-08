## 0.15.0-rc1

A test build of 0.15.0. Install it over 0.14.0. It uses the same protocol (62) as 0.14.0, so a friend still on 0.14.0 can join.

This version brings a new multiplayer menu, contributed by a developer from outside the project. The menu is a new file, `Data\Interface\F4Multiplayer\MPMenu.swf`: if you install by hand, copy the zip's `Interface` folder into `Data` along with `F4SE`. Without it, the old message-box menu comes back.

New:

- **A MULTIPLAYER entry** in the main menu and the pause menu, after Load.
- **The menu key opens a panel in the middle of the screen.** The game keeps running under it and the HUD stays up. Hosting, joining, host controls, settings and mods are all in it, and each row says what it does when you choose it.
- **A player list**: each friend's name, health, level and where they are. Choose a friend to teleport to them or, as the host, to kick or ban them.
- **Passwords you can type**, for hosting and joining, with View, Change and Remove. *Make one up* still works on a controller.
- **Controls**: change the mod's keys from the menu.
- **About**, with this version's release notes.
- The menu works with the keyboard, the mouse and a controller (D-pad, A and B).

Changed:

- Hosting waits until a game is loaded: you can't host from the main menu.
- The mod's keys act only as they go down, so a key held while a menu closes no longer counts as a new press.
- Going down closes the panel, so you see the downed screen. Opened while you're down, it stays.

Known issues:

- Key names in Controls are in English in every language.
- Random events that aren't fights (a synth held at gunpoint, the Diamond City synth accusation) still play out in one player's game only. Sharing them comes in the next version.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
