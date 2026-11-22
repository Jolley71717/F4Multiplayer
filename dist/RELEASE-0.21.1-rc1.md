## 0.21.1-rc1

A test build of 0.21.1. It uses the same protocol as 0.20.0 to 0.21.0 (84), so it can play with them, but the fixes below need everyone in the session on 0.21.1. Install it over 0.21.0.

Fixed:

- **An update no longer replaces your F4Multiplayer.ini.** Installing a new version used to put the default settings file over yours. Your settings now stay, and new settings are added to the end of the file. A save of your settings, your player id or the ban list also never writes over a file it couldn't read.
- **Companions in power armor show in it on a friend's game.** A friend used to see your companion standing in the open while they were in a suit. Now the suit shows with them in it, and they step out of it the same way in both games.
- **A memory lounger that never seats you gives the controls back.** If the Memory Den's lounger didn't seat you, you could be left with no controls. Now you get them back.
- **A friend you can't see is found and shown again.** A friend's figure that stayed invisible near you is now rebuilt. Friends' figures also no longer say settler lines of their own.
- **A swarm of flying ants can no longer crash the game.** Killing many of the Nuka-World ant swarms at once could crash the game.
- **The game keeps running when you switch windows.** From the moment you host or join, the game keeps running when it's not the window in front, so the loading and the joining don't stall while you're in another window.
- **More of the mod is in your language.** Name tags, compass marker names, key names, conversation notices, Steam's status and the workshop texts are now translated in every language the mod has.

Notes for testers:

- Install a crash logger (Buffout 4 or Addictol) before testing, or a crash leaves nothing to read.
- Report with the plugin log (`Documents\My Games\Fallout4\F4SE\F4Multiplayer.log`, or `F4Multiplayer.1.log` if you restarted the game since) and the crash log; the first lines of the plugin log name the build. The plugin log names players as `player N` and leaves Steam ids and addresses out; crash logs and screenshots can still hold names.
