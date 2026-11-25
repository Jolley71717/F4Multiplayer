It uses the same protocol as 0.22.0 (85), so it can play with it. Install it over 0.22.0.

Fixed:

- **Vault 81's door no longer stays shut for a player who catches up on the intercom talk elsewhere.** When a friend had the talk and you were caught up to it away from the vault's cave (after loading a save, or joining later), the talk was marked done but the vault door never opened, and the talk couldn't be had again. Now the talk's last step waits until you reach the cave, then the door opens for you. Loading an earlier save doesn't undo story progress shared in the session; it catches you up to it. A save that is already stuck this way is not repaired by this.

This build is identical to 0.22.1-rc1: its log still names rc1.
