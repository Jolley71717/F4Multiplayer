## 0.8.5-rc1 (release candidate)

A candidate for 0.8.5: it passed the unit tests and one full pass of the two-game test set on the rig in both directions, and is now in a multi-hour soak. It becomes 0.8.5 once the soak is clean and nobody reports a regression. Install a crash logger (Buffout 4 or Addictol) before trying it; report with the plugin log (Documents\My Games\Fallout4\F4SE\F4Multiplayer.log, whose first lines name the build) and the crash log if any.

What to try: the F1 menu (host, join a friend, settings); emotes on F2, F3, F4; a friend in power armor; a companion following a friend across the map; story mods you run alongside.

- In-game menu on F1: host or stop hosting, invite, join a friend who is hosting (listed when their Steam shows them in Fallout 4), who is in the session with ping, and the main settings, written back to the ini. Built on the game's own message box; see docs/MENU.md.
- Emotes move to F2, F3 and F4. F9, the old "point" key, is the game's quickload: pointing reloaded the game mid-session, seen as the Pip-Boy not opening and nobody able to shoot. If you set iKeyEmote* yourself, avoid F5 and F9.
- Quest stages and spoken lines are shared only for the game's own files. A quest from another mod set to a stage by a friend ran that mod's scripts in a game that never got there (reported with a story mod: locked controls and a crash in its dialogue).
- A friend's companion that came along from a part of the map you have not loaded now appears beside them instead of staying where it lived.
- HUD messages (a friend's line, "X joined") wait while a loading screen is up; writing them into a HUD being rebuilt crashed the game.
- The compass does nothing while there is nobody to show.
- A friend's stand-in wears their power armor pieces only once it stands in its frame copy; before that the pieces hung off a normal-sized body (the "dwarf in a frame").
- Protocol 26, as 0.8.0 to 0.8.4. Newer builds are on GitHub first; Nexus follows less often.
