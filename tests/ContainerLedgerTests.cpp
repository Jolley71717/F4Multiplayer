// ContainerLedger's rules on their own (SessionLifeTests drives it with a real server).

#include "Test.h"

#include "game/ContainerLedger.h"

namespace
{
	constexpr std::uint32_t CHEST = 0x0002C001;
	constexpr std::uint32_t BOX = 0x0002C002;
	constexpr std::uint32_t ITEM = 0x00023736;

	struct World
	{
		std::vector<Protocol::ContainerChange> applied;
		bool                                   loaded = true;  // the containers are there

		auto TryApply()
		{
			return [this](const Protocol::ContainerChange& a_change) {
				if (!loaded) {
					return false;
				}
				applied.push_back(a_change);
				return true;
			};
		}
	};
}

TEST("container ledger: a world state that carries changes never lowers the count, an empty one where our count is past its end does")
{
	ContainerLedger ledger;
	ledger.OnRecordLoaded(0x5E55, 5, {});
	ledger.OnWelcome(0x5E55, 2);
	CHECK(!ledger.OnWorldState(3, true));  // a live change got ahead of what we asked for: the changes say which we lack
	CHECK(ledger.Next() == 5);
	CHECK(!ledger.OnWorldState(5, false));  // the table ends where we are
	CHECK(ledger.OnWorldState(4, false));   // it ends before: the server lost #4
	CHECK(ledger.Next() == 4);
}

TEST("container ledger: a welcome into another session than the save's record starts that session's count from the first")
{
	World           world;
	ContainerLedger ledger;
	ledger.OnRecordLoaded(0xAAAA, 7, { { 6, { CHEST, ITEM, -1 } } });
	ledger.OnWelcome(0xBBBB, 3);
	CHECK(ledger.ResyncPoint().sessionId == 0xBBBB);
	CHECK(ledger.Next() == 0);
	CHECK(ledger.Waiting().empty());
	CHECK(ledger.Offer(0, 1, { CHEST, ITEM, -1 }, world.TryApply()) == ContainerLedger::Offered::kApplied);
}

TEST("container ledger: our own changes are counted but not applied again, under any of our IDs this session")
{
	World           world;
	ContainerLedger ledger;
	ledger.OnWelcome(0x5E55, 2);
	ledger.OnDisconnected();
	ledger.OnWelcome(0x5E55, 4);  // came back while the server still held the old connection
	CHECK(ledger.Offer(0, 2, { CHEST, ITEM, -1 }, world.TryApply()) == ContainerLedger::Offered::kOurs);
	CHECK(ledger.Offer(1, 4, { CHEST, ITEM, -1 }, world.TryApply()) == ContainerLedger::Offered::kOurs);
	CHECK(ledger.Next() == 2);
	CHECK(world.applied.empty());
}

TEST("container ledger: a change to a container that isn't loaded waits, and later ones to it wait behind it, in order")
{
	World           world;
	ContainerLedger ledger;
	ledger.OnWelcome(0x5E55, 2);
	world.loaded = false;
	CHECK(ledger.Offer(0, 1, { CHEST, ITEM, -1 }, world.TryApply()) == ContainerLedger::Offered::kWaiting);
	world.loaded = true;
	CHECK(ledger.Offer(1, 1, { CHEST, ITEM, 5 }, world.TryApply()) == ContainerLedger::Offered::kWaiting);
	CHECK(ledger.Offer(2, 1, { BOX, ITEM, 1 }, world.TryApply()) == ContainerLedger::Offered::kApplied);
	ledger.Retry(world.TryApply());
	REQUIRE(world.applied.size() == 3);
	CHECK(world.applied[1].count == -1);
	CHECK(world.applied[2].count == 5);
	CHECK(ledger.Waiting().empty());
}

TEST("container ledger: a load with no session joined leaves the save's record as it is")
{
	ContainerLedger ledger;
	ledger.OnRecordLoaded(0x5E55, 3, {});
	CHECK(!ledger.BindToLiveSession());
	CHECK(ledger.ResyncPoint().sessionId == 0x5E55);
	CHECK(ledger.ResyncPoint().containerFrom == 3);
}
