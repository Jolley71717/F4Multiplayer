// An NPC our game hands to a friend's (HandOffClaim.h, as NpcSync uses it): how long ours waits for their claim, what
// it does with one that comes back unclaimed, and an owner's game played scan by scan against a friend's that cannot
// claim the NPC (it has no reference of it, only a copy of ours: the road encounter's Mr Gutsy), counting the hand-offs
// and the time nobody runs it.

#include "Test.h"

#include "game/HandOffClaim.h"
#include "game/SpawnBook.h"

using namespace std::chrono_literals;

namespace
{
	using HandOffClaim::Kind;
	using Clock = HandOffClaim::Clock;

	constexpr std::uint32_t GUTSY = 0x0002F61D;
	constexpr std::uint32_t GUARD = 0x00095E8A;
	const auto              T0 = Clock::time_point{} + 1h;
}

TEST("hand-off claim: our game waits CLAIM_WAIT for the friend's claim, then the NPC is ours to claim back")
{
	HandOffClaim::Book book;
	CHECK(!book.Waits(GUTSY, T0));
	CHECK(!book.Lately(GUTSY, T0));
	book.HandedOff(GUTSY, Kind::kNearer, T0);
	CHECK(book.Waits(GUTSY, T0));
	CHECK(book.Waits(GUTSY, T0 + HandOffClaim::CLAIM_WAIT - 1ms));
	CHECK(!book.Waits(GUTSY, T0 + HandOffClaim::CLAIM_WAIT));
	// Not handed off again within RETRY, whoever runs it meanwhile.
	CHECK(book.Lately(GUTSY, T0 + HandOffClaim::RETRY - 1ms));
	CHECK(!book.Lately(GUTSY, T0 + HandOffClaim::RETRY));
	// Another NPC is not waited for.
	CHECK(!book.Waits(GUARD, T0));
	// Their game claims well within the wait (it looks four times a second).
	CHECK(HandOffClaim::CLAIM_WAIT >= 1s);
	CHECK(HandOffClaim::CLAIM_WAIT < HandOffClaim::RETRY);
}

TEST("hand-off claim: one a friend's game took is theirs, and one that comes back later is no refusal")
{
	HandOffClaim::Book book;
	book.HandedOff(GUARD, Kind::kFriendly, T0);
	book.Theirs(GUARD);  // the session names the friend its runner
	CHECK(!book.BackedOff(GUARD));
	// They hand it back to us ten seconds on: ours again, not backed off, and still not handed off again within RETRY.
	CHECK(!book.Ours(GUARD, T0 + 10s));
	CHECK(!book.BackedOff(GUARD));
	CHECK(book.Lately(GUARD, T0 + 10s));
	book.Expire(T0 + HandOffClaim::RETRY);
	CHECK(!book.Lately(GUARD, T0 + HandOffClaim::RETRY));
}

TEST("hand-off claim: one that comes back to us unclaimed is not handed off again for BACKOFF, whichever hand-off let it go")
{
	for (const auto kind : { Kind::kNearer, Kind::kFriendly }) {
		HandOffClaim::Book book;
		book.HandedOff(GUTSY, kind, T0);
		// Nobody claimed it: ours takes it back after the wait, and the session names us its runner again.
		const auto back = book.Ours(GUTSY, T0 + 2300ms);
		REQUIRE(back.has_value());
		CHECK(*back == kind);
		CHECK(book.BackedOff(GUTSY));
		CHECK(book.BackedOffCount() == 1);
		// Told once: the session naming us again is no second refusal.
		CHECK(!book.Ours(GUTSY, T0 + 2400ms));
		book.Expire(T0 + 2300ms + HandOffClaim::BACKOFF - 1ms);
		CHECK(book.BackedOff(GUTSY));
		book.Expire(T0 + 2300ms + HandOffClaim::BACKOFF);
		CHECK(!book.BackedOff(GUTSY));
		CHECK(book.BackedOffCount() == 0);
	}
}

TEST("hand-off claim: a game stopped by a menu waits the same, but what comes back to it is not backed off")
{
	HandOffClaim::Book book;
	book.HandedOff(GUARD, Kind::kPaused, T0);
	CHECK(book.Waits(GUARD, T0 + 1s));
	CHECK(book.Lately(GUARD, T0 + 1s));
	// The menu closed and no friend had taken it: ours again, and free to go at the next menu or to a nearer player.
	CHECK(!book.Ours(GUARD, T0 + 5s));
	CHECK(!book.BackedOff(GUARD));
	// One backed off from a hand-off to a nearer player stays so through a menu.
	book.HandedOff(GUTSY, Kind::kNearer, T0);
	REQUIRE(book.Ours(GUTSY, T0 + 3s).has_value());
	book.HandedOff(GUTSY, Kind::kPaused, T0 + 40s);
	CHECK(!book.Ours(GUTSY, T0 + 50s));
	CHECK(book.BackedOff(GUTSY));
	book.Clear();
	CHECK(!book.BackedOff(GUTSY));
	CHECK(!book.Lately(GUTSY, T0 + 50s));
}

namespace
{
	// The owner's game and one NPC, scan by scan (NpcSync's 250 ms), with a friend much nearer to it the whole time
	// (ShouldHandOff's facts hold at every scan) whose game either claims what is nobody's on its next scan, or never
	// (it has no reference of the NPC). The session answers a claim within the scan.
	struct Road
	{
		static constexpr std::uint32_t OWNER = 1;
		static constexpr std::uint32_t FRIEND = 2;

		HandOffClaim::Book book;
		std::uint32_t      runner = OWNER;
		Clock::time_point  now = T0;
		std::uint32_t      handOffs = 0;
		std::uint32_t      cameBack = 0;
		Clock::duration    unrun{};         // the time nobody ran it, all told
		Clock::duration    longestUnrun{};  // and the longest stretch of it
		Clock::time_point  unrunSince{};

		void Scan(bool a_friendClaims)
		{
			book.Expire(now);
			if (runner == OWNER) {
				if (!book.Lately(GUTSY, now) && !book.BackedOff(GUTSY)) {
					book.HandedOff(GUTSY, Kind::kNearer, now);
					runner = 0;
					unrunSince = now;
					++handOffs;
				}
			} else if (runner == 0) {
				if (a_friendClaims) {
					runner = FRIEND;
					book.Theirs(GUTSY);
				} else if (!book.Waits(GUTSY, now)) {
					runner = OWNER;
					if (book.Ours(GUTSY, now)) {
						++cameBack;
					}
				}
				if (runner != 0) {
					unrun += now - unrunSince;
					longestUnrun = (std::max)(longestUnrun, now - unrunSince);
				}
			}
			now += 250ms;
		}

		void Play(Clock::duration a_for, bool a_friendClaims)
		{
			for (const auto until = now + a_for; now < until;) {
				Scan(a_friendClaims);
			}
		}
	};
}

TEST("hand-off claim: an NPC a friend's game cannot claim is handed off once in five minutes, and stands still for little over CLAIM_WAIT")
{
	// The 0.9.2 road encounter: the joiner's game runs a Mr Gutsy the host's has no reference of, the joiner 7000 units
	// from it and the host 5000. Before, the joiner's game let go of it every 30 s and took it back 10 s later: it
	// stood still a third of the time, and the host's copy of it went and was placed again each time.
	Road road;
	road.Play(HandOffClaim::BACKOFF - 1s, false);
	CHECK(road.handOffs == 1);
	CHECK(road.cameBack == 1);
	CHECK(road.runner == Road::OWNER);
	CHECK(road.longestUnrun >= HandOffClaim::CLAIM_WAIT);
	CHECK(road.longestUnrun <= HandOffClaim::CLAIM_WAIT + 250ms);
	// The friend's copy of it is kept through that (SpawnBook::UNRUN_GRACE), with a scan and the way there to spare.
	CHECK(SpawnBook::UNRUN_GRACE >= road.longestUnrun + 1s);
	// Half an hour of it: tried again once each BACKOFF, never more.
	road.Play(30min, false);
	CHECK(road.handOffs <= 7);
	CHECK(road.handOffs == road.cameBack);
	CHECK(road.unrun <= road.handOffs * (HandOffClaim::CLAIM_WAIT + 250ms));
}

TEST("hand-off claim: an NPC a friend's game can claim goes to it within a scan, and nothing is backed off")
{
	Road road;
	road.Play(10s, true);
	CHECK(road.handOffs == 1);
	CHECK(road.cameBack == 0);
	CHECK(road.runner == Road::FRIEND);
	CHECK(road.longestUnrun <= 250ms);
	CHECK(!road.book.BackedOff(GUTSY));
}
