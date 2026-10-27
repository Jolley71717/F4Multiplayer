// Which disabled power armor frames a load brings are frame copies of ours a save kept switched off (FrameLeftovers): only
// one proven ours (lingering from the load, a workshop copy's keyword, or a stand-in's base for its owner) and nobody's
// goes, never a frame our player owns, one a quest holds, one worn, one lent, nor one with no proof at all.

#include "Test.h"
#include "game/FrameLeftovers.h"

namespace
{
	using FrameLeftovers::Facts;
	using FrameLeftovers::Owner;
	using Clock = std::chrono::steady_clock;

	// A copy of ours a save kept: made at runtime, disabled, empty, nobody's, and proven ours (it lingers from the load).
	constexpr Facts LEFTOVER{ .frame = true, .created = true, .disabled = true, .lingering = true };

	constexpr std::uint32_t STAND_IN_BASE = 0x0020A57B;  // a base stand-ins are placed from (the male settler)
	constexpr std::uint32_t PLAYER_BASE = 0x00000007;

	constexpr bool StandInBase(std::uint32_t a_base) { return a_base == STAND_IN_BASE; }

	int dummy = 0, other = 0;  // addresses for the watch to compare
}

TEST("frame leftovers: a disabled, empty frame copy nobody holds goes, with proof that it is ours")
{
	CHECK(FrameLeftovers::Leftover(LEFTOVER));
}

TEST("frame leftovers: proof is the lingering copy, the workshop copy's keyword, or a stand-in's base as owner; none, it stays")
{
	Facts plain = LEFTOVER;
	plain.lingering = false;
	CHECK(!FrameLeftovers::Proven(plain));
	CHECK(!FrameLeftovers::Leftover(plain));  // a plain disabled frame made at runtime: the game's for all we can tell
	auto marked = plain;
	marked.marked = true;
	CHECK(FrameLeftovers::Proven(marked));
	CHECK(FrameLeftovers::Leftover(marked));
	auto standIn = plain;
	standIn.owner = Owner::kStandIn;
	CHECK(FrameLeftovers::Proven(standIn));
	CHECK(FrameLeftovers::Leftover(standIn));
	CHECK(FrameLeftovers::Proven(LEFTOVER));
}

TEST("frame leftovers: only a disabled frame made at runtime, not deleted already")
{
	auto notFrame = LEFTOVER;
	notFrame.frame = false;
	CHECK(!FrameLeftovers::Leftover(notFrame));
	auto plugin = LEFTOVER;
	plugin.created = false;  // a frame a plugin placed: every game has it
	CHECK(!FrameLeftovers::Leftover(plugin));
	auto shown = LEFTOVER;
	shown.disabled = false;
	CHECK(!FrameLeftovers::Leftover(shown));
	auto deleted = LEFTOVER;
	deleted.deleted = true;
	CHECK(!FrameLeftovers::Leftover(deleted));
}

TEST("frame leftovers: a frame someone wears, or that our player left last, stays, proven ours or not")
{
	for (const bool marked : { false, true }) {
		auto worn = LEFTOVER;
		worn.marked = marked;
		worn.worn = true;
		CHECK(!FrameLeftovers::Leftover(worn));
		// An NPC away in their suit: the engine keeps the frame persistent while they wear it, wherever they are.
		auto promoted = LEFTOVER;
		promoted.marked = marked;
		promoted.persistent = true;
		CHECK(!FrameLeftovers::Leftover(promoted));
	}
}

TEST("frame leftovers: a frame with anything in it, or that its base would fill, stays")
{
	auto holds = LEFTOVER;
	holds.holdsItems = true;
	CHECK(!FrameLeftovers::Leftover(holds));
	holds.marked = true;
	holds.owner = Owner::kStandIn;
	CHECK(!FrameLeftovers::Leftover(holds));  // however well proven ours: a frame full of pieces is never deleted
}

TEST("frame leftovers: a frame a quest holds, or that the game links or enables, stays")
{
	auto quest = LEFTOVER;
	quest.quest = true;
	CHECK(!FrameLeftovers::Leftover(quest));
	auto linked = LEFTOVER;
	linked.linked = true;
	CHECK(!FrameLeftovers::Leftover(linked));
}

TEST("frame leftovers: a frame lent to or by a friend, or one of the session's own copies, stays")
{
	auto lent = LEFTOVER;
	lent.lent = true;
	CHECK(!FrameLeftovers::Leftover(lent));
	auto session = LEFTOVER;
	session.session = true;
	CHECK(!FrameLeftovers::Leftover(session));
}

TEST("frame leftovers: a frame with an owner is someone's, unless the owner is a stand-in's base")
{
	using FrameLeftovers::OwnerOf;
	CHECK(OwnerOf(false, std::nullopt, StandInBase) == Owner::kNone);
	CHECK(OwnerOf(true, PLAYER_BASE, StandInBase) == Owner::kOther);      // our player got into it once: theirs
	CHECK(OwnerOf(true, STAND_IN_BASE, StandInBase) == Owner::kStandIn);  // the switch-in gave our copy the stand-in's base
	// An owner that cannot be told (gone after the load, or not readable): someone's all the same.
	CHECK(OwnerOf(true, std::nullopt, StandInBase) == Owner::kOther);
	auto owned = LEFTOVER;
	owned.owner = OwnerOf(true, PLAYER_BASE, StandInBase);
	CHECK(!FrameLeftovers::Leftover(owned));
	owned.marked = true;
	CHECK(!FrameLeftovers::Leftover(owned));  // our player's own suit, however it is marked
}

TEST("frame leftovers: a frame found after a load is judged only once it has stood so a while, with no loading screen")
{
	FrameLeftovers::Watch watch;
	const auto            t0 = Clock::now();
	watch.Add(0xFF000801, 0x0019DED9, &dummy, t0);
	CHECK(watch.Size() == 1);
	CHECK(watch.TakeDue(t0 + std::chrono::seconds(1), false).empty());
	CHECK(watch.TakeDue(t0 + FrameLeftovers::LOOK_AGAIN, true).empty());  // a loading screen: not yet
	const auto due = watch.TakeDue(t0 + FrameLeftovers::LOOK_AGAIN, false);
	REQUIRE(due.size() == 1);
	CHECK(due[0].id == 0xFF000801);
	CHECK(watch.Size() == 0);
}

TEST("frame leftovers: each frame is watched once, at most MAX_WATCHED, and a load starts afresh")
{
	FrameLeftovers::Watch watch;
	const auto            t0 = Clock::now();
	watch.Add(0xFF000801, 0x0019DED9, &dummy, t0);
	watch.Add(0xFF000801, 0x0019DED9, &dummy, t0);
	CHECK(watch.Size() == 1);
	for (std::uint32_t i = 0; i < FrameLeftovers::MAX_WATCHED + 10; ++i) {
		watch.Add(0xFF100000 + i, 0x0019DED9, &dummy, t0);
	}
	CHECK(watch.Size() == FrameLeftovers::MAX_WATCHED);
	watch.Clear();
	CHECK(watch.Size() == 0);
}

TEST("frame leftovers: only the very form found after the load is judged")
{
	const FrameLeftovers::Watch::Entry found{ .id = 0xFF000801, .base = 0x0019DED9, .ref = &dummy };
	CHECK(FrameLeftovers::Same(found, &dummy, 0x0019DED9));
	CHECK(!FrameLeftovers::Same(found, &other, 0x0019DED9));  // another form under the id now
	CHECK(!FrameLeftovers::Same(found, &dummy, 0x00140000));  // another base
	CHECK(!FrameLeftovers::Same(found, nullptr, 0x0019DED9));  // gone
}
