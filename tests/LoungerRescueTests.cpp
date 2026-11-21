// A player left without controls by their own press on Kellogg's memory lounger that never seated them
// (LoungerRescue::Guard, as MemoryLounger::Frame runs it twice a second).

#include "Test.h"

#include "game/LoungerRescue.h"

#include <vector>

using namespace std::chrono_literals;

namespace
{
	using LoungerRescue::Facts;
	using LoungerRescue::Step;

	// MQ202 160 ran (its layer held), 170 never came, in no furniture: the friend's press on a taken lounger.
	Facts Stuck()
	{
		return { .inWorld = true, .sitDone = true, .layerHeld = true };
	}

	struct Run
	{
		LoungerRescue::Guard     guard;
		LoungerRescue::Clock::time_point t0 = LoungerRescue::Clock::now();
		std::vector<std::pair<std::chrono::milliseconds, Step>> steps;  // what was asked, and when

		// Looks every 500 ms from a_from to a_until with the same facts.
		void Look(const Facts& a_facts, std::chrono::milliseconds a_from, std::chrono::milliseconds a_until)
		{
			for (auto t = a_from; t <= a_until; t += 500ms) {
				if (const auto step = guard.Look(a_facts, t0 + t); step != Step::kNothing) {
					steps.emplace_back(t, step);
				}
			}
		}
	};
}

TEST("memory lounger: a player stuck after the press has the seat asked again after the grace time, then the press undone")
{
	Run run;
	run.Look(Stuck(), 0ms, 60000ms);
	REQUIRE(run.steps.size() == static_cast<std::size_t>(LoungerRescue::SEATS + 1));
	CHECK(run.steps[0].first == 5000ms);  // GRACE after it was first seen
	CHECK(run.steps[0].second == Step::kSeat);
	CHECK(run.steps[1].first == 9000ms);  // RETRY apart
	CHECK(run.steps[1].second == Step::kSeat);
	CHECK(run.steps[2].first == 13000ms);
	CHECK(run.steps[2].second == Step::kSeat);
	CHECK(run.steps[3].first == 17000ms);  // the give-back: once
	CHECK(run.steps[3].second == Step::kGiveBack);
	CHECK(run.guard.GaveBack());
}

TEST("memory lounger: the press that seats the player as the game means (the lounger free) is never touched")
{
	Run run;
	// 160 then the seat under way at once, then 170 by OnSit: no step.
	run.Look({ .inWorld = true, .sitDone = true, .layerHeld = true, .inFurniture = true }, 0ms, 3000ms);
	run.Look({ .inWorld = true, .sitDone = true, .seatedDone = true, .layerHeld = true, .inFurniture = true }, 3500ms, 30000ms);
	// Into the memory and out of the furniture, 170 done: still nothing.
	run.Look({ .inWorld = true, .sitDone = true, .seatedDone = true, .layerHeld = true }, 30500ms, 90000ms);
	CHECK(run.steps.empty());
}

TEST("memory lounger: 160 marked done without its script (a friend's memory closed here) has no layer and is left alone")
{
	Run run;
	run.Look({ .inWorld = true, .sitDone = true }, 0ms, 120000ms);
	CHECK(run.steps.empty());
	CHECK(!LoungerRescue::Stuck({ .inWorld = true, .sitDone = true }));
	CHECK(LoungerRescue::Stuck(Stuck()));
}

TEST("memory lounger: the seat asked again that takes ends it; one that lets the player out again counts its asks")
{
	Run run;
	run.Look(Stuck(), 0ms, 5000ms);
	REQUIRE(run.steps.size() == 1u);  // the first seat
	// It takes: in the lounger, then 170.
	run.Look({ .inWorld = true, .sitDone = true, .layerHeld = true, .inFurniture = true }, 5500ms, 7000ms);
	run.Look({ .inWorld = true, .sitDone = true, .seatedDone = true, .layerHeld = true, .inFurniture = true }, 7500ms, 60000ms);
	CHECK(run.steps.size() == 1u);
	CHECK(run.guard.Seats() == 0);

	// Another game: each seat takes for a moment and lets the player out again; the asks still run out, and the press is undone.
	Run bounce;
	auto t = 0ms;
	for (int i = 0; i < 6; ++i) {
		bounce.Look(Stuck(), t, t + 6000ms);
		t += 6500ms;
		bounce.Look({ .inWorld = true, .sitDone = true, .layerHeld = true, .inFurniture = true }, t, t + 1000ms);
		t += 1500ms;
	}
	int seats = 0, givebacks = 0;
	for (const auto& [when, step] : bounce.steps) {
		seats += step == Step::kSeat;
		givebacks += step == Step::kGiveBack;
	}
	CHECK(seats == LoungerRescue::SEATS);
	CHECK(givebacks == 1);
}

TEST("memory lounger: a load or a pause neither ends the wait nor counts; a game's start looks afresh; a save made stuck is got out of at its load")
{
	Run run;
	run.Look(Stuck(), 0ms, 3000ms);
	run.Look({ .inWorld = false, .sitDone = true, .layerHeld = true }, 3500ms, 20000ms);  // paused: nothing asked
	CHECK(run.steps.empty());
	run.Look(Stuck(), 20500ms, 20500ms);  // back: the wait went on from 0 ms
	REQUIRE(run.steps.size() == 1u);
	CHECK(run.steps[0].second == Step::kSeat);

	// The stuck save loaded: armed afresh, the grace time from the first look in the world.
	run.guard.Arm();
	run.steps.clear();
	run.Look({ .inWorld = false, .sitDone = true, .layerHeld = true }, 21000ms, 30000ms);  // the loading screen
	run.Look(Stuck(), 30500ms, 36000ms);
	REQUIRE(run.steps.size() == 1u);
	CHECK(run.steps[0].first == 35500ms);
	CHECK(run.steps[0].second == Step::kSeat);
}

TEST("memory lounger: once undone (160 no longer done) it is over, and a later press that strands the player again is looked at anew")
{
	Run run;
	run.Look(Stuck(), 0ms, 20000ms);
	CHECK(run.guard.GaveBack());
	run.Look({ .inWorld = true, .layerHeld = true }, 20500ms, 30000ms);  // 160 cleared, the deleted layer still in the property
	CHECK(!run.guard.GaveBack());
	CHECK(run.guard.Seats() == 0);
	run.steps.clear();
	run.Look(Stuck(), 30500ms, 36000ms);
	REQUIRE(run.steps.size() == 1u);
	CHECK(run.steps[0].second == Step::kSeat);
}

TEST("memory lounger: the quest's current stage after the undo is the highest done below the sit stage")
{
	const std::vector<std::uint16_t> done{ 10, 20, 100, 150, 155, 160 };
	CHECK(LoungerRescue::StageBefore(160, 160, done) == 155);
	CHECK(LoungerRescue::StageBefore(155, 160, done) == 155);  // already below: left
	const std::vector<std::uint16_t> none{};
	CHECK(LoungerRescue::StageBefore(160, 160, none) == 0);
	CHECK(LoungerRescue::DOORS[0].quest == 0x000229E9u);
	CHECK(LoungerRescue::DOORS[0].sitStage == 160);
	CHECK(LoungerRescue::DOORS[0].seatedStage == 170);
	CHECK(LoungerRescue::DOORS[0].layer == "MQ202EnableLayer");
	CHECK(LoungerRescue::DOORS[0].lounger == "PlayerMemoryLounger");
}
