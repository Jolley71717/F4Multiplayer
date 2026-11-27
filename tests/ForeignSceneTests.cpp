#include "Test.h"

#include "game/ForeignScene.h"

#include <chrono>

namespace
{
	using namespace std::chrono_literals;
	using ForeignScene::Verdict;

	constexpr std::uint32_t SEARCH = 0x0019E137;  // a quest's scene of an NPC and the player
	constexpr std::uint32_t OTHER = 0x000835A6;

	const ForeignScene::Clock::time_point T0 = ForeignScene::Clock::time_point{} + 1h;

	// A shared quest's scene, our player in it, played by an NPC another game runs.
	ForeignScene::Facts Theirs(std::uint32_t a_scene = SEARCH)
	{
		return { .scene = a_scene, .shared = true, .foreignNpcs = true };
	}
}

TEST("foreign scene: a scene of an NPC another game runs is stopped once the facts held for the grace")
{
	ForeignScene::Watch watch;
	CHECK(watch.Update(Theirs(), T0) == Verdict::kWait);
	CHECK(watch.Foreign() == SEARCH);  // its NPCs are not claimed meanwhile
	CHECK(watch.Update(Theirs(), T0 + 1s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(), T0 + ForeignScene::GRACE) == Verdict::kStop);
	CHECK(watch.Stops() == 1);
}

TEST("foreign scene: a conversation of ours, an NPC of ours, an unshared quest or no scene keep it")
{
	ForeignScene::Watch watch;
	auto                talking = Theirs();
	talking.talking = true;
	auto ours = Theirs();
	ours.ourNpcs = true;  // one of the scene's NPCs is ours to run: our game plays it
	auto unshared = Theirs();
	unshared.shared = false;
	auto unowned = Theirs();
	unowned.foreignNpcs = false;  // nobody's NPC (or none known): left alone
	const ForeignScene::Facts none{};
	for (const auto& facts : { talking, ours, unshared, unowned, none }) {
		CHECK(watch.Update(facts, T0) == Verdict::kKeep);
		CHECK(watch.Update(facts, T0 + 10s) == Verdict::kKeep);
		CHECK(watch.Foreign() == 0);
	}
	CHECK(watch.Stops() == 0);
}

TEST("foreign scene: a conversation opened during the grace keeps the scene, and the wait starts over after it")
{
	ForeignScene::Watch watch;
	CHECK(watch.Update(Theirs(), T0) == Verdict::kWait);
	auto talking = Theirs();
	talking.talking = true;
	CHECK(watch.Update(talking, T0 + 1s) == Verdict::kKeep);
	// the talk ends: the grace is counted from here
	CHECK(watch.Update(Theirs(), T0 + 2s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(), T0 + 3s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(), T0 + 2s + ForeignScene::GRACE) == Verdict::kStop);
}

TEST("foreign scene: a stop that did not take is asked again after the retry, a few times, then given up")
{
	ForeignScene::Watch watch;
	auto                at = T0;
	CHECK(watch.Update(Theirs(), at) == Verdict::kWait);
	at += ForeignScene::GRACE;
	CHECK(watch.Update(Theirs(), at) == Verdict::kStop);
	CHECK(watch.Update(Theirs(), at + 1s) == Verdict::kWait);
	at += ForeignScene::RETRY;
	CHECK(watch.Update(Theirs(), at) == Verdict::kStop);
	at += ForeignScene::RETRY;
	CHECK(watch.Update(Theirs(), at) == Verdict::kStop);
	CHECK(watch.GaveUp());
	at += ForeignScene::RETRY;
	CHECK(watch.Update(Theirs(), at) == Verdict::kWait);
	CHECK(watch.Stops() == 3);
	// scene over: the next one starts afresh
	CHECK(watch.Update({}, at + 1s) == Verdict::kKeep);
	CHECK(!watch.GaveUp());
	CHECK(watch.Update(Theirs(OTHER), at + 2s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(OTHER), at + 2s + ForeignScene::GRACE) == Verdict::kStop);
}

TEST("foreign scene: another scene taking the place of the watched one starts the wait over")
{
	ForeignScene::Watch watch;
	CHECK(watch.Update(Theirs(SEARCH), T0) == Verdict::kWait);
	CHECK(watch.Update(Theirs(OTHER), T0 + 1s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(OTHER), T0 + 2s) == Verdict::kWait);
	CHECK(watch.Update(Theirs(OTHER), T0 + 1s + ForeignScene::GRACE) == Verdict::kStop);
}

TEST("foreign scene guard: a stage of the quest set here after a stop is held back for the window, then reported")
{
	ForeignScene::Guard guard;
	CHECK(!guard.Holds(0x0001E64E, T0));  // nothing stopped
	guard.Stopped(0x0001E64E, T0);
	CHECK(guard.Holds(0x0001E64E, T0));
	CHECK(guard.Holds(0x0001E64E, T0 + 9s));
	CHECK(!guard.Holds(0x0001E64E, T0 + ForeignScene::HOLD_BACK));
	CHECK(!guard.Holds(0x0001E64E, T0 + 30s));
}

TEST("foreign scene guard: only the stopped scene's quest is held, and a second stop renews the window")
{
	ForeignScene::Guard guard;
	guard.Stopped(0x0001E64E, T0);
	CHECK(!guard.Holds(0x000229E6, T0 + 1s));
	guard.Stopped(0x0001E64E, T0 + 8s);
	CHECK(guard.Holds(0x0001E64E, T0 + 15s));
	CHECK(!guard.Holds(0x0001E64E, T0 + 19s));
}

TEST("foreign scene guard: the other game's stage of the quest after the stop ends the hold, one before it does not")
{
	ForeignScene::Guard guard;
	guard.Stopped(0x0001E64E, T0 + 5s);
	CHECK(guard.Holds(0x0001E64E, T0 + 6s, T0 + 2s));  // theirs came before the stop
	CHECK(guard.Holds(0x0001E64E, T0 + 6s, std::nullopt));
	CHECK(!guard.Holds(0x0001E64E, T0 + 7s, T0 + 6s));  // theirs came after: what is set here may be theirs
	guard.Clear();
	CHECK(!guard.Holds(0x0001E64E, T0 + 6s));
}

TEST("foreign scene: a menu the scene opened, its NPCs all another game's and no activation of ours, is stopped")
{
	ForeignScene::Talk talk;
	auto               facts = Theirs();
	facts.menu = true;
	facts.talking = talk.Update(true, false);  // no activation of its speaker by our player
	CHECK(!facts.talking);
	CHECK(ForeignScene::IsForeign(facts));
	ForeignScene::Watch watch;
	CHECK(watch.Update(facts, T0) == Verdict::kWait);
	CHECK(watch.Foreign() == SEARCH);  // its NPCs are not claimed meanwhile
	CHECK(watch.Update(facts, T0 + ForeignScene::GRACE) == Verdict::kStop);
}

TEST("foreign scene: a menu our own E opened is left alone, for as long as it is open")
{
	ForeignScene::Talk talk;
	auto               facts = Theirs();
	facts.menu = true;
	facts.talking = talk.Update(true, true);
	CHECK(facts.talking);
	CHECK(!ForeignScene::IsForeign(facts));
	// the activation is old by now, the same menu is still ours
	CHECK(talk.Update(true, false));
	ForeignScene::Watch watch;
	CHECK(watch.Update(facts, T0 + 30s) == Verdict::kKeep);
	// closed: the next menu is judged afresh
	CHECK(!talk.Update(false, false));
	CHECK(!talk.Update(true, false));
}

TEST("foreign scene: an NPC of ours in the scene keeps it and its menu, whoever opened the menu")
{
	auto facts = Theirs();
	facts.menu = true;
	facts.ourNpcs = true;
	CHECK(!ForeignScene::IsForeign(facts));
	ForeignScene::Watch watch;
	CHECK(watch.Update(facts, T0 + 30s) == Verdict::kKeep);
}

TEST("foreign scene: a menu of an unshared quest's scene, or with no scene, is left alone")
{
	auto unshared = Theirs();
	unshared.menu = true;
	unshared.shared = false;
	CHECK(!ForeignScene::IsForeign(unshared));
	ForeignScene::Facts none;
	none.menu = true;
	CHECK(!ForeignScene::IsForeign(none));
}

TEST("foreign scene: a menu the scene opens again after being stopped is stopped again, not taken for ours")
{
	ForeignScene::Talk  talk;
	ForeignScene::Watch watch;
	auto                facts = Theirs();
	facts.menu = true;
	facts.talking = talk.Update(true, false);
	CHECK(watch.Update(facts, T0) == Verdict::kWait);
	CHECK(watch.Update(facts, T0 + ForeignScene::GRACE) == Verdict::kStop);
	// the stop closed the menu, the scene opened it once more
	facts.talking = talk.Update(false, false);
	facts.talking = talk.Update(true, false);
	CHECK(!facts.talking);
	CHECK(watch.Update(facts, T0 + ForeignScene::GRACE + ForeignScene::RETRY) == Verdict::kStop);
	CHECK(watch.Stops() == 2);
}
