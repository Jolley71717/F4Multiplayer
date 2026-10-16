// A friend in furniture: what their game tells of the chair or bed they use (StandInSeat::Tell, as
// Party sends it), through the real server, to what each friend's game makes of it (StandInSeat::Pick for its own copy
// of the furniture, StandInSeat::Choose each frame for the stand-in, as RemotePlayers::UpdateSeat runs them).

#include "ServerFixture.h"
#include "game/StandInSeat.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using StandInSeat::Act;
	using StandInSeat::Facts;
	using StandInSeat::SitState;
	using Protocol::Seat;

	constexpr std::uint32_t COUCH = 0x0001E4C2;       // a couch reference every game has (placed by a plugin)
	constexpr std::uint32_t COUCH_BASE = 0x0004A0F5;
	constexpr std::uint32_t CHAIR_BASE = 0x000AE3C1;
	constexpr std::uint32_t BUILT_CHAIR = 0xFF00A1B2;  // a chair built in a settlement in their game
	constexpr std::uint32_t OUR_COPY = 0xFF003C4D;     // our copy of it (WorkshopSync placed it at their spot)
	constexpr std::uint32_t FRAME_BASE = 0x0017F6E4;

	constexpr std::int32_t State(SitState a_state) { return static_cast<std::int32_t>(a_state); }

	// The 2026-10-15 report: the host sat on the couch in Publick Occurrences for Piper's interview.
	StandInSeat::Own OnTheCouch()
	{
		return { .sitState = State(SitState::kIsSitting), .found = true, .furniture = true, .ref = COUCH, .base = COUCH_BASE, .x = 1200.3f, .y = -350.6f, .z = 64.2f };
	}

	struct Point
	{
		float x = 0, y = 0, z = 0;
	};

	float Distance(const Point& a_a, const Point& a_b)
	{
		return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z);
	}

	// The hearer's game, one friend's stand-in in it, as RemotePlayers::UpdateSeat drives it with Puppets. The engine's side
	// is played here: which way in takes (none, the snap, activation), and that it takes a frame later.
	struct Hearer
	{
		std::optional<Seat> seat;           // as their last status said
		bool                found = true;   // our copy of the furniture is here (FindSeat)
		Point               furniture{ 1200.0f, -350.0f, 64.0f };
		std::optional<int>  takes;          // the try that takes (1 the snap, 2 activation), none for neither

		Point                       standIn;  // where it stands
		bool                        held = false;
		bool                        inSeat = false;
		std::optional<int>          asked;    // a way in asked for, taken by the engine on the next frame
		int                         tries = 0;
		StandInSeat::Duration       triedAt{};
		int                         stoodUp = 0;
		std::vector<Act>            acts;

		// Takes what the server sent through the next status, as Party::ApplyStatus hands it to SetSeat.
		bool Hear(Fixture& a_f, PeerId a_peer)
		{
			const auto packets = a_f.fake->TakeThrough(a_peer, Type(MT::kPlayerStatus));
			if (packets.empty() || packets.back()[0] != Type(MT::kPlayerStatus)) {
				return false;
			}
			const auto status = Protocol::DecodePlayerStatus(packets.back());
			REQUIRE(status);
			if (status->seat != seat) {
				seat = status->seat;
				tries = 0;  // ForgetSeat
			}
			return true;
		}

		// One frame: a_spot is where their states put the stand-in.
		Act Step(StandInSeat::Duration a_now, const Point& a_spot)
		{
			if (asked) {
				inSeat = takes == *asked;  // the engine took it in, or did not
				if (inSeat) {
					standIn = furniture;
				}
				asked.reset();
			}
			Facts facts;
			facts.wanted = seat.has_value();
			facts.found = seat && found;
			if (seat) {
				facts.near = Distance(a_spot, found ? furniture : Point{ seat->x, seat->y, seat->z }) <= StandInSeat::NEAR;
			}
			facts.inTarget = inSeat;
			facts.tries = tries;
			facts.sinceTried = a_now - triedAt;
			const auto act = StandInSeat::Choose(facts);
			switch (act) {
			case Act::kFollow:
				held = false;
				standIn = a_spot;
				break;
			case Act::kStand:
				inSeat = false;
				held = false;
				++stoodUp;
				standIn = a_spot;
				break;
			case Act::kEnter:
				++tries;
				triedAt = a_now;
				asked = tries;
				held = true;
				break;
			case Act::kHold:
				held = true;
				break;
			}
			acts.push_back(act);
			return act;
		}
	};

	// Their game: whether they sit, sent as Party sends it.
	void Send(Fixture& a_f, PeerId a_peer, const StandInSeat::Own& a_own)
	{
		Protocol::PlayerStatus status;
		status.level = 20;
		status.health = 100;
		status.powerArmor = a_own.powerArmor;
		status.seat = StandInSeat::Tell(a_own);
		a_f.fake->Deliver(a_peer, Protocol::Encode(status, MT::kReportStatus));
	}
}

TEST("seats: in furniture is sitting, lying down or on the way into either; getting up is out already")
{
	CHECK(!StandInSeat::InFurniture(State(SitState::kNormal)));
	for (const auto in : { SitState::kWantToSit, SitState::kWaitingForSitAnim, SitState::kIsSitting, SitState::kWantToSleep, SitState::kWaitingForSleepAnim, SitState::kIsSleeping }) {
		CHECK(StandInSeat::InFurniture(State(in)));
	}
	CHECK(!StandInSeat::InFurniture(State(SitState::kWantToStand)));
	CHECK(!StandInSeat::InFurniture(State(SitState::kWantToWake)));
	CHECK(!StandInSeat::InFurniture(9));
	CHECK(!StandInSeat::InFurniture(-1));
}

TEST("seats: the furniture told is the one used, never a power armor frame, a power armor station, a terminal, a workbench or a seat on an actor")
{
	const auto told = StandInSeat::Tell(OnTheCouch());
	REQUIRE(told);
	CHECK(told->ref == COUCH);
	CHECK(told->base == COUCH_BASE);
	// To the unit: a hair's difference sends no new status.
	CHECK(told->x == 1200.0f);
	CHECK(told->y == -351.0f);
	CHECK(told->z == 64.0f);

	// Lying in a bed, the same.
	auto own = OnTheCouch();
	own.sitState = State(SitState::kIsSleeping);
	CHECK(StandInSeat::Tell(own) == told);
	own.sitState = State(SitState::kWantToSit);  // on the way in: their stand-in goes in as they do
	CHECK(StandInSeat::Tell(own) == told);

	const auto none = [](auto a_change) {
		auto own = OnTheCouch();
		a_change(own);
		return !StandInSeat::Tell(own);
	};
	CHECK(none([](auto& a_o) { a_o.sitState = State(SitState::kNormal); }));
	CHECK(none([](auto& a_o) { a_o.sitState = State(SitState::kWantToStand); }));  // getting up
	CHECK(none([](auto& a_o) { a_o.sitState = State(SitState::kWantToWake); }));
	CHECK(none([](auto& a_o) { a_o.powerArmor = true; }));                        // the frames' own
	CHECK(none([](auto& a_o) { a_o.frame = true; a_o.base = FRAME_BASE; }));      // climbing into a frame
	CHECK(none([](auto& a_o) { a_o.station = true; }));                           // a power armor station
	CHECK(none([](auto& a_o) { a_o.workplace = true; }));                         // a terminal, a workbench
	CHECK(none([](auto& a_o) { a_o.actor = true; a_o.furniture = false; }));      // a vertibird's seat
	CHECK(none([](auto& a_o) { a_o.furniture = false; }));
	CHECK(none([](auto& a_o) { a_o.found = false; }));                            // the engine names none yet
	CHECK(none([](auto& a_o) { a_o.base = 0; }));
	CHECK(none([](auto& a_o) { a_o.x = std::numeric_limits<float>::quiet_NaN(); }));
	CHECK(none([](auto& a_o) { a_o.y = 1.0e9f; }));

	// A chair built in their settlement: its ID is another reference, or none, elsewhere; its base and place go.
	own = OnTheCouch();
	own.ref = BUILT_CHAIR;
	own.base = CHAIR_BASE;
	const auto built = StandInSeat::Tell(own);
	REQUIRE(built);
	CHECK(built->ref == 0u);
	CHECK(built->base == CHAIR_BASE);
}

TEST("seats: a seat travels in the status (protocol 31), and one while in power armor, or a bad one, is refused")
{
	using MT = Protocol::MessageType;
	static_assert(Protocol::VERSION >= 31);
	Protocol::PlayerStatus status{ .playerId = 3, .health = 80, .level = 7 };
	status.seat = StandInSeat::Tell(OnTheCouch());
	REQUIRE(status.seat);
	for (const auto type : { MT::kReportStatus, MT::kPlayerStatus }) {
		const auto packet = Protocol::Encode(status, type);
		CHECK((packet[16] & 32) != 0);  // bit 5 of the flags byte: a seat follows the frame, reference (and any parked frame)
		const auto out = Protocol::DecodePlayerStatus(packet);
		REQUIRE(out);
		CHECK(*out == status);
	}
	// None: the packet is as it was before seats.
	auto none = status;
	none.seat.reset();
	CHECK(Protocol::Encode(none, MT::kReportStatus).size() + 20 == Protocol::Encode(status, MT::kReportStatus).size());
	CHECK(Protocol::DecodePlayerStatus(Protocol::Encode(none, MT::kReportStatus)) == none);

	// With a parked frame too: both follow, the frame first.
	auto both = status;
	both.parked = Protocol::ParkedFrame{ .base = FRAME_BASE, .worldspace = 0x3C, .x = 10.0f, .y = 20.0f, .z = 30.0f, .heading = 1.0f };
	CHECK(Protocol::DecodePlayerStatus(Protocol::Encode(both, MT::kReportStatus)) == both);

	const auto refused = [&](auto a_change) {
		auto bad = status;
		a_change(bad);
		return !Protocol::DecodePlayerStatus(Protocol::Encode(bad, MT::kReportStatus));
	};
	CHECK(refused([](auto& a_s) { a_s.powerArmor = true; }));  // in power armor, the frame is the furniture
	CHECK(refused([](auto& a_s) { a_s.seat->base = 0; }));
	CHECK(refused([](auto& a_s) { a_s.seat->ref = BUILT_CHAIR; }));  // a reference only their game has
	CHECK(refused([](auto& a_s) { a_s.seat->ref = Protocol::PLAYER_REF_ID; }));
	CHECK(refused([](auto& a_s) { a_s.seat->x = 1.0e9f; }));
	CHECK(refused([](auto& a_s) { a_s.seat->z = std::numeric_limits<float>::infinity(); }));
	// Cut short: refused, not read past its end.
	auto packet = Protocol::Encode(status, MT::kReportStatus);
	packet.pop_back();
	CHECK(!Protocol::DecodePlayerStatus(packet));
}

TEST("seats: our copy of the furniture is the one they named when every game has it, else the nearest of its base where they said")
{
	const Seat couch{ .ref = COUCH, .base = COUCH_BASE, .x = 1200.0f, .y = -350.0f, .z = 64.0f };
	using C = StandInSeat::Candidate;
	// The one they named.
	const std::vector<C> named{ { .id = 0x0001E4C3, .base = COUCH_BASE, .x = 1200.0f, .y = -350.0f, .z = 64.0f, .usable = true }, { .id = COUCH, .base = COUCH_BASE, .x = 1260.0f, .y = -350.0f, .z = 64.0f, .usable = true } };
	CHECK(StandInSeat::Pick(couch, named) == COUCH);
	// Named, but another base here (a mod swapped it), or disabled or not loaded: the nearest of its base at their spot.
	std::vector<C> swapped = named;
	swapped[1].base = CHAIR_BASE;
	CHECK(StandInSeat::Pick(couch, swapped) == 0x0001E4C3u);
	swapped[1].base = COUCH_BASE;
	swapped[1].usable = false;
	CHECK(StandInSeat::Pick(couch, swapped) == 0x0001E4C3u);

	// Built in their settlement: our copy at the same spot, not a chair of another base nor one across the room.
	const Seat built{ .ref = 0, .base = CHAIR_BASE, .x = 500.0f, .y = 500.0f, .z = 0.0f };
	const std::vector<C> room{
		{ .id = 0xFF00AAAA, .base = COUCH_BASE, .x = 500.0f, .y = 500.0f, .z = 0.0f, .usable = true },
		{ .id = OUR_COPY, .base = CHAIR_BASE, .x = 500.4f, .y = 499.8f, .z = 0.0f, .usable = true },
		{ .id = 0xFF00BBBB, .base = CHAIR_BASE, .x = 520.0f, .y = 500.0f, .z = 0.0f, .usable = true },
		{ .id = 0xFF00CCCC, .base = CHAIR_BASE, .x = 900.0f, .y = 500.0f, .z = 0.0f, .usable = true },
	};
	CHECK(StandInSeat::Pick(built, room) == OUR_COPY);
	// Nothing of that base within MATCH_RADIUS: none (the stand-in is left standing beside it, not on it).
	const std::vector<C> far{ room[0], room[3] };
	CHECK(!StandInSeat::Pick(built, far));
	CHECK(!StandInSeat::Pick(built, {}));
}

TEST("seats: a friend who sits down has their stand-in sent in, held in it, and stood up as they get up")
{
	Facts facts{ .wanted = true, .found = true, .near = true };
	CHECK(StandInSeat::Choose(facts) == Act::kEnter);  // the snap
	facts.tries = 1;
	CHECK(StandInSeat::Choose(facts) == Act::kHold);  // the engine takes a frame or more
	facts.sinceTried = StandInSeat::ENTRY_WAIT - 1ms;
	CHECK(StandInSeat::Choose(facts) == Act::kHold);
	facts.sinceTried = StandInSeat::ENTRY_WAIT;
	CHECK(StandInSeat::Choose(facts) == Act::kEnter);  // activation
	facts.tries = StandInSeat::MAX_TRIES;
	CHECK(StandInSeat::Choose(facts) == Act::kHold);  // spent: left beside it
	facts.sinceTried = 1h;
	CHECK(StandInSeat::Choose(facts) == Act::kHold);

	// In it: held, whatever the tries.
	facts.inTarget = true;
	CHECK(StandInSeat::Choose(facts) == Act::kHold);
	// They get up: stood up, then it follows.
	facts.wanted = false;
	CHECK(StandInSeat::Choose(facts) == Act::kStand);
	facts.inTarget = false;
	CHECK(StandInSeat::Choose(facts) == Act::kFollow);

	// In other furniture (they moved to another seat): out of that first.
	CHECK(StandInSeat::Choose({ .wanted = true, .found = true, .near = true, .inOther = true }) == Act::kStand);
	CHECK(StandInSeat::Choose({ .wanted = false, .inOther = true }) == Act::kStand);
	// No such furniture here: held where it stood, not following them up onto it.
	CHECK(StandInSeat::Choose({ .wanted = true, .found = false, .near = true }) == Act::kHold);
	// Their states are nowhere near it (a status from before a door): it follows them.
	CHECK(StandInSeat::Choose({ .wanted = true, .found = true, .near = false }) == Act::kFollow);
	CHECK(StandInSeat::Choose({ .wanted = true, .found = false, .near = false }) == Act::kFollow);
	// In power armor its frame is its furniture: never touched here.
	CHECK(StandInSeat::Choose({ .wanted = true, .found = true, .near = true, .inArmor = true }) == Act::kFollow);
	CHECK(StandInSeat::Choose({ .wanted = false, .inArmor = true, .inTarget = true }) == Act::kFollow);
}

TEST("seats: the host sits on the couch for Piper's interview, and the friend's stand-in sits on it rather than standing on it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Hearer vm{ .takes = 1 };
	const Point walkingUp{ 1200.0f, -300.0f, 0.0f };  // in front of the couch
	const Point onIt{ 1200.0f, -350.0f, 64.0f };      // where their states put them as they sit: on the seat

	CHECK(vm.Step(0ms, walkingUp) == Act::kFollow);
	Send(f, 1, OnTheCouch());
	REQUIRE(vm.Hear(f, 2));
	REQUIRE(vm.seat);
	CHECK(vm.seat->ref == COUCH);
	// Held where it stood as they sat down, while the snap is under way; then in the couch, held there however long they sit.
	CHECK(vm.Step(100ms, onIt) == Act::kEnter);
	CHECK(vm.held);
	CHECK(Distance(vm.standIn, walkingUp) == 0.0f);
	for (int i = 2; i < 600; ++i) {
		CHECK(vm.Step(i * 100ms, onIt) == Act::kHold);
	}
	CHECK(vm.inSeat);
	CHECK(vm.tries == 1);

	// They get up.
	auto up = OnTheCouch();
	up.sitState = State(SitState::kWantToStand);
	Send(f, 1, up);
	REQUIRE(vm.Hear(f, 2));
	CHECK(!vm.seat);
	CHECK(vm.Step(61s, onIt) == Act::kStand);
	CHECK(!vm.inSeat);
	CHECK(!vm.held);
	CHECK(vm.Step(61100ms, walkingUp) == Act::kFollow);
	CHECK(Distance(vm.standIn, walkingUp) == 0.0f);
	CHECK(vm.stoodUp == 1);
}

TEST("seats: when the snap does not take, activation is tried, and when neither does the stand-in waits beside the furniture")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const Point walkingUp{ 1200.0f, -300.0f, 0.0f };
	const Point onIt{ 1200.0f, -350.0f, 64.0f };

	for (const std::optional<int> takes : { std::optional<int>{ 2 }, std::optional<int>{} }) {
		Hearer vm{ .takes = takes };
		vm.Step(0ms, walkingUp);
		Send(f, 1, OnTheCouch());
		REQUIRE(vm.Hear(f, 2));
		for (int i = 1; i <= 100; ++i) {
			vm.Step(i * 100ms, onIt);
		}
		CHECK(vm.tries == (takes ? *takes : StandInSeat::MAX_TRIES));
		CHECK(vm.inSeat == takes.has_value());
		CHECK(vm.held);
		// Never stood on the couch: in it, or where it stood as they sat down.
		CHECK(Distance(vm.standIn, takes ? vm.furniture : walkingUp) == 0.0f);
		// Up again.
		Send(f, 1, {});
		REQUIRE(vm.Hear(f, 2));
		CHECK(vm.Step(20s, walkingUp) == (takes ? Act::kStand : Act::kFollow));
		CHECK(!vm.held);
	}
}

TEST("seats: a friend who joins while the host sits sees their stand-in in the seat")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	Send(f, 1, OnTheCouch());
	// The server keeps her last status for whoever comes next.
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Hearer late{ .takes = 1 };
	REQUIRE(late.Hear(f, 2));
	REQUIRE(late.seat);
	CHECK(late.Step(0ms, { 1200.0f, -350.0f, 64.0f }) == Act::kEnter);
	CHECK(late.Step(100ms, { 1200.0f, -350.0f, 64.0f }) == Act::kHold);
	CHECK(late.inSeat);
}

TEST("seats: a seat in a settlement is found by base and place in a friend's game")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	auto own = OnTheCouch();
	own.ref = BUILT_CHAIR;
	own.base = CHAIR_BASE;
	own.x = 500.2f;
	own.y = 499.9f;
	own.z = 0.0f;
	Send(f, 1, own);
	Hearer vm;
	REQUIRE(vm.Hear(f, 2));
	REQUIRE(vm.seat);
	CHECK(vm.seat->ref == 0u);
	const std::vector<StandInSeat::Candidate> here{ { .id = OUR_COPY, .base = CHAIR_BASE, .x = 500.0f, .y = 500.0f, .z = 0.0f, .usable = true } };
	CHECK(StandInSeat::Pick(*vm.seat, here) == OUR_COPY);
}
