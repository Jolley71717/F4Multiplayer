// A Port-A-Diner's pie (src/game/DinerShare.h): a friend's press replayed here cannot win, the presser's win is made
// or set here, and nothing of ours stays on the script. The script is modelled as PortaDinerScript runs it: a press
// in its state Waiting goes Busy, reads the count of presses, counts the press, rolls, and plays the win or the miss
// before it is Waiting again.

#include "Test.h"

#include "game/DinerShare.h"

#include <optional>

namespace
{
	using namespace std::chrono_literals;
	using DinerShare::Apply;
	using DinerShare::Clock;
	using DinerShare::Press;
	using DinerShare::Script;

	// PortaDinerScript, as far as a press goes. The roll is handed in: 1..RandomMax, as Utility.RandomInt gives it.
	struct Diner
	{
		std::int32_t attempts = 0;
		bool         won = false;    // PieSuccess
		bool         force = false;  // ForcePieSuccess
		bool         busy = false;
		int          pies = 0;       // put in its box
		int          misses = 0;     // "Fail01" played
		int          wins = 0;       // "Succeed" played

		// OnActivate, up to the animation: the script stays Busy until Done.
		void Activate(float a_luck, std::int32_t a_roll, float a_successMod = 0.0f)
		{
			if (busy) {
				return;
			}
			busy = true;
			const float chance = (1.0f + a_successMod) * a_luck + static_cast<float>(attempts / 2);
			if (!won) {
				++attempts;
				if (static_cast<float>(a_roll) <= chance || force) {
					++wins;
					++pies;
					won = true;
				} else {
					++misses;
				}
			} else {
				++wins;
			}
		}

		void Done() { busy = false; }

		[[nodiscard]] Script Read() const { return Script{ .won = won, .busy = busy }; }
	};
}

TEST("DinerShare: a levered count of presses says so of itself, and goes back to the count with the press on it")
{
	for (const std::int32_t count : { 0, 1, 7, 2000, DinerShare::MAX_TRIES }) {
		const auto lever = DinerShare::Lever(count);
		REQUIRE(lever.has_value());
		CHECK(DinerShare::IsLevered(*lever));
		CHECK(DinerShare::Restored(*lever) == count);          // the script never took the press
		CHECK(DinerShare::IsLevered(*lever + 1));              // ... or counted it
		CHECK(DinerShare::Restored(*lever + 1) == count + 1);
		CHECK(!DinerShare::WasRead(*lever, *lever));
		CHECK(DinerShare::WasRead(*lever, *lever + 1));
		CHECK(DinerShare::WasRead(*lever, count));             // something put it back for us: no longer ours to restore
	}
	// A count as the game has it is never taken for a lever, nor changed.
	for (const std::int32_t count : { 0, 1, 500, DinerShare::MAX_TRIES, -1, -1000 }) {
		CHECK(!DinerShare::IsLevered(count));
		CHECK(DinerShare::Restored(count) == count);
	}
	// What isn't a count of presses isn't levered: the press is not made here.
	CHECK(!DinerShare::Lever(-1).has_value());
	CHECK(!DinerShare::Lever(DinerShare::MAX_TRIES + 1).has_value());
	CHECK(!DinerShare::Lever(*DinerShare::Lever(3)).has_value());  // nor a lever a second time
}

TEST("DinerShare: a levered press loses whatever the Luck and the roll")
{
	for (const std::int32_t count : { 0, 9, 4000, DinerShare::MAX_TRIES }) {
		for (const float luck : { 1.0f, 10.0f, 35.0f, 1000.0f }) {
			Diner diner;
			diner.attempts = *DinerShare::Lever(count);
			diner.Activate(luck, 1, 10.0f);  // the best roll there is, a success modifier far past the game's
			CHECK(!diner.won);
			CHECK(diner.pies == 0);
			CHECK(diner.misses == 1);
			CHECK(DinerShare::Restored(diner.attempts) == count + 1);  // the press is counted, as in the presser's game
		}
	}
	// The same press without the lever would have won: this is what was wrong.
	Diner plain;
	plain.Activate(10.0f, 1);
	CHECK(plain.won);
}

TEST("DinerShare: a friend's press is made to lose where the pie isn't won, replayed where it is, and not made on a busy or stopped game")
{
	CHECK(DinerShare::OnPress(Script{ .won = false, .busy = false }, false) == Press::kLose);
	CHECK(DinerShare::OnPress(Script{ .won = true, .busy = false }, false) == Press::kReplay);
	CHECK(DinerShare::OnPress(Script{ .won = false, .busy = true }, false) == Press::kNothing);
	CHECK(DinerShare::OnPress(Script{ .won = true, .busy = true }, false) == Press::kNothing);
	CHECK(DinerShare::OnPress(Script{ .won = false, .busy = false }, true) == Press::kNothing);  // a lever on, a win being made, a menu
	CHECK(DinerShare::OnPress(Script{ .won = true, .busy = false }, true) == Press::kNothing);
}

TEST("DinerShare: a friend's win is made through the script soon after their press, set otherwise, and nothing is un-won")
{
	const Script waiting{ .won = false, .busy = false };
	const Script busy{ .won = false, .busy = true };
	const Script won{ .won = true, .busy = false };

	// Their diner isn't won: nothing, whatever ours is.
	CHECK(DinerShare::Decide(false, waiting, false, false, 1s) == Apply::kNothing);
	CHECK(DinerShare::Decide(false, won, false, false, 1s) == Apply::kNothing);
	// Won here already, or being won.
	CHECK(DinerShare::Decide(true, won, false, false, 1s) == Apply::kNothing);
	CHECK(DinerShare::Decide(true, waiting, false, true, 1s) == Apply::kNothing);
	// Their press is still playing here, or its lever is still on: asked again.
	CHECK(DinerShare::Decide(true, busy, false, false, 1s) == Apply::kWait);
	CHECK(DinerShare::Decide(true, waiting, true, false, 1s) == Apply::kWait);
	CHECK(DinerShare::Decide(true, busy, true, false, std::nullopt) == Apply::kWait);
	// Soon after their press: with the pie. Later, or with no press of theirs heard (a join, a place loaded later, a
	// stopped game): set.
	CHECK(DinerShare::Decide(true, waiting, false, false, 0s) == Apply::kWin);
	CHECK(DinerShare::Decide(true, waiting, false, false, DinerShare::LIVE) == Apply::kWin);
	CHECK(DinerShare::Decide(true, waiting, false, false, DinerShare::LIVE + 1ms) == Apply::kSet);
	CHECK(DinerShare::Decide(true, waiting, false, false, std::nullopt) == Apply::kSet);
}

TEST("DinerShare: the time a lever may stay on counts the game running, not a menu or a hitch")
{
	CHECK(DinerShare::Ran(16ms, false) == 16ms);
	CHECK(DinerShare::Ran(16ms, true) == 0ms);
	CHECK(DinerShare::Ran(5min, true) == 0ms);
	CHECK(DinerShare::Ran(5min, false) == DinerShare::MAX_TICK);
	CHECK(DinerShare::Ran(DinerShare::MAX_TICK, false) == DinerShare::MAX_TICK);
	CHECK(DinerShare::Ran(-1s, false) == 0ms);
	CHECK(DinerShare::Ran(0ms, false) == 0ms);

	// A loading hitch of a minute, then frames: the lever is not timed out by the hitch.
	Clock::duration ran{};
	ran += DinerShare::Ran(60s, false);
	CHECK(ran <= DinerShare::LEVER_TIME);
	int frames = 0;
	while (ran <= DinerShare::LEVER_TIME) {
		ran += DinerShare::Ran(16ms, false);
		++frames;
	}
	CHECK(frames > 500);  // some nine seconds of frames
}

TEST("DinerShare: two games, one roll: the presser's miss and the presser's win are both games'")
{
	// The presser's game (Luck 3) and a friend's (Luck 10, which would win any roll up to 10).
	Diner theirs, ours;
	theirs.attempts = ours.attempts = 4;

	// A miss in their game.
	theirs.Activate(3.0f, 9);
	REQUIRE(DinerShare::OnPress(ours.Read(), false) == Press::kLose);
	const auto lever = DinerShare::Lever(ours.attempts);
	REQUIRE(lever.has_value());
	ours.attempts = *lever;
	ours.Activate(10.0f, 9);
	CHECK(DinerShare::WasRead(*lever, ours.attempts));
	ours.attempts = DinerShare::Restored(ours.attempts);
	CHECK(!theirs.won);
	CHECK(!ours.won);
	CHECK(ours.attempts == theirs.attempts);
	CHECK(ours.misses == 1);
	// A diner not won says nothing; a "not won" heard anyway changes nothing here.
	CHECK(DinerShare::Decide(false, ours.Read(), false, false, 1s) == Apply::kNothing);
	theirs.Done();
	ours.Done();

	// A win in their game: the miss plays here first (the press came before the word of the win), then the win.
	theirs.Activate(3.0f, 1);
	REQUIRE(theirs.won);
	const auto second = DinerShare::Lever(ours.attempts);
	REQUIRE(second.has_value());
	ours.attempts = *second;
	ours.Activate(10.0f, 1);
	ours.attempts = DinerShare::Restored(ours.attempts);
	CHECK(!ours.won);
	CHECK(DinerShare::Decide(true, ours.Read(), false, false, 4s) == Apply::kWait);  // the miss is still playing
	ours.Done();
	REQUIRE(DinerShare::Decide(true, ours.Read(), false, false, 6s) == Apply::kWin);
	const bool forceWas = ours.force;
	ours.force = true;
	ours.Activate(10.0f, 100);  // whatever the roll
	ours.force = forceWas;
	CHECK(ours.won);
	CHECK(ours.pies == 1);
	CHECK(theirs.pies == 1);
	CHECK(!ours.force);
	ours.Done();
	theirs.Done();

	// Heard again (the session's world state, a place loaded again): nothing more, no second pie.
	CHECK(DinerShare::Decide(true, ours.Read(), false, false, std::nullopt) == Apply::kNothing);
	// A press at the won diner is replayed as it is: the script's win again, no dice, no pie.
	CHECK(DinerShare::OnPress(ours.Read(), false) == Press::kReplay);
	ours.Activate(10.0f, 100);
	CHECK(ours.pies == 1);
	CHECK(ours.wins == 2);

	// A joiner, or a game that was elsewhere: set won, no pie of its own.
	Diner late;
	REQUIRE(DinerShare::Decide(true, late.Read(), false, false, std::nullopt) == Apply::kSet);
	late.won = true;
	CHECK(late.pies == 0);
	CHECK(DinerShare::OnPress(late.Read(), false) == Press::kReplay);
}
