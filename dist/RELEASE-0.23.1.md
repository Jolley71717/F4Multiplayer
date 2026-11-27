It uses the same protocol as 0.23.0 (86), so it can play with it. Install it over 0.23.0.

Fixed:

- **Quest NPCs no longer pull the other player into a conversation that isn't theirs.** Some quest scenes start a conversation by themselves when you come near, such as Jack and Edward when you enter Cabot House. In the other player's game that scene could start again, open the conversation with them, take the NPCs over and leave the quest at a different stage in each game, which broke it for both. Now that game closes the conversation and leaves those NPCs to the game that runs them. A conversation you start yourself by pressing E is unchanged. (0.23.0 fixed this for scenes that didn't open a conversation; this covers the rest.)
