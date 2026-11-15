// An NPC our game hands to a friend's (HandOffClaim.h, as NpcSync uses it): how long ours waits for their claim, what
// it does with one that comes back unclaimed (a strike against the player it was for, two in a row a back-off for that
// player), and an owner's game played scan by scan against a friend's that cannot claim the NPC (it has no reference
// of it, only a copy of ours: the road encounter's Mr Gutsy) or claims it late, counting the hand-offs and the time
// nobody runs it.

#include "Test.h"

#include "game/HandOffClaim.h"
#include "game/SpawnBook.h"

#include <optional>

using namespace std::chrono_literals;

namespace
{
	using HandOffClaim::Kind;
	using Clock = HandOffClaim::Clock;

	constexpr std::uint32_t GUTSY = 0x0002F61D;
	constexpr std::uint32_t GUARD = 0x00095E8A;
	constexpr std::uint32_t PLAYER_2 = 2;
	constexpr std::uint32_t PLAYER_3 = 3;
	const auto              T0 = Clock::time_point{} + 1h;
}

TEST("hand-off claim: our game waits CLAIM_WAIT for the friend's claim, then the NPC is ours to claim back")
{
	HandOffClaim::Book book;
	CHECK(!book.Waits(GUTSY, T0));
	CHECK(!book.Lately(GUTSY, T0));
	CHECK(book.PendingFor(GUTSY) == 0);
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, T0);
	CHECK(book.PendingFor(GUTSY) == PLAYER_2);
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

TEST("hand-off claim: one a friend's game took is theirs, and one that comes back later is no strike")
{
	HandOffClaim::Book book;
	book.HandedOff(GUARD, Kind::kFriendly, PLAYER_2, T0);
	book.Theirs(GUARD, PLAYER_2);  // the session names the friend its runner
	CHECK(book.PendingFor(GUARD) == 0);
	CHECK(!book.BackedOff(GUARD, PLAYER_2));
	// They hand it back to us ten seconds on: ours again, no strike, and still not handed off again within RETRY.
	CHECK(!book.Ours(GUARD, false, T0 + 10s));
	CHECK(!book.BackedOff(GUARD, PLAYER_2));
	CHECK(book.Lately(GUARD, T0 + 10s));
	book.Expire(T0 + HandOffClaim::RETRY);
	CHECK(!book.Lately(GUARD, T0 + HandOffClaim::RETRY));
}

TEST("hand-off claim: one unclaimed return is a strike and no back-off; a second in a row backs it off for that player, whichever hand-off let it go")
{
	for (const auto kind : { Kind::kNearer, Kind::kFriendly }) {
		HandOffClaim::Book book;
		book.HandedOff(GUTSY, kind, PLAYER_2, T0);
		// Nobody claimed it: ours takes it back after the wait, and the session names us its runner again.
		const auto first = book.Ours(GUTSY, false, T0 + 2300ms);
		REQUIRE(first.has_value());
		CHECK((*first == HandOffClaim::Back{ kind, PLAYER_2, 1, false }));
		CHECK(!book.BackedOff(GUTSY, PLAYER_2));
		CHECK(book.BackedOffCount() == 0);
		// Told once: the session naming us again is no second strike.
		CHECK(!book.Ours(GUTSY, false, T0 + 2400ms));
		// It waits only RETRY, as it always did, and is let go for them again.
		book.Expire(T0 + HandOffClaim::RETRY);
		CHECK(!book.Lately(GUTSY, T0 + HandOffClaim::RETRY));
		book.HandedOff(GUTSY, kind, PLAYER_2, T0 + 30s);
		const auto second = book.Ours(GUTSY, false, T0 + 32300ms);
		REQUIRE(second.has_value());
		CHECK((*second == HandOffClaim::Back{ kind, PLAYER_2, 2, true }));
		CHECK(book.BackedOff(GUTSY, PLAYER_2));
		CHECK(book.BackedOffCount() == 1);
		book.Expire(T0 + 32300ms + HandOffClaim::BACKOFF - 1ms);
		CHECK(book.BackedOff(GUTSY, PLAYER_2));
		book.Expire(T0 + 32300ms + HandOffClaim::BACKOFF);
		CHECK(!book.BackedOff(GUTSY, PLAYER_2));
		CHECK(book.BackedOffCount() == 0);
		// Tried again after the back-off: one more unclaimed return backs it off again, so it is tried once each BACKOFF.
		const auto later = T0 + 32300ms + HandOffClaim::BACKOFF;
		book.HandedOff(GUTSY, kind, PLAYER_2, later);
		const auto third = book.Ours(GUTSY, false, later + 2300ms);
		REQUIRE(third.has_value());
		CHECK(third->backedOff);
		CHECK(book.BackedOff(GUTSY, PLAYER_2));
	}
}

TEST("hand-off claim: the two strikes are in a row: the player's game running the NPC in between starts the count again")
{
	HandOffClaim::Book book;
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, T0);
	REQUIRE(book.Ours(GUTSY, false, T0 + 2300ms).has_value());  // their claim was late: a hitch as a cell loaded
	book.Expire(T0 + 30s);
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, T0 + 30s);
	book.Theirs(GUTSY, PLAYER_2);  // this time it came within the wait
	CHECK(!book.Ours(GUTSY, false, T0 + 50s));
	book.Expire(T0 + 60s);
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, T0 + 60s);
	const auto back = book.Ours(GUTSY, false, T0 + 62300ms);
	REQUIRE(back.has_value());
	CHECK(back->strikes == 1);
	CHECK(!back->backedOff);
	CHECK(!book.BackedOff(GUTSY, PLAYER_2));
	// A strike with no second within BACKOFF is forgotten.
	book.Expire(T0 + 62300ms + HandOffClaim::BACKOFF);
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, T0 + 10min);
	const auto again = book.Ours(GUTSY, false, T0 + 10min + 2300ms);
	REQUIRE(again.has_value());
	CHECK(again->strikes == 1);
	CHECK(!book.BackedOff(GUTSY, PLAYER_2));
}

TEST("hand-off claim: a player whose game was stopped by a menu as it came back gets no strike")
{
	HandOffClaim::Book book;
	// They opened the Pip-Boy as we let go, twice: their game claimed nothing, and neither time counts.
	for (const auto at : { T0, T0 + 30s }) {
		book.Expire(at);
		book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, at);
		const auto back = book.Ours(GUTSY, true, at + 2300ms);
		REQUIRE(back.has_value());
		CHECK((*back == HandOffClaim::Back{ Kind::kNearer, PLAYER_2, 0, false }));
		CHECK(!book.BackedOff(GUTSY, PLAYER_2));
	}
	CHECK(book.BackedOffCount() == 0);
}

TEST("hand-off claim: the back-off is one player's: a third player near the NPC is still handed it")
{
	HandOffClaim::Book book;
	for (const auto at : { T0, T0 + 30s }) {
		book.Expire(at);
		book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, at);
		REQUIRE(book.Ours(GUTSY, false, at + 2300ms).has_value());
	}
	CHECK(book.BackedOff(GUTSY, PLAYER_2));
	CHECK(!book.BackedOff(GUTSY, PLAYER_3));
	CHECK(!book.BackedOff(GUARD, PLAYER_2));  // and one NPC's
	// Player 3 walks up: handed to them once the RETRY is over, and their game takes it.
	book.Expire(T0 + 60s);
	CHECK(!book.Lately(GUTSY, T0 + 60s));
	book.HandedOff(GUTSY, Kind::kNearer, PLAYER_3, T0 + 60s);
	CHECK(book.PendingFor(GUTSY) == PLAYER_3);
	book.Theirs(GUTSY, PLAYER_3);
	CHECK(!book.BackedOff(GUTSY, PLAYER_3));
	CHECK(book.BackedOff(GUTSY, PLAYER_2));  // player 3's game running it says nothing of player 2's
	// Player 2's own game running it does.
	book.Theirs(GUTSY, PLAYER_2);
	CHECK(!book.BackedOff(GUTSY, PLAYER_2));
}

TEST("hand-off claim: a game stopped by a menu waits the same, but what comes back to it is no strike")
{
	HandOffClaim::Book book;
	book.HandedOff(GUARD, Kind::kPaused, 0, T0);
	CHECK(book.Waits(GUARD, T0 + 1s));
	CHECK(book.Lately(GUARD, T0 + 1s));
	CHECK(book.PendingFor(GUARD) == 0);
	// The menu closed and no friend had taken it: ours again, and free to go at the next menu or to a nearer player.
	CHECK(!book.Ours(GUARD, false, T0 + 5s));
	CHECK(book.BackedOffCount() == 0);
	// One backed off from hand-offs to a nearer player stays so through a menu.
	for (const auto at : { T0, T0 + 30s }) {
		book.Expire(at);
		book.HandedOff(GUTSY, Kind::kNearer, PLAYER_2, at);
		REQUIRE(book.Ours(GUTSY, false, at + 3s).has_value());
	}
	book.HandedOff(GUTSY, Kind::kPaused, 0, T0 + 70s);
	CHECK(!book.Ours(GUTSY, false, T0 + 80s));
	CHECK(book.BackedOff(GUTSY, PLAYER_2));
	book.Clear();
	CHECK(!book.BackedOff(GUTSY, PLAYER_2));
	CHECK(!book.Lately(GUTSY, T0 + 80s));
}

namespace
{
	// The owner's game and one NPC, scan by scan (NpcSync's 250 ms), with a friend much nearer to it the whole time
	// (ShouldHandOff's facts hold at every scan) whose game claims what is nobody's a while after the hand-off (its next
	// scan: 250 ms; later for a hitch or a slow link), or never (it has no reference of the NPC). A claim that finds the
	// NPC run again by its owner is refused by the session.
	struct Road
	{
		static constexpr std::uint32_t OWNER = 1;
		static constexpr std::uint32_t FRIEND = 2;

		HandOffClaim::Book book;
		std::uint32_t      runner = OWNER;
		Clock::time_point  now = T0;
		Clock::time_point  handedAt{};
		std::uint32_t      handOffs = 0;
		std::uint32_t      cameBack = 0;
		std::uint32_t      backOffs = 0;
		Clock::duration    unrun{};         // the time nobody ran it, all told
		Clock::duration    longestUnrun{};  // and the longest stretch of it
		Clock::time_point  unrunSince{};

		void Scan(std::optional<Clock::duration> a_claimsAfter)
		{
			book.Expire(now);
			if (runner == OWNER) {
				if (!book.Lately(GUTSY, now) && !book.BackedOff(GUTSY, FRIEND)) {
					book.HandedOff(GUTSY, Kind::kNearer, FRIEND, now);
					runner = 0;
					unrunSince = now;
					handedAt = now;
					++handOffs;
				}
			} else if (runner == 0) {
				if (a_claimsAfter && now - handedAt >= *a_claimsAfter) {
					runner = FRIEND;
					book.Theirs(GUTSY, FRIEND);
				} else if (!book.Waits(GUTSY, now)) {
					runner = OWNER;
					if (const auto back = book.Ours(GUTSY, false, now)) {
						++cameBack;
						backOffs += back->backedOff ? 1 : 0;
					}
				}
				if (runner != 0) {
					unrun += now - unrunSince;
					longestUnrun = (std::max)(longestUnrun, now - unrunSince);
				}
			}
			now += 250ms;
		}

		void Play(Clock::duration a_for, std::optional<Clock::duration> a_claimsAfter)
		{
			for (const auto until = now + a_for; now < until;) {
				Scan(a_claimsAfter);
			}
		}
	};
}

TEST("hand-off claim: an NPC a friend's game cannot claim is handed off twice, then once in five minutes, and stands still for little over CLAIM_WAIT")
{
	// The 0.9.2 road encounter: the joiner's game runs a Mr Gutsy the host's has no reference of, the joiner 7000 units
	// from it and the host 5000. Before, the joiner's game let go of it every 30 s and took it back 10 s later: it
	// stood still a third of the time, and the host's copy of it went and was placed again each time.
	Road road;
	road.Play(HandOffClaim::BACKOFF - 1s, std::nullopt);
	CHECK(road.handOffs == 2);  // the second strike, RETRY after the first, backs it off
	CHECK(road.cameBack == 2);
	CHECK(road.backOffs == 1);
	CHECK(road.runner == Road::OWNER);
	CHECK(road.book.BackedOff(GUTSY, Road::FRIEND));
	CHECK(road.longestUnrun >= HandOffClaim::CLAIM_WAIT);
	CHECK(road.longestUnrun <= HandOffClaim::CLAIM_WAIT + 250ms);
	// The friend's copy of it is kept through that (SpawnBook::UNRUN_GRACE), with a scan and the way there to spare.
	CHECK(SpawnBook::UNRUN_GRACE >= road.longestUnrun + 1s);
	// Half an hour of it: tried again once each BACKOFF, never more.
	road.Play(30min, std::nullopt);
	CHECK(road.handOffs <= 8);
	CHECK(road.handOffs == road.cameBack);
	CHECK(road.backOffs == road.handOffs - 1);
	CHECK(road.unrun <= road.handOffs * (HandOffClaim::CLAIM_WAIT + 250ms));
}

TEST("hand-off claim: an NPC a friend's game can claim goes to it within a scan, and nothing is backed off")
{
	Road road;
	road.Play(10s, 250ms);
	CHECK(road.handOffs == 1);
	CHECK(road.cameBack == 0);
	CHECK(road.runner == Road::FRIEND);
	CHECK(road.longestUnrun <= 250ms);
	CHECK(!road.book.BackedOff(GUTSY, Road::FRIEND));
}

TEST("hand-off claim: a claim that lands a scan after CLAIM_WAIT costs one RETRY, not a back-off")
{
	// The friend's game hitched as a cell loaded: its claim came 2.25 s after the hand-off, a scan after the owner had
	// claimed the NPC back, and the session refused it.
	Road road;
	road.Play(29s, HandOffClaim::CLAIM_WAIT + 250ms);
	CHECK(road.handOffs == 1);
	CHECK(road.cameBack == 1);
	CHECK(road.backOffs == 0);
	CHECK(road.runner == Road::OWNER);
	CHECK(!road.book.BackedOff(GUTSY, Road::FRIEND));
	// RETRY after the first hand-off it goes again, and the friend's game, running as it does, has it within a scan.
	road.Play(10s, 250ms);
	CHECK(road.handOffs == 2);
	CHECK(road.cameBack == 1);
	CHECK(road.runner == Road::FRIEND);
	CHECK(!road.book.BackedOff(GUTSY, Road::FRIEND));
	CHECK(road.book.BackedOffCount() == 0);
}

TEST("hand-off claim: two late claims in a row do back off, for five minutes")
{
	Road road;
	road.Play(60s, HandOffClaim::CLAIM_WAIT + 250ms);
	CHECK(road.handOffs == 2);
	CHECK(road.cameBack == 2);
	CHECK(road.backOffs == 1);
	CHECK(road.runner == Road::OWNER);
	CHECK(road.book.BackedOff(GUTSY, Road::FRIEND));
	road.Play(HandOffClaim::BACKOFF - 60s, 250ms);
	CHECK(road.handOffs == 2);
	// The back-off over, it is tried again, and their game takes it.
	road.Play(60s, 250ms);
	CHECK(road.handOffs == 3);
	CHECK(road.runner == Road::FRIEND);
	CHECK(!road.book.BackedOff(GUTSY, Road::FRIEND));
}
