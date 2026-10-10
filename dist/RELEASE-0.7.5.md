## 0.7.5

A stability release from a code audit for object-lifetime bugs of the kind that crashed the Red Rocket terminal in 0.7.0 and 0.7.1 (see docs/AUDIT-LIFETIME.md).

- Stand-ins and mirrored NPCs are always looked up by form ID, never by a pointer the game may have reused; a friend's face copy survives save loads; deletes waiting for a safe moment are forgotten on load.
- Stray grenade projectiles are removed after the engine's reference walk and only once loaded.
- Positions and explosion coordinates from other players must be finite and inside the world.
- Door and lock state is never read on actors; the ragdoll push on shared kills goes the right way; loose mods are not loot.
- Workshop event sinks no longer read the copy list while the game thread edits it; the explosion watch time is atomic.
- Face data for a friend's stand-in comes from the engine's heap, as the engine frees it.
- A friend's replayed melee swings do no damage of their own (the real damage arrives through the session).
- Protocol unchanged: 0.7.0 to 0.7.5 play together.
