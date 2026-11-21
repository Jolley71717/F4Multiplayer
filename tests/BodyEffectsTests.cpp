// What a death effect made of a body is made the same in every game (BodyEffects.h, as BodyStates asks): a game tells
// what its dead bodies show once the effect has ended, and makes of its own body what another told and its own lacks.

#include "Test.h"

#include "game/BodyEffects.h"

#include <algorithm>

using namespace std::chrono_literals;

namespace
{
	using BodyEffects::Book;
	using BodyEffects::Clock;
	using BodyEffects::State;
	using BodyEffects::Verdict;

	constexpr std::uint32_t RAIDER = 0x001AE4A2;       // a plugin's reference: the same id in every game
	constexpr std::uint32_t SPAWN = 0xFF000042;        // a spawn id
	constexpr std::uint32_t BLUE_PILE = 0x00187990;    // AshPileBlue
	constexpr std::uint32_t QUEEN_PILE = 0x001C6BD1;   // MirelurkQueenGooPile01

	constexpr State WHOLE{};                                               // a body as it fell
	constexpr State ASH{ BodyEffects::DISINTEGRATE_END, BodyEffects::ASH_PILE };
	constexpr State GOO{ BodyEffects::GOO_END, BodyEffects::GOO_PILE };
	constexpr State FROZEN{ BodyEffects::FREEZE_END, 0 };
	constexpr State BURNING{ BodyEffects::DISINTEGRATE_START, 0 };         // the laser's effect still playing

	const Clock::time_point T0{};
}

TEST("body effects: the stages an effect ends at are told and made; the ones it plays through are waited out")
{
	for (const std::uint8_t stage : { BodyEffects::GOO_END, BodyEffects::DISINTEGRATE_END, BodyEffects::FREEZE_END }) {
		CHECK(BodyEffects::Ended(stage));
		CHECK(!BodyEffects::Running(stage));
		CHECK(BodyEffects::StartOf(stage) == stage - 1);
		CHECK(BodyEffects::Running(BodyEffects::StartOf(stage)));
	}
	for (const std::uint8_t stage : { std::uint8_t{ 0 }, std::uint8_t{ 1 }, std::uint8_t{ 3 }, std::uint8_t{ 5 }, std::uint8_t{ 7 } }) {
		CHECK(!BodyEffects::Ended(stage));
		CHECK(BodyEffects::StartOf(stage) == 0);
	}
	CHECK(!BodyEffects::Running(0));
	CHECK(!BodyEffects::Running(7));

	// Ash and goo leave a pile for the body; a frozen body stays.
	CHECK(BodyEffects::LeavesPile(BodyEffects::DISINTEGRATE_END));
	CHECK(BodyEffects::LeavesPile(BodyEffects::GOO_END));
	CHECK(!BodyEffects::LeavesPile(BodyEffects::FREEZE_END));
	CHECK(!BodyEffects::LeavesPile(BodyEffects::DISINTEGRATE_START));
	CHECK(BodyEffects::DefaultPile(BodyEffects::DISINTEGRATE_END) == BodyEffects::ASH_PILE);
	CHECK(BodyEffects::DefaultPile(BodyEffects::GOO_END) == BodyEffects::GOO_PILE);
	CHECK(BodyEffects::DefaultPile(BodyEffects::FREEZE_END) == 0);
	CHECK(BodyEffects::DefaultPile(0) == 0);
}

TEST("body effects: a body told unseen with no pile gets the game's own, so what it carried can be looted")
{
	// The teller's pile was a mod's this game lacks (the translator made it none).
	CHECK(BodyEffects::Whole(State{ BodyEffects::DISINTEGRATE_END, 0 }) == ASH);
	CHECK(BodyEffects::Whole(State{ BodyEffects::GOO_END, 0 }) == GOO);
	// A pile that was told stands, and a frozen body has none.
	CHECK(BodyEffects::Whole(State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }) == (State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }));
	CHECK(BodyEffects::Whole(State{ BodyEffects::GOO_END, QUEEN_PILE }).pile == QUEEN_PILE);
	CHECK(BodyEffects::Whole(FROZEN) == FROZEN);
	CHECK(BodyEffects::Whole(WHOLE) == WHOLE);
}

TEST("body effects: the first thing known of a body stands; a pile is only added to a body of its stage")
{
	CHECK(BodyEffects::Merged(WHOLE, ASH) == ASH);
	CHECK(BodyEffects::Merged(ASH, WHOLE) == ASH);
	CHECK(BodyEffects::Merged(ASH, ASH) == ASH);
	// Both games killed it at once, each with its own weapon: each keeps what it has.
	CHECK(BodyEffects::Merged(ASH, GOO) == ASH);
	CHECK(BodyEffects::Merged(GOO, ASH) == GOO);
	CHECK(BodyEffects::Merged(FROZEN, ASH) == FROZEN);
	// The stage known, the pile learned after (our game couldn't read its own pile yet), and the other way round.
	CHECK(BodyEffects::Merged(State{ BodyEffects::DISINTEGRATE_END, 0 }, State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }) == (State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }));
	CHECK(BodyEffects::Merged(State{ 0, BLUE_PILE }, ASH) == (State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }));
	// A goo pile is not put on a frozen body.
	CHECK(BodyEffects::Merged(FROZEN, GOO) == FROZEN);

	CHECK(BodyEffects::Adds(ASH, WHOLE));
	CHECK(!BodyEffects::Adds(ASH, ASH));
	CHECK(!BodyEffects::Adds(GOO, ASH));
	CHECK(!BodyEffects::Adds(WHOLE, ASH));
	CHECK(BodyEffects::Adds(ASH, State{ BodyEffects::DISINTEGRATE_END, 0 }));
}

TEST("body effects: our body gets only what it lacks, and is never taken from one stage to another")
{
	CHECK(BodyEffects::Needed(ASH, WHOLE) == ASH);
	CHECK(BodyEffects::Needed(GOO, WHOLE) == GOO);
	CHECK(BodyEffects::Needed(FROZEN, WHOLE) == FROZEN);
	CHECK(!BodyEffects::Needed(ASH, ASH).Any());
	// At the stage, with no pile (the engine set the stage and nothing attached one): the pile alone.
	CHECK(BodyEffects::Needed(ASH, State{ BodyEffects::DISINTEGRATE_END, 0 }) == (State{ 0, BodyEffects::ASH_PILE }));
	// Ours shows another effect (our own kill's): left as it is, with no second pile.
	CHECK(!BodyEffects::Needed(ASH, GOO).Any());
	CHECK(!BodyEffects::Needed(ASH, FROZEN).Any());
	CHECK(!BodyEffects::Needed(GOO, State{ BodyEffects::DISINTEGRATE_END, 0 }).Any());
	// Ours has another pile already: kept.
	CHECK(!BodyEffects::Needed(ASH, State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }).Any());
	// A pile told with no stage is attached to a whole body.
	CHECK(BodyEffects::Needed(State{ 0, BLUE_PILE }, WHOLE) == (State{ 0, BLUE_PILE }));
	// A stage that never ends an effect is not made of a body.
	CHECK(!BodyEffects::Needed(State{ BodyEffects::DISINTEGRATE_START, 0 }, WHOLE).Any());
}

TEST("body effects: a game tells what its body shows once, after the effect has ended")
{
	Book book;
	// The laser's kill: nothing to tell while the body is whole or the effect plays.
	CHECK(!book.Tell(RAIDER, WHOLE, T0));
	CHECK(!book.Tell(RAIDER, BURNING, T0 + 500ms));
	CHECK(book.Size() == 0);
	const auto told = book.Tell(RAIDER, ASH, T0 + 2s);
	REQUIRE(told.has_value());
	CHECK(*told == ASH);
	CHECK(book.Known(RAIDER) == ASH);
	// Said once, however often the body is looked at.
	CHECK(!book.Tell(RAIDER, ASH, T0 + 3s));
	CHECK(!book.Tell(RAIDER, ASH, T0 + 60s));
	// A spawn's body under its own id.
	CHECK(book.Tell(SPAWN, GOO, T0 + 3s).has_value());
	CHECK(!book.Tell(0, ASH, T0));

	// A friend joined, who heard none of it: told again, once.
	book.ForgetTold();
	CHECK(book.Tell(RAIDER, ASH, T0 + 70s).has_value());
	CHECK(!book.Tell(RAIDER, ASH, T0 + 71s));
}

TEST("body effects: the stage told first and the pile read later are told again, whole")
{
	Book book;
	// The pile couldn't be read on the first look.
	const auto first = book.Tell(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0);
	REQUIRE(first.has_value());
	const auto second = book.Tell(RAIDER, State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }, T0 + 500ms);
	REQUIRE(second.has_value());
	CHECK(*second == (State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }));
	CHECK(!book.Tell(RAIDER, State{ BodyEffects::DISINTEGRATE_END, BLUE_PILE }, T0 + 1s));
}

TEST("body effects: what a friend's game told is not told back")
{
	Book book;
	CHECK(book.Heard(RAIDER, ASH, T0));
	// Our body shows it now (we made it so): the others know.
	CHECK(!book.Tell(RAIDER, ASH, T0 + 1s));
	// Heard again (the teller said it again for a joiner): nothing new is wanted.
	CHECK(!book.Heard(RAIDER, ASH, T0 + 2s));
	// A mod's pile this game lacks came as none: ours got the game's own, and that is no news to anyone.
	CHECK(book.Heard(SPAWN, State{ BodyEffects::GOO_END, 0 }, T0));
	CHECK(book.Wanted(SPAWN) == GOO);
	CHECK(!book.Tell(SPAWN, GOO, T0 + 1s));
	// Nothing, or an effect still playing, is never taken.
	CHECK(!book.Heard(RAIDER + 1, WHOLE, T0));
	CHECK(!book.Heard(RAIDER + 1, BURNING, T0));
	CHECK(!book.Heard(0, ASH, T0));
	CHECK(!book.Wanted(RAIDER + 1).Any());
}

TEST("body effects: a whole body here is made what the friend's game told, and said done once it shows it")
{
	Book book;
	// Nothing heard: nothing to do.
	CHECK(book.Apply(RAIDER, WHOLE, T0).verdict == Verdict::kNone);
	REQUIRE(book.Heard(RAIDER, ASH, T0));
	const auto step = book.Apply(RAIDER, WHOLE, T0 + 100ms);
	CHECK(step.verdict == Verdict::kApply);
	CHECK(step.what == ASH);
	CHECK(step.attempt == 1);
	// It shows it on the next look: done, said once.
	const auto done = book.Apply(RAIDER, ASH, T0 + 600ms);
	CHECK(done.verdict == Verdict::kDone);
	CHECK(done.attempt == 1);
	CHECK(book.Apply(RAIDER, ASH, T0 + 1100ms).verdict == Verdict::kNone);
	// And it is no news to the others.
	CHECK(!book.Tell(RAIDER, ASH, T0 + 1100ms));
}

TEST("body effects: a body that already shows what was told is left alone")
{
	Book book;
	// Both games ran the effect (each player's shot was a critical kill).
	REQUIRE(book.Tell(RAIDER, ASH, T0).has_value());
	CHECK(book.Heard(RAIDER, ASH, T0 + 100ms));  // wanted of ours, which has it
	CHECK(book.Apply(RAIDER, ASH, T0 + 200ms).verdict == Verdict::kNone);
	// Each made something else of it: ours stays as it is.
	Book other;
	REQUIRE(other.Tell(RAIDER, GOO, T0).has_value());
	other.Heard(RAIDER, ASH, T0 + 100ms);
	CHECK(other.Apply(RAIDER, GOO, T0 + 200ms).verdict == Verdict::kNone);
	CHECK(other.Known(RAIDER) == GOO);
}

TEST("body effects: our body's own effect is waited out, not cut short")
{
	Book book;
	REQUIRE(book.Heard(RAIDER, ASH, T0));
	// Our player's shot was a critical kill too, a moment later: the script ends it.
	CHECK(book.Apply(RAIDER, BURNING, T0 + 100ms).verdict == Verdict::kWait);
	CHECK(book.Apply(RAIDER, BURNING, T0 + 600ms).verdict == Verdict::kWait);
	CHECK(book.Apply(RAIDER, ASH, T0 + 2s).verdict == Verdict::kNone);
	// Another effect of ours plays: waited out as well, and what it leaves is kept.
	REQUIRE(book.Heard(SPAWN, ASH, T0));
	CHECK(book.Apply(SPAWN, State{ BodyEffects::GOO_START, 0 }, T0 + 100ms).verdict == Verdict::kWait);
	CHECK(book.Apply(SPAWN, GOO, T0 + 2s).verdict == Verdict::kNone);
}

TEST("body effects: a try that left the body mid-way is finished; three that fail are given up, said once")
{
	Book book;
	REQUIRE(book.Heard(RAIDER, ASH, T0));
	CHECK(book.Apply(RAIDER, WHOLE, T0).verdict == Verdict::kApply);
	// Our try set the start stage and the end stage didn't take: not our game's own effect, so not waited out for ever.
	CHECK(book.Apply(RAIDER, BURNING, T0 + 500ms).verdict == Verdict::kWait);  // too soon after the try
	const auto again = book.Apply(RAIDER, BURNING, T0 + 2s);
	CHECK(again.verdict == Verdict::kApply);
	CHECK(again.what == ASH);
	CHECK(again.attempt == 2);
	// The stage took, the pile didn't: the pile alone.
	const auto pile = book.Apply(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0 + 4s);
	CHECK(pile.verdict == Verdict::kApply);
	CHECK(pile.what == (State{ 0, BodyEffects::ASH_PILE }));
	CHECK(pile.attempt == 3);
	// Still none: given up, said once, and never tried again.
	const auto gaveUp = book.Apply(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0 + 6s);
	CHECK(gaveUp.verdict == Verdict::kGiveUp);
	CHECK(gaveUp.what == (State{ 0, BodyEffects::ASH_PILE }));
	CHECK(gaveUp.attempt == 3);
	CHECK(book.Apply(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0 + 8s).verdict == Verdict::kNone);
	CHECK(book.Apply(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0 + 60s).verdict == Verdict::kNone);
	// Something new heard of it (nothing here: the same again) changes nothing.
	CHECK(!book.Heard(RAIDER, ASH, T0 + 61s));
	CHECK(book.Apply(RAIDER, State{ BodyEffects::DISINTEGRATE_END, 0 }, T0 + 62s).verdict == Verdict::kNone);
}

TEST("body effects: a frozen body is made frozen, with no pile")
{
	Book book;
	REQUIRE(book.Heard(SPAWN, FROZEN, T0));
	const auto step = book.Apply(SPAWN, WHOLE, T0);
	CHECK(step.verdict == Verdict::kApply);
	CHECK(step.what == FROZEN);
	CHECK(book.Apply(SPAWN, FROZEN, T0 + 500ms).verdict == Verdict::kDone);
}

TEST("body effects: what is wanted of a body not here yet waits, and is counted")
{
	Book book;
	CHECK(book.Waiting() == 0);
	REQUIRE(book.Heard(RAIDER, ASH, T0));
	REQUIRE(book.Heard(SPAWN, GOO, T0));
	CHECK(book.Waiting() == 2);
	// Ours is seen showing it (both games' effects ran): waits no more.
	book.Seen(RAIDER, ASH);
	CHECK(book.Waiting() == 1);
	// The other loads and falls dead here a minute later: made then.
	CHECK(book.Apply(SPAWN, WHOLE, T0 + 60s).verdict == Verdict::kApply);
	book.Seen(SPAWN, GOO);
	CHECK(book.Apply(SPAWN, GOO, T0 + 61s).verdict == Verdict::kDone);
	CHECK(book.Waiting() == 0);
}

TEST("body effects: after a load all that was heard is wanted of our bodies again, and what we see is told again")
{
	Book book;
	REQUIRE(book.Heard(RAIDER, ASH, T0));
	CHECK(book.Apply(RAIDER, WHOLE, T0).verdict == Verdict::kApply);
	CHECK(book.Apply(RAIDER, ASH, T0 + 1s).verdict == Verdict::kDone);
	REQUIRE(book.Tell(SPAWN, GOO, T0).has_value());
	// One we gave up on.
	REQUIRE(book.Heard(RAIDER + 1, ASH, T0));
	for (int i = 0; i < 4; ++i) {
		book.Apply(RAIDER + 1, WHOLE, T0 + std::chrono::seconds(10 * i));
	}
	CHECK(book.Apply(RAIDER + 1, WHOLE, T0 + 60s).verdict == Verdict::kNone);

	// The save is from before the kills: the raiders die again (the session's deaths), whole.
	auto ids = book.WantedAgain();
	std::sort(ids.begin(), ids.end());
	REQUIRE(ids.size() == 2);
	CHECK(ids[0] == RAIDER);
	CHECK(ids[1] == RAIDER + 1);
	CHECK(book.Apply(RAIDER, WHOLE, T0 + 120s).verdict == Verdict::kApply);
	CHECK(book.Apply(RAIDER + 1, WHOLE, T0 + 120s).verdict == Verdict::kApply);
	// What a friend told is not told back; our own is told again (they keep theirs: the first stands).
	CHECK(!book.Tell(RAIDER, ASH, T0 + 121s));
	CHECK(book.Tell(SPAWN, GOO, T0 + 121s).has_value());
}

TEST("body effects: the book holds no more than MAX bodies, the one least lately heard of forgotten first")
{
	Book book;
	for (std::uint32_t i = 0; i < BodyEffects::MAX; ++i) {
		REQUIRE(book.Heard(0x00100000 + i, ASH, T0 + std::chrono::seconds(i)));
	}
	CHECK(book.Size() == BodyEffects::MAX);
	// The first is heard of again: the second is now the oldest.
	book.Heard(0x00100000, ASH, T0 + 1000s);
	REQUIRE(book.Heard(SPAWN, GOO, T0 + 1001s));
	CHECK(book.Size() == BodyEffects::MAX);
	CHECK(book.Wanted(0x00100000) == ASH);
	CHECK(!book.Wanted(0x00100001).Any());
	CHECK(book.Wanted(SPAWN) == GOO);

	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(!book.Wanted(SPAWN).Any());
}
