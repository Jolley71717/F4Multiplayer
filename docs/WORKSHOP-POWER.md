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
