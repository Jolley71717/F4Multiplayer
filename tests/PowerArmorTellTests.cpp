// A friend's power armor as it travels: what the wearer's game says it sits in, through the real server, to what each
// friend's game makes of it. The wearer's game logs a wear whose frame it never knew (ArmorFrames::UnknownFrame), each
// hearer one whose frame it cannot place (ArmorFrames::MissingFrame), so a power armor glitch leaves a line on both sides.

#include "ServerFixture.h"
#include "game/ArmorFrames.h"

#include <chrono>
#include <map>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	constexpr std::uint32_t VANILLA_FRAME = 0x0017F6E4;  // a frame every game has
	constexpr std::uint32_t PLUGIN_FRAME = 0x0A001F2C;   // a frame from a plugin only some of the friends have

	Protocol::PlayerStatus Wearing(bool a_on, std::uint32_t a_frame, std::uint8_t a_health = 100)
	{
		Protocol::PlayerStatus status;
		status.location = 0x1000;
		status.level = 12;
		status.health = a_health;
		status.powerArmor = a_on;
		status.powerArmorFrame = a_on ? a_frame : 0;
		return status;
	}

	// One friend's game, hearing statuses: the frames it has, and a watch per player as RemotePlayers keeps one.
	struct Hearer
	{
		PeerId                                      peer;
		std::set<std::uint32_t>                     framesHere;
		std::map<std::uint32_t, ArmorFrames::MissingFrame> watches;
		int                                         told = 0;

		// Takes the next status the server sent this friend, and returns whose it was.
		std::uint32_t HearNext(Fixture& a_f)
		{
			const auto packet = a_f.fake->WaitFor(peer, Type(MT::kPlayerStatus));
			REQUIRE(packet);
			const auto status = Protocol::DecodePlayerStatus(*packet);
			REQUIRE(status);
			told += watches[status->playerId].Update(status->powerArmor, status->powerArmorFrame, framesHere.contains(status->powerArmorFrame));
			return status->playerId;
		}

		bool HearsNothing(Fixture& a_f) { return !a_f.fake->WaitFor(peer, Type(MT::kPlayerStatus), 200ms); }
	};
}

TEST("power armor tell: a wear whose frame is unknown or not in a friend's game is logged once on each side, through the server, not once a status")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	Hearer bob{ .peer = 2, .framesHere = { VANILLA_FRAME } };
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));

	// Alice's game: in power armor, sitting in something it does not take for a frame. Its own log says so once,
	// a second into the wear; Bob's says so as soon as he hears it.
	ArmorFrames::UnknownFrame wearer;
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(!wearer.Update(true, 0, t0));
	f.fake->Deliver(1, Protocol::Encode(Wearing(true, 0), MT::kReportStatus));
	CHECK(bob.HearNext(f) == alice->playerId);
	CHECK(bob.told == 1);
	CHECK(!wearer.Update(true, 0, t0 + 500ms));
	CHECK(wearer.Update(true, 0, t0 + 1100ms));
	CHECK(!wearer.Update(true, 0, t0 + 3s));

	// Her status is sent again as her health changes: nothing new in either log.
	f.fake->Deliver(1, Protocol::Encode(Wearing(true, 0, 80), MT::kReportStatus));
	bob.HearNext(f);
	f.fake->Deliver(1, Protocol::Encode(Wearing(true, 0, 60), MT::kReportStatus));
	bob.HearNext(f);
	CHECK(bob.told == 1);

	// Out, and into a frame both games have: nobody logs anything, however long she wears it.
	f.fake->Deliver(1, Protocol::Encode(Wearing(false, 0), MT::kReportStatus));
	bob.HearNext(f);
	CHECK(!wearer.Update(false, 0, t0 + 4s));
	f.fake->Deliver(1, Protocol::Encode(Wearing(true, VANILLA_FRAME), MT::kReportStatus));
	bob.HearNext(f);
	CHECK(!wearer.Update(true, VANILLA_FRAME, t0 + 5s));
	CHECK(!wearer.Update(true, VANILLA_FRAME, t0 + 9s));
	CHECK(bob.told == 1);

	// Out, and into a plugin's frame Bob's game lacks: his log says so once; hers says nothing (she knows her frame).
	f.fake->Deliver(1, Protocol::Encode(Wearing(false, 0), MT::kReportStatus));
	bob.HearNext(f);
	f.fake->Deliver(1, Protocol::Encode(Wearing(true, PLUGIN_FRAME), MT::kReportStatus));
	bob.HearNext(f);
	CHECK(bob.told == 2);
	CHECK(!wearer.Update(false, 0, t0 + 10s));
	CHECK(!wearer.Update(true, PLUGIN_FRAME, t0 + 10s));
	CHECK(!wearer.Update(true, PLUGIN_FRAME, t0 + 12s));

	// Two friends join late: the server tells each Alice's last status. The one with the plugin logs nothing; the one
	// without it logs it once, as Bob did.
	Hearer carol{ .peer = 3, .framesHere = { VANILLA_FRAME, PLUGIN_FRAME } };
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	Hearer dave{ .peer = 4, .framesHere = { VANILLA_FRAME } };
	REQUIRE(f.Join(4, Fixture::MakeHello("Dave")));
	bool carolHeardAlice = false;
	bool daveHeardAlice = false;
	for (int i = 0; i < 4 && !(carolHeardAlice && daveHeardAlice); ++i) {
		if (!carolHeardAlice) {
			carolHeardAlice = carol.HearNext(f) == alice->playerId;
		}
		if (!daveHeardAlice) {
			daveHeardAlice = dave.HearNext(f) == alice->playerId;
		}
	}
	CHECK(carolHeardAlice);
	CHECK(daveHeardAlice);
	CHECK(carol.told == 0);
	CHECK(dave.told == 1);
}

TEST("power armor tell: a status naming a frame made in the sender's own game never reaches a friend, and the next good one does")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	Hearer alice{ .peer = 1, .framesHere = { VANILLA_FRAME } };
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);

	// Bob's game names the frame reference it made itself: an id that names another reference, or none, in Alice's.
	auto status = Wearing(true, VANILLA_FRAME);
	status.powerArmorFrameRef = 0xFF000123;
	f.fake->Deliver(2, Protocol::Encode(status, MT::kReportStatus));
	CHECK(alice.HearsNothing(f));
	CHECK(alice.told == 0);

	// His next status, of a plugin frame Alice lacks, gets through and is logged once.
	f.fake->Deliver(2, Protocol::Encode(Wearing(true, PLUGIN_FRAME), MT::kReportStatus));
	CHECK(alice.HearNext(f) == bob->playerId);
	CHECK(alice.told == 1);
}

TEST("power armor tell: a friend who leaves and joins again in the same armor is logged again, for their new stand-in")
{
	Fixture f;
	Hearer alice{ .peer = 1, .framesHere = { VANILLA_FRAME } };
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob", 0xB0B)));
	f.fake->Deliver(2, Protocol::Encode(Wearing(true, PLUGIN_FRAME), MT::kReportStatus));
	alice.HearNext(f);
	CHECK(alice.told == 1);

	// Bob's game drops out; Alice's forgets him (RemotePlayers::Remove), and he comes back in the same suit.
	f.fake->Drop(2);
	const auto left = f.fake->WaitFor(1, Type(MT::kPlayerLeft));
	REQUIRE(left);
	const auto gone = Protocol::DecodePlayerLeft(*left);
	REQUIRE(gone);
	alice.watches.erase(gone->playerId);
	REQUIRE(f.Join(5, Fixture::MakeHello("Bob", 0xB0B)));
	f.fake->Deliver(5, Protocol::Encode(Wearing(true, PLUGIN_FRAME), MT::kReportStatus));
	alice.HearNext(f);
	CHECK(alice.told == 2);
}
