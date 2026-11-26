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
