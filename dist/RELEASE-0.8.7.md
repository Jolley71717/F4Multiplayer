## 0.8.7

Coming from 0.8.6:

- The story is shared by default: when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (`sStory = shared`). The host can set `sStory = own` to have each player play the story in their own game, as before. If you keep your old `F4Multiplayer.ini` instead of the new one, it still says `sStory = own`: change it to `shared`, or switch "Shared story" on in the F1 menu. The dedicated server shares the story too unless started with `--own-story`.
