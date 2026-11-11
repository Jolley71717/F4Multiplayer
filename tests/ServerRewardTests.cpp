// Shared quest rewards played against a real server (common/RewardBook.h in Server.cpp): a reward one game reports is sent
// once to every other game there now, and to one that joins later with its welcome; the same turn-in from another player
// at once is refused, and nobody's game is sent its own.

#include "ServerFixture.h"

#include "RewardBook.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Protocol::QuestReward;
	using Protocol::RewardKind;

	constexpr std::uint32_t CAPS = 0x0000000F;
	constexpr std::uint32_t QUEST = 0x000A8258;
	constexpr std::uint32_t INFO = 0x000D60BB;

	std::vector<std::uint8_t> Report(RewardKind a_kind, std::uint32_t a_source, std::int32_t a_caps, std::uint32_t a_xp = 0)
	{
		QuestReward reward{ 0, 0, a_kind, QUEST, a_source, a_xp, {} };
		if (a_caps > 0) {
			reward.items.push_back({ CAPS, a_caps });
		}
		return Protocol::Encode(reward);
	}

	std::vector<QuestReward> Rewards(Fixture& a_f, PeerId a_peer)
	{
		return Decoded(Received(a_f, a_peer), MT::kQuestReward, [](auto a_data) { return Protocol::DecodeQuestReward(a_data); });
	}
}

TEST("server rewards: a turn-in's reward is sent to every other game, numbered and named by who turned it in")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	JoinAs(f, 2, "Bob", 0x22);
	JoinAs(f, 3, "Cait", 0x33);
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(1, Report(RewardKind::kLine, INFO, 500));
	CHECK(Rewards(f, 1).empty());  // never the giver's own
	for (const PeerId peer : { 2, 3 }) {
		const auto got = Rewards(f, peer);
		REQUIRE(got.size() == 1);
		CHECK(got[0].id != 0);
		CHECK(got[0].playerId == host);
		CHECK(got[0].kind == RewardKind::kLine);
		CHECK(got[0].quest == QUEST);
		CHECK(got[0].source == INFO);
		REQUIRE(got[0].items.size() == 1);
		CHECK(got[0].items[0].form == CAPS);
		CHECK(got[0].items[0].count == 500);
	}
	CHECK(f.LoggedSoon(std::format("server: player {} turned in {:08X} (line {})", host, QUEST, INFO)));
}

TEST("server rewards: the same turn-in from another player at once is refused, and a repeat too")
{
	Fixture f;
	f.HoldClock();
	JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, Report(RewardKind::kStage, 30, 50, 150));
	CHECK(Rewards(f, 2).size() == 1);
	f.Advance(10s);
	f.fake->Deliver(2, Report(RewardKind::kStage, 30, 50, 150));  // Bob turned it in too, at the same time: his game gave his own
	CHECK(Rewards(f, 1).empty());
	CHECK(f.LoggedSoon(std::format("server: player {}'s reward ({:08X} stage 30) refused: another player turned the same in just before", bob, QUEST)));
	f.fake->Deliver(1, Report(RewardKind::kStage, 30, 50, 150));
	CHECK(Rewards(f, 2).empty());
	CHECK(f.LoggedSoon("refused: they reported it already"));

	// Past the window, the same stage is another turn-in (a repeatable quest's).
	f.Advance(RewardBook::SAME_TURN_IN);
	f.fake->Deliver(2, Report(RewardKind::kStage, 30, 50, 150));
	CHECK(Rewards(f, 1).size() == 1);
}

TEST("server rewards: one who joins later, or comes back, gets what was turned in while away, once")
{
	Fixture    f;
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	// Bob is away when the host turns in a line; Cait isn't here yet.
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	f.fake->Deliver(1, Report(RewardKind::kLine, INFO, 500));
	Clear(f, { 1 });

	JoinAs(f, 3, "Bob", 0x22);
	const auto bobGot = Rewards(f, 3);
	REQUIRE(bobGot.size() == 1);
	CHECK(bobGot[0].playerId == host);
	CHECK(bobGot[0].source == INFO);
	const auto cait = JoinAs(f, 4, "Cait", 0x33);
	REQUIRE(Rewards(f, 4).size() == 1);
	CHECK(f.LoggedSoon(std::format("server: player {} is sent reward", cait)));

	// Back again: had already.
	f.fake->Disconnect(4);
	CHECK(f.LoggedSoon(std::format("server: player {} left", cait)));
	JoinAs(f, 5, "Cait", 0x33);
	CHECK(Rewards(f, 5).empty());
	// The host, coming back, is never sent its own.
	f.fake->Disconnect(1);
	CHECK(f.LoggedSoon(std::format("server: player {} left", host)));
	JoinAs(f, 6, "Host", 0x11);
	CHECK(Rewards(f, 6).empty());
}

TEST("server rewards: a bad report is dropped, and nothing is sent")
{
	Fixture f;
	JoinAs(f, 1, "Host", 0x11);
	JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });
	f.fake->Deliver(1, Protocol::Encode(QuestReward{ 0, 0, RewardKind::kLine, QUEST, INFO, 0, {} }));  // nothing in it
	f.fake->Deliver(1, Protocol::Encode(QuestReward{ 0, 0, RewardKind::kStage, QUEST, 0x10000, 10, {} }));  // no such stage
	f.fake->Deliver(1, Protocol::Encode(QuestReward{ 0, 0, RewardKind::kLine, 0, INFO, 10, {} }));  // no quest
	CHECK(Rewards(f, 2).empty());
}
