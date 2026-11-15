// Who is in VATS, against a real server (common/TimeScaleBook.h in Server.cpp): a joiner hears the host's group VATS
// speed after its welcome, a game's report goes to the others (not back to it) only when it changes something, and a
// player who leaves is gone from the table the others hear.

#include "ServerFixture.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	auto Report(std::uint8_t a_flags, float a_multiplier, float a_playerFactor = 1.0f) { return Protocol::Encode(Protocol::TimeScale{ 0, a_flags, a_multiplier, a_playerFactor }); }

	std::vector<Protocol::TimeScales> Tables(Fixture& a_f, PeerId a_peer)
	{
		return Decoded(Received(a_f, a_peer), MT::kTimeScales, [](auto a_data) { return Protocol::DecodeTimeScales(a_data); });
	}

	Server::Options Grouped(float a_speed)
	{
		Server::Options options;
		options.groupVatsSpeed = a_speed;
		return options;
	}
}

TEST("server time scales: a joiner hears the host's group VATS speed after its welcome, off by default")
{
	{
		Fixture f;
		JoinAs(f, 1, "Host", 0x11);
		const auto tables = Tables(f, 1);
		REQUIRE(tables.size() == 1);
		CHECK(tables[0].groupSpeed == 0.0f);
		CHECK(tables[0].players.empty());
	}
	{
		Fixture f{ Grouped(0.3f) };
		CHECK(f.LoggedSoon("server: group VATS slow motion on: while anyone is in VATS, every game runs at x0.30"));
		JoinAs(f, 1, "Host", 0x11);
		const auto tables = Tables(f, 1);
		REQUIRE(tables.size() == 1);
		CHECK(tables[0].groupSpeed == 0.3f);
	}
	{
		Fixture f{ Grouped(0.001f) };  // all but a stop: held to the least
		JoinAs(f, 1, "Host", 0x11);
		const auto tables = Tables(f, 1);
		REQUIRE(tables.size() == 1);
		CHECK(tables[0].groupSpeed == Protocol::MIN_GROUP_VATS_SPEED);
	}
}

TEST("server time scales: a report goes to the others when it changes something, and one who leaves is gone from the table")
{
	Fixture    f{ Grouped(0.3f) };
	const auto host = JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	const auto carol = JoinAs(f, 3, "Carol", 0x33);
	Clear(f, { 1, 2, 3 });

	f.fake->Deliver(2, Report(Protocol::kTimeVats | Protocol::kTimeSlowed, 0.3f));
	for (const PeerId peer : { 1, 3 }) {
		const auto tables = Tables(f, peer);
		REQUIRE(tables.size() == 1);
		CHECK(tables[0].groupSpeed == 0.3f);
		REQUIRE(tables[0].players.size() == 1);
		CHECK(tables[0].players[0].playerId == bob);
		CHECK(tables[0].players[0].flags == (Protocol::kTimeVats | Protocol::kTimeSlowed));
	}
	CHECK(Tables(f, 2).empty());  // not back to the one who said it
	CHECK(f.LoggedSoon(std::format("server: player {} in VATS (x0.30; 1 in VATS, group slow motion on)", bob)));

	// The same again: nobody hears it.
	f.fake->Deliver(2, Report(Protocol::kTimeVats | Protocol::kTimeSlowed, 0.3f));
	CHECK(Tables(f, 1).empty());

	// The host's game slows with it and says so: Bob and Carol hear both rows.
	f.fake->Deliver(1, Report(Protocol::kTimeSlowed, 0.3f));
	auto tables = Tables(f, 3);
	REQUIRE(tables.size() == 1);
	CHECK(tables[0].players.size() == 2);
	Clear(f, { 1, 2, 3 });

	// A nonsense report is dropped.
	f.fake->Deliver(1, Report(0x80, 0.3f));
	CHECK(Tables(f, 3).empty());

	// Bob leaves in VATS: the others hear the table without him.
	f.fake->Disconnect(2);
	CHECK(f.LoggedSoon(std::format("server: player {} left", bob)));
	tables = Tables(f, 3);
	REQUIRE(!tables.empty());
	REQUIRE(tables.back().players.size() == 1);
	CHECK(tables.back().players[0].playerId == host);
	(void)carol;
}

TEST("server time scales: the pace a game's own player keeps goes to the others with its row, and a change of it alone is told")
{
	Fixture f;
	JoinAs(f, 1, "Host", 0x11);
	const auto bob = JoinAs(f, 2, "Bob", 0x22);
	Clear(f, { 1, 2 });

	// On Jet: the world at 0.3, the one who took it at twice that.
	f.fake->Deliver(2, Report(Protocol::kTimeSlowed, 0.3f, 2.0f));
	auto tables = Tables(f, 1);
	REQUIRE(tables.size() == 1);
	REQUIRE(tables[0].players.size() == 1);
	CHECK(tables[0].players[0].playerId == bob);
	CHECK(tables[0].players[0].flags == Protocol::kTimeSlowed);
	CHECK(tables[0].players[0].multiplier == 0.3f);
	CHECK(tables[0].players[0].playerFactor == 2.0f);

	// The same again: nobody hears it. Their player's pace alone changes: told.
	f.fake->Deliver(2, Report(Protocol::kTimeSlowed, 0.3f, 2.0f));
	CHECK(Tables(f, 1).empty());
	f.fake->Deliver(2, Report(Protocol::kTimeSlowed, 0.3f, 2.5f));
	tables = Tables(f, 1);
	REQUIRE(tables.size() == 1);
	REQUIRE(tables[0].players.size() == 1);
	CHECK(tables[0].players[0].playerFactor == 2.5f);

	// A pace that is no number is dropped: the table stays.
	f.fake->Deliver(2, Report(Protocol::kTimeSlowed, 0.3f, -1.0f));
	CHECK(Tables(f, 1).empty());

	// One who joins now hears it with the welcome's table.
	JoinAs(f, 3, "Carol", 0x33);
	const auto joined = Tables(f, 3);
	REQUIRE(joined.size() == 1);
	REQUIRE(joined[0].players.size() == 1);
	CHECK(joined[0].players[0].playerId == bob);
	CHECK(joined[0].players[0].playerFactor == 2.5f);
}
