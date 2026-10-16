## 0.8.7

Coming from 0.8.6:

- The story is shared by default: when any player moves the main story or a faction quest forward, everyone else is fast-forwarded to the same point (`sStory = shared`). The host can set `sStory = own` to have each player play the story in their own game, as before. If you keep your old `F4Multiplayer.ini` instead of the new one, it still says `sStory = own`: change it to `shared`, or switch "Shared story" on in the F1 menu. The dedicated server shares the story too unless started with `--own-story`.
- Enemies that appear mid-game (random encounters, ambushes) can no longer be worn down in one game by something the other game never saw, which could leave one dead for you while it kept fighting your friend.
- A friend's thrown grenade no longer leaves a grenade warning stuck on your screen.
- A grenade goes off once in your friends' games: throwing grenades a few seconds apart could set an earlier one off a second time for them, and a grenade could go missing for them after one of theirs went off near you.
- You hear what's said near you in a friend's game, not just read it: the lines of NPCs they talk to, and their own character's lines, play in the speaker's voice from where the speaker stands, fading with distance. It is a plain sound, so it can never leave you stuck in a conversation without your weapons, Pip-Boy or workshop. It is on by default (`bSpokenLines`, also "Spoken lines" in the F1 menu); the old `bExperimentalVoiceLines` setting is no longer read.
