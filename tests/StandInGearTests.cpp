// A friend's gear as their stand-in here is dressed in it (game/StandInGear.h). What their game reports goes through a
// real server, and what reaches our game is fed, frame by frame, to the rules RemotePlayers dresses the stand-in by.

#include "ServerFixture.h"

#include "game/StandInGear.h"

#include <set>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using StandInGear::Step;

	constexpr std::uint32_t PISTOL = 0x0000DF42;
	constexpr std::uint32_t RIFLE = 0x0000E27B;
	constexpr std::uint32_t VAULT_SUIT = 0x0001EED7;
	constexpr std::uint32_t HAT = 0x000F4D22;
	constexpr std::uint32_t GLASSES = 0x0018F5B3;
	constexpr std::uint32_t FRAG = 0x0000E56B;  // a grenade: counted as worn by its owner's game while they have one

	bool InHand(std::uint32_t a_item)
	{
		return a_item == PISTOL || a_item == RIFLE;
	}

	// Our game's stand-in for Alice, dressed frame by frame the way RemotePlayers::Update does it. Counts what an
	// onlooker would see: a stand-in emptied and dressed again, a change put on over what it wears, and its gun put away
	// and drawn again.
	struct Ours
	{
		StandInGear::Gear gear;
		Clock::time_point now = Clock::time_point{} + 1h;
		int               dresses = 0;
		int               changes = 0;
		int               redraws = 0;

		void Receive(const std::optional<Protocol::Equipment>& a_report)
		{
			REQUIRE(a_report);
			StandInGear::Report(gear, a_report->items);
		}

		void Frame()
		{
			now += 16ms;
			const auto step = StandInGear::Next(gear, now);
			if (step == Step::kNone) {
				return;
			}
			dresses += step == Step::kDress;
			changes += step == Step::kChange;
			redraws += StandInGear::Done(gear, step, now, InHand);
		}

		void Frames(int a_count)
		{
			for (int i = 0; i < a_count; ++i) {
				Frame();
			}
		}
	};

	// Bob (peer 1, whose game is looked at) and Alice (peer 2), both in.
	struct Session
	{
		Fixture       f;
		std::uint32_t alice = 0;

		Session()
		{
			REQUIRE(f.Join(1, Fixture::MakeHello("Bob", 0xB0B)));
			const auto welcome = f.Join(2, Fixture::MakeHello("Alice", 0xA11CE));
			REQUIRE(welcome);
			alice = welcome->playerId;
			REQUIRE(f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
			f.fake->Take(1);
		}

		// Alice's game reports her gear; what Bob's game is told, if anything.
		std::optional<Protocol::Equipment> Report(std::vector<std::uint32_t> a_items, std::chrono::milliseconds a_wait = 2000ms)
		{
			f.fake->Deliver(2, Protocol::Encode(Protocol::Equipment{ 0, std::move(a_items) }, MT::kEquipment));
			const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerEquipment), a_wait);
			const auto report = packet ? Protocol::DecodeEquipment(*packet) : std::nullopt;
			if (report) {
				CHECK(report->playerId == alice);
			}
			return report;
		}
	};
}

TEST("stand-in gear: a friend's hat and their last grenade never make their stand-in put its gun away; a gun swap does, once")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT, FRAG }));
	bob.Frames(2);
	CHECK(bob.dresses == 1);
	CHECK(bob.redraws == 1);  // a new stand-in draws the gun it was given
	bob.Frames(70);

	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, FRAG }));  // hat off mid-fight
	bob.Frames(70);
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));  // last grenade thrown
	bob.Frames(70);
	CHECK(bob.changes == 2);
	CHECK(bob.redraws == 1);  // the gun stayed out: their shots kept their sound
	CHECK((bob.gear.worn == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));

	bob.Receive(session.Report({ RIFLE, VAULT_SUIT }));
	bob.Frames(70);
	CHECK(bob.changes == 3);
	CHECK(bob.redraws == 2);
	CHECK(bob.dresses == 1);  // never stripped and dressed again for a change
}

TEST("stand-in gear: a friend's game sending its gear again, or in another order, changes nothing on their stand-in")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, GLASSES }));
	bob.Frames(70);
	// Their game forgets what it sent each time it loads a save and sends it again: the server keeps it to itself.
	for (int i = 0; i < 3; ++i) {
		CHECK(!session.Report({ PISTOL, VAULT_SUIT, GLASSES }, 300ms));
	}
	// The same items in another order (only a modified game sends that) get past the server, but are no change here.
	bob.Receive(session.Report({ GLASSES, VAULT_SUIT, PISTOL }));
	bob.Frames(70);
	CHECK(bob.dresses == 1);
	CHECK(bob.changes == 0);
	CHECK(bob.redraws == 1);
}

TEST("stand-in gear: a friend's game flipping between two outfits as fast as it can redresses their stand-in at most once a second")
{
	Session session;
	Ours    bob;
	const std::vector<std::uint32_t> one{ PISTOL, VAULT_SUIT, HAT };
	const std::vector<std::uint32_t> other{ RIFLE, VAULT_SUIT };
	bob.Receive(session.Report(one));
	bob.Frame();
	// Each report differs from the one before, so the server passes every one on; we read one a frame.
	for (int i = 0; i < 40; ++i) {
		bob.Receive(session.Report(i % 2 ? one : other));
		bob.Frame();
	}
	CHECK(bob.changes == 0);  // within a second of being dressed: put on together, later
	// It ends on what the stand-in has on already: nothing to put on at all.
	bob.Frames(70);
	CHECK(bob.changes == 0);
	CHECK(bob.redraws == 1);
	CHECK(bob.dresses == 1);
	CHECK(bob.gear.worn == one);
	// Once more, more than a second on and ending on the other: the first flip goes on at once (its gun swapped), and
	// the forty after it, within the second, end on what it then wears.
	for (int i = 0; i < 41; ++i) {
		bob.Receive(session.Report(i % 2 ? one : other));
		bob.Frame();
	}
	bob.Frames(70);
	CHECK(bob.changes == 1);
	CHECK(bob.redraws == 2);
	CHECK(bob.gear.worn == other);
}

TEST("stand-in gear: a friend whose game restarted is dressed again in the same gear, and a newcomer sees it too")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(2);
	REQUIRE(bob.gear.dressed);

	// Her game restarts and says hello again on the same connection: Bob's game is told she left and joined, and
	// starts her afresh.
	session.f.fake->Deliver(2, Protocol::Encode(Fixture::MakeHello("Alice", 0xA11CE)));
	REQUIRE(session.f.fake->WaitFor(1, Type(MT::kPlayerLeft)));
	REQUIRE(session.f.fake->WaitFor(1, Type(MT::kPlayerJoined)));
	bob = Ours{};
	// The same gear as before the restart: passed on, since it is her first report since.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(2);
	CHECK(bob.dresses == 1);
	CHECK((bob.gear.worn == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));

	// Carol joins afterwards: told Alice's gear as she joins, with nothing new from Alice.
	REQUIRE(session.f.Join(3, Fixture::MakeHello("Carol", 0xCA201)));
	const auto packet = session.f.fake->WaitFor(3, Type(MT::kPlayerEquipment));
	REQUIRE(packet);
	Ours carol;
	carol.Receive(Protocol::DecodeEquipment(*packet));
	carol.Frames(2);
	CHECK((carol.gear.worn == std::vector<std::uint32_t>{ PISTOL, VAULT_SUIT }));
}

TEST("stand-in gear: a stand-in placed again (a door, a load, power armor) is dressed afresh with nothing new from the server")
{
	Session session;
	Ours    bob;
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT, HAT }));
	bob.Frames(70);
	// Through a door: a new stand-in, dressed in what she last reported, though the server sends nothing new.
	StandInGear::Redress(bob.gear);
	CHECK(!session.Report({ PISTOL, VAULT_SUIT, HAT }, 300ms));
	bob.Frame();
	CHECK(bob.dresses == 2);
	CHECK(bob.redraws == 2);
	// A change reported right after is put on a second later, over what it wears.
	bob.Receive(session.Report({ PISTOL, VAULT_SUIT }));
	bob.Frames(30);
	CHECK(bob.changes == 0);
	bob.Frames(40);
	CHECK(bob.changes == 1);
	CHECK(bob.redraws == 2);
	// Into power armor and back out: dressed afresh both times, no wait for the gap.
	StandInGear::Redress(bob.gear);
	bob.Frame();
	StandInGear::Redress(bob.gear);
	bob.Frame();
	CHECK(bob.dresses == 4);
	CHECK(bob.changes == 1);
}

TEST("stand-in gear: no report yet, nothing put on; a stand-in dressed in what it wears is not dressed again")
{
	StandInGear::Gear gear;
	const auto        now = Clock::time_point{} + 1h;
	CHECK(StandInGear::Next(gear, now) == Step::kNone);  // the NPC's own outfit until their gear comes
	CHECK(StandInGear::Report(gear, { PISTOL }));
	CHECK(StandInGear::Next(gear, now) == Step::kDress);
	StandInGear::Done(gear, Step::kDress, now, InHand);
	CHECK(StandInGear::Next(gear, now + 10s) == Step::kNone);
	CHECK(!StandInGear::Report(gear, { PISTOL, PISTOL }));  // a duplicate entry is the same list
	CHECK(StandInGear::Report(gear, {}));                    // everything off is a change
	CHECK(StandInGear::Next(gear, now + 999ms) == Step::kNone);
	CHECK(StandInGear::Next(gear, now + 1s) == Step::kChange);
	CHECK(StandInGear::Done(gear, Step::kChange, now + 1s, InHand));  // the pistol went: drawn again, empty-handed
}
