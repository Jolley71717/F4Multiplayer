A test build of 0.22.2. It uses the same protocol as 0.22.0 and 0.22.1 (85), so it can play with them. Install it over 0.22.1.

Fixed:

- **Your own buildings at a friend's settlement no longer vanish.** Since 0.22.0, when you joined a friend at a settlement they own, every piece you had built there in your own save was hidden (still solid and working, but invisible), even where your friend's settlement had nothing in its place. Now a piece of yours is hidden only where your friend's settlement has the same piece at the same spot, so the two don't overlap. Everything else stays as you built it. The first time pieces are hidden in a session, a note says so. `bPutAwayOwnSettlements = false` still turns the hiding off.

Notes for testers: install a crash logger (Buffout 4 or Addictol) before testing. Logs and crash reports go to the GitHub bug form, with your game's `F4SE\F4Multiplayer.log` attached.
