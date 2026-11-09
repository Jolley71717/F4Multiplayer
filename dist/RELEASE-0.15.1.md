## 0.15.1

A small fix release. Install it over 0.15.0. (This is the same file as the 0.15.1-rc2 test build, so seeing "rc2" in your log is normal.) It uses the same protocol (62) as 0.15.0 and 0.14.0, so a friend on either can join.

Fixed:

- **Key names in Controls are in your language.** Space, Shift, Mouse 4, the arrows, Esc and the rest used to read in English in every language; so did the back key at the bottom of the panel.
- **Your shots show for your friend right after you change guns.** With two copies of the same gun in your inventory, switching to another gun could make your friend's game keep putting your character's gun away and back for about 9 seconds, and your shots showed no tracers meanwhile.

Known issues:

- Random events that aren't fights (a synth held at gunpoint, the Diamond City synth accusation) still play out in one player's game only. Sharing them comes in the next version.
- Closing the game (Alt+F4 or the console's `qqq`) while in workshop mode crashes it on the way out. The unmodded game crashes there too. Quitting from the pause menu is fine.
- A workbench is shared only at settlements the host owns.
- Settlers' beds aren't counted for a friend's settlement yet, and a friend's pieces don't make real daily water or food in your workbench: they count in the numbers only.
- Players whose mods change the leveled lists differently can still see different enemies.

Reporting a bug: install a crash logger (Buffout 4 or Addictol), then use the GitHub bug form with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`) and the crash log.
