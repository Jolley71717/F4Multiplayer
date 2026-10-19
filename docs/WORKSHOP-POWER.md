# Workshop across games: snapping, scrapping and power

Where settlement sharing stands (protocol 23) and what the next steps need. Written before
starting the work, so the order and the risks are agreed first.

## What exists

- A player's workshop placements, moves and scraps are reported from the workshop's own events
  and copied into the other games (`WorkshopSync`): a plain reference of the same base at the
  same transform, keyed by (builder, builder's reference ID), kept by the server for joiners and
  in the host's session file.
- Copies are only of bases the workshop can build (statics, furniture, containers, doors, lights,
  terminals, misc, flora, non-unique NPCs for turrets). They are not workshop items in the
  receiving game: not counted, not scrappable, not wired, not assigned.
- A moved piece's copy is moved with its 3D (an actor through its controller), or made again when it
  changes cells. A load lets the copies' handles go and the server sends the building again; the
  save's copies are adopted at their spots. Reports are placed in order, a few a frame
  (`WorkshopInbox`); what each report does (scrap, follow, adopt, make, wait) is `WorkshopApply`, a
  game-free template the plugin runs against the engine and `tests/WorkshopSessionTests.cpp` runs
  against a stand-in world, behind a real server. Only a container copy's contents are handed to the
  player when it goes (`WorkshopApply::Rescues`): a turret's or robot's inventory is its own gear.

## Three steps, in order

### 1. Snapping to a friend's pieces (small, needs a manual test first)

Snap points are nodes in the object's model, and the placement code looks for snap points on
nearby references within `PlacementItemData::snapPointQueryRadius`. A copy has the same model,
so snapping to it may already work. Test by hand (the build menu cannot be driven by the rig's
scripts): host builds a wall, the friend enters workshop mode and brings a wall next to the copy.
If it snaps, step 1 is done. If not, the query only considers references linked to the workbench,
and step 2 covers it.

### 2. Copies as real workshop items (medium)

Make the copy belong to the receiver's workbench the way a placed object does: a linked
reference to the workbench with the `WorkshopItemKeyword` keyword (`TESObjectREFR::GetLinkedRef`
exists; setting one needs the ExtraLinkedRef write, or the engine's placement path
`Workshop::InitializePlacementReference` + `PlaceCurrentReference` with a `ContextData` built for
the workbench, which would do everything the game does including counting it against the build
limit). Then the copy is scrappable and counted locally.

That creates a conflict: two games can now scrap the same wall. Rule: the builder owns the object.
A scrap in the receiver's game is reported as a *request* (new message, keyed by the builder's
reference); the builder's game scraps its original, which relays the scrap as today, and the
receiver's copy goes with it. If the builder is away, the server scraps on their behalf (removes
the stored item and relays). The receiver's local scrap must be undone until confirmed, or the
object simply re-placed from the stored item if the request is refused.

Moving a friend's object follows the same request path.

### 3. Power (large)

- A wire is a spline reference between two connection references. Share it as a workshop item
  whose endpoints are (builder, reference) keys, mapped to local copies on receipt.
- Powered state: each game computes its own grids (`PowerUtils::PowerGrid`, `currentlyPowered`)
  from its own generators and wires. Once wires are shared and copies are real workshop items
  (step 2), the receiver's game computes the same grid on its own and no power state needs
  sending. Without step 2, power can only be faked: the owner reports its grids' powered
  references and the receiver flips the copies' powered flag, which needs an engine call that is
  not in CommonLibF4 yet (`ExtraPowerLinks` is known; the setter is not).
- Generators and switches are furniture and activators, so they are already copied; their
  activation state travels through the existing door/switch state path if the receiver can
  activate them, which step 2 allows.

So power is step 2 plus wires, not a feature on its own.

## Protocol cost

Step 2 adds one message (scrap or move request, builder-keyed); wires reuse the workshop item
message with two extra reference keys. Both change the protocol, so they should ship together
after 0.6.1, with everyone updating once.

## Risks

- The engine's placement path with a synthetic context is undocumented; if it misbehaves, the
  fallback is writing the linked reference and the workshop extra data by hand.
- Build-limit counting: copies count against the receiver's budget. Acceptable, since the
  settlement is shared.
- Settlers assigned by the builder (to a copy of a crop, a guard post) stay the builder's: NPC
  assignment is per game and would need a separate message. Leave for later.

## Found in play (protocol 38)

Tried on the rig, host and client in Sanctuary, after wires were shared:

- A friend's copy is not as plain as step 2 assumed: workshop mode highlights it and offers Store, Scrap
  and Attach Wire, like any object the workshop builds. Scrapping or storing it works in that game only
  (nothing is reported: it is a copy), and the scrap pays out its components.
- Wiring your light to a friend's generator works in your game: the game links the copy to your
  workbench as one of its items, the light comes on, and the generator adds to your settlement's power
  (it counts in both games). The friend sees the wire drawn, but in their game the wire is only drawn:
  their generator powers nothing of yours, and your light's copy stays dark.
- A copy of anything powered stays dark in the friend's game whatever its builder's grid does: copies
  are on no grid. Mirroring the builder's powered state still needs the engine call noted above.
- A copy wired into your grid is deleted like any copy when the session ends (or its builder scraps
  it). The game is not told: the wire stays, the light stays lit, and the grid keeps the deleted
  reference, whose ID the game later gives to something else. Nothing crashed in a save and reload, but
  this is how power grids get corrupted. Until copies are real workshop items (step 2), the safe course
  is to keep players from wiring to copies, or to unwire a copy through the game before deleting it.

## Done in protocol 46 (see DESIGN.md, "Settlement power")

Copies stay plain copies; power is mirrored, not shared:

- A friend's copy carries the game's WorkshopBlockInteractionKeyword, so workshop mode can't pick it: no wire
  reaches it, and it can't be stored, scrapped or moved here.
- None is ever left on one of our power grids. Every 2 seconds, before every save, before a copy is deleted and
  when the session ends, any copy a grid names, or with a wire or a workbench link, is taken off as the game's
  own scrapping takes a piece off (the workbench's RemoveItem, the wires disconnected, ours removed and reported
  scrapped). One the game still kept would be switched off and kept, never deleted.
- Each player's game sends which of its pieces are powered (on a change, to a joiner, after a load, every 30 s);
  the friend's game shows that on its copies: the lights in the copy's model switched and its graph told
  "Powered" / "Unpowered", as the game's own powering does, without the power events scripts act on.

Rig, 2026-10-19 (host and VM in Sanctuary, wires strung with `wswire`):

- Host's generator wired to its light: the host's grid at 000250FE had 2 nodes, both powered; the VM's copies
  said `theirPower=on` and the light copy lit. Scrapping the generator in host workshop mode: the VM's light copy
  went `theirPower=off`. The same from the VM to the host.
- Copies in both games had the keyword (`blocked=true`).
- With blocking off, a light of ours wired to a friend's generator copy: the grid named the copy (`COPY`); within
  2 s the audit had taken it off (0 grids, the copy without power links, our wire removed, our light unpowered).
  The same when the copy's builder scrapped it, and when the session ended (`net leave`) with the copy still
  tied: then a save and reload of that game found no grid naming a copy or a deleted reference.
- No copy was ever retired (`retired=0`): the game let go of every one.
