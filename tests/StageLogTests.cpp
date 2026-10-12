#include "Test.h"

#include "game/StageLog.h"

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace
{
	using StageLog::Clock;
	using Stage = StageLog::Queue::Stage;

	constexpr std::uint32_t MQ104 = 0x0001F25E;
	constexpr std::uint32_t MQ105 = 0x000229E6;
	constexpr std::uint32_t SIDE = 0x0001ED86;

	// A game's quests as the queue sees them. A stage set through the console runs at the next Tick, as
	// in the game, where it is set a moment later; a stage's script runs then too (onStage).
	struct FakeGame
	{
		struct Quest
		{
			bool                    running = true;
			bool                    completed = false;
			std::uint16_t           current = 0;
			std::set<std::uint16_t> done;
			std::set<std::uint16_t> missing;  // stages this quest doesn't have: setting them does nothing
		};

		std::map<std::uint32_t, Quest>                    quests;
		std::vector<Stage>                                set;       // what the queue set, in order
		std::vector<Stage>                                commands;  // not run yet
		std::function<void(std::uint32_t, std::uint16_t)> onStage;
		bool                                              busy = false;

		void SetStage(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			auto& quest = quests[a_quest];
			if (!quest.running || quest.missing.contains(a_stage)) {
				return;
			}
			quest.done.insert(a_stage);
			quest.current = (std::max)(quest.current, a_stage);
			if (onStage) {
				onStage(a_quest, a_stage);
			}
		}

		void Tick()
		{
			for (const auto& command : std::exchange(commands, {})) {
				SetStage(command.quest, command.stage);
			}
		}

		std::vector<Stage> Poll(StageLog::Queue& a_queue, Clock::time_point a_now)
		{
			return a_queue.Poll(
				a_now,
				[&](std::uint32_t a_quest) {
					const auto it = quests.find(a_quest);
					if (it == quests.end() || it->second.completed) {
						return StageLog::Quest::kGone;
					}
					return !it->second.running || busy ? StageLog::Quest::kWait : StageLog::Quest::kReady;
				},
				[&](std::uint32_t a_quest, std::uint16_t a_stage) {
					const auto& quest = quests.at(a_quest);
					return quest.done.contains(a_stage) || a_stage <= quest.current;
				},
				[&](std::uint32_t a_quest, std::uint16_t a_stage) {
					set.push_back({ a_quest, a_stage });
					commands.push_back({ a_quest, a_stage });
					return true;
				});
		}

		// Polls and runs the console until nothing is waiting (or a_polls run out).
		void Run(StageLog::Queue& a_queue, Clock::time_point& a_now, int a_polls = 50)
		{
			for (int i = 0; i < a_polls && a_queue.Size() > 0; ++i) {
				Poll(a_queue, a_now);
				Tick();
				a_now += std::chrono::milliseconds(500);
			}
		}
	};
}

TEST("stage log: newly done stages are each reported once, ascending")
{
	StageLog::Stages known;
	CHECK((StageLog::NewlyDone(known, { 10, 5 }) == StageLog::Stages{ 5, 10 }));
	CHECK((known == StageLog::Stages{ 5, 10 }));
	CHECK(StageLog::NewlyDone(known, { 5, 10 }).empty());
	CHECK((StageLog::NewlyDone(known, { 12, 5, 10 }) == StageLog::Stages{ 12 }));
	// A stage's script set an earlier stage (MQ104's 17 sets 12): still new, still reported.
	CHECK((StageLog::NewlyDone(known, { 5, 10, 12, 3 }) == StageLog::Stages{ 3 }));
	// Two stages in one look (a stage whose script moves the quest on at once), and a repeat.
	CHECK((StageLog::NewlyDone(known, { 3, 5, 10, 12, 1000, 210, 210 }) == StageLog::Stages{ 210, 1000 }));
	CHECK((known == StageLog::Stages{ 3, 5, 10, 12, 210, 1000 }));
}

TEST("stage log: a reset quest's stages count again once done again")
{
	StageLog::Stages known{ 10, 20 };
	CHECK(StageLog::NewlyDone(known, {}).empty());
	CHECK(known.empty());
	CHECK((StageLog::NewlyDone(known, { 10 }) == StageLog::Stages{ 10 }));
}

TEST("stage log: a stage set from another player is known, in order, once")
{
	StageLog::Stages known{ 10, 30 };
	StageLog::Insert(known, 20);
	StageLog::Insert(known, 20);
	StageLog::Insert(known, 40);
	StageLog::Insert(known, 5);
	CHECK((known == StageLog::Stages{ 5, 10, 20, 30, 40 }));
	CHECK(StageLog::NewlyDone(known, { 5, 10, 20, 30, 40 }).empty());
}

TEST("stage queue: stages are set lowest first, the next once the last reads done")
{
	FakeGame game;
	game.quests[SIDE].current = 5;
	StageLog::Queue queue;
	// The session's list is in the order the stages were reported, not their order.
	CHECK(queue.Add(SIDE, 30));
	CHECK(queue.Add(SIDE, 10));
	CHECK(queue.Add(SIDE, 20));
	CHECK(!queue.Add(SIDE, 20));
	CHECK(queue.Size() == 3);

	auto now = Clock::time_point{} + std::chrono::hours(1);
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
	CHECK(queue.Setting() == 1);
	// Not done yet (the console runs it a moment later): nothing more is set.
	game.Poll(queue, now + std::chrono::milliseconds(500));
	CHECK(game.set.size() == 1);
	CHECK((queue.Waiting(SIDE) == StageLog::Stages{ 10, 20, 30 }));

	game.Tick();
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 }, { SIDE, 20 }, { SIDE, 30 } }));
	CHECK(queue.Size() == 0);
	CHECK(queue.Setting() == 0);
	CHECK((game.quests[SIDE].done == std::set<std::uint16_t>{ 10, 20, 30 }));
}

TEST("stage queue: a quest another quest's stage starts gets its stages then (MQ104 210 starts MQ105)")
{
	FakeGame game;
	game.quests[MQ104].current = 205;
	game.quests[MQ105].running = false;
	// MQ104's stage 210: starts MQ105 at 10, then MQ104 is done.
	game.onStage = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		if (a_quest == MQ104 && a_stage == 210) {
			game.quests[MQ105].running = true;
			game.SetStage(MQ105, 10);
			game.SetStage(MQ104, 1000);
			game.quests[MQ104].completed = true;
		}
	};
	StageLog::Queue queue;
	// As the other game reported them: MQ104's last stages, and MQ105's that its 210 started.
	for (const auto& [quest, stage] : std::vector<Stage>{ { MQ104, 210 }, { MQ104, 1000 }, { MQ105, 10 }, { MQ105, 20 }, { MQ105, 30 } }) {
		CHECK(queue.Add(quest, stage));
	}

	auto now = Clock::time_point{} + std::chrono::hours(1);
	game.Poll(queue, now);
	// MQ105 isn't running here yet: its stages wait, they aren't dropped.
	CHECK((game.set == std::vector<Stage>{ { MQ104, 210 } }));
	CHECK((queue.Waiting(MQ105) == StageLog::Stages{ 10, 20, 30 }));

	game.Tick();
	game.Run(queue, now);
	// MQ104's 1000 and MQ105's 10 came with 210; the rest of MQ105 follows.
	CHECK((game.set == std::vector<Stage>{ { MQ104, 210 }, { MQ105, 20 }, { MQ105, 30 } }));
	CHECK(queue.Size() == 0);
	CHECK(game.quests[MQ105].current == 30);
}

TEST("stage queue: a stage that never reads done is given up on after the timeout, then the next goes")
{
	FakeGame game;
	game.quests[SIDE].missing = { 50 };
	StageLog::Queue queue;
	queue.Add(SIDE, 50);
	queue.Add(SIDE, 60);

	const auto start = Clock::time_point{} + std::chrono::hours(1);
	CHECK(game.Poll(queue, start).empty());
	game.Tick();
	CHECK(game.Poll(queue, start + std::chrono::seconds(4)).empty());
	CHECK(game.set.size() == 1);
	const auto gaveUp = game.Poll(queue, start + StageLog::Queue::TIMEOUT);
	CHECK((gaveUp == std::vector<Stage>{ { SIDE, 50 } }));
	CHECK((game.set == std::vector<Stage>{ { SIDE, 50 }, { SIDE, 60 } }));
	game.Tick();
	game.Poll(queue, start + StageLog::Queue::TIMEOUT + std::chrono::milliseconds(500));
	CHECK(queue.Size() == 0);
}

TEST("stage queue: stages done here or already passed are dropped, and a completed quest's all")
{
	FakeGame game;
	game.quests[SIDE].current = 40;
	game.quests[SIDE].done = { 10, 40, 45 };
	game.quests[MQ104].completed = true;
	StageLog::Queue queue;
	for (const auto stage : StageLog::Stages{ 20, 30, 40, 45, 50 }) {
		queue.Add(SIDE, stage);
	}
	queue.Add(MQ104, 300);
	queue.Add(0x00ABCDEF, 10);  // a quest this game doesn't have
	CHECK(queue.Size() == 7);

	game.Poll(queue, Clock::time_point{} + std::chrono::hours(1));
	CHECK((game.set == std::vector<Stage>{ { SIDE, 50 } }));
	CHECK(queue.Size() == 1);
	CHECK(queue.Waiting(MQ104).empty());
}

TEST("stage queue: a quest not started here keeps its stages; what it reached by itself is dropped once it starts")
{
	FakeGame game;
	game.quests[SIDE].running = false;
	StageLog::Queue queue;
	for (const auto stage : StageLog::Stages{ 10, 20, 30 }) {
		queue.Add(SIDE, stage);
	}
	auto now = Clock::time_point{} + std::chrono::hours(1);
	for (int i = 0; i < 10; ++i) {
		game.Poll(queue, now);
		now += std::chrono::seconds(1);
	}
	CHECK(game.set.empty());
	CHECK(queue.Size() == 3);
	// The player starts it and gets to 20 on their own.
	game.quests[SIDE].running = true;
	game.SetStage(SIDE, 20);
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 30 } }));
	CHECK(queue.Size() == 0);
}

TEST("stage queue: nothing is set while the player is busy, and it waits")
{
	FakeGame game;
	game.quests[SIDE].current = 5;
	game.busy = true;
	StageLog::Queue queue;
	queue.Add(SIDE, 10);
	auto now = Clock::time_point{} + std::chrono::hours(1);
	for (int i = 0; i < 20; ++i) {
		game.Poll(queue, now);
		now += std::chrono::seconds(1);
	}
	CHECK(game.set.empty());
	CHECK(queue.Size() == 1);
	game.busy = false;
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
}

TEST("stage queue: at most a few stages per poll, the quests taking turns")
{
	FakeGame game;
	StageLog::Queue queue;
	for (std::uint32_t quest = 1; quest <= 6; ++quest) {
		game.quests[quest];
		queue.Add(quest, 10);
		queue.Add(quest, 20);
	}
	auto now = Clock::time_point{} + std::chrono::hours(1);
	game.Poll(queue, now);
	CHECK(game.set.size() == StageLog::Queue::MAX_PER_POLL);
	CHECK((game.set == std::vector<Stage>{ { 1, 10 }, { 2, 10 }, { 3, 10 }, { 4, 10 } }));
	game.Tick();
	game.set.clear();
	game.Poll(queue, now + std::chrono::milliseconds(500));
	// 5 and 6 haven't had a turn: they go first.
	CHECK((game.set == std::vector<Stage>{ { 5, 10 }, { 6, 10 }, { 1, 20 }, { 2, 20 } }));
	game.Tick();
	game.Run(queue, now);
	CHECK(queue.Size() == 0);
	for (std::uint32_t quest = 1; quest <= 6; ++quest) {
		CHECK((game.quests[quest].done == std::set<std::uint16_t>{ 10, 20 }));
	}
}

TEST("stage queue: a full queue refuses more, and clears")
{
	StageLog::Queue queue;
	for (std::size_t i = 0; i < StageLog::Queue::MAX_WAITING; ++i) {
		CHECK(queue.Add(static_cast<std::uint32_t>(1 + i / 100), static_cast<std::uint16_t>(i % 100)));
	}
	CHECK(queue.Size() == StageLog::Queue::MAX_WAITING);
	CHECK(!queue.Add(0x00FFFFFF, 1));
	queue.Clear();
	CHECK(queue.Size() == 0);
	CHECK(queue.Add(0x00FFFFFF, 1));
}
