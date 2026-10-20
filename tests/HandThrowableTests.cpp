#include "Test.h"

#include "game/HandThrowable.h"

#include <algorithm>
#include <utility>

using namespace std::chrono_literals;
using HandThrowable::AtAttach;
using HandThrowable::Book;
using HandThrowable::Clock;
using HandThrowable::Cue;
using HandThrowable::Drop;
using HandThrowable::Skip;
using HandThrowable::WhileHeld;

namespace
{
	const auto              START = Clock::time_point{} + 1h;
	constexpr std::uint32_t STAND_IN = 0xFF000801;
	constexpr std::uint32_t OTHER = 0xFF000802;
	constexpr std::uint32_t FRAG = 0x00A00001;     // two throwables with models of their own (made-up form IDs)
	constexpr std::uint32_t MOLOTOV = 0x00A00002;

	// A stand-in at its replayed throw's weaponAttach(2), holding a frag grenade, with the dev's handthrowable model on
	// (the database's node allowed: the hand has no WeaponLeft of the engine's in these).
	AtAttach Attaching(std::uint32_t a_throwable = FRAG)
	{
		AtAttach seen;
		seen.enabled = true;
		seen.replayed = true;
		seen.loaded = true;
		seen.handFound = true;
		seen.throwable = a_throwable;
		seen.modelFallback = true;
		return seen;
	}

	// The same as a player has it by default (bGrenadeInHand on): the engine's own culled WeaponLeft, holding the
	// grenade, under the hand, and the database's node not allowed.
	AtAttach AttachingAsShipped(std::uint32_t a_throwable = FRAG)
	{
		auto seen = Attaching(a_throwable);
		seen.engineHides = true;
		seen.modelFallback = false;
		return seen;
	}

	// The same stand-in a frame later, the model still where it was hung.
	WhileHeld Holding(std::uint32_t a_throwable = FRAG)
	{
		WhileHeld seen;
		seen.enabled = true;
		seen.loaded = true;
		seen.underHand = true;
		seen.throwable = a_throwable;
		return seen;
	}
}

TEST("hand throwable: the throwable's attach and its release are read off the graph's events, equip index 2 only")
{
	CHECK(HandThrowable::CueOf("weaponAttach", "2") == Cue::kAttach);
	CHECK(HandThrowable::CueOf("WeaponFire", "2") == Cue::kRelease);
	CHECK(HandThrowable::CueOf("weaponDetach", "2") == Cue::kRelease);
	// A gun's shot, and the gun's own attach and detach, say nothing about the grenade.
	CHECK(HandThrowable::CueOf("WeaponFire", "") == Cue::kNone);
	CHECK(HandThrowable::CueOf("weaponAttach", "0") == Cue::kNone);
	CHECK(HandThrowable::CueOf("weaponDetach", "") == Cue::kNone);
	CHECK(HandThrowable::CueOf("weaponSwing", "2") == Cue::kNone);
}

TEST("hand throwable: nothing is hung while turned off, whatever else holds")
{
	Book book;
	auto seen = Attaching();
	seen.enabled = false;
	CHECK(book.ShouldHang(STAND_IN, seen) == Skip::kOff);
	CHECK(book.ShouldHang(STAND_IN, Attaching()) == Skip::kNone);
}

TEST("hand throwable: as shipped, the engine's own culled WeaponLeft is drawn and nothing of the database's is hung")
{
	Book book;
	CHECK(book.ShouldHang(STAND_IN, AttachingAsShipped()) == Skip::kNone);
	// No WeaponLeft to draw (or an empty one): the hand stays empty rather than getting the database's shared node.
	auto none = AttachingAsShipped();
	none.engineHides = false;
	CHECK(book.ShouldHang(STAND_IN, none) == Skip::kNoEngineNode);
	// Only the dev's handthrowable model lets that node be hung.
	none.modelFallback = true;
	CHECK(book.ShouldHang(STAND_IN, none) == Skip::kNone);
	// Off (bGrenadeInHand = false, or the dev's handthrowable off), nothing is drawn either.
	auto off = AttachingAsShipped();
	off.enabled = false;
	CHECK(book.ShouldHang(STAND_IN, off) == Skip::kOff);
}

TEST("hand throwable: as shipped, many throws in a row each draw and each come off, two friends at once included")
{
	Book book;
	auto now = START;
	for (int throwNo = 0; throwNo < 50; ++throwNo) {
		CHECK(book.ShouldHang(STAND_IN, AttachingAsShipped()) == Skip::kNone);
		book.Hung(STAND_IN, FRAG, now, false);
		CHECK(book.ShouldHang(OTHER, AttachingAsShipped()) == Skip::kNone);
		book.Hung(OTHER, FRAG, now, false);
		CHECK(!book.ModelBusy(FRAG));
		CHECK(book.Check(STAND_IN, Holding(), now + 300ms) == Drop::kKeep);
		book.Released(STAND_IN);
		book.Released(OTHER);
		CHECK(book.Check(STAND_IN, Holding(), now + 500ms) == Drop::kReleased);
		CHECK(book.Check(OTHER, Holding(), now + 500ms) == Drop::kReleased);
		book.Forget(STAND_IN);
		book.Forget(OTHER);
		now += 2s;
	}
	CHECK(book.Size() == 0);
}

TEST("hand throwable: only a friend's throw replayed here gets the model, never an NPC's own")
{
	Book book;
	auto seen = Attaching();
	seen.replayed = false;
	CHECK(book.ShouldHang(STAND_IN, seen) == Skip::kNotReplayed);
}

TEST("hand throwable: nothing is hung on an actor without its 3D or hand, or with nothing to throw")
{
	Book book;
	auto unloaded = Attaching();
	unloaded.loaded = false;
	CHECK(book.ShouldHang(STAND_IN, unloaded) == Skip::kNotLoaded);
	auto handless = Attaching();
	handless.handFound = false;
	CHECK(book.ShouldHang(STAND_IN, handless) == Skip::kNoHand);
	CHECK(book.ShouldHang(STAND_IN, Attaching(0)) == Skip::kNoThrowable);
}

TEST("hand throwable: where the engine hung its own WeaponLeft, ours stays out")
{
	Book book;
	auto seen = Attaching();
	seen.engineShows = true;
	CHECK(book.ShouldHang(STAND_IN, seen) == Skip::kEngineShows);
}

TEST("hand throwable: one model hangs on one stand-in at a time, and a second attach on the holder hangs nothing more")
{
	Book book;
	CHECK(book.ShouldHang(STAND_IN, Attaching()) == Skip::kNone);
	book.Hung(STAND_IN, FRAG, START);
	CHECK(book.Holds(STAND_IN));
	CHECK(book.ShouldHang(STAND_IN, Attaching()) == Skip::kHolding);
	// The database keeps one node per model: another friend's frag waits; their Molotov is another model.
	CHECK(book.ModelBusy(FRAG));
	CHECK(book.ShouldHang(OTHER, Attaching()) == Skip::kModelBusy);
	CHECK(book.ShouldHang(OTHER, Attaching(MOLOTOV)) == Skip::kNone);
	// Once the first lets go, the frag's model is free again.
	book.Forget(STAND_IN);
	CHECK(!book.ModelBusy(FRAG));
	CHECK(book.ShouldHang(OTHER, Attaching()) == Skip::kNone);
}

TEST("hand throwable: drawing a stand-in's own culled WeaponLeft shares nothing, so two stand-ins can show the same grenade")
{
	Book book;
	auto hides = Attaching();
	hides.engineHides = true;
	CHECK(book.ShouldHang(STAND_IN, hides) == Skip::kNone);
	book.Hung(STAND_IN, FRAG, START, false);
	CHECK(book.Holds(STAND_IN));
	CHECK(!book.ModelBusy(FRAG));
	// Another friend's frag: their own WeaponLeft is drawn too, or the database's node is free to hang.
	CHECK(book.ShouldHang(OTHER, hides) == Skip::kNone);
	CHECK(book.ShouldHang(OTHER, Attaching()) == Skip::kNone);
	// The database's node hung on one stand-in is still busy for a second that needs it, not for one that hides its own.
	book.Hung(OTHER, FRAG, START);
	CHECK(book.ModelBusy(FRAG));
	CHECK(book.ShouldHang(0xFF000803, Attaching()) == Skip::kModelBusy);
	CHECK(book.ShouldHang(0xFF000803, hides) == Skip::kNone);
	// Its own drawn WeaponLeft comes off at the release like a hung model.
	book.Released(STAND_IN);
	CHECK(book.Check(STAND_IN, Holding(), START + 400ms) == Drop::kReleased);
}

TEST("hand throwable: a WeaponLeft the engine draws itself keeps ours out, one it culls does not")
{
	Book book;
	auto drawn = Attaching();
	drawn.engineShows = true;
	drawn.engineHides = false;
	CHECK(book.ShouldHang(STAND_IN, drawn) == Skip::kEngineShows);
	auto culled = Attaching();
	culled.engineHides = true;
	CHECK(book.ShouldHang(STAND_IN, culled) == Skip::kNone);
}

TEST("hand throwable: no more than MAX_HOLDS stand-ins hold a model at once")
{
	Book book;
	for (std::uint32_t i = 0; i < HandThrowable::MAX_HOLDS; ++i) {
		book.Hung(0xFF001000 + i, 0x00100000 + i, START);
	}
	CHECK(book.ShouldHang(STAND_IN, Attaching()) == Skip::kFull);
}

TEST("hand throwable: the model stays in the hand through the wind-up and comes off at the release")
{
	Book book;
	book.Hung(STAND_IN, FRAG, START);
	CHECK(book.Check(STAND_IN, Holding(), START + 500ms) == Drop::kKeep);
	book.Released(STAND_IN);
	CHECK(book.Check(STAND_IN, Holding(), START + 900ms) == Drop::kReleased);
	// A release for an actor holding nothing is nothing to do.
	book.Released(OTHER);
	CHECK(!book.Holds(OTHER));
	CHECK(book.Check(OTHER, Holding(), START) == Drop::kKeep);
}

TEST("hand throwable: a release that never comes leaves no grenade in the hand past MAX_HOLD")
{
	Book book;
	book.Hung(STAND_IN, FRAG, START);
	CHECK(book.Check(STAND_IN, Holding(), START + HandThrowable::MAX_HOLD - 1ms) == Drop::kKeep);
	CHECK(book.Check(STAND_IN, Holding(), START + HandThrowable::MAX_HOLD) == Drop::kTimedOut);
}

TEST("hand throwable: every other way out takes the model off at once, before the release or the time limit")
{
	Book book;
	book.Hung(STAND_IN, FRAG, START);
	const auto soon = START + 100ms;

	auto off = Holding();
	off.enabled = false;
	CHECK(book.Check(STAND_IN, off, soon) == Drop::kOff);

	auto gone = Holding();
	gone.loaded = false;
	CHECK(book.Check(STAND_IN, gone, soon) == Drop::kGone);

	auto moved = Holding();
	moved.underHand = false;
	CHECK(book.Check(STAND_IN, moved, soon) == Drop::kMoved);

	auto engine = Holding();
	engine.engineShows = true;
	CHECK(book.Check(STAND_IN, engine, soon) == Drop::kEngineShows);

	CHECK(book.Check(STAND_IN, Holding(MOLOTOV), soon) == Drop::kGearChanged);
	CHECK(book.Check(STAND_IN, Holding(0), soon) == Drop::kGearChanged);
}

TEST("hand throwable: a 3D gone wins over a release, so the model is never looked for in a 3D that is not there")
{
	Book book;
	book.Hung(STAND_IN, FRAG, START);
	book.Released(STAND_IN);
	auto gone = Holding();
	gone.loaded = false;
	gone.underHand = false;
	CHECK(book.Check(STAND_IN, gone, START + 100ms) == Drop::kGone);
}

TEST("hand throwable: the book lists who holds a model, for taking them all off before a save or a load")
{
	Book book;
	book.Hung(STAND_IN, FRAG, START);
	book.Hung(OTHER, MOLOTOV, START);
	auto actors = book.Actors();
	std::ranges::sort(actors);
	CHECK((actors == std::vector<std::uint32_t>{ STAND_IN, OTHER }));
	for (const auto actor : actors) {
		book.Forget(actor);
	}
	CHECK(book.Size() == 0);
}

TEST("hand throwable: every reason has a name for the log")
{
	for (auto skip = std::to_underlying(Skip::kNone); skip <= std::to_underlying(Skip::kFull); ++skip) {
		CHECK(HandThrowable::Name(static_cast<Skip>(skip)) != "?");
	}
	for (auto drop = std::to_underlying(Drop::kKeep); drop <= std::to_underlying(Drop::kDeleted); ++drop) {
		CHECK(HandThrowable::Name(static_cast<Drop>(drop)) != "?");
	}
}
