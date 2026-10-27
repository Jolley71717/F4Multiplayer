#include "Test.h"

#include "game/OwnGear.h"

namespace
{
	using OwnGear::Change;
	using OwnGear::Target;
	using OwnGear::Verdict;

	constexpr Change ALL[] = { Change::kDressStandIn, Change::kDressCopy, Change::kEmptyFrame, Change::kTakeLoose, Change::kContainer, Change::kDropCopy, Change::kGiftCopy };

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
	held.heldHere = true;
	CHECK(OwnGear::Judge(Change::kTakeLoose, held) == Verdict::kInInventory);
	Target ours;
	ours.inWorld = false;
	ours.heldByPlayer = true;
	CHECK(OwnGear::Judge(Change::kTakeLoose, ours) == Verdict::kInOurInventory);
	// In no cell and no inventory the engine names: being loaded or moved, for now.
	Target between;
	between.inWorld = false;
	CHECK(OwnGear::Judge(Change::kTakeLoose, between) == Verdict::kNotPlaced);
	// A cell wins over a holder the engine still names.
	Target back = ours;
	back.inWorld = true;
	CHECK(OwnGear::Judge(Change::kTakeLoose, back) == Verdict::kGo);
}

TEST("own gear: a pickup of an item in no cell waits and is tried again, for a while; one an inventory holds is dropped at once")
{
	using namespace std::chrono_literals;
	CHECK(OwnGear::TryAgain(Verdict::kNotPlaced, 0s));
	CHECK(OwnGear::TryAgain(Verdict::kNotPlaced, 29s));
	CHECK(!OwnGear::TryAgain(Verdict::kNotPlaced, OwnGear::PICKUP_WAIT));
	CHECK(!OwnGear::TryAgain(Verdict::kNotPlaced, 5min));
	// Held by our player, a container or an NPC, or gone: final, whatever the wait.
	CHECK(!OwnGear::TryAgain(Verdict::kInOurInventory, 0s));
	CHECK(!OwnGear::TryAgain(Verdict::kInInventory, 0s));
	CHECK(!OwnGear::TryAgain(Verdict::kOurPlayer, 0s));
	CHECK(!OwnGear::TryAgain(Verdict::kGone, 0s));
	CHECK(!OwnGear::TryAgain(Verdict::kGo, 0s));
}

TEST("own gear: benign refusals have a log budget of their own and can't hide the alarming ones")
{
	CHECK(OwnGear::Alarming(Verdict::kOurPlayer));
	CHECK(OwnGear::Alarming(Verdict::kWrongActor));
	CHECK(OwnGear::Alarming(Verdict::kNotCopy));
	CHECK(OwnGear::Alarming(Verdict::kOurFrame));
	CHECK(OwnGear::Alarming(Verdict::kInOurInventory));
	CHECK(!OwnGear::Alarming(Verdict::kGone));
	CHECK(!OwnGear::Alarming(Verdict::kInInventory));
	CHECK(!OwnGear::Alarming(Verdict::kNotPlaced));

	using Log = OwnGear::LogBudget::Log;
	OwnGear::LogBudget budget;
	CHECK(budget.Take(Verdict::kGo) == Log::kSkip);
	// A join's catch-up: far more gone items than the benign budget.
	std::uint32_t logged = 0;
	for (int i = 0; i < 500; ++i) {
		if (budget.Take(Verdict::kGone) != Log::kSkip) {
			++logged;
		}
	}
	CHECK(logged == OwnGear::LogBudget::BENIGN_LIMIT);
	// The alarming ones are still logged after it, up to their own cap, the last one saying so.
	CHECK(budget.Take(Verdict::kWrongActor) == Log::kLog);
	CHECK(budget.Take(Verdict::kOurPlayer) == Log::kLog);
	for (std::uint32_t i = 2; i + 1 < OwnGear::LogBudget::ALARMING_LIMIT; ++i) {
		CHECK(budget.Take(Verdict::kNotCopy) == Log::kLog);
	}
	CHECK(budget.Take(Verdict::kOurPlayer) == Log::kLogLast);
	CHECK(budget.Take(Verdict::kOurPlayer) == Log::kSkip);
	// A new session starts the budgets again.
	budget = {};
	CHECK(budget.Take(Verdict::kOurPlayer) == Log::kLog);
	CHECK(budget.Take(Verdict::kGone) == Log::kLog);
}

TEST("own gear: a friend's loot goes to containers and corpses, never to a stand-in")
{
	CHECK(OwnGear::Judge(Change::kContainer, Target{}) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kContainer, Mirrored()) == Verdict::kGo);  // LootScope waits for its death first
	CHECK(OwnGear::Judge(Change::kContainer, StandIn()) == Verdict::kWrongActor);
	CHECK(OwnGear::Name(Change::kContainer) == "container");
	CHECK(OwnGear::Name(Verdict::kOurPlayer) == "our player");
}

TEST("own gear: an item given by dropping it: only this game's own copy, lying loose, is placed, taken away or swapped")
{
	Target copy;
	copy.madeHere = true;
	CHECK(OwnGear::Judge(Change::kDropCopy, copy) == Verdict::kGo);
	// One of the world's own items (a plugin's), never ours to take away.
	CHECK(OwnGear::Judge(Change::kDropCopy, Target{}) == Verdict::kNotCopy);
	// Already picked up: the local player's from then on, or another inventory's.
	auto ours = copy;
	ours.inWorld = false;
	ours.heldByPlayer = true;
	CHECK(OwnGear::Judge(Change::kDropCopy, ours) == Verdict::kInOurInventory);
	CHECK(OwnGear::Alarming(Verdict::kInOurInventory));
	auto held = copy;
	held.inWorld = false;
	held.heldHere = true;
	CHECK(OwnGear::Judge(Change::kDropCopy, held) == Verdict::kInInventory);
	CHECK(OwnGear::Judge(Change::kDropCopy, Player()) == Verdict::kOurPlayer);
	CHECK(OwnGear::Name(Change::kDropCopy) == "dropCopy");
}

TEST("own gear: a refused move at a friend's shared storage is put right on our player, and on nothing else")
{
	CHECK(OwnGear::Judge(Change::kStorageUndo, Player()) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kStorageUndo, Target{}) == Verdict::kWrongActor);  // a container, an NPC: never by this change
	CHECK(OwnGear::Judge(Change::kStorageUndo, StandIn()) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kStorageUndo, Mirrored()) == Verdict::kWrongActor);
	Target gone;
	gone.exists = false;
	gone.player = true;
	CHECK(OwnGear::Judge(Change::kStorageUndo, gone) == Verdict::kGone);
	CHECK(OwnGear::Name(Change::kStorageUndo) == "storageUndo");
	// Every other change still never touches our player.
	for (const auto change : ALL) {
		CHECK(OwnGear::Judge(change, Player()) == Verdict::kOurPlayer);
	}
}

TEST("own gear: a gift's copy goes in and out of a container only, and our player's take of one is the one change to them")
{
	Target chest;
	chest.container = true;
	CHECK(OwnGear::Judge(Change::kGiftCopy, chest) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kGiftCopy, Target{}) == Verdict::kWrongActor);  // an NPC's inventory, or anything not a container
	CHECK(OwnGear::Judge(Change::kGiftCopy, StandIn()) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kGiftCopy, Mirrored()) == Verdict::kWrongActor);
	auto odd = chest;
	odd.standIn = true;
	CHECK(OwnGear::Judge(Change::kGiftCopy, odd) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kGiftCopy, Player()) == Verdict::kOurPlayer);
	CHECK(OwnGear::Judge(Change::kGiftTake, Player()) == Verdict::kGo);
	CHECK(OwnGear::Judge(Change::kGiftTake, chest) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kGiftTake, StandIn()) == Verdict::kWrongActor);
	CHECK(OwnGear::Judge(Change::kGiftTake, Mirrored()) == Verdict::kWrongActor);
	CHECK(OwnGear::Name(Change::kGiftCopy) == "giftCopy");
	CHECK(OwnGear::Name(Change::kGiftTake) == "giftTake");
	for (const auto change : ALL) {
		CHECK(OwnGear::Judge(change, Player()) == Verdict::kOurPlayer);
	}
}
