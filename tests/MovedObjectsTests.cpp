// Loose objects moved by hand, shared: the server's book of who moves what (common/ObjectMovers.h), the game-free rules
// of src/game/ObjectShare.h, the wire's checks of a peer's messages, and the server end to end.

#include "ServerFixture.h"

#include "ObjectMovers.h"
#include "game/ObjectShare.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <vector>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using ObjectShare::Pose;
	using ObjectShare::Vec3;
	using Verdict = ObjectMovers::Verdict;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr std::uint32_t BROOM = 0x0001A001;
	constexpr std::uint32_t TIRE = 0x0001A002;
	constexpr std::uint32_t RUNTIME = 0xFF00A001;
	constexpr std::uint32_t SANCTUARY = 0x0000003C;  // a worldspace

	bool Near(float a_a, float a_b, float a_within = 0.01f) { return std::fabs(a_a - a_b) <= a_within; }
	bool Near(const Vec3& a_a, const Vec3& a_b, float a_within = 0.01f)
	{
		return Near(a_a[0], a_b[0], a_within) && Near(a_a[1], a_b[1], a_within) && Near(a_a[2], a_b[2], a_within);
	}

	Protocol::ObjectHold Hold(std::uint32_t a_ref, std::uint32_t a_sequence, Vec3 a_offset = { 0, 80, 100 })
	{
		return { .refId = a_ref, .sequence = a_sequence, .offset = a_offset, .rotation = { 0.1f, 0.0f, 0.5f } };
	}

	Protocol::ObjectRest Rest(std::uint32_t a_ref, std::uint8_t a_kind, std::uint32_t a_sequence = 0, Vec3 a_at = { 100, 200, 30 }, Vec3 a_velocity = {})
	{
		return { .refId = a_ref, .kind = a_kind, .sequence = a_sequence, .worldspace = SANCTUARY, .position = a_at, .rotation = { 0, 0, 1 }, .velocity = a_velocity };
	}

	Pose At(Vec3 a_position, Vec3 a_angles = {})
	{
		return { .worldspace = SANCTUARY, .position = a_position, .angles = a_angles };
	}

	// A world for Carry: where each object is.
	struct World
	{
		std::unordered_map<std::uint32_t, Pose> poses;
		ObjectShare::Carry::PoseOf              PoseOf()
		{
			return [this](std::uint32_t a_ref) -> std::optional<Pose> {
				const auto it = poses.find(a_ref);
				return it == poses.end() ? std::nullopt : std::optional{ it->second };
			};
		}
	};
}

// --- the server's book --------------------------------------------------------------------------------------------

TEST("moved objects book: the first grab wins; the other is refused and told the holder's hold")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	CHECK(book.Hold(Hold(BROOM, 1), 1, t) == Verdict::kRelay);
	CHECK(book.Hold(Hold(BROOM, 1), 2, t + 10ms) == Verdict::kRefused);
	const auto holder = book.Holder(BROOM, t + 10ms);
	REQUIRE(holder);
	CHECK(holder->playerId == 1);
	CHECK(holder->refId == BROOM);
	CHECK(book.Owner(BROOM, t) == 1);
	// Nor may the other report it come to rest, or let go of it.
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest), 2, t + 20ms) == Verdict::kRefused);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 5), 2, t + 20ms) == Verdict::kRefused);
	// Another object is anyone's.
	CHECK(book.Hold(Hold(TIRE, 1), 2, t + 10ms) == Verdict::kRelay);
}

TEST("moved objects book: a rest refused for another player's is told theirs; none while they hold it or it flies from their hands")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	// Two games claim the tire's rest (a friend stood in the pile a grenade threw about): the first wins, and the second
	// is told the first's, as the first player's.
	CHECK(book.Rest(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 100, 0, 0 }), 1, t) == Verdict::kRelay);
	CHECK(book.Rest(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 140, 0, 0 }), 2, t + 10ms) == Verdict::kRefused);
	const auto theirs = book.LastRest(TIRE, t + 10ms);
	REQUIRE(theirs);
	CHECK(theirs->playerId == 1);
	CHECK(theirs->refId == TIRE);
	CHECK(theirs->kind == Protocol::ObjectRestKind::kRest);
	CHECK(Near(theirs->position, { 100, 0, 0 }));
	// The first player's newer rest is what the next refused one is told; past SETTLED there is nothing to tell, and the
	// second player's rest goes.
	CHECK(book.Rest(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 110, 0, 0 }), 1, t + 1s) == Verdict::kRelay);
	CHECK(Near(book.LastRest(TIRE, t + 2s)->position, { 110, 0, 0 }));
	CHECK(!book.LastRest(TIRE, t + 1s + ObjectMovers::SETTLED));
	CHECK(book.Rest(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 140, 0, 0 }), 2, t + 1s + ObjectMovers::SETTLED) == Verdict::kRelay);
	CHECK(book.LastRest(TIRE, t + 1s + ObjectMovers::SETTLED)->playerId == 2);
	// Held, or flying from a let-go, its rest is still to come: a refused rest of it is told nothing (a hold: the
	// holder's hold, as a refused grab is).
	CHECK(book.Hold(Hold(BROOM, 1), 1, t) == Verdict::kRelay);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest), 2, t + 10ms) == Verdict::kRefused);
	CHECK(!book.LastRest(BROOM, t + 10ms));
	CHECK(book.Holder(BROOM, t + 10ms));
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 300, 50 }), 1, t + 20ms) == Verdict::kRelay);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest), 2, t + 30ms) == Verdict::kRefused);
	CHECK(!book.LastRest(BROOM, t + 30ms));
	CHECK(!book.Holder(BROOM, t + 30ms));
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 50, 60, 0 }), 1, t + 2s) == Verdict::kRelay);
	CHECK(Near(book.LastRest(BROOM, t + 2s)->position, { 50, 60, 0 }));
	// Grabbed again: the rest is over.
	CHECK(book.Hold(Hold(BROOM, 2), 2, t + 2s + 10ms) == Verdict::kRelay);
	CHECK(!book.LastRest(BROOM, t + 2s + 10ms));
}

TEST("moved objects book: a hold older than the holder's newest, or than their let-go, is dropped")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	CHECK(book.Hold(Hold(BROOM, 5), 1, t) == Verdict::kRelay);
	CHECK(book.Hold(Hold(BROOM, 4), 1, t + 10ms) == Verdict::kDropped);
	CHECK(book.Hold(Hold(BROOM, 5), 1, t + 10ms) == Verdict::kDropped);
	CHECK(book.Hold(Hold(BROOM, 6), 1, t + 20ms) == Verdict::kRelay);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 6), 1, t + 30ms) == Verdict::kRelay);
	CHECK(!book.Holder(BROOM, t + 30ms));
	CHECK(book.Hold(Hold(BROOM, 6), 1, t + 40ms) == Verdict::kDropped);  // overtook the let-go on the way
	CHECK(book.Hold(Hold(BROOM, 7), 1, t + 50ms) == Verdict::kRelay);    // grabbed again
	// Sequence numbers wrap.
	CHECK(book.Hold(Hold(TIRE, 0xFFFFFFFF), 1, t) == Verdict::kRelay);
	CHECK(book.Hold(Hold(TIRE, 0), 1, t + 10ms) == Verdict::kRelay);
}

TEST("moved objects book: a let-go only from the holder; after it a friend may take the object")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1), 1, t) == Verdict::kRefused);  // never held
	CHECK(book.Hold(Hold(BROOM, 1), 1, t) == Verdict::kRelay);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1), 1, t + 1s) == Verdict::kRelay);
	// Flying: its rest is still the thrower's to say, not the friend's.
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest), 2, t + 2s) == Verdict::kRefused);
	CHECK(book.Rest(Rest(BROOM, Protocol::ObjectRestKind::kRest), 1, t + 2s) == Verdict::kRelay);
	// Handed over: the friend grabs it.
	CHECK(book.Hold(Hold(BROOM, 1), 2, t + 3s) == Verdict::kRelay);
	CHECK(book.Owner(BROOM, t + 3s) == 2);
	CHECK(book.Hold(Hold(BROOM, 9), 1, t + 3s) == Verdict::kRefused);
}

TEST("moved objects book: a holder gone quiet, or gone, lets the object go")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	CHECK(book.Hold(Hold(BROOM, 1), 1, t) == Verdict::kRelay);
	CHECK(book.Hold(Hold(BROOM, 1), 2, t + ObjectMovers::HOLD_TIMEOUT + 1ms) == Verdict::kRelay);
	CHECK(book.Owner(BROOM, t + ObjectMovers::HOLD_TIMEOUT + 1ms) == 2);

	CHECK(book.Hold(Hold(TIRE, 1), 2, t) == Verdict::kRelay);
	book.Leave(2);
	CHECK(book.Owner(BROOM, t) == 0);
	CHECK(book.Hold(Hold(TIRE, 1), 1, t + 10ms) == Verdict::kRelay);

	book.Prune(t + 1h);
	CHECK(book.Size() == 0);
}

TEST("moved objects book: at most MAX_OBJECTS at once")
{
	ObjectMovers::Book book;
	const auto         t = Clock::now();
	for (std::uint32_t i = 0; i < ObjectMovers::MAX_OBJECTS; ++i) {
		REQUIRE(book.Hold(Hold(0x00100000 + i, 1), 1, t) == Verdict::kRelay);
	}
	CHECK(book.Hold(Hold(BROOM, 1), 2, t + 1ms) == Verdict::kDropped);
	CHECK(book.Size() == ObjectMovers::MAX_OBJECTS);
	// Once they are over, there is room again.
	CHECK(book.Hold(Hold(BROOM, 1), 2, t + ObjectMovers::HOLD_TIMEOUT + 1ms) == Verdict::kRelay);
	CHECK(book.Size() == 1);
}

// --- the session's places -----------------------------------------------------------------------------------------
// A move was passed on once, to the games that had the object loaded then; a friend who loaded the cell later found it
// where their own save had it. The session keeps where each object came to rest last, for the games that load it later.

TEST("moved objects places: an object's last rest is kept after its move is over, naming nobody; a let-go is not")
{
	ObjectMovers::Book   book;
	ObjectMovers::Places places;
	const auto           t = Clock::now();
	auto                 rest = Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 10, 20, 30 });
	rest.playerId = 1;
	REQUIRE(book.Rest(rest, 1, t) == Verdict::kRelay);
	places.Rested(rest);
	book.Prune(t + ObjectMovers::FLIGHT + ObjectMovers::SETTLED + 1s);
	CHECK(book.Size() == 0);
	const auto* kept = places.Find(BROOM);
	REQUIRE(kept);
	CHECK(kept->playerId == 0);
	CHECK(kept->kind == Protocol::ObjectRestKind::kRest);
	CHECK(kept->worldspace == SANCTUARY);
	CHECK(Near(kept->position, { 10, 20, 30 }));
	// Flying: where it comes to rest follows.
	places.Rested(Rest(TIRE, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 300, 50 }));
	CHECK(!places.Find(TIRE));
	// Moved again: the newer rest is the one kept.
	places.Rested(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 40, 20, 30 }));
	REQUIRE(places.Find(BROOM));
	CHECK(Near(places.Find(BROOM)->position, { 40, 20, 30 }));
	CHECK(places.Size() == 1);
	// Picked up: at no place.
	places.Forget(BROOM);
	CHECK(places.Size() == 0);
	CHECK(places.All().empty());
}

TEST("moved objects places: past the cap, the object moved longest ago is forgotten first")
{
	ObjectMovers::Places places{ 3 };
	constexpr std::uint32_t A = 0x00100001, B = 0x00100002, C = 0x00100003, D = 0x00100004;
	for (const auto ref : { A, B, C, A }) {  // A moved again: the newest
		places.Rested(Rest(ref, Protocol::ObjectRestKind::kRest));
	}
	places.Rested(Rest(D, Protocol::ObjectRestKind::kRest));
	CHECK(places.Size() == 3);
	CHECK(!places.Find(B));
	const auto all = places.All();
	REQUIRE(all.size() == 3);
	CHECK(all[0].refId == C);
	CHECK(all[1].refId == A);
	CHECK(all[2].refId == D);
}

TEST("moved objects places: a kept place near us is put down once our game has the object, however far it was carried, and never under a move in progress")
{
	using ObjectShare::Verdict;
	ObjectShare::Facts facts{ .loaded = true, .sameSpace = true, .fromUs = 3000.0f, .fromPlace = 20000.0f };
	CHECK(ObjectShare::JudgePlace(facts, false) == Verdict::kApply);
	CHECK(ObjectShare::JudgeRest(facts) == Verdict::kTooFar);  // a live rest is: its holds brought our copy along
	CHECK(ObjectShare::JudgePlace(facts, true) == Verdict::kOurs);  // in a friend's hands, or flying
	auto beyond = facts;
	beyond.fromUs = ObjectShare::APPLY_RANGE + 1.0f;  // the ground there may not be loaded: it waits until we come near
	CHECK(ObjectShare::JudgePlace(beyond, false) == Verdict::kNotHere);
	auto held = facts;
	held.oursHeld = true;
	CHECK(ObjectShare::JudgePlace(held, false) == Verdict::kOurs);
	auto elsewhere = facts;
	elsewhere.sameSpace = false;
	CHECK(ObjectShare::JudgePlace(elsewhere, false) == Verdict::kNotHere);
	CHECK(ObjectShare::JudgePlace({}, false) == Verdict::kNotHere);  // not loaded here yet: it waits

	ObjectShare::WaitingPlaces waiting;
	waiting.Add(Rest(BROOM, Protocol::ObjectRestKind::kRest));
	waiting.Add(Rest(TIRE, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 300, 50 }));  // its rest follows
	CHECK(waiting.Size() == 1);
	waiting.Add(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 70, 0, 0 }));
	REQUIRE(waiting.Find(BROOM));
	CHECK(Near(waiting.Find(BROOM)->position, { 70, 0, 0 }));
	CHECK(waiting.Refs() == std::vector<std::uint32_t>{ BROOM });
	waiting.Erase(BROOM);
	CHECK(waiting.Empty());
	// A flood for objects we never load starts it over.
	for (std::uint32_t i = 0; i <= ObjectShare::WaitingPlaces::MAX; ++i) {
		waiting.Add(Rest(0x00100000 + i, Protocol::ObjectRestKind::kRest));
	}
	CHECK(waiting.Size() <= ObjectShare::WaitingPlaces::MAX);
	CHECK(waiting.Find(0x00100000 + static_cast<std::uint32_t>(ObjectShare::WaitingPlaces::MAX)) != nullptr);
}

TEST("moved objects places: a flood of one friend's rests for objects we never load keeps the session's places and the others' waiting")
{
	ObjectShare::WaitingPlaces waiting;
	auto                       caughtUp = Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 });
	waiting.Add(caughtUp);  // a catch-up: playerId 0
	auto hosts = Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 70, 0, 0 });
	hosts.playerId = 1;  // the host's live rest, its cell not loaded here
	waiting.Add(hosts);
	auto made = Rest(0x7F000000, Protocol::ObjectRestKind::kRest);
	made.playerId = 2;
	for (std::uint32_t i = 0; i <= ObjectShare::WaitingPlaces::MAX * 2; ++i) {
		made.refId = 0x7F000000 + i;
		waiting.Add(made);
	}
	CHECK(waiting.Size() <= ObjectShare::WaitingPlaces::MAX);
	REQUIRE(waiting.Find(TIRE));
	CHECK(Near(waiting.Find(TIRE)->position, { -50, 20, 4 }));
	REQUIRE(waiting.Find(BROOM));
	CHECK(waiting.Find(BROOM)->playerId == 1);
	CHECK(Near(waiting.Find(BROOM)->position, { 70, 0, 0 }));
	CHECK(waiting.Find(made.refId) != nullptr);  // the newest still waits
}

// A catch-up waited (it came while loading, or before we were near); then a friend moved the object on from where the
// session has it. Our copy still lay where our save had it, across the map: judged from it, the friend's move was too
// far and dropped, the catch-up with it, and the object stayed in its old place in our game for good.
TEST("moved objects places: a friend's move of an object whose place still waits here is judged from that place, and never loses it")
{
	using ObjectShare::Verdict;
	const Vec3 saved{ 400, 20600, 30 };  // where our save has the broom: 20000 from where the friend leaves it
	auto       caughtUp = Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 100, 600, 30 });
	auto       moved = Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 400, 600, 30 });
	moved.playerId = 1;

	// From the waiting place, not our copy.
	CHECK(Near(ObjectShare::FromWhereItLies(saved, &caughtUp, moved.cell, moved.worldspace, moved.position), 300.0f));
	CHECK(Near(ObjectShare::FromWhereItLies(saved, nullptr, moved.cell, moved.worldspace, moved.position), 20000.0f));
	// Whichever is nearer: our copy may be in the friend's hands already.
	CHECK(Near(ObjectShare::FromWhereItLies({ 410, 600, 30 }, &caughtUp, moved.cell, moved.worldspace, moved.position), 10.0f));
	// A place in another space says nothing of where it lies in this one.
	auto inside = caughtUp;
	inside.worldspace = 0;
	inside.cell = 0x0001F2A3;
	CHECK(Near(ObjectShare::FromWhereItLies(saved, &inside, moved.cell, moved.worldspace, moved.position), 20000.0f));

	ObjectShare::Facts facts{ .loaded = true, .sameSpace = true, .fromUs = 500.0f,
		.fromPlace = ObjectShare::FromWhereItLies(saved, &caughtUp, moved.cell, moved.worldspace, moved.position) };
	CHECK(ObjectShare::JudgeRest(facts) == Verdict::kApply);
	facts.fromPlace = ObjectShare::FromWhereItLies(saved, nullptr, moved.cell, moved.worldspace, moved.position);
	CHECK(ObjectShare::JudgeRest(facts) == Verdict::kTooFar);  // as it was: a made-up jump across the map

	// A move not applied: what waits after it.
	CHECK(!ObjectShare::StillWaiting(Verdict::kApply, moved, &caughtUp));
	CHECK(!ObjectShare::StillWaiting(Verdict::kOurs, moved, &caughtUp));  // our player holds it: their move is the newer
	const auto notHere = ObjectShare::StillWaiting(Verdict::kNotHere, moved, &caughtUp);
	REQUIRE(notHere);
	CHECK(notHere->playerId == 1);  // their rest, the newer
	CHECK(Near(notHere->position, moved.position));
	const auto tooFar = ObjectShare::StillWaiting(Verdict::kTooFar, moved, &caughtUp);  // too far even from the place
	REQUIRE(tooFar);
	CHECK(tooFar->playerId == 1);  // their rest waits: the session keeps the object there now, not at the old place
	CHECK(Near(tooFar->position, moved.position));
	CHECK(!ObjectShare::StillWaiting(Verdict::kTooFar, moved, nullptr));  // a made-up jump from a live copy: dropped
	const auto letGoFar = Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 3, { 9000, 600, 80 }, { 0, 300, 50 });
	const auto keptFar = ObjectShare::StillWaiting(Verdict::kTooFar, letGoFar, &caughtUp);  // a let-go: its rest follows
	REQUIRE(keptFar);
	CHECK(keptFar->playerId == 0);
	// A let-go not applied here: what waited stays, so the rest that follows is judged from it.
	const auto letGo = Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 3, { 150, 600, 80 }, { 0, 300, 50 });
	const auto flying = ObjectShare::StillWaiting(Verdict::kNotHere, letGo, &caughtUp);
	REQUIRE(flying);
	CHECK(flying->kind == Protocol::ObjectRestKind::kRest);
	CHECK(Near(flying->position, caughtUp.position));
	CHECK(!ObjectShare::StillWaiting(Verdict::kNotHere, letGo, nullptr));

	// The whole way through, as MovedObjects::OnRested takes them: the catch-up waits, the friend's let-go comes while
	// our game can't apply it, then their rest, once our game has the broom near.
	ObjectShare::WaitingPlaces waiting;
	waiting.Add(caughtUp);
	const auto take = [&](const Protocol::ObjectRest& a_move, ObjectShare::Facts a_facts) {
		std::optional<Protocol::ObjectRest> waited;
		if (const auto place = waiting.Find(a_move.refId)) {
			waited = *place;
		}
		waiting.Erase(a_move.refId);
		a_facts.fromPlace = ObjectShare::FromWhereItLies(saved, waited ? &*waited : nullptr, a_move.cell, a_move.worldspace, a_move.position);
		const auto verdict = ObjectShare::JudgeRest(a_facts);
		if (const auto still = ObjectShare::StillWaiting(verdict, a_move, waited ? &*waited : nullptr)) {
			waiting.Add(*still);
		}
		return verdict;
	};
	CHECK(take(letGo, {}) == Verdict::kNotHere);
	REQUIRE(waiting.Find(BROOM));
	CHECK(take(moved, { .loaded = true, .sameSpace = true, .fromUs = 500.0f }) == Verdict::kApply);
	CHECK(waiting.Empty());
}

// --- the holder's frame -------------------------------------------------------------------------------------------

TEST("moved objects frame: an object ahead of a holder facing +x is ahead in their frame, and back")
{
	const ObjectShare::Holder east{ .position = { 1000, 2000, 50 }, .heading = std::numbers::pi_v<float> / 2 };
	const auto                relative = ObjectShare::ToHolderFrame({ 1100, 2000, 150 }, { 0.2f, -0.1f, 2.0f }, east);
	CHECK(Near(relative.offset, { 0, 100, 100 }));
	CHECK(Near(ObjectShare::Turned(ObjectShare::ToHolderFrame({ 0, 0, 0 }, { 0, 0, 2.0f }, east).rotation, { 0, 0, 2.0f - std::numbers::pi_v<float> / 2 }), 0.0f));  // a level one: its heading less theirs
	const auto [position, angles] = ObjectShare::FromHolderFrame(relative, east);
	CHECK(Near(position, { 1100, 2000, 150 }));
	CHECK(Near(angles, { 0.2f, -0.1f, 2.0f }));
	// To the holder's right (facing +y: right is +x).
	const ObjectShare::Holder north{ .position = { 0, 0, 0 }, .heading = 0 };
	CHECK(Near(ObjectShare::ToHolderFrame({ 30, 0, 0 }, {}, north).offset, { 30, 0, 0 }));
}

TEST("moved objects frame: a held object turns and walks with the friend's stand-in")
{
	// The holder's game: the broom 80 ahead of them. Our stand-in of them is elsewhere and faces elsewhere.
	const ObjectShare::Holder holder{ .position = { 500, 500, 0 }, .heading = 0.3f };
	const auto [world, angles] = ObjectShare::FromHolderFrame({ .offset = { 0, 80, 90 }, .rotation = { 0, 0, 0 } }, holder);
	const auto relative = ObjectShare::ToHolderFrame(world, angles, holder);
	const ObjectShare::Holder standIn{ .position = { 510, 495, 2 }, .heading = 0.3f + std::numbers::pi_v<float> };
	const auto [drawn, drawnAngles] = ObjectShare::FromHolderFrame(relative, standIn);
	// 80 ahead of the stand-in, which faces the other way: behind where the holder's own broom is.
	CHECK(Near(ObjectShare::Distance(drawn, standIn.position), std::sqrt(80.0f * 80 + 90.0f * 90), 0.05f));
	CHECK(Near(drawn[0], 510 - 80 * std::sin(0.3f), 0.05f));
	CHECK(Near(drawn[1], 495 - 80 * std::cos(0.3f), 0.05f));
	CHECK(Near(ObjectShare::Wrap(drawnAngles[2] - standIn.heading), 0.0f));
}

TEST("moved objects frame: angles are the game's rotation Rz Ry Rx, and come back from it")
{
	// A shovel lying in Sanctuary, as the game had it: its angles and its 3D's rotation (rows), read with `objects probe`.
	const auto shovel = ObjectShare::FromAngles({ -0.3990f, 0.2646f, 3.9796f });
	const ObjectShare::Mat3 game{ Vec3{ -0.646f, 0.753f, 0.128f }, Vec3{ -0.717f, -0.541f, -0.439f }, Vec3{ -0.262f, -0.375f, 0.889f } };
	for (std::size_t r = 0; r < 3; ++r) {
		CHECK(Near(shovel[r], game[r], 0.002f));
	}
	// And back: z comes back the short way round (3.9796 - 2 pi).
	CHECK(Near(ObjectShare::ToAngles(game), { -0.3990f, 0.2646f, 3.9796f - 2 * std::numbers::pi_v<float> }, 0.003f));
	// A player's own rotation is the yaw alone: facing 45 degrees right of +y, their right is (+x, -y).
	CHECK(Near(ObjectShare::Yaw(0.7854f)[0], { 0.7071f, -0.7071f, 0.0f }, 0.001f));
	CHECK(Near(ObjectShare::Yaw(0.7854f)[1], { 0.7071f, 0.7071f, 0.0f }, 0.001f));
	// Pitched straight up: x folds into z, the rotation is the same.
	const Vec3 up{ 0.3f, std::numbers::pi_v<float> / 2, 1.0f };
	const auto back = ObjectShare::FromAngles(ObjectShare::ToAngles(ObjectShare::FromAngles(up)));
	const auto want = ObjectShare::FromAngles(up);
	for (std::size_t r = 0; r < 3; ++r) {
		CHECK(Near(back[r], want[r], 0.002f));
	}
}

TEST("moved objects frame: a tilted object carried round keeps its angles in the holder's frame")
{
	// Turned with its holder by d, an object's rotation is R Rz(d) (measured: a tilted tire carried round).
	const Vec3 tilted{ 0.6f, -0.4f, 1.2f };
	const ObjectShare::Holder before{ .position = { 0, 0, 0 }, .heading = 0.5f };
	const auto                relative = ObjectShare::ToHolderFrame({ 0, 80, 40 }, tilted, before);
	for (const float turn : { 0.3f, 1.5f, -2.8f }) {
		const ObjectShare::Holder after{ .position = { 0, 0, 0 }, .heading = 0.5f + turn };
		const auto                angles = ObjectShare::ToAngles(ObjectShare::Multiply(ObjectShare::FromAngles(tilted), ObjectShare::Yaw(turn)));
		CHECK(Near(ObjectShare::ToHolderFrame({ 0, 80, 40 }, angles, after).rotation, relative.rotation, 0.001f));
		// And drawn in front of a stand-in at that heading, it is turned the same.
		const auto drawn = ObjectShare::FromAngles(ObjectShare::FromHolderFrame(relative, after).second);
		const auto want = ObjectShare::FromAngles(angles);
		for (std::size_t r = 0; r < 3; ++r) {
			CHECK(Near(drawn[r], want[r], 0.001f));
		}
	}
	// Its x and y angles change as it turns (z alone would not do): so they go in the holder's frame.
	const auto turned = ObjectShare::ToAngles(ObjectShare::Multiply(ObjectShare::FromAngles(tilted), ObjectShare::Yaw(1.5f)));
	CHECK(!Near(turned[0], tilted[0], 0.05f));
}

// --- what our player holds ----------------------------------------------------------------------------------------

TEST("moved objects carry: holds go 15 a second while it moves, twice a second while it doesn't")
{
	ObjectShare::Carry carry;
	World              world;
	const ObjectShare::Holder me{ .position = { 0, 0, 0 }, .heading = 0 };
	auto                      t = Clock::now();
	world.poses[BROOM] = At({ 0, 80, 100 });
	carry.Frame(BROOM, me, world.PoseOf(), t);
	auto holds = carry.TakeHolds();
	REQUIRE(holds.size() == 1);
	CHECK(holds[0].refId == BROOM);
	CHECK(Near(holds[0].offset, { 0, 80, 100 }));
	CHECK(carry.Holding() == BROOM);

	// Moving, but sooner than HOLD_INTERVAL: nothing.
	world.poses[BROOM] = At({ 5, 80, 100 });
	carry.Frame(BROOM, me, world.PoseOf(), t + 30ms);
	CHECK(carry.TakeHolds().empty());
	carry.Frame(BROOM, me, world.PoseOf(), t + 70ms);
	holds = carry.TakeHolds();
	REQUIRE(holds.size() == 1);
	CHECK(holds[0].sequence > 0);

	// Still: only the keepalive.
	int sent = 0;
	for (int i = 1; i <= 60; ++i) {
		carry.Frame(BROOM, me, world.PoseOf(), t + 70ms + i * 16ms);  // about a second of frames
		sent += static_cast<int>(carry.TakeHolds().size());
	}
	CHECK(sent == 1 || sent == 2);

	// Walking with it held still in their hands: the same offset in their frame, so only keepalives.
	sent = 0;
	auto at = t + 2s;
	for (int i = 0; i < 30; ++i) {
		const ObjectShare::Holder walking{ .position = { 0, i * 10.0f, 0 }, .heading = 0 };
		world.poses[BROOM] = At({ 5, 80 + i * 10.0f, 100 });
		carry.Frame(BROOM, walking, world.PoseOf(), at += 16ms);
		sent += static_cast<int>(carry.TakeHolds().size());
	}
	CHECK(sent <= 2);
}

TEST("moved objects carry: a hold out of reach of the holder isn't sent")
{
	ObjectShare::Carry carry;
	World              world;
	world.poses[BROOM] = At({ 0, 5000, 0 });
	carry.Frame(BROOM, { .position = { 0, 0, 0 } }, world.PoseOf(), Clock::now());
	CHECK(carry.TakeHolds().empty());
}

TEST("moved objects carry: a throw is a let-go with its launch velocity, then a rest once still")
{
	ObjectShare::Carry carry;
	World              world;
	const ObjectShare::Holder me{};
	const auto                t = Clock::now();
	world.poses[BROOM] = At({ 0, 80, 100 });
	carry.Frame(BROOM, me, world.PoseOf(), t);
	const auto sequence = carry.TakeHolds().at(0).sequence;

	carry.Frame(0, me, world.PoseOf(), t + 16ms);  // thrown
	CHECK(carry.Holding() == 0);
	CHECK(carry.IsSettling(BROOM));
	CHECK(carry.TakeRests().empty());  // its flight's start is measured first
	world.poses[BROOM] = At({ 0, 110, 103 });
	carry.Frame(0, me, world.PoseOf(), t + 116ms);
	auto rests = carry.TakeRests();
	REQUIRE(rests.size() == 1);
	CHECK(rests[0].kind == Protocol::ObjectRestKind::kLetGo);
	CHECK(rests[0].sequence == sequence);
	CHECK(Near(rests[0].position, { 0, 80, 100 }));
	CHECK(Near(rests[0].velocity, { 0, 300, 30 }, 1.0f));
	CHECK(rests[0].worldspace == SANCTUARY);
	CHECK(Protocol::DecodeObjectRest(Protocol::Encode(rests[0], Protocol::MessageType::kReportObjectRest)).has_value());

	// It flies, lands, and lies still.
	world.poses[BROOM] = At({ 0, 300, 0 }, { 1.5f, 0, 0.2f });
	carry.Frame(0, me, world.PoseOf(), t + 500ms);
	carry.Frame(0, me, world.PoseOf(), t + 900ms);
	CHECK(carry.TakeRests().empty());
	carry.Frame(0, me, world.PoseOf(), t + 1200ms);
	rests = carry.TakeRests();
	REQUIRE(rests.size() == 1);
	CHECK(rests[0].kind == Protocol::ObjectRestKind::kRest);
	CHECK(Near(rests[0].position, { 0, 300, 0 }));
	CHECK(Near(rests[0].rotation, { 1.5f, 0, 0.2f }));
	CHECK(rests[0].velocity == Vec3{});
	CHECK(!carry.IsSettling(BROOM));
	CHECK(Protocol::DecodeObjectRest(Protocol::Encode(rests[0], Protocol::MessageType::kReportObjectRest)).has_value());
}

TEST("moved objects carry: one that never stops is given up on after REST_GIVE_UP")
{
	ObjectShare::Carry carry;
	World              world;
	const auto         t = Clock::now();
	world.poses[BROOM] = At({ 0, 80, 100 });
	carry.Frame(BROOM, {}, world.PoseOf(), t);
	carry.Frame(0, {}, world.PoseOf(), t + 16ms);
	std::size_t rests = 0;
	for (int i = 1; i <= 700; ++i) {
		world.poses[BROOM] = At({ 0, 80, 100.0f + (i % 2) * 5.0f });  // rolling about
		carry.Frame(0, {}, world.PoseOf(), t + 16ms + i * 16ms);
		for (const auto& rest : carry.TakeRests()) {
			rests += rest.kind == Protocol::ObjectRestKind::kRest;
		}
	}
	CHECK(rests == 1);
}

TEST("moved objects carry: refused, nothing more of it is sent, nor its let-go")
{
	ObjectShare::Carry carry;
	World              world;
	const auto         t = Clock::now();
	world.poses[BROOM] = At({ 0, 80, 100 });
	carry.Frame(BROOM, {}, world.PoseOf(), t);
	carry.TakeHolds();
	carry.Refused(BROOM);
	CHECK(carry.IsRefused(BROOM));
	CHECK(carry.Holding() == 0);
	world.poses[BROOM] = At({ 0, 90, 100 });
	carry.Frame(BROOM, {}, world.PoseOf(), t + 600ms);
	CHECK(carry.TakeHolds().empty());
	carry.Frame(0, {}, world.PoseOf(), t + 700ms);
	carry.Frame(0, {}, world.PoseOf(), t + 2s);
	CHECK(carry.TakeRests().empty());
	CHECK(!carry.IsRefused(BROOM));
	// Grabbed again later: theirs to send.
	carry.Frame(BROOM, {}, world.PoseOf(), t + 3s);
	CHECK(carry.TakeHolds().size() == 1);
}

TEST("moved objects carry: a let-go of one nobody saw held says nothing")
{
	ObjectShare::Carry carry;
	World              world;
	const auto         t = Clock::now();
	world.poses[BROOM] = At({ 0, 5000, 0 });  // out of reach: never sent
	carry.Frame(BROOM, {}, world.PoseOf(), t);
	carry.Frame(0, {}, world.PoseOf(), t + 16ms);
	carry.Frame(0, {}, world.PoseOf(), t + 2s);
	CHECK(carry.TakeRests().empty());
}

TEST("moved objects carry: a friend's move ends our let-go of it")
{
	ObjectShare::Carry carry;
	World              world;
	const auto         t = Clock::now();
	world.poses[BROOM] = At({ 0, 80, 100 });
	carry.Frame(BROOM, {}, world.PoseOf(), t);
	carry.Frame(0, {}, world.PoseOf(), t + 16ms);
	carry.Frame(0, {}, world.PoseOf(), t + 200ms);
	CHECK(carry.TakeRests().size() == 1);  // the let-go
	carry.TakenOver(BROOM);
	carry.Frame(0, {}, world.PoseOf(), t + 2s);
	CHECK(carry.TakeRests().empty());
}

// --- what friends hold --------------------------------------------------------------------------------------------

TEST("moved objects carried: newer holds of the holder are taken, older ones and those after the let-go dropped")
{
	ObjectShare::Carried carried;
	const auto           t = Clock::now();
	auto                 hold = Hold(BROOM, 10);
	hold.playerId = 1;
	CHECK(carried.OnHold(hold, t));
	hold.sequence = 9;
	CHECK(!carried.OnHold(hold, t + 10ms));
	hold.sequence = 11;
	CHECK(carried.OnHold(hold, t + 20ms));
	REQUIRE(carried.Find(BROOM));
	CHECK(carried.Find(BROOM)->sequence == 11);

	auto letGo = Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 12);
	letGo.playerId = 1;
	CHECK(carried.OnLetGo(letGo, t + 30ms));
	CHECK(!carried.Find(BROOM));
	hold.sequence = 12;
	CHECK(!carried.OnHold(hold, t + 40ms));  // overtook the let-go
	hold.sequence = 13;
	CHECK(carried.OnHold(hold, t + 50ms));  // grabbed again
	// A let-go from someone who doesn't hold it here.
	letGo.playerId = 2;
	CHECK(!carried.OnLetGo(letGo, t + 60ms));
	CHECK(carried.Find(BROOM));
}

TEST("moved objects carried: drawn eased towards the newest hold, never past it")
{
	ObjectShare::Carried carried;
	const auto           t = Clock::now();
	auto                 hold = Hold(BROOM, 1, { 0, 80, 100 });
	hold.playerId = 1;
	carried.OnHold(hold, t);
	auto first = carried.Step(BROOM, 0.016f);
	REQUIRE(first);
	CHECK(Near(first->offset, { 0, 80, 100 }));

	hold = Hold(BROOM, 2, { 40, 80, 100 });
	hold.playerId = 1;
	carried.OnHold(hold, t + 66ms);
	const auto a = carried.Step(BROOM, 0.016f);
	REQUIRE(a);
	CHECK(a->offset[0] > 0.0f);
	CHECK(a->offset[0] < 40.0f);
	float last = a->offset[0];
	for (int i = 0; i < 30; ++i) {
		const auto step = carried.Step(BROOM, 0.016f);
		CHECK(step->offset[0] >= last);
		CHECK(step->offset[0] <= 40.0f);
		last = step->offset[0];
	}
	CHECK(Near(last, 40.0f, 0.1f));
	// A long frame (a hitch) doesn't overshoot either.
	CHECK(Near(carried.Step(BROOM, 5.0f)->offset[0], 40.0f, 0.01f));
	CHECK(!carried.Step(TIRE, 0.016f));
}

TEST("moved objects carried: smoothing takes angles the short way round")
{
	const ObjectShare::Relative from{ .rotation = { 0, 0, 3.1f } };
	const ObjectShare::Relative to{ .rotation = { 0, 0, -3.1f } };
	const auto                  mid = ObjectShare::Smooth(from, to, ObjectShare::SMOOTH_SECONDS);
	CHECK(std::fabs(mid.rotation[2]) > 3.1f);  // through pi, not through 0
	CHECK(ObjectShare::Smooth(from, to, 0.0f) == from);
}

TEST("moved objects carried: a holder gone quiet, or gone, holds it no more")
{
	ObjectShare::Carried carried;
	const auto           t = Clock::now();
	auto                 hold = Hold(BROOM, 1);
	hold.playerId = 1;
	carried.OnHold(hold, t);
	hold = Hold(TIRE, 1);
	hold.playerId = 2;
	carried.OnHold(hold, t);
	CHECK(carried.TakeStale(t + 1s).empty());
	hold = Hold(TIRE, 2);
	hold.playerId = 2;
	carried.OnHold(hold, t + 1s);
	const auto stale = carried.TakeStale(t + ObjectShare::HELD_TIMEOUT + 1ms);
	CHECK(stale == std::vector<std::uint32_t>{ BROOM });
	CHECK(carried.Leave(2) == std::vector<std::uint32_t>{ TIRE });
	CHECK(carried.Size() == 0);
}

// --- knocked about ------------------------------------------------------------------------------------------------

namespace
{
	using Seen = ObjectShare::KnockWatch::Seen;

	// Scans every 250 ms from a_from, a_count times, with the object at a_pose.
	std::vector<Protocol::ObjectRest> ScanFor(ObjectShare::KnockWatch& a_watch, Clock::time_point& a_at, int a_count, const Seen& a_seen)
	{
		std::vector<Protocol::ObjectRest> out;
		for (int i = 0; i < a_count; ++i) {
			a_at += 250ms;
			const std::array seen{ a_seen };
			for (auto& rest : a_watch.Scan(seen, a_at)) {
				out.push_back(rest);
			}
		}
		return out;
	}
}

TEST("moved objects knocks: one our player knocked over is reported where it came to rest, once")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	CHECK(ScanFor(watch, t, 2, { .refId = TIRE, .pose = At({ 100, 100, 0 }) }).empty());
	CHECK(ScanFor(watch, t, 1, { .refId = TIRE, .pose = At({ 130, 100, 0 }), .byUs = true }).empty());  // moving
	const auto rests = ScanFor(watch, t, 6, { .refId = TIRE, .pose = At({ 150, 100, 0 }, { 0, 1.5f, 0 }) });
	REQUIRE(rests.size() == 1);
	CHECK(rests[0].refId == TIRE);
	CHECK(rests[0].kind == Protocol::ObjectRestKind::kRest);
	CHECK(Near(rests[0].position, { 150, 100, 0 }));
	CHECK(Near(rests[0].rotation, { 0, 1.5f, 0 }));
	CHECK(Protocol::DecodeObjectRest(Protocol::Encode(rests[0], Protocol::MessageType::kReportObjectRest)).has_value());
	CHECK(watch.Reported() == 1);
	CHECK(watch.Moving() == 0);
}

TEST("moved objects knocks: not ours, held or just placed by a friend: not reported")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	// Something else moved it (an NPC, an explosion of theirs).
	ScanFor(watch, t, 1, { .refId = TIRE, .pose = At({ 0, 0, 0 }) });
	CHECK(ScanFor(watch, t, 6, { .refId = TIRE, .pose = At({ 50, 0, 0 }) }).empty());
	ScanFor(watch, t, 1, { .refId = BROOM, .pose = At({ 0, 0, 0 }) });
	// Busy: in someone's hands.
	CHECK(ScanFor(watch, t, 6, { .refId = BROOM, .pose = At({ 100, 0, 0 }), .byUs = true, .busy = true }).empty());
	// A friend's move put it there: our physics settling it (under MOVE_DISTANCE) is not ours to report, now or later.
	watch.Placed(BROOM, At({ 200, 0, 0 }), t);
	CHECK(ScanFor(watch, t, 40, { .refId = BROOM, .pose = At({ 202, 0, 0 }), .byUs = true }).empty());
	CHECK(watch.Reported() == 0);
}

TEST("moved objects knocks: a shelf knocked over at once is reported on a budget")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	std::vector<Seen>       still, moved;
	for (std::uint32_t i = 0; i < 40; ++i) {
		still.push_back({ .refId = 0x00200000 + i, .pose = At({ i * 10.0f, 0, 0 }) });
		moved.push_back({ .refId = 0x00200000 + i, .pose = At({ i * 10.0f, 50, 0 }), .byUs = true });
	}
	watch.Scan(still, t);
	watch.Scan(moved, t += 250ms);
	std::size_t reported = 0;
	for (int i = 0; i < 4; ++i) {
		reported += watch.Scan(moved, t += 250ms).size();
	}
	CHECK(reported <= static_cast<std::size_t>(ObjectShare::KNOCK_BURST) + 1);
	CHECK(reported >= static_cast<std::size_t>(ObjectShare::KNOCK_BURST));
	CHECK(watch.Watching() == 40);
}

TEST("moved objects knocks: walked into is near the player's feet")
{
	CHECK(ObjectShare::Near({ 0, 0, 0 }, { 100, 50, 30 }));
	CHECK(!ObjectShare::Near({ 0, 0, 0 }, { 300, 0, 0 }));
	CHECK(!ObjectShare::Near({ 0, 0, 0 }, { 0, 0, 500 }));
}

// --- rolling on after a rest ---------------------------------------------------------------------------------------
// A ball our player kicked slowed to what looked like a rest (reported), then rolled on, far from our player by then and
// long after the kick: nothing said it was ours any more, so it was never reported again and the friend's copy stayed
// at the early rest, 230 units off. The follow-on keeps it ours until it truly rests.

namespace
{
	constexpr std::uint32_t BALL = 0x0001A003;

	// The object rolls from a_from to a_to over a_looks scans (250 ms apart), a_how at every look. What our game says.
	std::vector<Protocol::ObjectRest> Roll(ObjectShare::KnockWatch& a_watch, Clock::time_point& a_at, Vec3 a_from, Vec3 a_to, int a_looks, Seen a_how = {})
	{
		std::vector<Protocol::ObjectRest> out;
		a_how.refId = BALL;
		for (int i = 1; i <= a_looks; ++i) {
			const float f = static_cast<float>(i) / static_cast<float>(a_looks);
			a_how.pose = At({ a_from[0] + (a_to[0] - a_from[0]) * f, a_from[1] + (a_to[1] - a_from[1]) * f, a_from[2] + (a_to[2] - a_from[2]) * f });
			const std::array seen{ a_how };
			for (auto& rest : a_watch.Scan(seen, a_at += 250ms)) {
				out.push_back(rest);
			}
		}
		return out;
	}

	// The ball kicked by our player at the origin, rolled to a_pause and stopped there long enough to rest: what it said.
	std::vector<Protocol::ObjectRest> KickedTo(ObjectShare::KnockWatch& a_watch, Clock::time_point& a_at, Vec3 a_pause)
	{
		ScanFor(a_watch, a_at, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
		auto out = Roll(a_watch, a_at, { 0, 0, 0 }, { 20, 0, 0 }, 1, { .byUs = true });
		for (auto& rest : Roll(a_watch, a_at, { 20, 0, 0 }, a_pause, 4)) {
			out.push_back(rest);
		}
		for (auto& rest : ScanFor(a_watch, a_at, 4, { .refId = BALL, .pose = At(a_pause) })) {
			out.push_back(rest);
		}
		return out;
	}
}

TEST("knocks rolling on: a ball that rolls on after its rest is reported again where it truly rests")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	const auto              first = KickedTo(watch, t, { 60, 0, 0 });
	REQUIRE(first.size() == 1);
	CHECK(Near(first[0].position, { 60, 0, 0 }));
	// Two seconds later it rolls on, 230 units, with nobody near and the kick long forgotten.
	ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 60, 0, 0 }) });
	CHECK(Roll(watch, t, { 60, 0, 0 }, { 290, 0, 0 }, 12).empty());  // still rolling: nothing yet
	const auto second = ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 290, 0, 0 }) });
	REQUIRE(second.size() == 1);
	CHECK(second[0].refId == BALL);
	CHECK(Near(second[0].position, { 290, 0, 0 }));
	// Then it lies still: nothing more.
	CHECK(ScanFor(watch, t, 40, { .refId = BALL, .pose = At({ 290, 0, 0 }) }).empty());
	CHECK(watch.Reported() == 2);
}

TEST("knocks rolling on: one still rolling when we gave up on it is reported again once it stops")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
	// Kicked, and it rolls on and on down a long hill, past MOVING_GIVE_UP.
	const int looks = static_cast<int>(ObjectShare::MOVING_GIVE_UP / 250ms) + 8;
	auto      said = Roll(watch, t, { 0, 0, 0 }, { 10, 0, 0 }, 1, { .byUs = true });
	for (auto& rest : Roll(watch, t, { 10, 0, 0 }, { 10.0f * looks, 0, 0 }, looks)) {
		said.push_back(rest);
	}
	REQUIRE(said.size() == 1);  // given up on while it still rolled
	CHECK(said[0].position[0] < 10.0f * looks - 50.0f);
	const auto last = ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 10.0f * looks, 0, 0 }) });
	REQUIRE(last.size() == 1);
	CHECK(Near(last[0].position, { 10.0f * looks, 0, 0 }));
}

TEST("knocks rolling on: a slow roll, under a unit a look, is not a rest")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
	auto said = Roll(watch, t, { 0, 0, 0 }, { 10, 0, 0 }, 1, { .byUs = true });
	// 0.8 units a look for 3 s: each look is within STILL_DISTANCE of the last, never of where it last moved.
	for (auto& rest : Roll(watch, t, { 10, 0, 0 }, { 19.6f, 0, 0 }, 12)) {
		said.push_back(rest);
	}
	CHECK(said.empty());
	CHECK(watch.Moving() == 1);
	for (auto& rest : ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 19.6f, 0, 0 }) })) {
		said.push_back(rest);
	}
	REQUIRE(said.size() == 1);
	CHECK(Near(said[0].position, { 19.6f, 0, 0 }));
}

TEST("knocks rolling on: a friend's place for it never echoes back, however our copy rolls on")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
	// A friend's rest for it came (their player kicked it on): our copy is put there, and our physics rolls it further.
	watch.Placed(BALL, At({ 120, 0, 0 }), t);
	CHECK(Roll(watch, t, { 120, 0, 0 }, { 200, 0, 0 }, 8).empty());
	CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 200, 0, 0 }) }).empty());
	// Long after the quiet too: it is not ours any more.
	CHECK(Roll(watch, t, { 200, 0, 0 }, { 260, 0, 0 }, 4).empty());
	CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 260, 0, 0 }) }).empty());
	CHECK(watch.Reported() == 1);
}

TEST("knocks rolling on: a hand on it, a friend's blast, or a long wait, and it is not ours")
{
	{
		// A friend's grenade went off by it as it rolled on: what it throws about is theirs.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
		CHECK(Roll(watch, t, { 60, 0, 0 }, { 160, 0, 0 }, 4, { .theirs = true }).empty());
		CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 160, 0, 0 }) }).empty());
	}
	{
		// Someone picked it up: what it does after is theirs, whoever's hands they were.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
		ScanFor(watch, t, 2, { .refId = BALL, .pose = At({ 60, 0, 50 }), .busy = true });
		ScanFor(watch, t, 20, { .refId = BALL, .pose = At({ 60, 0, 50 }) });
		CHECK(Roll(watch, t, { 60, 0, 50 }, { 160, 0, 0 }, 4).empty());
		CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 160, 0, 0 }) }).empty());
	}
	{
		// Moved again long after its rest: whatever moved it, it was not our kick.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
		ScanFor(watch, t, static_cast<int>(ObjectShare::FOLLOW_ON / 250ms) + 1, { .refId = BALL, .pose = At({ 60, 0, 0 }) });
		CHECK(Roll(watch, t, { 60, 0, 0 }, { 160, 0, 0 }, 4).empty());
		CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 160, 0, 0 }) }).empty());
	}
}

// --- a friend standing by it ---------------------------------------------------------------------------------------
// A grenade into a pile of cans with the friend standing in the pile left the two games' copies 27 to 520 units apart
// (#207's rig). The follow-on used to end when a friend's stand-in stood within 200 units of the object as it moved
// again (their game might report the move), so a can of ours that rolled on by them was never reported again; and the
// friend's copy, put down where our rest said but in their player's own body there, was pushed out by their physics
// with nobody to say so (our blast's reach made it ours, and the quiet after our place held their game back). Now each
// game says what its own player, NPCs and blasts moved and what rolled on from its own rests, a friend by it or not,
// and the server settles a rest both claim (ObjectMovers::Book, tested below with the server).

TEST("knocks by a friend: a ball that rolls on by a friend's stand-in is still ours, and reported again where it stops")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
	// Their stand-in stands by it as it rolls on: nothing the scan sees of it changes.
	CHECK(Roll(watch, t, { 60, 0, 0 }, { 160, 0, 0 }, 4).empty());
	const auto again = ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 160, 0, 0 }) });
	REQUIRE(again.size() == 1);
	CHECK(Near(again[0].position, { 160, 0, 0 }));
	CHECK(watch.Reported() == 2);
}

TEST("knocks by a friend: a copy their rest put in our player's way is said where our body pushed it once the quiet is over; one that only settles there is not")
{
	const int quietLooks = static_cast<int>(ObjectShare::QUIET_AFTER_PLACED / 250ms);
	{
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
		// A friend's rest puts it where our player stands: our physics pushes it 30 units out in the first half second,
		// and their blast (the one that threw it, set off here for their throw) still reaches it.
		watch.Placed(BALL, At({ 100, 0, 0 }), t);
		auto said = Roll(watch, t, { 100, 0, 0 }, { 130, 0, 0 }, 2, { .byUs = true, .theirs = true });
		for (auto& rest : ScanFor(watch, t, quietLooks - 3, { .refId = BALL, .pose = At({ 130, 0, 0 }), .byUs = true, .theirs = true })) {
			said.push_back(rest);
		}
		CHECK(said.empty());  // the quiet: their rest could still cross ours
		for (auto& rest : ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 130, 0, 0 }), .byUs = true })) {
			said.push_back(rest);
		}
		REQUIRE(said.size() == 1);
		CHECK(Near(said[0].position, { 130, 0, 0 }));
		CHECK(said[0].kind == Protocol::ObjectRestKind::kRest);
		// Once: it lies there.
		CHECK(ScanFor(watch, t, 40, { .refId = BALL, .pose = At({ 130, 0, 0 }), .byUs = true }).empty());
		CHECK(watch.Reported() == 1);
	}
	{
		// Pushed out, and their newer rest comes before the quiet is over: theirs holds, and ours is never said.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
		watch.Placed(BALL, At({ 100, 0, 0 }), t);
		auto said = Roll(watch, t, { 100, 0, 0 }, { 130, 0, 0 }, 2, { .byUs = true });
		for (auto& rest : ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 130, 0, 0 }), .byUs = true })) {
			said.push_back(rest);
		}
		watch.Placed(BALL, At({ 140, 0, 0 }), t);
		for (auto& rest : ScanFor(watch, t, quietLooks + 8, { .refId = BALL, .pose = At({ 140, 0, 0 }), .byUs = true })) {
			said.push_back(rest);
		}
		CHECK(said.empty());
	}
	{
		// Rolled on from their place with nothing of ours by it (our physics alone): not ours to speak of.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
		watch.Placed(BALL, At({ 100, 0, 0 }), t);
		auto said = Roll(watch, t, { 100, 0, 0 }, { 130, 0, 0 }, 2);
		for (auto& rest : ScanFor(watch, t, quietLooks + 8, { .refId = BALL, .pose = At({ 130, 0, 0 }) })) {
			said.push_back(rest);
		}
		CHECK(said.empty());
	}
	{
		// Pushed out and then still rolling when the quiet ends: said where it truly stops, once.
		ObjectShare::KnockWatch watch;
		auto                    t = Clock::now();
		ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
		watch.Placed(BALL, At({ 100, 0, 0 }), t);
		auto said = Roll(watch, t, { 100, 0, 0 }, { 300, 0, 0 }, quietLooks + 4, { .byUs = true });
		CHECK(said.empty());
		for (auto& rest : ScanFor(watch, t, 4, { .refId = BALL, .pose = At({ 300, 0, 0 }), .byUs = true })) {
			said.push_back(rest);
		}
		REQUIRE(said.size() == 1);
		CHECK(Near(said[0].position, { 300, 0, 0 }));
	}
}

TEST("knocks by a friend: a grenade into a pile with a friend standing in it ends with both games' copies together")
{
	// The thrower's game owns what the blast threw; the bystander's copy of one can is put down in their own player's
	// body and pushed out. Each game is a KnockWatch; the server's part (first rest wins, within SETTLED) is played by
	// hand: here the bystander's rest comes after the thrower's SETTLED, as the quiet after a place is longer. Host and
	// joiner each way, with the server between them: the sim's cross tests.
	{
		ObjectShare::KnockWatch thrower, bystander;
		auto                    tt = Clock::now(), tb = tt;
		constexpr std::uint32_t CAN_1 = 0x0001B001, CAN_2 = 0x0001B002;
		const std::vector<Seen> still{ { .refId = CAN_1, .pose = At({ 0, 0, 0 }) }, { .refId = CAN_2, .pose = At({ 10, 0, 0 }) } };
		thrower.Scan(still, tt += 250ms);
		bystander.Scan(still, tb += 250ms);
		// The blast: the thrower's book calls it theirs (serial 1), the bystander's the friend's; the bystander's player
		// stands in the pile, so their cans are byUs as well. Each game's physics throws the cans differently.
		std::vector<Protocol::ObjectRest> throwerSaid, bystanderSaid;
		for (int look = 0; look < 8; ++look) {
			const bool              first = look < 2;
			const std::vector<Seen> inThrower{ { .refId = CAN_1, .pose = At({ 200, 0, 0 }), .blast = first ? 1u : 0u }, { .refId = CAN_2, .pose = At({ 0, 150, 0 }), .blast = first ? 1u : 0u } };
			const std::vector<Seen> inBystander{ { .refId = CAN_1, .pose = At({ 180, 20, 0 }), .byUs = true, .theirs = first }, { .refId = CAN_2, .pose = At({ 10, 140, 0 }), .byUs = true, .theirs = first } };
			for (auto& rest : thrower.Scan(inThrower, tt += 250ms)) {
				throwerSaid.push_back(rest);
			}
			for (auto& rest : bystander.Scan(inBystander, tb += 250ms)) {
				bystanderSaid.push_back(rest);
			}
		}
		REQUIRE(throwerSaid.size() == 2);
		CHECK(bystanderSaid.empty());
		// The thrower's rests reach the bystander: both copies go where the thrower's lie.
		for (const auto& rest : throwerSaid) {
			bystander.Placed(rest.refId, At(rest.position), tb);
		}
		// Can 2 was put down in the bystander's player's body: their physics pushes it 40 units out. Their player
		// still stands there (byUs), and the thrower's blast is still in reach of the bystander's book.
		const int quietLooks = static_cast<int>(ObjectShare::QUIET_AFTER_PLACED / 250ms);
		for (int look = 0; look < quietLooks + 4; ++look) {
			const std::vector<Seen> inThrower{ { .refId = CAN_1, .pose = At({ 200, 0, 0 }) }, { .refId = CAN_2, .pose = At({ 0, 150, 0 }) } };
			const std::vector<Seen> inBystander{ { .refId = CAN_1, .pose = At({ 200, 0, 0 }), .byUs = true, .theirs = look < 8 }, { .refId = CAN_2, .pose = At({ 0, 190, 0 }), .byUs = true, .theirs = look < 8 } };
			for (auto& rest : thrower.Scan(inThrower, tt += 250ms)) {
				throwerSaid.push_back(rest);
			}
			for (auto& rest : bystander.Scan(inBystander, tb += 250ms)) {
				bystanderSaid.push_back(rest);
			}
		}
		CHECK(throwerSaid.size() == 2);
		REQUIRE(bystanderSaid.size() == 1);
		CHECK(bystanderSaid[0].refId == CAN_2);
		CHECK(Near(bystanderSaid[0].position, { 0, 190, 0 }));
		// The bystander's rest reaches the thrower: can 2 goes where the bystander's body left it, and lies there.
		thrower.Placed(CAN_2, At(bystanderSaid[0].position), tt);
		for (int look = 0; look < 40; ++look) {
			const std::vector<Seen> inThrower{ { .refId = CAN_1, .pose = At({ 200, 0, 0 }) }, { .refId = CAN_2, .pose = At({ 0, 190, 0 }) } };
			const std::vector<Seen> inBystander{ { .refId = CAN_1, .pose = At({ 200, 0, 0 }), .byUs = true }, { .refId = CAN_2, .pose = At({ 0, 190, 0 }), .byUs = true } };
			CHECK(thrower.Scan(inThrower, tt += 250ms).empty());
			CHECK(bystander.Scan(inBystander, tb += 250ms).empty());
		}
		CHECK(thrower.Reported() == 2);
		CHECK(bystander.Reported() == 1);
	}
}

TEST("knocks by a friend: two games whose physics never agree trade a copy only ECHO_MAX times each in ECHO_WINDOW, then it lies")
{
	// Each game's physics pushes the copy 30 units on from the place the other's rest put it (both players stand by it),
	// every time: the rest each says is the other's next place. Without a bound it goes on for ever.
	const int               quietLooks = static_cast<int>(ObjectShare::QUIET_AFTER_PLACED / 250ms);
	ObjectShare::KnockWatch games[2];
	Clock::time_point       clocks[2] = { Clock::now(), Clock::now() };
	for (int i = 0; i < 2; ++i) {
		ScanFor(games[i], clocks[i], 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
	}
	const auto trade = [&](int a_first) {
		Vec3 place{ 100, 0, 0 };
		int  rounds = 0;
		for (int side = a_first; rounds < 40; side = 1 - side, ++rounds) {
			games[side].Placed(BALL, At(place), clocks[side]);
			const Vec3 pushed{ place[0] + 30, place[1], place[2] };
			auto       said = Roll(games[side], clocks[side], place, pushed, 2, { .byUs = true });
			for (auto& rest : ScanFor(games[side], clocks[side], quietLooks + 8, { .refId = BALL, .pose = At(pushed), .byUs = true })) {
				said.push_back(rest);
			}
			if (said.empty()) {
				break;
			}
			place = said[0].position;
		}
		return rounds;
	};
	const int rounds = trade(0);
	CHECK(rounds <= 2 * ObjectShare::ECHO_MAX + 1);
	CHECK(games[0].Reported() <= static_cast<std::uint64_t>(ObjectShare::ECHO_MAX));
	CHECK(games[1].Reported() <= static_cast<std::uint64_t>(ObjectShare::ECHO_MAX));
	CHECK(games[0].Reported() + games[1].Reported() >= 2);  // it did trade before it stopped
	// A long while later the bound is spent: a push-out is said again.
	for (int i = 0; i < 2; ++i) {
		clocks[i] += ObjectShare::ECHO_WINDOW;
	}
	const auto before = games[0].Reported() + games[1].Reported();
	trade(0);
	CHECK(games[0].Reported() + games[1].Reported() > before);
}

TEST("knocks rolling on: two games, one kick: only the kicker's game reports it, each time it comes to rest")
{
	// Alice kicked it. Bob's copy is put at each rest of hers (Placed) and rolls on in his physics too; his game says
	// nothing, and hers says where it truly rests.
	ObjectShare::KnockWatch alice, bob;
	auto                    ta = Clock::now(), tb = ta;
	ScanFor(bob, tb, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
	const auto first = KickedTo(alice, ta, { 60, 0, 0 });
	REQUIRE(first.size() == 1);
	bob.Placed(BALL, At(first[0].position), tb);
	ScanFor(alice, ta, 4, { .refId = BALL, .pose = At({ 60, 0, 0 }) });
	CHECK(Roll(bob, tb, { 60, 0, 0 }, { 280, 0, 0 }, 12).empty());
	CHECK(Roll(alice, ta, { 60, 0, 0 }, { 290, 0, 0 }, 12).empty());
	const auto second = ScanFor(alice, ta, 4, { .refId = BALL, .pose = At({ 290, 0, 0 }) });
	REQUIRE(second.size() == 1);
	CHECK(ScanFor(bob, tb, 4, { .refId = BALL, .pose = At({ 280, 0, 0 }) }).empty());
	bob.Placed(BALL, At(second[0].position), tb);
	CHECK(ScanFor(bob, tb, 40, { .refId = BALL, .pose = At({ 290, 0, 0 }) }).empty());
	CHECK(bob.Reported() == 0);
	CHECK(alice.Reported() == 2);
}

TEST("knocks rolling on: one that never stops is reported once a MOVING_GIVE_UP at most")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	ScanFor(watch, t, 1, { .refId = BALL, .pose = At({ 0, 0, 0 }) });
	auto       said = Roll(watch, t, { 0, 0, 0 }, { 10, 0, 0 }, 1, { .byUs = true });
	const int  looks = 4 * 60;  // a minute of rolling round and round
	for (int i = 0; i < looks; ++i) {
		const float a = static_cast<float>(i) * 0.05f;
		const std::array seen{ Seen{ .refId = BALL, .pose = At({ 200.0f * std::cos(a), 200.0f * std::sin(a), 0 }) } };
		for (auto& rest : watch.Scan(seen, t += 250ms)) {
			said.push_back(rest);
		}
	}
	CHECK(!said.empty());
	CHECK(said.size() <= static_cast<std::size_t>(60s / ObjectShare::MOVING_GIVE_UP) + 1);
}

TEST("knocks rolling on: a thing that swings for ever is followed only so long, until something says it is ours again")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
	// Then it swings 20 units either side, every 4 s, with nobody near: ten minutes of it.
	std::size_t said = 0;
	auto        swing = [&](int a_looks, Seen a_how = {}) {
		for (int i = 0; i < a_looks; ++i) {
			const float      a = static_cast<float>(i) * 0.4f;
			Seen             s = a_how;
			s.refId = BALL;
			s.pose = At({ 60.0f + 20.0f * std::sin(a), 0, 0 });
			const std::array seen{ s };
			said += watch.Scan(seen, t += 250ms).size();
		}
	};
	swing(4 * 60 * 10);
	CHECK(said >= 2);
	CHECK(said <= static_cast<std::size_t>(ObjectShare::FOLLOW_ON_MAX));
	const auto before = said;
	// Our player kicks it again: it is ours once more.
	swing(4, { .byUs = true });
	swing(4 * 60);
	CHECK(said > before);
}

TEST("knocks rolling on: not looked at for a while (a loading screen), what made it ours is gone")
{
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	REQUIRE(KickedTo(watch, t, { 60, 0, 0 }).size() == 1);
	t += ObjectShare::FORGET_AFTER + 1s;  // under FOLLOW_ON, but nothing looked at it meanwhile
	CHECK(Roll(watch, t, { 60, 0, 0 }, { 160, 0, 0 }, 4).empty());
	CHECK(ScanFor(watch, t, 8, { .refId = BALL, .pose = At({ 160, 0, 0 }) }).empty());
}

// --- knocked about by NPCs and blasts -----------------------------------------------------------------------------
// A loose object an NPC shoves or an explosion throws lands differently in each game; only our own player's knocks used
// to be reported, so a friend's chair or a table of clutter stayed where it was in their game. The game that runs the NPC
// (or caused the blast) reports what it moved; every other game, whose own physics shoved the same object, stays quiet.

namespace
{
	constexpr std::uint32_t ALICE = 1;
	constexpr std::uint32_t BOB = 2;
	constexpr std::uint32_t RAIDER = 0x00140001;

	// One game's view of loose objects.
	struct Game
	{
		ObjectShare::KnockWatch watch;
		Clock::time_point       t = Clock::now();

		void Still(std::uint32_t a_ref, Vec3 a_at)
		{
			const std::array seen{ Seen{ .refId = a_ref, .pose = At(a_at) } };
			watch.Scan(seen, t += 250ms);
		}

		// The object is shoved to a_to in this game, and rests there; a_how is what the game knew of who moved it, at the
		// first look only. Returns what the game reports.
		std::vector<Protocol::ObjectRest> Shoved(std::uint32_t a_ref, Vec3 a_to, Seen a_how)
		{
			a_how.refId = a_ref;
			a_how.pose = At(a_to);
			std::vector<Protocol::ObjectRest> out;
			for (int i = 0; i < 8; ++i) {
				const std::array seen{ a_how };
				for (auto& rest : watch.Scan(seen, t += 250ms)) {
					out.push_back(rest);
				}
				a_how.byUs = a_how.byNpc = a_how.theirs = false;
				a_how.blast = 0;
			}
			return out;
		}
	};

	// What each game's NPC rule says of an NPC at a_npc next to an object at a_object, the NPC run by a_owner.
	bool ByNpc(std::uint32_t a_local, std::uint32_t a_owner, const Vec3& a_npc, const Vec3& a_object)
	{
		return ObjectShare::Answers(RAIDER, a_owner, a_local) && ObjectShare::NpcNear(a_npc, a_object);
	}
}

TEST("npc knocks: an object an NPC our game runs shoved is reported where it came to rest, once")
{
	Game game;
	game.Still(TIRE, { 100, 100, 0 });
	const auto rests = game.Shoved(TIRE, { 160, 100, 0 }, { .byNpc = ByNpc(ALICE, ALICE, { 90, 100, 0 }, { 100, 100, 0 }) });
	REQUIRE(rests.size() == 1);
	CHECK(rests[0].refId == TIRE);
	CHECK(Near(rests[0].position, { 160, 100, 0 }));
}

TEST("npc knocks: the game that does not run the NPC says nothing of what its copy shoved")
{
	// Alice's game runs the raider; in Bob's game it is a copy that shoves the same table in its own way.
	Game alice, bob;
	alice.Still(TIRE, { 100, 100, 0 });
	bob.Still(TIRE, { 100, 100, 0 });
	const auto aliceSays = alice.Shoved(TIRE, { 160, 100, 0 }, { .byNpc = ByNpc(ALICE, ALICE, { 90, 100, 0 }, { 100, 100, 0 }) });
	const auto bobSays = bob.Shoved(TIRE, { 140, 120, 0 }, { .byNpc = ByNpc(BOB, ALICE, { 90, 100, 0 }, { 100, 100, 0 }) });
	CHECK(aliceSays.size() == 1);
	CHECK(bobSays.empty());
	// Nobody runs it yet (owner 0), or it is not shared (id 0), or we are not in a session: nobody claims what it shoved.
	CHECK(!ByNpc(ALICE, 0, { 90, 100, 0 }, { 100, 100, 0 }));
	CHECK(!ObjectShare::Answers(0, ALICE, ALICE));
	CHECK(!ObjectShare::Answers(RAIDER, 0, 0));
}

TEST("npc knocks: an NPC far from the object did not shove it")
{
	CHECK(ObjectShare::NpcNear({ 0, 0, 0 }, { 150, 100, 40 }));
	CHECK(!ObjectShare::NpcNear({ 0, 0, 0 }, { 300, 0, 0 }));
	CHECK(!ObjectShare::NpcNear({ 0, 0, 0 }, { 0, 0, 400 }));
	Game game;
	game.Still(TIRE, { 0, 0, 0 });
	CHECK(game.Shoved(TIRE, { 50, 0, 0 }, {}).empty());  // moved by something we cannot name
}

TEST("npc knocks: an object a friend's blast reaches is theirs whoever else moved it, and a friend's put-down is quiet")
{
	Game game;
	game.Still(TIRE, { 100, 100, 0 });
	CHECK(game.Shoved(TIRE, { 160, 100, 0 }, { .byUs = true, .byNpc = true, .theirs = true }).empty());
	game.Still(BROOM, { 0, 0, 0 });
	game.watch.Placed(BROOM, At({ 300, 0, 0 }), game.t);
	CHECK(game.Shoved(BROOM, { 310, 0, 0 }, { .byNpc = true }).empty());
}

TEST("blasts: the cause says whose blast it is")
{
	using ObjectShare::BlastRole;
	using ObjectShare::Cause;
	CHECK(ObjectShare::RoleOf(false, Cause::kPlayer, false) == BlastRole::kOurs);
	CHECK(ObjectShare::RoleOf(false, Cause::kNpc, true) == BlastRole::kOurs);  // an NPC our game runs threw it
	CHECK(ObjectShare::RoleOf(false, Cause::kNpc, false) == BlastRole::kTheirs);
	CHECK(ObjectShare::RoleOf(false, Cause::kStandIn, false) == BlastRole::kTheirs);
	CHECK(!ObjectShare::RoleOf(false, Cause::kNobody, false).has_value());         // a trap: no game claims it
	CHECK(ObjectShare::RoleOf(true, Cause::kPlayer, true) == BlastRole::kTheirs);  // set off here for a friend's throw
	CHECK(ObjectShare::RoleOf(true, Cause::kNobody, false) == BlastRole::kTheirs);
}

TEST("blasts: what a blast reaches, for how long, and what two blasts mean")
{
	using ObjectShare::BlastRole;
	ObjectShare::BlastBook book;
	auto                   t = Clock::now();
	CHECK(book.Note(7, { 0, 0, 0 }, 500, BlastRole::kOurs, t));
	CHECK(!book.Note(7, { 0, 0, 0 }, 500, BlastRole::kOurs, t + 1s));  // the same explosion, still there
	CHECK(!book.Note(8, { 0, 0, 0 }, std::numeric_limits<float>::quiet_NaN(), BlastRole::kOurs, t));
	CHECK(!book.Note(9, { 0, 0, 0 }, 0, BlastRole::kOurs, t));
	const auto serial = book.Reaching({ 300, 0, 0 }, t + 100ms).serial;
	CHECK(serial != 0);
	CHECK(book.Reaching({ 600, 0, 0 }, t + 100ms).serial == serial);  // the margin: bodies are bigger than centres
	CHECK(book.Reaching({ 700, 0, 0 }, t + 100ms).serial == 0);
	CHECK(book.Reaching({ 300, 0, 0 }, t + ObjectShare::BLAST_REACH_FOR + 1ms).serial == 0);
	CHECK(!book.Reaching({ 300, 0, 0 }, t + 100ms).theirs);
	// A friend's blast over the same place: the object is theirs, and never ours to report.
	CHECK(book.Note(10, { 100, 0, 0 }, 400, BlastRole::kTheirs, t + 200ms));
	CHECK(book.Reaching({ 300, 0, 0 }, t + 300ms).theirs);
	// A nuke's reach is capped by the scan's.
	CHECK(book.Note(11, { 5000, 0, 0 }, 50000, BlastRole::kOurs, t + 300ms));
	CHECK(book.Reaching({ 5000 + ObjectShare::BLAST_MAX_RADIUS + ObjectShare::BLAST_MARGIN + 10, 0, 0 }, t + 400ms).serial == 0);
	// Past the explosion's own lifetime it is noted again (a freed reference's id comes back).
	CHECK(book.Note(7, { 0, 0, 0 }, 500, BlastRole::kOurs, t + ObjectShare::BLAST_KEY_MEMORY + 1s));
	for (std::uint32_t i = 100; i < 100 + 2 * ObjectShare::MAX_BLASTS; ++i) {
		book.Note(i, { 0, 0, 0 }, 100, BlastRole::kOurs, t + ObjectShare::BLAST_KEY_MEMORY + 2s);
	}
	CHECK(book.Size() <= ObjectShare::MAX_BLASTS);
}

TEST("blasts: a grenade into a table of clutter is reported by the thrower's game only, on a cap")
{
	using ObjectShare::BlastRole;
	// Alice threw it: her book calls it hers. In Bob's game it is the explosion set off for her throw: his book calls it
	// theirs, and his own player stands by the table, so his objects are byUs as well.
	ObjectShare::BlastBook aliceBook, bobBook;
	Game                   alice, bob;
	constexpr std::uint32_t COUNT = 40;
	std::vector<Seen>       still;
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		still.push_back({ .refId = 0x00300000 + i, .pose = At({ i * 5.0f, 0, 0 }) });
	}
	alice.watch.Scan(still, alice.t += 250ms);
	bob.watch.Scan(still, bob.t += 250ms);
	CHECK(aliceBook.Note(1, { 0, 0, 0 }, 300, BlastRole::kOurs, alice.t));
	CHECK(bobBook.Note(1, { 0, 0, 0 }, 300, BlastRole::kTheirs, bob.t));
	std::size_t             aliceSaid = 0, bobSaid = 0;
	std::set<std::uint32_t> told;
	// Every object is thrown a little in each game, differently; each game's scan has all 40 moving at once.
	for (int look = 0; look < 40; ++look) {
		std::vector<Seen> a, b;
		for (std::uint32_t i = 0; i < COUNT; ++i) {
			const std::uint32_t id = 0x00300000 + i;
			const Vec3          inA{ i * 5.0f + 80.0f, 40.0f, 0 }, inB{ i * 5.0f + 60.0f, 70.0f, 0 };
			const bool          first = look < 2;
			const auto          ra = aliceBook.Reaching(inA, alice.t), rb = bobBook.Reaching(inB, bob.t);
			a.push_back({ .refId = id, .pose = At(inA), .blast = first ? ra.serial : 0 });
			b.push_back({ .refId = id, .pose = At(inB), .byUs = first, .theirs = first && rb.theirs });
		}
		for (const auto& rest : alice.watch.Scan(a, alice.t += 250ms)) {
			++aliceSaid;
			CHECK(told.insert(rest.refId).second);  // never twice
		}
		bobSaid += bob.watch.Scan(b, bob.t += 250ms).size();
	}
	CHECK(bobSaid == 0);
	CHECK(aliceSaid > 0);
	CHECK(aliceSaid <= ObjectShare::BLAST_MAX_REPORTS);
	CHECK(alice.watch.Reported() == aliceSaid);
}

TEST("blasts: a burst waits for the budget instead of dropping its rests, up to the cap")
{
	using ObjectShare::BlastRole;
	ObjectShare::BlastBook  book;
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	book.Note(1, { 0, 0, 0 }, 600, BlastRole::kOurs, t);
	const auto              serial = book.Reaching({ 0, 0, 0 }, t).serial;
	constexpr std::uint32_t COUNT = 12;  // more than the burst of 8, fewer than the cap
	std::vector<Seen>       still, moved;
	for (std::uint32_t i = 0; i < COUNT; ++i) {
		still.push_back({ .refId = 0x00310000 + i, .pose = At({ i * 10.0f, 0, 0 }) });
		moved.push_back({ .refId = 0x00310000 + i, .pose = At({ i * 10.0f, 90, 0 }), .blast = serial });
	}
	watch.Scan(still, t += 250ms);
	watch.Scan(moved, t += 250ms);
	for (auto& m : moved) {
		m.blast = 0;
	}
	std::size_t said = 0, atOnce = 0;
	for (int i = 0; i < 40; ++i) {
		const auto n = watch.Scan(moved, t += 250ms).size();
		if (said == 0) {
			atOnce = n;
		}
		said += n;
	}
	CHECK(atOnce >= static_cast<std::size_t>(ObjectShare::KNOCK_BURST));
	CHECK(atOnce <= static_cast<std::size_t>(ObjectShare::KNOCK_BURST) + 3);
	CHECK(said == COUNT);  // the others came as the budget filled
}

TEST("blasts: each blast has its own cap")
{
	using ObjectShare::BlastRole;
	ObjectShare::BlastBook  book;
	ObjectShare::KnockWatch watch;
	auto                    t = Clock::now();
	constexpr std::uint32_t COUNT = 20;  // under each blast: more than a blast's cap
	std::size_t             said[2]{};
	for (std::uint32_t blast = 0; blast < 2; ++blast) {
		// A second blast, much later, where the first one's budget is long filled again.
		t += 30s;
		const float x = blast * 5000.0f;
		std::vector<Seen> still, moved;
		for (std::uint32_t i = 0; i < COUNT; ++i) {
			still.push_back({ .refId = 0x00320000 + blast * 100 + i, .pose = At({ x + i * 5.0f, 0, 0 }) });
		}
		watch.Scan(still, t += 250ms);
		CHECK(book.Note(1 + blast, { x, 0, 0 }, 300, BlastRole::kOurs, t));
		const auto serial = book.Reaching({ x, 0, 0 }, t).serial;
		CHECK(serial != 0);
		for (std::uint32_t i = 0; i < COUNT; ++i) {
			moved.push_back({ .refId = 0x00320000 + blast * 100 + i, .pose = At({ x + i * 5.0f, 90, 0 }), .blast = serial });
		}
		for (int look = 0; look < 40; ++look) {
			said[blast] += watch.Scan(moved, t += 250ms).size();
			for (auto& m : moved) {
				m.blast = 0;
			}
		}
	}
	CHECK(said[0] == ObjectShare::BLAST_MAX_REPORTS);
	CHECK(said[1] == ObjectShare::BLAST_MAX_REPORTS);
}

// --- what a friend's message may do -------------------------------------------------------------------------------

TEST("moved objects judge: a hold or rest applies only to a loose object loaded here, near, not ours, in reach")
{
	using ObjectShare::Facts;
	using V = ObjectShare::Verdict;
	const Facts ok{ .loaded = true, .sameSpace = true, .fromUs = 500, .fromPlace = 100 };
	CHECK(ObjectShare::JudgeHold(ok) == V::kApply);
	CHECK(ObjectShare::JudgeRest(ok) == V::kApply);
	auto f = ok;
	f.loaded = false;
	CHECK(ObjectShare::JudgeHold(f) == V::kNotHere);
	f = ok;
	f.sameSpace = false;
	CHECK(ObjectShare::JudgeRest(f) == V::kNotHere);
	f = ok;
	f.fromUs = ObjectShare::APPLY_RANGE + 1;
	CHECK(ObjectShare::JudgeRest(f) == V::kNotHere);
	f = ok;
	f.fromUs = std::numeric_limits<float>::quiet_NaN();
	CHECK(ObjectShare::JudgeHold(f) == V::kNotHere);
	f = ok;
	f.oursHeld = true;
	CHECK(ObjectShare::JudgeHold(f) == V::kOurs);
	CHECK(ObjectShare::JudgeRest(f) == V::kOurs);
	f = ok;
	f.fromPlace = ObjectShare::HOLD_REACH + 1;
	CHECK(ObjectShare::JudgeHold(f) == V::kTooFar);
	CHECK(ObjectShare::JudgeRest(f) == V::kApply);
	f.fromPlace = ObjectShare::MAX_JUMP + 1;
	CHECK(ObjectShare::JudgeRest(f) == V::kTooFar);
	f.fromPlace = std::numeric_limits<float>::quiet_NaN();
	CHECK(ObjectShare::JudgeRest(f) == V::kTooFar);
}

TEST("moved objects judge: only a finite pose in the world is written; a launch is capped")
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(ObjectShare::Writable({ 100, 200, 300 }, { 0, 0, 3 }));
	CHECK(!ObjectShare::Writable({ nan, 200, 300 }, { 0, 0, 0 }));
	CHECK(!ObjectShare::Writable({ 100, 200, 300 }, { 0, inf, 0 }));
	CHECK(!ObjectShare::Writable({ 1e9f, 0, 0 }, { 0, 0, 0 }));
	CHECK(!ObjectShare::Writable({ 0, 0, 0 }, { 0, 0, 100 }));
	CHECK(ObjectShare::Launch({ nan, 0, 0 }) == Vec3{});
	CHECK(ObjectShare::Launch({ 100, 200, 0 }) == (Vec3{ 100, 200, 0 }));
	const auto fast = ObjectShare::Launch({ 1e6f, 0, 0 });
	CHECK(Near(fast[0], Protocol::MAX_OBJECT_SPEED, 0.5f));
}

// --- the wire -----------------------------------------------------------------------------------------------------

TEST("moved objects protocol: a peer's hold or rest that could hurt the game is turned down")
{
	using MT = Protocol::MessageType;
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const auto  holdOk = [](const Protocol::ObjectHold& a_hold) { return Protocol::DecodeObjectHold(Protocol::Encode(a_hold, MT::kReportObjectHold)).has_value(); };
	const auto  restOk = [](const Protocol::ObjectRest& a_rest) { return Protocol::DecodeObjectRest(Protocol::Encode(a_rest, MT::kReportObjectRest)).has_value(); };
	CHECK(holdOk(Hold(BROOM, 1)));
	CHECK(!holdOk(Hold(RUNTIME, 1)));
	CHECK(!holdOk(Hold(0, 1)));
	CHECK(!holdOk(Hold(Protocol::PLAYER_REF_ID, 1)));
	CHECK(!holdOk(Hold(BROOM, 1, { nan, 0, 0 })));
	CHECK(!holdOk(Hold(BROOM, 1, { 0, Protocol::MAX_HOLD_OFFSET + 1, 0 })));
	auto spun = Hold(BROOM, 1);
	spun.rotation[2] = 20.0f;
	CHECK(!holdOk(spun));
	// A truncated or padded one.
	auto bytes = Protocol::Encode(Hold(BROOM, 1), MT::kReportObjectHold);
	bytes.push_back(0);
	CHECK(!Protocol::DecodeObjectHold(bytes));
	bytes.resize(bytes.size() - 5);
	CHECK(!Protocol::DecodeObjectHold(bytes));

	CHECK(restOk(Rest(BROOM, Protocol::ObjectRestKind::kRest)));
	CHECK(restOk(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 3, { 0, 0, 0 }, { 100, 0, 50 })));
	CHECK(!restOk(Rest(RUNTIME, Protocol::ObjectRestKind::kRest)));
	CHECK(!restOk(Rest(BROOM, 2)));
	CHECK(!restOk(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 0, 0, 0 }, { 1, 0, 0 })));  // at rest, moving
	CHECK(!restOk(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 3, { 0, 0, 0 }, { Protocol::MAX_OBJECT_SPEED * 2, 0, 0 })));
	CHECK(!restOk(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { nan, 0, 0 })));
	CHECK(!restOk(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 1e9f, 0, 0 })));
	auto nowhere = Rest(BROOM, Protocol::ObjectRestKind::kRest);
	nowhere.worldspace = 0;
	CHECK(!restOk(nowhere));
	auto both = Rest(BROOM, Protocol::ObjectRestKind::kRest);
	both.cell = 0x0001F2A3;
	CHECK(!restOk(both));
}

// --- the server end to end ----------------------------------------------------------------------------------------

namespace
{
	auto HoldPacket(std::uint32_t a_ref, std::uint32_t a_sequence) { return Protocol::Encode(Hold(a_ref, a_sequence), Protocol::MessageType::kReportObjectHold); }
	auto RestPacket(const Protocol::ObjectRest& a_rest) { return Protocol::Encode(a_rest, Protocol::MessageType::kReportObjectRest); }
	auto Helds(const Packets& a_packets) { return Decoded(a_packets, MT::kObjectHeld, [](auto a_data) { return Protocol::DecodeObjectHold(a_data); }); }
	auto Resteds(const Packets& a_packets) { return Decoded(a_packets, MT::kObjectRested, [](auto a_data) { return Protocol::DecodeObjectRest(a_data); }); }
}

TEST("moved objects server: a hold goes to the others as the holder's; a second grab is refused and told the holder")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, HoldPacket(BROOM, 1));
	auto helds = Helds(Received(f, 2));
	REQUIRE(helds.size() == 1);
	CHECK(helds[0].playerId == alice);
	CHECK(helds[0].refId == BROOM);
	CHECK(Helds(Received(f, 1)).empty());

	f.fake->Deliver(2, HoldPacket(BROOM, 1));
	helds = Helds(Received(f, 2));
	REQUIRE(helds.size() == 1);  // the answer: Alice holds it
	CHECK(helds[0].playerId == alice);
	CHECK(Helds(Received(f, 1)).empty());
	// Nor does Bob's rest of it go.
	f.fake->Deliver(2, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kRest)));
	CHECK(Resteds(Received(f, 1)).empty());
}

TEST("moved objects server: a let-go and rest go to the others reliably; then the friend may take it")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	const auto bob = JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	f.fake->Deliver(1, HoldPacket(BROOM, 1));
	f.fake->Deliver(1, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 300, 50 })));
	f.fake->Deliver(1, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kRest)));
	const auto rested = Resteds(Received(f, 2));
	REQUIRE(rested.size() == 2);
	CHECK(rested[0].playerId == alice);
	CHECK(rested[0].kind == Protocol::ObjectRestKind::kLetGo);
	CHECK(Near(rested[0].velocity, { 0, 300, 50 }));
	CHECK(rested[1].kind == Protocol::ObjectRestKind::kRest);

	f.fake->Deliver(2, HoldPacket(BROOM, 1));
	const auto helds = Helds(Received(f, 1));
	REQUIRE(helds.size() == 1);
	CHECK(helds[0].playerId == bob);
}

TEST("moved objects server: a rest refused for another player's is answered with theirs, to the refused game alone")
{
	Fixture    f;
	const auto alice = JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	JoinAs(f, 3, "Carol");
	Clear(f, { 1, 2, 3 });

	// Alice's blast threw the tire, and Bob stood by it: both say where it rests, Alice first.
	f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 100, 0, 0 })));
	f.fake->Deliver(2, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 140, 0, 0 })));
	auto toBob = Resteds(Received(f, 2));
	REQUIRE(toBob.size() == 2);  // hers as it went round, and hers again as the answer to his
	for (const auto& rest : toBob) {
		CHECK(rest.playerId == alice);
		CHECK(rest.refId == TIRE);
		CHECK(rest.kind == Protocol::ObjectRestKind::kRest);
		CHECK(Near(rest.position, { 100, 0, 0 }));
	}
	CHECK(Resteds(Received(f, 1)).empty());  // nothing of his reaches her
	auto toCarol = Resteds(Received(f, 3));
	REQUIRE(toCarol.size() == 1);  // nor Carol
	CHECK(Near(toCarol[0].position, { 100, 0, 0 }));

	// Past SETTLED his rest of it goes round like any other.
	f.Advance(ObjectMovers::SETTLED + 100ms);
	f.fake->Deliver(2, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { 140, 0, 0 })));
	auto toAlice = Resteds(Received(f, 1));
	REQUIRE(toAlice.size() == 1);
	CHECK(Near(toAlice[0].position, { 140, 0, 0 }));
	CHECK(Resteds(Received(f, 2)).empty());

	// A rest refused for a hold is answered with the holder's hold, as a refused grab is; a let-go of what the refused
	// player never held is answered with nothing.
	f.fake->Deliver(1, HoldPacket(BROOM, 1));
	Clear(f, { 1, 2, 3 });
	f.fake->Deliver(2, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kRest)));
	auto helds = Helds(Received(f, 2));
	REQUIRE(helds.size() == 1);
	CHECK(helds[0].playerId == alice);
	CHECK(Resteds(Received(f, 2)).empty());
	f.fake->Deliver(2, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 100, 0 })));
	const auto received = Received(f, 2);
	CHECK(Helds(received).empty());
	CHECK(Resteds(received).empty());
}

TEST("moved objects server: holds on a budget; nothing from a player not yet welcomed; a leaver's hold is over")
{
	Fixture f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });
	for (std::uint32_t i = 1; i <= 100; ++i) {
		f.fake->Deliver(1, HoldPacket(BROOM, i));
	}
	const auto relayed = Helds(Received(f, 2)).size();
	CHECK(relayed >= 1);
	CHECK(relayed <= 2 * static_cast<std::size_t>(Protocol::HOLDS_PER_SECOND));  // a second may turn over meanwhile

	f.fake->Connect(3);
	f.fake->Deliver(3, HoldPacket(TIRE, 1));
	CHECK(Helds(Received(f, 2)).empty());

	// Alice leaves holding the broom: Bob may grab it at once.
	f.fake->Drop(1);
	JoinAs(f, 4, "Carol");
	Clear(f, { 2, 4 });
	f.fake->Deliver(2, HoldPacket(BROOM, 1));
	CHECK(Helds(Received(f, 4)).size() == 1);
}

TEST("moved objects server: where an object came to rest is told to a player who joins later, naming nobody; a refused rest and a picked-up object are not")
{
	constexpr std::uint32_t BOTTLE = 0x0001A003;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob");
	Clear(f, { 1, 2 });

	// Alice throws the broom; Bob's say of where it lies is refused while it is still hers.
	f.fake->Deliver(1, HoldPacket(BROOM, 1));
	f.fake->Deliver(1, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kLetGo, 1, { 0, 0, 0 }, { 0, 300, 50 })));
	f.fake->Deliver(1, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 100, 600, 30 })));
	Clear(f, { 1 });
	f.fake->Deliver(2, RestPacket(Rest(BROOM, Protocol::ObjectRestKind::kRest, 0, { 900, 900, 0 })));
	// Bob knocks the tire over, with nobody near; and a bottle, which he then picks up.
	f.fake->Deliver(2, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 })));
	f.fake->Deliver(2, RestPacket(Rest(BOTTLE, Protocol::ObjectRestKind::kRest)));
	f.fake->Deliver(2, Protocol::Encode(Protocol::RefPickedUp{ BOTTLE }, MT::kReportPickup));
	Clear(f, { 1, 2 });

	JoinAs(f, 3, "Carol");
	const auto places = Resteds(Received(f, 3));
	REQUIRE(places.size() == 2);
	CHECK(places[0].refId == BROOM);  // moved longest ago first
	CHECK(Near(places[0].position, { 100, 600, 30 }));
	CHECK(places[1].refId == TIRE);
	CHECK(Near(places[1].position, { -50, 20, 4 }));
	for (const auto& place : places) {
		CHECK(place.playerId == 0);
		CHECK(place.kind == Protocol::ObjectRestKind::kRest);
		CHECK(place.worldspace == SANCTUARY);
	}
}

TEST("moved objects server: a joiner's game gives places to so many objects a run, so their made-up ones can't push out the host's; past it theirs still go round, unkept")
{
	constexpr std::uint64_t HOST_IDENTITY = 0x4057;
	constexpr std::uint32_t MADE_UP = 0x7F000001;  // a plugin index nobody loads
	Server::Options         options;
	options.hostIdentity = HOST_IDENTITY;
	options.maxObjectPlacesPerPlayer = 3;
	Fixture f{ options };
	f.fake->SetLocal(1);
	JoinAs(f, 1, "Host", HOST_IDENTITY);
	const auto mallory = JoinAs(f, 2, "Mallory");
	JoinAs(f, 3, "Bob");
	Clear(f, { 1, 2, 3 });

	// The host's game has no share: the tire and four more are kept.
	f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 })));
	for (std::uint32_t i = 0; i < 4; ++i) {
		f.fake->Deliver(1, RestPacket(Rest(0x0001B001 + i, Protocol::ObjectRestKind::kRest)));
	}
	// Mallory's ten made-up ones all go to the others live; three are kept.
	Clear(f, { 3 });
	for (std::uint32_t i = 0; i < 10; ++i) {
		f.fake->Deliver(2, RestPacket(Rest(MADE_UP + i, Protocol::ObjectRestKind::kRest)));
	}
	CHECK(Resteds(Received(f, 3)).size() == 10);
	CHECK(f.LogLines(std::format("player {} has added 3 moved object place entries this run", mallory)).size() == 1);
	// A new rest of one already kept takes nothing from her share.
	f.fake->Deliver(2, RestPacket(Rest(MADE_UP, Protocol::ObjectRestKind::kRest, 0, { 7, 8, 9 })));
	Clear(f, { 1, 2, 3 });

	JoinAs(f, 4, "Carol");
	std::set<std::uint32_t> kept;
	for (const auto& place : Resteds(Received(f, 4))) {
		kept.insert(place.refId);
		if (place.refId == MADE_UP) {
			CHECK(Near(place.position, { 7, 8, 9 }));
		}
		if (place.refId == TIRE) {
			CHECK(Near(place.position, { -50, 20, 4 }));
		}
	}
	CHECK(kept.size() == 8);
	CHECK(kept.contains(TIRE));
	CHECK(kept.contains(0x0001B004));
	CHECK(kept.contains(MADE_UP + 2));
	CHECK(!kept.contains(MADE_UP + 3));
}

TEST("moved objects server: a joiner's share of places is well under what the session keeps, whatever --max-share gives")
{
	CHECK(Server::Options{}.maxObjectPlacesPerPlayer * 2 <= ObjectMovers::MAX_PLACES);
	CHECK(ObjectMovers::PlacesShare(Server::Options{}.maxObjectPlacesPerPlayer) == Server::Options{}.maxObjectPlacesPerPlayer);
	// --max-share sets it with the other shares (15000, 10000 by default): any higher is held to the cap, a lower one kept.
	CHECK(ObjectMovers::PlacesShare(5000) * 2 <= ObjectMovers::MAX_PLACES);
	CHECK(ObjectMovers::PlacesShare(ObjectMovers::MAX_PLACES) * 2 <= ObjectMovers::MAX_PLACES);
	CHECK(ObjectMovers::PlacesShare(3) == 3);
	CHECK(ObjectMovers::PlacesShare(5000, 8) == 2);  // a quarter of what the session keeps
}

TEST("moved objects server: a joiner's share of places set as high as the session keeps is held to a quarter, so the host's stay")
{
	constexpr std::uint64_t HOST_IDENTITY = 0x4057;
	constexpr std::uint32_t MADE_UP = 0x7F000001;
	Server::Options         options;
	options.hostIdentity = HOST_IDENTITY;
	options.maxObjectPlaces = 8;  // MAX_PLACES scaled down: the share it allows is 2
	options.maxObjectPlacesPerPlayer = 5000;  // --max-share 5000
	Fixture f{ options };
	f.fake->SetLocal(1);
	JoinAs(f, 1, "Host", HOST_IDENTITY);
	const auto mallory = JoinAs(f, 2, "Mallory");
	Clear(f, { 1, 2 });
	CHECK(f.LogLines("2 moved object places of 8").size() == 1);

	f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 })));
	for (std::uint32_t i = 0; i < 3; ++i) {
		f.fake->Deliver(1, RestPacket(Rest(0x0001B001 + i, Protocol::ObjectRestKind::kRest)));
	}
	// Ten made-up ones: with the setting as given they would fill the session and push the host's out.
	for (std::uint32_t i = 0; i < 10; ++i) {
		f.fake->Deliver(2, RestPacket(Rest(MADE_UP + i, Protocol::ObjectRestKind::kRest)));
	}
	Clear(f, { 1, 2 });  // handled by now
	CHECK(f.LogLines(std::format("player {} has added 2 moved object place entries this run", mallory)).size() == 1);

	JoinAs(f, 3, "Carol");
	std::set<std::uint32_t> kept;
	for (const auto& place : Resteds(Received(f, 3))) {
		kept.insert(place.refId);
	}
	CHECK(kept.size() == 6);
	CHECK(kept.contains(TIRE));
	for (std::uint32_t i = 0; i < 3; ++i) {
		CHECK(kept.contains(0x0001B001 + i));
	}
	CHECK(kept.contains(MADE_UP + 1));
	CHECK(!kept.contains(MADE_UP + 2));
}

TEST("moved objects server: the places are saved with the session, and a joiner after a restart gets them")
{
	const TempSessionFile file;
	Server::Options       options;
	options.sessionFile = file.path.string();
	{
		Fixture f{ options };
		JoinAs(f, 1, "Alice", 0xA11CE);
		f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 })));
		Clear(f, { 1 });
	}  // stopping saves
	{
		Fixture f{ options };
		JoinAs(f, 2, "Bob");
		const auto places = Resteds(Received(f, 2));
		REQUIRE(places.size() == 1);
		CHECK(places[0].refId == TIRE);
		CHECK(places[0].playerId == 0);
		CHECK(Near(places[0].position, { -50, 20, 4 }));
	}
}

TEST("moved objects server: a game that loads a save, or rejoins, is told the places again, each object's newest")
{
	constexpr std::uint64_t BOB_IDENTITY = 0xB0B;
	Fixture                 f;
	JoinAs(f, 1, "Alice");
	JoinAs(f, 2, "Bob", BOB_IDENTITY);
	Clear(f, { 1, 2 });
	// Alice knocks the tire over with Bob away, then again.
	f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -50, 20, 4 })));
	f.fake->Deliver(1, RestPacket(Rest(TIRE, Protocol::ObjectRestKind::kRest, 0, { -80, 25, 4 })));
	Clear(f, { 1, 2 });
	const auto told = [](const Packets& a_packets) {
		const auto places = Resteds(a_packets);
		return places.size() == 1 && places[0].refId == TIRE && places[0].playerId == 0 && Near(places[0].position, { -80, 25, 4 });
	};

	// Bob loads a save from before it: the world state he asks for carries the place.
	f.fake->Deliver(2, Protocol::Encode(Protocol::WorldRequest{ 0, 0 }));
	CHECK(told(Received(f, 2)));
	// Bob leaves and comes back.
	f.fake->Drop(2);
	REQUIRE(f.fake->Settle());
	JoinAs(f, 3, "Bob", BOB_IDENTITY);
	CHECK(told(Received(f, 3)));
}
