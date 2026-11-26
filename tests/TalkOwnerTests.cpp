// Who owns a conversation with an NPC (TalkOwner): one player at a time, whoever starts first. A friend's status names
// the NPC they talk to (PlayerStatus::talkingTo); a game that finds the engine opened a conversation with an NPC a friend
// talks to ends it (NpcSync). The tests play two or three games' statuses through a real server, as the games would,
// feed what comes back into each game's book and ask TalkOwner::Decide what NpcSync asks it.

#include "ServerFixture.h"

#include "game/TalkOwner.h"

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using TalkOwner::Clock;

	constexpr std::uint32_t NPC = 0x0001CA7D;  // Codsworth
	constexpr std::uint32_t OTHER_NPC = 0x00045D71;

	Clock::time_point T(int a_ms)
	{
		return Clock::time_point{} + std::chrono::milliseconds(a_ms) + 1h;
	}
}

TEST("talk owner: nobody talks to the NPC, so the conversation is ours")
{
	TalkOwner::Book book;
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(0)).ours);
	book.Heard(1, OTHER_NPC, T(0));
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(10)).ours);  // a friend's other conversation is no matter
}

TEST("talk owner: the one who started first owns it, whichever way round")
{
	for (const std::uint32_t first : { 1u, 2u }) {
		const std::uint32_t second = 3 - first;
		TalkOwner::Book     book;  // the second player's game, which has heard the first's status
		book.Heard(first, NPC, T(0));
		const auto verdict = TalkOwner::Decide(book, second, NPC, false, T(500));
		CHECK(!verdict.ours);
		CHECK(verdict.owner == first);
	}
}

TEST("talk owner: both press E before either status arrives: exactly one keeps it, the lower player id")
{
	// Each game decided it was ours (nothing heard yet), told its status, and then hears the other's.
	TalkOwner::Book host;
	TalkOwner::Book friendBook;
	host.Heard(2, NPC, T(100));
	friendBook.Heard(1, NPC, T(100));
	const auto hosts = TalkOwner::Decide(host, 1, NPC, true, T(110));
	const auto friends = TalkOwner::Decide(friendBook, 2, NPC, true, T(110));
	CHECK(hosts.ours);
	CHECK(!friends.ours);
	CHECK(friends.owner == 1);
	CHECK(hosts.ours != friends.ours);
}

TEST("talk owner: three games on one NPC: the lowest id keeps it, the others both give it up")
{
	TalkOwner::Book books[4];
	for (std::uint32_t me = 1; me <= 3; ++me) {
		for (std::uint32_t other = 1; other <= 3; ++other) {
			if (other != me) {
				books[me].Heard(other, NPC, T(0));
			}
		}
	}
	CHECK(TalkOwner::Decide(books[1], 1, NPC, true, T(10)).ours);
	CHECK(!TalkOwner::Decide(books[2], 2, NPC, true, T(10)).ours);
	CHECK(!TalkOwner::Decide(books[3], 3, NPC, true, T(10)).ours);
	CHECK(TalkOwner::Decide(books[3], 3, NPC, true, T(10)).owner == 1);
}

TEST("talk owner: the claim ends when their status stops naming the NPC, when they leave, and when it goes stale")
{
	TalkOwner::Book book;
	book.Heard(1, NPC, T(0));
	CHECK(!TalkOwner::Decide(book, 2, NPC, false, T(1000)).ours);
	book.Heard(1, 0, T(2000));  // their conversation ended
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(2010)).ours);

	book.Heard(1, NPC, T(3000));
	book.Gone(1);  // they left, or lost the connection
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(3010)).ours);

	book.Heard(1, NPC, T(4000));  // their game went quiet: the status is no claim past STALE
	const auto stale = T(4000) + TalkOwner::STALE;
	CHECK(!TalkOwner::Decide(book, 2, NPC, false, stale).ours);
	CHECK(TalkOwner::Decide(book, 2, NPC, false, stale + 1ms).ours);
	book.Heard(1, NPC, stale);  // their next status renews it
	CHECK(!TalkOwner::Decide(book, 2, NPC, false, stale + 1s).ours);

	book.Heard(1, NPC, T(5000));
	book.Clear();  // the session ended
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(5010)).ours);
}

TEST("talk owner: a conversation of our own is never refused for our own status")
{
	TalkOwner::Book book;
	book.Heard(2, NPC, T(0));  // our own id in the book (a status looped back) is no friend
	CHECK(TalkOwner::Decide(book, 2, NPC, true, T(10)).ours);
	CHECK(TalkOwner::Decide(book, 2, NPC, false, T(10)).ours);
}

TEST("talk owner: statuses through the server: the friend's game hears who talks to whom, and is released when they stop")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	TalkOwner::Book bobsBook;

	// Alice's game sends its status as Party does; Bob's game keeps what it is told (NpcSync::OnPeerTalk).
	const auto tell = [&](std::uint32_t a_talkingTo, int a_at) {
		Protocol::PlayerStatus status;
		status.health = 100;
		status.level = 12;
		status.menu = a_talkingTo ? Protocol::MenuKind::kTalking : Protocol::MenuKind::kNone;
		status.talkingTo = a_talkingTo;
		f.fake->Deliver(1, Protocol::Encode(status, MT::kReportStatus));
		while (const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerStatus), 2000ms)) {
			const auto heard = Protocol::DecodePlayerStatus(*packet);
			REQUIRE(heard);
			if (heard->playerId == alice && heard->talkingTo == a_talkingTo) {
				bobsBook.Heard(heard->playerId, heard->talkingTo, T(a_at));
				return;
			}
		}
		REQUIRE(false);  // her status never reached Bob
	};

	tell(NPC, 0);
	const auto refused = TalkOwner::Decide(bobsBook, bob, NPC, false, T(100));
	CHECK(!refused.ours);
	CHECK(refused.owner == alice);
	CHECK(TalkOwner::Decide(bobsBook, bob, OTHER_NPC, false, T(100)).ours);

	tell(0, 200);  // her menu closed
	CHECK(TalkOwner::Decide(bobsBook, bob, NPC, false, T(300)).ours);
}
