## 0.8.2

- A friend's stand-in left behind in an interior (a building you both left) is replaced the next time it is seen outside, instead of standing in an unloaded cell for good.
- Using a terminal while in power armor no longer sends the terminal as the frame; the frame you climbed into is remembered.
- A friend's power armor frame copy is abandoned only when you really occupy it, not when you stand near it in your own armor (this made their stand-in run without a frame for the rest of the session).
- A joiner is told where each dead NPC fell, so corpses lie where the killer saw them rather than at the spawn point.
- Trust: a player's container changes stop at their share of the shared table; workshop pieces built remotely are turrets and robots only, and must stand within reach of their workbench.
- Map marker data is read through a size-checked mirror of the engine type.
- Control characters are dropped from every name, reason and line as it is read off the wire.
- A grenade thrown across an exterior cell seam now goes off for your friends too; the explosion scan covers every loaded cell around you, not only the one you stand in.
- An explosion your game set off for a friend's grenade is no longer reported back as your own after your next throw, so it does not go off twice for everyone.
- Loading-screen crashes: the compass markers for friends are no longer written to the HUD while a loading screen is up (the HUD is torn down and rebuilt then), and unchanged marker values are not rewritten every frame. On the test rig the second game crashed on a loading screen three times in nine ten-minute cycles with the markers on and not once in six with them off; the crash was in the HUD's render tree with nothing of this plugin on the stack. New setting bCompassMarkers (default on).
- Protocol 26, as 0.8.0 and 0.8.1. Newer builds are on GitHub first; Nexus follows less often.
