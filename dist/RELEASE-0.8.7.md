## 0.8.7

Coming from 0.8.6:

- The story is shared by default: when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (`sStory = shared`). The host can set `sStory = own` to have each player play the story in their own game, as before. If you keep your old `F4Multiplayer.ini` instead of the new one, it still says `sStory = own`: change it to `shared`, or switch "Shared story" on in the F1 menu. The dedicated server shares the story too unless started with `--own-story`.
- Enemies that appear mid-game (random encounters, ambushes) can no longer be worn down in one game by something the other game never saw, which could leave one dead for you while it kept fighting your friend.
- A friend's thrown grenade no longer leaves a grenade warning stuck on your screen.
- A grenade goes off once in your friends' games: throwing grenades a few seconds apart could set an earlier one off a second time for them, and a grenade could go missing for them after one of theirs went off near you.
- A friend who sits on a chair, couch, bench or stool, lies in a bed or sleeping bag, or works at a workbench, a station or a terminal is shown doing the same in your game, and gets up when they do. Their stand-in used to stand on the couch they sat on; where your game can't put it in the furniture, it waits beside it instead. Everyone needs 0.8.7 to play together: it uses protocol 31, so a friend on 0.8.6 gets "Version mismatch".
