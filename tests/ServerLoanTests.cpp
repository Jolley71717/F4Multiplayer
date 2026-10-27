// Power armor frames lent between players (src/game/FrameLoan.h) as the server sees them: a friend's ask goes to the
// frame's owner alone, the owner's kit to everyone, the return to everyone; and the refusals and endings, played against a
// real server.

#include "ServerFixture.h"

#include <algorithm>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using Protocol::LoanRefusal;

	constexpr std::uint32_t WORLD = 0x0000003C;   // the Commonwealth
	constexpr std::uint32_t FRAME = 0x0019DED9;   // a power armor frame's base (furniture)
	constexpr std::uint32_t PIECE = 0x00154ABC;   // a power armor piece
	constexpr std::uint32_t CORE = 0x00075FE4;    // a fusion core
	constexpr std::uint32_t SPAWN = 0xFF001234;   // the spawn id the owner's game shares its frame under
	constexpr std::uint32_t SPAWN2 = 0xFF005678;

	auto Frame(std::uint32_t a_id)
	{
		return Protocol::Encode(Protocol::Spawn{ 0, a_id, FRAME, 0, WORLD, 1000.0f, 2000.0f, 100.0f, 0.5f }, MT::kReportSpawn);
	}

	auto Status(bool a_lends)
	{
		Protocol::PlayerStatus status;
		status.health = 100;
		status.level = 5;
		status.lends = a_lends;
		return Protocol::Encode(status, MT::kReportStatus);
	}

	auto Ask(std::uint32_t a_id)
	{
		return Protocol::Encode(Protocol::FrameBorrow{ 0, a_id }, MT::kBorrowFrame);
	}

	std::vector<Protocol::LentItem> Kit()
	{
		return { Protocol::LentItem{ .slot = 0, .base = PIECE, .health = 0.5f }, Protocol::LentItem{ .slot = 1, .base = CORE, .health = 0.9f, .core = true } };
	}

	auto Lend(std::uint32_t a_borrower, std::uint32_t a_id, LoanRefusal a_refusal = LoanRefusal::kNone)
	{
		Protocol::FrameLending lending{ 0, a_borrower, a_id, a_refusal, {} };
		if (a_refusal == LoanRefusal::kNone) {
			lending.kit = Kit();
		}
		return Protocol::Encode(lending, MT::kLendFrame);
	}

	auto Return(std::uint32_t a_id)
	{
		return Protocol::Encode(Protocol::FrameReturn{ 0, 0, a_id, false, 0, WORLD, 1500.0f, 2000.0f, 100.0f, 0.25f, { { 0, true, 0.25f }, { 1, false, -1.0f } } }, MT::kReturnFrame);
	}

	// Where a player's game says they are: in the Commonwealth (a_cell 0) or in the interior a_cell.
	auto At(std::uint32_t a_sequence, float a_x, float a_y, float a_z, std::uint32_t a_cell = 0)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.cell = a_cell;
		state.worldspace = a_cell ? 0 : WORLD;
		state.x = a_x, state.y = a_y, state.z = a_z;
		return Protocol::Encode(state);
	}

	std::vector<Protocol::FrameBorrow> Asked(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kLoanAsked, [](auto a_data) { return Protocol::DecodeFrameBorrow(a_data); });
	}

	std::vector<Protocol::FrameLending> Lent(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kFrameLent, [](auto a_data) { return Protocol::DecodeFrameLending(a_data); });
	}

	std::vector<Protocol::FrameReturn> Returned(const Packets& a_packets)
	{
		return Decoded(a_packets, MT::kFrameReturned, [](auto a_data) { return Protocol::DecodeFrameReturn(a_data); });
	}

	Server::Options Room()
	{
		Server::Options options;
		options.maxPlayers = 5;  // Alice, Bob, Carol and two who join later (the default room is four)
		return options;
	}

	// Alice shares a frame (SPAWN) and lends; Bob and Carol are there. Everyone's mail is read.
	struct Session
	{
		Fixture       f{ Room() };
		std::uint32_t alice = 0, bob = 0, carol = 0;

		explicit Session(bool a_lends = true)
		{
			alice = JoinAs(f, 1, "Alice");
			bob = JoinAs(f, 2, "Bob");
			carol = JoinAs(f, 3, "Carol");
			f.fake->Deliver(1, Frame(SPAWN));
			f.fake->Deliver(1, Status(a_lends));
			Clear(f, { 1, 2, 3 });
		}

		// Bob asks for SPAWN and Alice lends it: what each was sent then.
		void Borrow()
		{
			f.fake->Deliver(2, Ask(SPAWN));
			Received(f, 2);
			REQUIRE(Asked(Received(f, 1)).size() == 1);
			f.fake->Deliver(1, Lend(bob, SPAWN));
			REQUIRE(Lent(Received(f, 1)).size() == 1);
			REQUIRE(Lent(Received(f, 2)).size() == 1);
			REQUIRE(Lent(Received(f, 3)).size() == 1);
		}
	};
}

TEST("frame loans: a friend's ask reaches the frame's owner alone, and the kit lent reaches everyone")
{
	Session s;
	s.f.fake->Deliver(2, Ask(SPAWN));
	CHECK(Lent(Received(s.f, 2)).empty());  // no answer yet: the owner's game gives it
	const auto asked = Asked(Received(s.f, 1));
	REQUIRE(asked.size() == 1);
	CHECK(asked[0] == (Protocol::FrameBorrow{ s.bob, SPAWN }));
	CHECK(Asked(Received(s.f, 3)).empty());

	s.f.fake->Deliver(1, Lend(s.bob, SPAWN));
	for (const PeerId peer : { 1, 2, 3 }) {
		const auto lent = Lent(Received(s.f, peer));
		REQUIRE(lent.size() == 1);
		CHECK(lent[0].ownerId == s.alice);  // the server's word, not the packet's
		CHECK(lent[0].borrowerId == s.bob);
		CHECK(lent[0].spawnId == SPAWN);
		CHECK(lent[0].refusal == LoanRefusal::kNone);
		CHECK(lent[0].kit == Kit());
	}
}

TEST("frame loans: a joiner hears of a frame lent out after the spawn it is, and the return reaches everyone")
{
	Session s;
	s.Borrow();

	const auto dave = JoinAs(s.f, 4, "Dave");
	const auto daveGot = Received(s.f, 4);
	const auto spawned = std::ranges::find_if(daveGot, [](const auto& a_packet) { return a_packet[0] == Type(MT::kSpawned); });
	const auto lent = std::ranges::find_if(daveGot, [](const auto& a_packet) { return a_packet[0] == Type(MT::kFrameLent); });
	REQUIRE(spawned != daveGot.end());
	REQUIRE(lent != daveGot.end());
	CHECK(spawned < lent);
	CHECK(Lent(daveGot).size() == 1);
	CHECK(Lent(daveGot)[0].borrowerId == s.bob);

	s.f.fake->Deliver(2, At(1, 1450.0f, 2050.0f, 100.0f));  // Bob stands beside where he got out
	s.f.fake->Deliver(2, Return(SPAWN));
	for (const PeerId peer : { 1, 2, 3, 4 }) {
		const auto back = Returned(Received(s.f, peer));
		REQUIRE(back.size() == 1);
		CHECK(back[0].borrowerId == s.bob);
		CHECK(back[0].ownerId == s.alice);
		CHECK(!back[0].ended);
		CHECK(back[0].worldspace == WORLD);
		CHECK(back[0].x == 1500.0f);  // his own word for the spot: it is near where he was seen
		CHECK(back[0].items.size() == 2);
	}
	// Over: a second return is nobody's, and the next joiner hears of no loan.
	s.f.fake->Deliver(2, Return(SPAWN));
	CHECK(Returned(Received(s.f, 1)).empty());
	JoinAs(s.f, 5, "Erin");
	CHECK(Lent(Received(s.f, 5)).empty());
	(void)dave;
}

TEST("frame loans: an owner who doesn't lend, a frame lent already, a second loan and a frame that is nobody's are refused to the asker alone")
{
	{
		Session s{ false };
		s.f.fake->Deliver(2, Ask(SPAWN));
		const auto refused = Lent(Received(s.f, 2));
		REQUIRE(refused.size() == 1);
		CHECK(refused[0].refusal == LoanRefusal::kNotLending);
		CHECK(refused[0].ownerId == s.alice);
		CHECK(refused[0].kit.empty());
		CHECK(Asked(Received(s.f, 1)).empty());
		CHECK(Lent(Received(s.f, 3)).empty());
	}
	{
		Session s;
		s.Borrow();
		// Carol asks for the frame Bob has.
		s.f.fake->Deliver(3, Ask(SPAWN));
		const auto refused = Lent(Received(s.f, 3));
		REQUIRE(refused.size() == 1);
		CHECK(refused[0].refusal == LoanRefusal::kLent);
		CHECK(Asked(Received(s.f, 1)).empty());
		// Bob asks for a second frame of Alice's while he has one.
		s.f.fake->Deliver(1, Frame(SPAWN2));
		Clear(s.f, { 1, 2, 3 });
		s.f.fake->Deliver(2, Ask(SPAWN2));
		const auto second = Lent(Received(s.f, 2));
		REQUIRE(second.size() == 1);
		CHECK(second[0].refusal == LoanRefusal::kLent);
		CHECK(Asked(Received(s.f, 1)).empty());
		// Bob asks again for the one he has: told it is his.
		s.f.fake->Deliver(2, Ask(SPAWN));
		const auto again = Lent(Received(s.f, 2));
		REQUIRE(again.size() == 1);
		CHECK(again[0].refusal == LoanRefusal::kNone);
		CHECK(again[0].kit == Kit());
	}
	{
		Session s;
		// No spawn by that id; and Alice asking for her own frame.
		s.f.fake->Deliver(2, Ask(SPAWN2));
		const auto none = Lent(Received(s.f, 2));
		REQUIRE(none.size() == 1);
		CHECK(none[0].refusal == LoanRefusal::kUnavailable);
		s.f.fake->Deliver(1, Ask(SPAWN));
		const auto own = Lent(Received(s.f, 1));
		REQUIRE(own.size() == 1);
		CHECK(own[0].refusal == LoanRefusal::kUnavailable);
	}
}

TEST("frame loans: the owner's refusal reaches the asker alone, and only an answer to an ask passed on counts")
{
	Session s;
	// Alice "lends" to Carol, who never asked: nothing.
	s.f.fake->Deliver(1, Lend(s.carol, SPAWN));
	CHECK(Lent(Received(s.f, 1)).empty());
	CHECK(Lent(Received(s.f, 3)).empty());

	s.f.fake->Deliver(2, Ask(SPAWN));
	Received(s.f, 2);
	Clear(s.f, { 1 });
	// Carol answers for Alice's frame: nothing.
	s.f.fake->Deliver(3, Lend(s.bob, SPAWN));
	CHECK(Lent(Received(s.f, 3)).empty());
	CHECK(Lent(Received(s.f, 2)).empty());

	s.f.fake->Deliver(1, Lend(s.bob, SPAWN, LoanRefusal::kUnavailable));
	const auto refused = Lent(Received(s.f, 2));
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].refusal == LoanRefusal::kUnavailable);
	CHECK(Lent(Received(s.f, 3)).empty());
	// The ask is over: a late grant is dropped, and Bob may ask again.
	s.f.fake->Deliver(1, Lend(s.bob, SPAWN));
	CHECK(Lent(Received(s.f, 2)).empty());
	s.f.fake->Deliver(2, Ask(SPAWN));
	Received(s.f, 2);
	CHECK(Asked(Received(s.f, 1)).size() == 1);

	// Only the borrower returns it.
	s.f.fake->Deliver(1, Lend(s.bob, SPAWN));
	Clear(s.f, { 1, 2, 3 });
	s.f.fake->Deliver(3, Return(SPAWN));
	CHECK(Returned(Received(s.f, 3)).empty());
	CHECK(Returned(Received(s.f, 1)).empty());
}

TEST("frame loans: the borrower leaving or loading a save, and the owner's spawn ending, end the loan for everyone")
{
	{
		Session s;
		s.Borrow();
		s.f.fake->Drop(2);
		for (const PeerId peer : { 1, 3 }) {
			Packets got;
			REQUIRE(Await(s.f, peer, MT::kFrameReturned, got));
			const auto back = Returned(got);
			REQUIRE(back.size() == 1);
			CHECK(back[0].ended);
			CHECK(back[0].borrowerId == s.bob);
			CHECK(back[0].ownerId == s.alice);
		}
	}
	{
		Session s;
		s.Borrow();
		s.f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{}));
		Received(s.f, 2);
		const auto back = Returned(Received(s.f, 1));
		REQUIRE(back.size() == 1);
		CHECK(back[0].ended);
	}
	{
		Session s;
		s.Borrow();
		s.f.fake->Deliver(1, Protocol::EncodeRelease({ SPAWN }));  // Alice's game let go of its frame (a load, say)
		Received(s.f, 1);
		const auto back = Returned(Received(s.f, 2));
		REQUIRE(back.size() == 1);
		CHECK(back[0].ended);
		CHECK(back[0].spawnId == SPAWN);
	}
	{
		Session s;
		s.Borrow();
		s.f.fake->Drop(1);  // the owner leaves: Bob hears his loan has nobody to go back to
		Packets got;
		REQUIRE(Await(s.f, 2, MT::kFrameReturned, got));
		CHECK(Returned(got)[0].ended);
	}
}

TEST("frame loans: an ask that ends before it is granted is told to the owner's game, so it forgets the frame it was to lend")
{
	const auto askOver = [](Session& a_s) {
		const auto back = Returned(Received(a_s.f, 1));
		REQUIRE(back.size() == 1);
		CHECK(back[0].ended);
		CHECK(back[0].spawnId == SPAWN);
		CHECK(back[0].ownerId == a_s.alice);
		CHECK(back[0].borrowerId == a_s.bob);
	};
	{
		// Bob gives up waiting (his game sends an ended return for it).
		Session s;
		s.f.fake->Deliver(2, Ask(SPAWN));
		REQUIRE(Asked(Received(s.f, 1)).size() == 1);
		s.f.fake->Deliver(2, Protocol::Encode(Protocol::FrameReturn{ 0, 0, SPAWN, true }, MT::kReturnFrame));
		askOver(s);
		CHECK(Returned(Received(s.f, 3)).empty());  // nobody else heard of the ask: nobody else hears it end
		// Alice's late grant is dropped, and Bob may ask again.
		s.f.fake->Deliver(1, Lend(s.bob, SPAWN));
		CHECK(Lent(Received(s.f, 2)).empty());
		s.f.fake->Deliver(2, Ask(SPAWN));
		CHECK(Asked(Received(s.f, 1)).size() == 1);
	}
	{
		// Bob loads a save while he waits.
		Session s;
		s.f.fake->Deliver(2, Ask(SPAWN));
		REQUIRE(Asked(Received(s.f, 1)).size() == 1);
		s.f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{}));
		askOver(s);
	}
	{
		// Bob leaves while he waits.
		Session s;
		s.f.fake->Deliver(2, Ask(SPAWN));
		REQUIRE(Asked(Received(s.f, 1)).size() == 1);
		s.f.fake->Drop(2);
		Packets got;
		REQUIRE(Await(s.f, 1, MT::kFrameReturned, got));
		const auto back = Returned(got);
		REQUIRE(back.size() == 1);
		CHECK(back[0].ended);
		CHECK(back[0].borrowerId == s.bob);
	}
}

TEST("frame loans: a return naming a spot far off or elsewhere is put where its borrower was last seen, and one from a borrower never seen leaves the frame where it stood")
{
	const auto returned = [](Session& a_s) {
		a_s.f.fake->Deliver(2, Return(SPAWN));
		const auto back = Returned(Received(a_s.f, 1));
		REQUIRE(back.size() == 1);
		CHECK(!back[0].ended);
		CHECK(back[0].items.size() == 2);  // what came back is the borrower's word: only the spot is moved
		return back[0];
	};
	{
		// Far off: the spot is where Bob was last seen.
		Session s;
		s.Borrow();
		s.f.fake->Deliver(2, At(1, 90000.0f, -40000.0f, 300.0f));
		const auto back = returned(s);
		CHECK(back.cell == 0);
		CHECK(back.worldspace == WORLD);
		CHECK(back.x == 90000.0f);
		CHECK(back.y == -40000.0f);
		CHECK(back.z == 300.0f);
	}
	{
		// In an interior while his return names the Commonwealth: the interior, where he was seen.
		constexpr std::uint32_t CELL = 0x0001F0A1;
		Session s;
		s.Borrow();
		s.f.fake->Deliver(2, At(1, 1500.0f, 2000.0f, 100.0f, CELL));
		const auto back = returned(s);
		CHECK(back.cell == CELL);
		CHECK(back.worldspace == 0);
	}
	{
		// Never seen: no spot, so the owner's game leaves its frame where it stood.
		Session s;
		s.Borrow();
		const auto back = returned(s);
		CHECK(back.cell == 0);
		CHECK(back.worldspace == 0);
	}
}
