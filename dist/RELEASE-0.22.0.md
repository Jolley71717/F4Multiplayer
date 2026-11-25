This version uses a new protocol (85), so everyone in a session needs 0.22.0. Install it over 0.21.3.

New:

- **Join by address from the menu.** For friends who aren't on your Steam friends list, or who are on the same network. The host keeps hosting through Steam, then turns on *Join by address* in *Host controls* (F1). The menu shows the address to give out, and makes up a password if you have none. A friend chooses *Join a friend*, *Join by address* and types or pastes it. The menu hides addresses on screen and remembers the last few. Over the internet the host needs UDP port 7779 forwarded, or a VPN such as Tailscale (simpler and safer). The install guide has the details.
- **The host lets each friend in by hand.** A friend who joins by address waits until the host chooses *Let in* or *Turn away* under *Host controls*. After a minute with no answer, they are turned away. Steam friends are never asked about. An address that tries too many wrong passwords is refused for a few minutes.
- **Your password is never sent as typed.** When joining by address, your game proves it knows the password without sending it, so nobody on the network can read it.
- **Your own buildings no longer show through the host's at a shared settlement.** In a friend's game, at a settlement the host owns, the pieces you built there in your own save are hidden while the session lasts (still solid, so nothing on them falls). They come back when you leave. Nothing is deleted or moved, so your save keeps every piece. Turn it off with `bPutAwayOwnSettlements = false`, or make the hidden pieces walk-through with `bPutAwayWalkThrough = true`.

This build is identical to 0.22.0-rc1: its log still names rc1.
