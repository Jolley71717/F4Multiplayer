// A friend's power armor frame where they left it: what the wearer's game tells of the frame it got out of last
// (ArmorFrames::TellParked, as Party sends it), through the real server, to what each friend's game makes of it
// (ArmorFrames::ChoosePark for the copy it shows there, as RemotePlayers::UpdateParked runs it each frame, and
// ArmorFrames::ChooseParkedOriginal / ChooseOriginal for its own copy of a pre-placed suit).

#include "ServerFixture.h"
#include "game/ArmorFrames.h"
#include "game/StandInRange.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using ArmorFrames::Original;
	using ArmorFrames::Park;
	using ArmorFrames::ParkFacts;
	using Protocol::ParkedFrame;

	constexpr std::uint32_t COMMONWEALTH = 0x0000003C;
	constexpr std::uint32_t VANILLA_FRAME = 0x0017F6E4;  // a frame every game has
	constexpr std::uint32_t MUSEUM_SUIT = 0x0003F2B4;    // a frame reference every game has (placed by a plugin)
	constexpr std::uint32_t MADE_HERE = 0xFF000F96;      // a frame reference the wearer's game made (a dropped suit)
	constexpr std::uint32_t INTERIOR = 0x0001A2B3;

	struct Point
	{
		float x = 0, y = 0, z = 0;
	};

	float Distance(const Point& a_a, const Point& a_b)
	{
		return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z);
	}

	// The 2026-10-13 report: the VM player left the suit at frame (-80620,89560); the stand-in that took their place
	// stood at (-80658,89626), where the copy was put.
	constexpr Point SANCTUARY_STATION{ -80620.0f, 89560.0f, 3000.0f };
	constexpr Point STEPPED_OUT{ -80658.0f, 89626.0f, 3000.0f };
	constexpr Point MUSEUM_ROOF{ -18000.0f, 78000.0f, 1500.0f };

	// The wearer's game: whether it is in power armor and the frame it got out of last, sent as Party sends it.
	struct Wearer
	{
		PeerId                 peer;
		bool                   on = false;
		ArmorFrames::LastFrame last;
		std::uint32_t          frameRef = 0;  // the frame it wears, when every game has it

		void Leave(std::uint32_t a_ref, const Point& a_at, bool a_enabled = true)
		{
			on = false;
			last = { .found = true, .disabled = !a_enabled, .frame = true, .ref = a_ref, .base = VANILLA_FRAME, .cell = 0, .worldspace = COMMONWEALTH, .x = a_at.x, .y = a_at.y, .z = a_at.z, .heading = 1.25f };
		}

		void Send(Fixture& a_f) const
		{
			Protocol::PlayerStatus status;
			status.level = 20;
			status.health = 100;
			status.powerArmor = on;
			status.powerArmorFrame = on ? VANILLA_FRAME : 0;
			status.powerArmorFrameRef = on ? frameRef : 0;
			status.parked = ArmorFrames::TellParked(on, last);
			a_f.fake->Deliver(peer, Protocol::Encode(status, MT::kReportStatus));
		}
	};

	// A copy of a frame standing in the hearer's world.
	struct Copy
	{
		std::uint32_t     base = VANILLA_FRAME;
		std::uint32_t     worldspace = COMMONWEALTH;
		Point             at;
		float             heading = 0.0f;
		bool              loaded = false;  // its 3D comes the step after it is placed
		bool              occupied = false;
		Clock::time_point placedAt;
	};

	// One friend's game: where its player stands, its own copy of the museum's suit, and what it keeps and shows of the
	// wearer's frame (RemotePlayers' SetPowerArmor and UpdateParked, UpdateOriginal's choice).
	struct Hearer
	{
		PeerId peer;
		Point  us{ SANCTUARY_STATION.x + 600.0f, SANCTUARY_STATION.y, SANCTUARY_STATION.z };
		bool   inside = false;
		Point  ours = MUSEUM_ROOF;  // its museum suit, where the world placed it
		bool   oursQuest = false;
		bool   oursHidden = false;
		std::optional<Original> parkedChoice;  // the last choice for its museum suit as the frame they left parked

		bool                       on = false;
		std::uint32_t              frameRef = 0;
		std::optional<ParkedFrame> parked;
		std::optional<ParkedFrame> taken;
		std::optional<Copy>        parkedCopy;
		std::optional<Copy>        leftCopy;  // the copy left where the stand-in stood as they got out (LeaveArmor)
		bool                       away = false;
		int                        placed = 0;
		int                        deleted = 0;
		int                        droppedLeft = 0;

		// Takes what the server sent through the next status, as Party::ApplyStatus hands it to SetPowerArmor.
		bool Hear(Fixture& a_f)
		{
			const auto packets = a_f.fake->TakeThrough(peer, Type(MT::kPlayerStatus));
			if (packets.empty() || packets.back()[0] != Type(MT::kPlayerStatus)) {
				return false;
			}
			const auto status = Protocol::DecodePlayerStatus(packets.back());
			REQUIRE(status);
			if (status->powerArmor && status->powerArmorFrameRef != 0 && (!on || status->powerArmorFrameRef != frameRef)) {
				away = parked && parked->ref == status->powerArmorFrameRef && (oursHidden || parkedChoice == Original::kNotLoaded);
			}
			on = status->powerArmor;
			frameRef = status->powerArmorFrameRef;
			parked = on ? std::nullopt : status->parked;
			if (on) {
				taken.reset();
			}
			return true;
		}

		bool InView(const ParkedFrame& a_parked) const
		{
			if (inside || a_parked.cell != 0) {
				return inside && a_parked.cell == INTERIOR;
			}
			return a_parked.worldspace == COMMONWEALTH && StandInRange::Visible(a_parked.x, a_parked.y, us.x, us.y, StandInRange::DEFAULT_GRIDS);
		}

		bool OursThere(const ParkedFrame& a_parked) const
		{
			return a_parked.ref == MUSEUM_SUIT && !inside && Distance(ours, { a_parked.x, a_parked.y, a_parked.z }) <= ArmorFrames::ORIGINAL_RADIUS;
		}

		// One frame of the hearer's game.
		Park Step(Clock::time_point a_now)
		{
			ParkFacts facts;
			facts.told = parked.has_value();
			const Point spot = parked ? Point{ parked->x, parked->y, parked->z } : Point{};
			if (parked) {
				facts.inView = InView(*parked);
				facts.oursThere = OursThere(*parked);
				facts.taken = taken == parked;
			}
			if (parkedCopy) {
				facts.exists = true;
				facts.sameBase = parked && parkedCopy->base == parked->base;
				facts.occupied = parkedCopy->occupied;
				facts.loaded = parkedCopy->loaded;
				facts.samePlace = parked && parkedCopy->worldspace == parked->worldspace;
				facts.distance = parked ? Distance(parkedCopy->at, spot) : 0.0f;
				facts.turn = parked ? std::abs(parkedCopy->heading - parked->heading) : 0.0f;
				facts.sincePlaced = a_now - parkedCopy->placedAt;
			}
			facts.leftCopy = parked && leftCopy && !on && leftCopy->base == parked->base && !leftCopy->occupied;

			const auto park = ArmorFrames::ChoosePark(facts);
			switch (park) {
			case Park::kPlace:
				parkedCopy = Copy{ .base = parked->base, .worldspace = parked->worldspace, .at = spot, .heading = parked->heading, .placedAt = a_now };
				++placed;
				break;
			case Park::kAdopt:
				parkedCopy = leftCopy;
				leftCopy.reset();
				++placed;
				break;
			case Park::kMove:
				parkedCopy->at = spot;
				parkedCopy->heading = parked->heading;
				break;
			case Park::kReplace:
			case Park::kDelete:
				parkedCopy.reset();
				++deleted;
				break;
			case Park::kAbandon:
				taken = parked;
				parkedCopy.reset();
				break;
			case Park::kDropLeft:
				leftCopy.reset();
				++droppedLeft;
				break;
			case Park::kNone:
			case Park::kShow:
				break;
			}
			if (parkedCopy) {
				parkedCopy->loaded = true;  // its 3D is in by the next frame
			}
			return park;
		}

		// Its own museum suit, looked at for the frame they left parked or wear (UpdateOriginal).
		Original LookAtOurs(const std::optional<Point>& a_wornAt = std::nullopt)
		{
			ArmorFrames::OriginalFacts facts{ .found = true, .base = VANILLA_FRAME, .frame = true, .quest = oursQuest, .loaded = !inside && Distance(us, ours) < 9000.0f };
			Original choice;
			if (on) {
				facts.away = away;
				if (a_wornAt) {
					facts.fromSpot = Distance(ours, *a_wornAt);
				}
				choice = ArmorFrames::ChooseOriginal(frameRef, VANILLA_FRAME, facts);
			} else {
				if (parked) {
					facts.fromSpot = Distance(ours, { parked->x, parked->y, parked->z });
				}
				choice = ArmorFrames::ChooseParkedOriginal(parked ? parked->ref : 0, VANILLA_FRAME, facts);
			}
			parkedChoice = on ? std::nullopt : std::optional{ choice };
			if (choice == Original::kHide) {
				oursHidden = true;
			} else if (choice != Original::kNotLoaded) {
				oursHidden = false;
			}
			return choice;
		}

		// Runs its frames for a while.
		void Run(Clock::time_point a_from, std::chrono::milliseconds a_for)
		{
			for (auto t = a_from; t <= a_from + a_for; t += 100ms) {
				Step(t);
			}
		}
	};
}

TEST("parked frames: the frame told is the one got out of last, only while out of it and once it stands in the world again")
{
	ArmorFrames::LastFrame last{ .found = true, .frame = true, .ref = MUSEUM_SUIT, .base = VANILLA_FRAME, .worldspace = COMMONWEALTH, .x = -80620.4f, .y = 89559.6f, .z = 3000.2f, .heading = 1.2345f };
	const auto told = ArmorFrames::TellParked(false, last);
	REQUIRE(told);
	CHECK(told->base == VANILLA_FRAME);
	CHECK(told->ref == MUSEUM_SUIT);
	CHECK(told->worldspace == COMMONWEALTH);
	CHECK(told->cell == 0u);
	// To the unit, and the heading to a hundredth: a frame nudged by a hair sends no new status.
	CHECK(told->x == -80620.0f);
	CHECK(told->y == 89560.0f);
	CHECK(told->z == 3000.0f);
	CHECK(std::abs(told->heading - 1.23f) < 1e-5f);
	auto nudged = last;
	nudged.x += 0.3f;
	CHECK(ArmorFrames::TellParked(false, nudged) == told);

	// In power armor, or while the engine still has the frame disabled (worn, or the exit not done): none.
	CHECK(!ArmorFrames::TellParked(true, last));
	auto facts = last;
	facts.disabled = true;
	CHECK(!ArmorFrames::TellParked(false, facts));
	// None used yet, gone, or not a frame.
	CHECK(!ArmorFrames::TellParked(false, {}));
	facts = last;
	facts.frame = false;
	CHECK(!ArmorFrames::TellParked(false, facts));
	// A suit dropped in their game: its place goes, its ID (another reference, or none, elsewhere) does not.
	facts = last;
	facts.ref = MADE_HERE;
	const auto dropped = ArmorFrames::TellParked(false, facts);
	REQUIRE(dropped);
	CHECK(dropped->ref == 0u);
	// Inside: the cell, not a worldspace. In no cell at all (still attaching), or somewhere no game can hold: none.
	facts = last;
	facts.cell = INTERIOR;
	facts.worldspace = 0;
	REQUIRE(ArmorFrames::TellParked(false, facts));
	CHECK(ArmorFrames::TellParked(false, facts)->cell == INTERIOR);
	facts.cell = 0;
	CHECK(!ArmorFrames::TellParked(false, facts));
	facts = last;
	facts.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(!ArmorFrames::TellParked(false, facts));
}

TEST("parked frames: a parked frame travels in the status, and a status that has one while in power armor, or a bad one, is refused")
{
	using MT = Protocol::MessageType;
	Protocol::PlayerStatus status{ .playerId = 3, .health = 80, .level = 7 };
	status.parked = ParkedFrame{ .base = VANILLA_FRAME, .ref = MUSEUM_SUIT, .worldspace = COMMONWEALTH, .x = -80620.0f, .y = 89560.0f, .z = 3000.0f, .heading = 1.25f };
	for (const auto type : { MT::kReportStatus, MT::kPlayerStatus }) {
		const auto packet = Protocol::Encode(status, type);
		CHECK((packet[16] & 16) != 0);  // bit 4 of the flags byte: a parked frame follows the frame and reference
		const auto out = Protocol::DecodePlayerStatus(packet);
		REQUIRE(out);
		CHECK(*out == status);
	}
	// None: the packet is as it was before parked frames.
	auto none = status;
	none.parked.reset();
	CHECK(Protocol::Encode(none, MT::kReportStatus).size() + 32 == Protocol::Encode(status, MT::kReportStatus).size());
	CHECK(Protocol::DecodePlayerStatus(Protocol::Encode(none, MT::kReportStatus)) == none);

	const auto refused = [&](auto a_change) {
		auto bad = status;
		a_change(bad);
		return !Protocol::DecodePlayerStatus(Protocol::Encode(bad, MT::kReportStatus));
	};
	CHECK(refused([](auto& a_s) { a_s.powerArmor = true; }));  // in power armor, nothing is parked
	CHECK(refused([](auto& a_s) { a_s.parked->base = 0; }));
	CHECK(refused([](auto& a_s) { a_s.parked->ref = MADE_HERE; }));
	CHECK(refused([](auto& a_s) { a_s.parked->ref = Protocol::PLAYER_REF_ID; }));
	CHECK(refused([](auto& a_s) { a_s.parked->worldspace = 0; }));  // nowhere
	CHECK(refused([](auto& a_s) { a_s.parked->cell = INTERIOR; }));  // inside and outside at once
	CHECK(refused([](auto& a_s) { a_s.parked->x = 1.0e9f; }));
	CHECK(refused([](auto& a_s) { a_s.parked->heading = std::numeric_limits<float>::infinity(); }));
	// Cut short: refused, not read past its end.
	auto packet = Protocol::Encode(status, MT::kReportStatus);
	packet.pop_back();
	CHECK(!Protocol::DecodePlayerStatus(packet));
}

TEST("parked frames: the copy left as a friend gets out goes to where their frame stands, not where their stand-in stepped out to, and stays")
{
	// The 2026-10-13 report: the copy was placed where the stand-in stood after stepping out, 76 units off the frame,
	// and deleted 4 s later.
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer vm{ .peer = 2, .on = true };
	Hearer host{ .peer = 1 };
	const auto t0 = Clock::now();
	vm.Send(f);
	REQUIRE(host.Hear(f));
	CHECK(host.on);
	CHECK(Distance(SANCTUARY_STATION, STEPPED_OUT) > 70.0f);

	// Out: the engine has not given the frame back to the world yet (it is still disabled), so no frame is told, and
	// the stand-in, replaced, leaves a copy where it stood (RemotePlayers::LeaveArmor).
	vm.Leave(MADE_HERE, SANCTUARY_STATION, false);
	vm.Send(f);
	REQUIRE(host.Hear(f));
	CHECK(!host.on);
	CHECK(!host.parked);
	host.leftCopy = Copy{ .at = STEPPED_OUT, .heading = 1.0f, .placedAt = t0 };
	CHECK(host.Step(t0) == Park::kNone);

	// A moment later the frame stands where they got out, and the next status says where: the left copy is taken over
	// and moved there, not another one placed.
	vm.Leave(MADE_HERE, SANCTUARY_STATION);
	vm.Send(f);
	REQUIRE(host.Hear(f));
	REQUIRE(host.parked);
	CHECK(host.parked->ref == 0u);  // a suit of their own game's
	CHECK(host.Step(t0 + 300ms) == Park::kAdopt);
	CHECK(!host.leftCopy);
	CHECK(host.Step(t0 + 400ms) == Park::kMove);
	REQUIRE(host.parkedCopy);
	CHECK(Distance(host.parkedCopy->at, SANCTUARY_STATION) <= ArmorFrames::PARK_FOLLOW);
	CHECK(host.Step(t0 + 500ms) == Park::kShow);

	// It stays as long as the frame stands there: well past the few seconds a left copy used to stand.
	host.Run(t0 + 500ms, 60s);
	REQUIRE(host.parkedCopy);
	CHECK(host.placed == 1);
	CHECK(host.deleted == 0);

	// Back in: theirs is no longer parked, and the stand-in gets a copy of its own to wear (UpdatePowerArmor).
	vm.on = true;
	vm.Send(f);
	REQUIRE(host.Hear(f));
	CHECK(host.Step(t0 + 61s) == Park::kDelete);
	CHECK(!host.parkedCopy);
}

TEST("parked frames: told together with getting out, the copy goes straight to where the frame stands")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer vm{ .peer = 2, .on = true };
	Hearer host{ .peer = 1 };
	const auto t0 = Clock::now();
	vm.Send(f);
	REQUIRE(host.Hear(f));
	vm.Leave(MADE_HERE, SANCTUARY_STATION);
	vm.Send(f);
	REQUIRE(host.Hear(f));
	// UpdateParked runs before the stand-in is replaced: the copy is placed at the frame, and LeaveArmor, finding it
	// there, leaves none where the stand-in stood.
	CHECK(host.Step(t0) == Park::kPlace);
	REQUIRE(host.parkedCopy);
	CHECK(Distance(host.parkedCopy->at, SANCTUARY_STATION) == 0.0f);
	CHECK(host.parkedCopy->heading == 1.25f);
	CHECK(host.Step(t0 + 100ms) == Park::kShow);
}

TEST("parked frames: a friend's suit parked in a station far from where it was placed shows there, and our own copy of it stays hidden")
{
	// The 2026-10-12 report: the host's suit sat in a Sanctuary power armor station, the VM's rack was empty, and the
	// VM's copy of that suit stood where the world placed it, on the museum roof.
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer host{ .peer = 1, .on = true, .frameRef = MUSEUM_SUIT };
	Hearer vm{ .peer = 2 };
	const auto t0 = Clock::now();

	// She wears the museum's suit (the VM hid its own while she did, at the roof: ChooseOriginal) and parks it at
	// Sanctuary.
	host.Send(f);
	REQUIRE(vm.Hear(f));
	vm.us = MUSEUM_ROOF;
	CHECK(vm.LookAtOurs(MUSEUM_ROOF) == Original::kHide);
	vm.us = { SANCTUARY_STATION.x + 600.0f, SANCTUARY_STATION.y, SANCTUARY_STATION.z };
	host.Leave(MUSEUM_SUIT, SANCTUARY_STATION);
	host.Send(f);
	REQUIRE(vm.Hear(f));
	REQUIRE(vm.parked);
	CHECK(vm.parked->ref == MUSEUM_SUIT);

	// The VM, at Sanctuary, sees her suit in the station.
	CHECK(vm.Step(t0) == Park::kPlace);
	vm.Run(t0 + 100ms, 10s);
	REQUIRE(vm.parkedCopy);
	CHECK(Distance(vm.parkedCopy->at, SANCTUARY_STATION) == 0.0f);
	// Its own museum suit is the same suit, taken away in her world: hidden once it is loaded, not shown again on the roof.
	CHECK(vm.LookAtOurs() == Original::kNotLoaded);
	vm.us = MUSEUM_ROOF;
	CHECK(vm.LookAtOurs() == Original::kHide);
	CHECK(vm.oursHidden);
	// Out of sight of the station, the copy goes; back, it stands there again.
	CHECK(vm.Step(t0 + 11s) == Park::kDelete);
	CHECK(!vm.parkedCopy);
	CHECK(vm.Step(t0 + 12s) == Park::kNone);
	vm.us = { SANCTUARY_STATION.x, SANCTUARY_STATION.y + 2000.0f, SANCTUARY_STATION.z };
	CHECK(vm.Step(t0 + 20s) == Park::kPlace);

	// She gets in again at the station, far from where the VM's suit stands: it was hidden as her parked suit, so it
	// stays hidden for this wear (without that, ChooseOriginal finds it not where she got in and shows it).
	host.on = true;
	host.last.disabled = true;
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.away);
	CHECK(vm.Step(t0 + 21s) == Park::kDelete);  // her stand-in wears its own copy now
	vm.us = MUSEUM_ROOF;
	CHECK(vm.LookAtOurs(SANCTUARY_STATION) == Original::kHide);
	vm.away = false;
	CHECK(vm.LookAtOurs(SANCTUARY_STATION) == Original::kElsewhere);
}

TEST("parked frames: our copy of a suit a friend parked elsewhere stays hidden when they get back in, though we were never near it meanwhile")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer host{ .peer = 1, .on = true, .frameRef = MUSEUM_SUIT };
	Hearer vm{ .peer = 2 };
	host.Send(f);
	REQUIRE(vm.Hear(f));
	// She parks the museum's suit at Sanctuary; the VM stays there, its own suit on the roof never loaded, so never hidden.
	host.Leave(MUSEUM_SUIT, SANCTUARY_STATION);
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.LookAtOurs() == Original::kNotLoaded);
	CHECK(!vm.oursHidden);
	// She gets in again at the station: it is still the suit that would be hidden, and is once the VM comes near it
	// (without that, ChooseOriginal finds it not where she got in and shows it: two of one suit).
	host.on = true;
	host.last.disabled = true;
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.away);
	vm.us = MUSEUM_ROOF;
	CHECK(vm.LookAtOurs(SANCTUARY_STATION) == Original::kHide);
}

TEST("parked frames: a friend who joins later sees a suit parked before they came")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	Wearer host{ .peer = 1, .on = true, .frameRef = MUSEUM_SUIT };
	host.Send(f);
	host.Leave(MUSEUM_SUIT, SANCTUARY_STATION);
	host.Send(f);
	// The server keeps her last status for whoever comes next.
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Hearer late{ .peer = 2 };
	REQUIRE(late.Hear(f));
	REQUIRE(late.parked);
	CHECK(late.Step(Clock::now()) == Park::kPlace);
	REQUIRE(late.parkedCopy);
	CHECK(Distance(late.parkedCopy->at, SANCTUARY_STATION) == 0.0f);
	// Its museum suit was never hidden for a wear of hers, and is hidden all the same: the same suit, parked elsewhere.
	late.us = MUSEUM_ROOF;
	CHECK(late.LookAtOurs() == Original::kHide);
}

TEST("parked frames: a suit got out of where our own copy of it stands is ours to show, with no copy on it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer host{ .peer = 1, .on = true, .frameRef = MUSEUM_SUIT };
	Hearer vm{ .peer = 2, .us = MUSEUM_ROOF };
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.LookAtOurs(MUSEUM_ROOF) == Original::kHide);
	// In and out on the roof: hers stands a few units from where ours stands.
	// Her stand-in, stepped out some 76 units off, left a copy where it stood before her frame was told.
	host.Leave(MUSEUM_SUIT, { MUSEUM_ROOF.x + 30.0f, MUSEUM_ROOF.y, MUSEUM_ROOF.z });
	host.Send(f);
	REQUIRE(vm.Hear(f));
	vm.leftCopy = Copy{ .at = { MUSEUM_ROOF.x + 30.0f, MUSEUM_ROOF.y + 76.0f, MUSEUM_ROOF.z }, .placedAt = Clock::now() };
	// Ours stands for hers: the left copy goes at once rather than standing beside it as a second suit.
	CHECK(vm.Step(Clock::now()) == Park::kDropLeft);
	CHECK(!vm.leftCopy);
	CHECK(vm.droppedLeft == 1);
	CHECK(vm.Step(Clock::now()) == Park::kNone);
	CHECK(!vm.parkedCopy);
	CHECK(vm.LookAtOurs() == Original::kThere);
	CHECK(!vm.oursHidden);
	// A quest of ours holds it (the museum's suit is the main quest's objective): never hidden, wherever hers stands.
	vm.oursQuest = true;
	host.Leave(MUSEUM_SUIT, SANCTUARY_STATION);
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.LookAtOurs() == Original::kQuest);
	CHECK(!vm.oursHidden);
}

TEST("parked frames: a parked copy somebody here gets into is theirs, and none is placed on top of it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Wearer host{ .peer = 1 };
	Hearer vm{ .peer = 2 };
	const auto t0 = Clock::now();
	host.Leave(MADE_HERE, SANCTUARY_STATION);
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.Step(t0) == Park::kPlace);
	vm.parkedCopy->occupied = true;
	CHECK(vm.Step(t0 + 100ms) == Park::kAbandon);
	CHECK(!vm.parkedCopy);
	vm.Run(t0 + 200ms, 5s);
	CHECK(!vm.parkedCopy);
	CHECK(vm.placed == 1);
	// She parks it again somewhere (in again, out again): shown once more.
	host.on = true;
	host.Send(f);
	REQUIRE(vm.Hear(f));
	host.Leave(MADE_HERE, { SANCTUARY_STATION.x + 500.0f, SANCTUARY_STATION.y, SANCTUARY_STATION.z });
	host.Send(f);
	REQUIRE(vm.Hear(f));
	CHECK(vm.Step(t0 + 6s) == Park::kPlace);
}

TEST("parked frames: the parked copy is moved when the frame is, placed again when far off, in another place or never loaded")
{
	const ParkFacts shown{ .told = true, .inView = true, .exists = true, .sameBase = true, .loaded = true, .samePlace = true, .distance = 0.0f, .sincePlaced = 30s };
	CHECK(ArmorFrames::ChoosePark(shown) == Park::kShow);
	auto facts = shown;
	facts.distance = ArmorFrames::PARK_FOLLOW + 0.5f;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kMove);
	facts = shown;
	facts.turn = ArmorFrames::PARK_TURN * 2.0f;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kMove);
	facts.distance = ArmorFrames::MOVE_LIMIT + 1.0f;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kReplace);
	facts = shown;
	facts.samePlace = false;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kReplace);
	facts = shown;
	facts.samePlace.reset();  // still attaching: kept
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kShow);
	facts = shown;
	facts.sameBase = false;  // they parked another suit
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kReplace);
	facts = shown;
	facts.loaded = false;
	facts.sincePlaced = ArmorFrames::LOAD_LIMIT;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kNone);
	facts.sincePlaced = ArmorFrames::LOAD_LIMIT + 1ms;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kReplace);
	// Someone here in it wins over everything else: never moved or deleted from under them.
	facts = shown;
	facts.occupied = true;
	facts.told = false;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kAbandon);
	// Nothing told, nothing placed.
	CHECK(ArmorFrames::ChoosePark({}) == Park::kNone);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = false }) == Park::kNone);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .oursThere = true }) == Park::kNone);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .oursThere = true, .leftCopy = true }) == Park::kDropLeft);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .taken = true }) == Park::kNone);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .leftCopy = true }) == Park::kAdopt);
}

TEST("parked frames: no parked copy stands in our player's way out of the frame a loaded save had them in, and one comes back once they are out")
{
	// The frame lent to us, its loan over with the load, is its owner's parked one again, made in their game: Spawns
	// withholds its copy there (FrameLoans::KeepsClear), so oursThere is false, and the parked copy must not stand in for it.
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .keptClear = true }) == Park::kNone);
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true, .leftCopy = true, .keptClear = true }) == Park::kNone);
	const ParkFacts shown{ .told = true, .inView = true, .exists = true, .sameBase = true, .loaded = true, .samePlace = true, .distance = 0.0f, .sincePlaced = 30s };
	auto            facts = shown;
	facts.keptClear = true;
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kDelete);
	facts.occupied = true;  // never from under someone in it
	CHECK(ArmorFrames::ChoosePark(facts) == Park::kAbandon);
	// Out of it: placed again.
	CHECK(ArmorFrames::ChoosePark({ .told = true, .inView = true }) == Park::kPlace);
	CHECK(ArmorFrames::ChoosePark(shown) == Park::kShow);
}

TEST("parked frames: our copy of a parked suit is hidden only when it is that suit, untouched in our world, and nobody here needs it")
{
	const ArmorFrames::OriginalFacts here{ .found = true, .base = VANILLA_FRAME, .frame = true, .loaded = true, .fromSpot = std::nullopt };
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, here) == Original::kHide);
	auto facts = here;
	facts.fromSpot = 5000.0f;  // in the same place, far off
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kHide);
	facts.fromSpot = ArmorFrames::ORIGINAL_RADIUS;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kThere);
	// A suit of their own game's, or none.
	CHECK(ArmorFrames::ChooseParkedOriginal(0, VANILLA_FRAME, here) == Original::kNone);
	CHECK(ArmorFrames::ChooseParkedOriginal(MADE_HERE, VANILLA_FRAME, here) == Original::kNone);
	// Not ours to hide: our player's own, someone here in it, moved here since it was placed, or another frame by that ID.
	facts = here;
	facts.ours = true;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kOurs);
	facts = here;
	facts.inUse = true;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kInUse);
	facts = here;
	facts.fromStart = 300.0f;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kUsed);
	facts = here;
	facts.base = 0x00140F1A;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kMissing);
	facts = here;
	facts.loaded = false;
	CHECK(ArmorFrames::ChooseParkedOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kNotLoaded);

	// Worn again after it was hidden as their parked suit: hidden wherever they got in, by the same guards.
	facts = here;
	facts.away = true;
	facts.fromSpot = 50000.0f;
	CHECK(ArmorFrames::ChooseOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kHide);
	facts.ours = true;
	CHECK(ArmorFrames::ChooseOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kOurs);
	facts = here;
	facts.away = true;
	facts.fromStart = 300.0f;
	CHECK(ArmorFrames::ChooseOriginal(MUSEUM_SUIT, VANILLA_FRAME, facts) == Original::kUsed);
}
