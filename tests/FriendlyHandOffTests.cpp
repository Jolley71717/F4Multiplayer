// A friendly NPC is handed to a friend standing next to it, well before the distance hand-off (FriendlyHandOff.h, as
// NpcSync::Scan uses it): what the facts must be, the DWELL and SETTLED holds, and two games played against each other
// scan by scan, counting its changes of hands as NpcHands does, so it neither stays a statue nor trades back and forth.

#include "Test.h"

#include "game/FriendlyHandOff.h"
#include "game/NpcHands.h"

using namespace std::chrono_literals;

namespace
{
	using FriendlyHandOff::Why;
	using Clock = FriendlyHandOff::Clock;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t DIAMOND_CITY = 0x0001F4DC;  // an interior, for "another space"
	constexpr std::uint32_t VENDOR = 0x0002F1C4;
	constexpr std::uint32_t GUARD = 0x00095E8A;

	FriendlyHandOff::Spot Outside(float a_x, float a_y = 0.0f)
	{
		return { 0, COMMONWEALTH, a_x, a_y, 0.0f };
	}

	// A vendor at the origin our game has run a while, with us 3000 units off and a friend 300 from it.
	FriendlyHandOff::Npc Vendor()
	{
		return { .at = Outside(0.0f), .ranFor = 60s };
	}
	const FriendlyHandOff::Spot                 US = Outside(3000.0f);
	const std::vector<FriendlyHandOff::Friend> BOB_NEAR{ { Outside(0.0f, 300.0f) } };
}

TEST("friendly hand-off: a friendly NPC a friend stands next to goes to them once we are over 2000 units off")
{
	CHECK(FriendlyHandOff::Judge(Vendor(), US, BOB_NEAR) == Why::kRelease);
	// We are near it: our game's AI greets us, so it stays.
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(1999.0f), BOB_NEAR) == Why::kNear);
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(2000.0f), BOB_NEAR) == Why::kNear);
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(2001.0f), { { Outside(0.0f, 600.0f) } }) == Why::kRelease);
	// The friend must be within 800 units of it.
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(5000.0f), { { Outside(800.0f) } }) == Why::kRelease);
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(5000.0f), { { Outside(801.0f) } }) == Why::kNoFriend);
	// And at most a third as far as we are.
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(2100.0f), { { Outside(700.0f) } }) == Why::kRelease);
	CHECK(FriendlyHandOff::Judge(Vendor(), Outside(2100.0f), { { Outside(701.0f) } }) == Why::kNoFriend);
	// Any one friend near will do.
	CHECK(FriendlyHandOff::Judge(Vendor(), US, { { Outside(90000.0f) }, { Outside(0.0f, -200.0f) } }) == Why::kRelease);
}

TEST("friendly hand-off: no friend, one in another space, one in a menu that stops their game, or one it is hostile to")
{
	CHECK(FriendlyHandOff::Judge(Vendor(), US, {}) == Why::kNoFriend);
	// The same coordinates in an interior are nowhere near it.
	CHECK(FriendlyHandOff::Judge(Vendor(), US, { { { DIAMOND_CITY, 0, 0.0f, 300.0f, 0.0f } } }) == Why::kNoFriend);
	for (const auto menu : { Protocol::MenuKind::kPipboy, Protocol::MenuKind::kPaused, Protocol::MenuKind::kLooting, Protocol::MenuKind::kTerminal }) {
		CHECK(FriendlyHandOff::Judge(Vendor(), US, { { Outside(0.0f, 300.0f), menu } }) == Why::kNoFriend);
	}
	// A friend talking (to it, perhaps) or building runs on.
	for (const auto menu : { Protocol::MenuKind::kTalking, Protocol::MenuKind::kCrafting }) {
		CHECK(FriendlyHandOff::Judge(Vendor(), US, { { Outside(0.0f, 300.0f), menu } }) == Why::kRelease);
	}
	CHECK(FriendlyHandOff::Judge(Vendor(), US, { { Outside(0.0f, 300.0f), Protocol::MenuKind::kNone, true } }) == Why::kNoFriend);
}

TEST("friendly hand-off: we in another space than the NPC count as far from it")
{
	// Our player went indoors while the market's vendor is still loaded and ours.
	CHECK(FriendlyHandOff::Judge(Vendor(), { DIAMOND_CITY, 0, 0.0f, 0.0f, 0.0f }, BOB_NEAR) == Why::kRelease);
}

TEST("friendly hand-off: hostile, fighting, ours, fought lately, let go lately or just taken on: it stays")
{
	auto npc = Vendor();
	npc.hostileToUs = true;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kHostile);
	npc = Vendor();
	npc.inCombat = true;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kHostile);
	npc = Vendor();
	npc.ours = true;  // our companion, in our conversation or scene, a settler we command
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kOurs);
	npc = Vendor();
	npc.fought = true;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kOurs);
	npc = Vendor();
	npc.handedOffLately = true;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kLately);
	npc = Vendor();
	npc.ranFor = FriendlyHandOff::SETTLED - 1s;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kLately);
	npc.ranFor = FriendlyHandOff::SETTLED;
	CHECK(FriendlyHandOff::Judge(npc, US, BOB_NEAR) == Why::kRelease);
}

TEST("friendly hand-off: the facts must hold for DWELL without a break; a friend walking past moves nothing")
{
	FriendlyHandOff::Book book;
	const auto            t0 = Clock::now();
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0) == Why::kWaiting);
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0 + 2s) == Why::kWaiting);
	// The friend walked on: the watch starts over.
	CHECK(book.Decide(VENDOR, Vendor(), US, {}, t0 + 2500ms) == Why::kNoFriend);
	CHECK(book.Watched() == 0);
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0 + 3s) == Why::kWaiting);
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0 + 5s) == Why::kWaiting);
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0 + 3s + FriendlyHandOff::DWELL) == Why::kRelease);
	CHECK(book.Watched() == 0);  // released: a watch is for one we run

	// One unloaded mid-watch is dropped after FORGET_AFTER; Forget drops it at once.
	CHECK(book.Decide(GUARD, Vendor(), US, BOB_NEAR, t0) == Why::kWaiting);
	CHECK(book.Decide(VENDOR, Vendor(), US, BOB_NEAR, t0) == Why::kWaiting);
	book.Expire(t0 + FriendlyHandOff::FORGET_AFTER - 1s);
	CHECK(book.Watched() == 2);
	book.Forget(VENDOR);
	CHECK(book.Watched() == 1);
	book.Expire(t0 + FriendlyHandOff::FORGET_AFTER);
	CHECK(book.Watched() == 0);
}

namespace
{
	// Two games and one vendor, scan by scan (NpcSync's 250 ms), as far as who runs it goes: the runner's Book decides,
	// a release is claimed by the other game on its next scan (the hand-off's HANDOFF_HOLD keeps the releasing one from
	// claiming it back), and NpcHands counts the changes as NpcSync::ApplyOwners notes them. A hand-off is a release
	// and a claim within the second, so NpcHands counts each as a quick change (`hands=` changes/quick): what tells a
	// settled NPC from one traded back and forth is the time each runner keeps it, the shortest kept here.
	struct Market
	{
		static constexpr std::uint32_t HOST = 1;
		static constexpr std::uint32_t CLIENT = 2;

		FriendlyHandOff::Book books[2];
		NpcHands::Book        hands;
		std::uint32_t         runner = HOST;
		Clock::time_point     ranSince;
		Clock::time_point     lastHandOff{};  // the releasing game's handedOffAt
		std::uint32_t         lastFrom = 0;
		Clock::time_point     now;
		std::uint32_t         handOffs = 0;
		Clock::duration       shortestRun = Clock::duration::max();  // between one claim and the next

		explicit Market(Clock::time_point a_t0) :
			ranSince(a_t0),
			now(a_t0)
		{
			hands.Note(VENDOR, HOST, a_t0);
		}

		// One scan with the host and the client where they are now, the vendor at the origin.
		void Scan(const FriendlyHandOff::Spot& a_host, const FriendlyHandOff::Spot& a_client)
		{
			if (runner == 0) {
				// Released last scan: the other game claims it.
				runner = lastFrom == HOST ? CLIENT : HOST;
				hands.Note(VENDOR, runner, now);
				if (handOffs > 1) {
					shortestRun = (std::min)(shortestRun, now - ranSince);
				}
				ranSince = now;
			} else {
				const auto& us = runner == HOST ? a_host : a_client;
				const auto& them = runner == HOST ? a_client : a_host;
				auto        npc = Vendor();
				npc.ranFor = now - ranSince;
				npc.handedOffLately = lastFrom == runner && now - lastHandOff < 30s;
				if (books[runner - 1].Decide(VENDOR, npc, us, { { them } }, now) == Why::kRelease) {
					lastFrom = runner;
					lastHandOff = now;
					runner = 0;
					++handOffs;
					hands.Note(VENDOR, 0, now);
				}
			}
			now += 250ms;
		}

		void For(Clock::duration a_time, const FriendlyHandOff::Spot& a_host, const FriendlyHandOff::Spot& a_client)
		{
			for (const auto end = now + a_time; now < end;) {
				Scan(a_host, a_client);
			}
		}
	};
}

TEST("friendly hand-off: the host across the market and the client at the stall: one change of hands, and it stays")
{
	Market m{ Clock::now() };
	// Both just arrived: the host's game took the vendor on. The host shops 3000 units off, the client stands at the stall.
	m.For(5min, Outside(3000.0f), Outside(0.0f, 200.0f));
	CHECK(m.runner == Market::CLIENT);
	CHECK(m.handOffs == 1);
	CHECK(m.hands.Changes() == 1);
	CHECK(m.hands.Quick() == 1);  // its claim, within the second of the host's release
	// The host strolls between 1000 and 2500 units off, the client between 100 and 900: nobody in reach of taking it back.
	for (int i = 0; i < 20; ++i) {
		m.For(7s, Outside(1000.0f + 75.0f * static_cast<float>(i)), Outside(0.0f, 100.0f + 40.0f * static_cast<float>(i)));
	}
	CHECK(m.runner == Market::CLIENT);
	CHECK(m.hands.Changes() == 1);
}

TEST("friendly hand-off: within 2000 units of it, the host keeps it; the client beside it changes nothing")
{
	Market m{ Clock::now() };
	m.For(5min, Outside(1800.0f), Outside(0.0f, 100.0f));
	CHECK(m.runner == Market::HOST);
	CHECK(m.hands.Changes() == 0);
}

TEST("friendly hand-off: players trading places every few seconds trade it at most once per SETTLED")
{
	Market     m{ Clock::now() };
	const auto near = Outside(0.0f, 300.0f);
	const auto far = Outside(3000.0f);
	for (int i = 0; i < 60; ++i) {  // 10 minutes
		m.For(5s, near, far);
		m.For(5s, far, near);
	}
	CHECK(m.handOffs >= 2);
	CHECK(m.hands.Changes() == m.handOffs);
	CHECK(m.handOffs <= 600 / 20);
	// Each runner kept it at least SETTLED and DWELL, never a scan or two.
	CHECK(m.shortestRun >= FriendlyHandOff::SETTLED + FriendlyHandOff::DWELL);
}
