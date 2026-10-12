## 0.8.6-rc2 (release candidate)

A candidate for 0.8.6, on top of 0.8.5: unit tests and one full pass of the two-game test set passed; a multi-hour soak follows. Install a crash logger (Buffout 4 or Addictol) before trying it and report with the plugin log and the crash log.

- The F1 menu's first button is always Close or Back. Tab presses a message box's first button, so leaving the menu with Tab could start hosting, leave the game or stop hosting by accident, and a host who did that lost shooting, the Pip-Boy and the map until a restart.
- Nothing is written to the HUD for six seconds after a game load, when the engine rebuilds its menus. The host crashed in the HUD text layout three seconds after loading a save; the loading-screen guard from 0.8.5 ended too early for that case.
- Protocol 26, as 0.8.0 to 0.8.5. Newer builds are on GitHub first; Nexus follows less often.
