#include "Test.h"

#include "game/OwnGear.h"

namespace
{
	using OwnGear::Change;
	using OwnGear::Target;
	using OwnGear::Verdict;

	constexpr Change ALL[] = { Change::kDressStandIn, Change::kDressCopy, Change::kEmptyFrame, Change::kTakeLoose, Change::kContainer };

	Target Player()
	{
		Target target;
		target.player = true;
		return target;
	}

	Target StandIn()
	{
		Target target;
		target.standIn = true;
		target.madeHere = true;
		return target;
	}

	Target Mirrored()
	{
		Target target;
		target.mirrored = true;
		return target;
	}
}

TEST("own gear: nothing a friend's game sends changes the local player's inventory or what they wear")
{
	// Whatever the change, and whatever else the engine says of the reference: a stand-in's gear, an NPC copy's, a frame
	// to empty, a pickup to take out of the world, a container's loot.
	for (const auto change : ALL) {
		CHECK(OwnGear::Judge(change, Player()) == Verdict::kOurPlayer);
		auto odd = Player();
		odd.standIn = true;
		odd.mirrored = true;
		odd.madeHere = true;
		odd.inWorld = true;
		CHECK(OwnGear::Judge(change, odd) == Verdict::kOurPlayer);
	}
	// And the tally says so: the one count that must stay 0.
	OwnGear::Tally tally;
	CHECK(!tally.Count(OwnGear::Judge(Change::kDressStandIn, Player())));
	CHECK(!tally.Count(OwnGear::Judge(Change::kContainer, Player())));
	CHECK(tally.ourPlayer == 2);
	CHECK(tally.Refused() == 2);
	CHECK(tally.Count(OwnGear::Judge(Change::kDressStandIn, StandIn())));
	CHECK(tally.Refused() == 2);
}

TEST("own gear: a stand-in is dressed with their gear, an NPC copy with its owner's, and never the one with the other's")
{
	CHECK(OwnGear::Judge(Change::kDressStandIn, StandIn()) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kDressCopy, Mirrored()) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kDressStandIn, Mirrored()) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kDressCopy, StandIn()) == Verdict::kWrongActor);
	// An NPC of this world that no friend's game runs (one of ours) is dressed by nobody else.
	CHECK(OwnGear::Judge(Change::kDressCopy, Target{}) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kDressStandIn, Target{}) == Verdict::kWrongActor);
	// Gone: nothing to do.
	Target gone;
	gone.exists = false;
	gone.player = true;
	CHECK(OwnGear::Judge(Change::kDressStandIn, gone) == Verdict::kGone);
}

TEST("own gear: only a frame this game placed for a stand-in is emptied, never the local player's or the world's")
{
	Target copy;
	copy.madeHere = true;
	CHECK(OwnGear::Judge(Change::kEmptyFrame, copy) == Verdict::kGo);
	// A pre-placed frame (the museum's T-45), or one a friend's game shares by its ID: its pieces stay.
	CHECK(OwnGear::Judge(Change::kEmptyFrame, Target{}) == Verdict::kNotCopy);
	// The frame the local player is in, a copy they got into too.
	Target ours = copy;
	ours.playersFrame = true;
	CHECK(OwnGear::Judge(Change::kEmptyFrame, ours) == Verdict::kOurFrame);
}

TEST("own gear: a friend's pickup takes the loose item out of this world, not out of an inventory")
{
	CHECK(OwnGear::Judge(Change::kTakeLoose, Target{}) == Verdict::kGo);
	// A persistent item's reference stays itself in the inventory that took it: the local player's own pocket, or a
	// container's.
	Target held;
	held.inWorld = false;
	CHECK(OwnGear::Judge(Change::kTakeLoose, held) == Verdict::kInInventory);
}

TEST("own gear: a friend's loot goes to containers and corpses, never to a stand-in")
{
	CHECK(OwnGear::Judge(Change::kContainer, Target{}) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kContainer, Mirrored()) == Verdict::kGo);  // LootScope waits for its death first
	CHECK(OwnGear::Judge(Change::kContainer, StandIn()) == Verdict::kWrongActor);
	CHECK(OwnGear::Name(Change::kContainer) == "container");
	CHECK(OwnGear::Name(Verdict::kOurPlayer) == "our player");
}
