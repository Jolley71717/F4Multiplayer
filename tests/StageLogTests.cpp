#include "Test.h"

#include "game/StageLog.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace
{
	using StageLog::Clock;
	using Stage = StageLog::Queue::Stage;
	using namespace std::chrono_literals;

	constexpr std::uint32_t MQ102 = 0x0001CC2A;
	constexpr std::uint32_t MQ103 = 0x0001CC2B;
	constexpr std::uint32_t MQ104 = 0x0001F25E;
	constexpr std::uint32_t MQ105 = 0x000229E6;
	constexpr std::uint32_t MQ206 = 0x000B1752;
	constexpr std::uint32_t MQ207 = 0x000229EC;
	constexpr std::uint32_t SIDE = 0x0001ED86;

	// A game's quests as the queue sees them. A stage set through the console runs at the next Tick, as
	// in the game, where it is set a moment later; a stage's script runs then too (onStage).
	struct FakeGame
	{
		struct Record
		{
			std::uint16_t           flags = StageLog::QUEST_RUNNING;
			std::uint16_t           current = 0;
			std::set<std::uint16_t> done;
			std::set<std::uint16_t> missing;  // stages this quest doesn't have: setting them does nothing
		};

		std::map<std::uint32_t, Record>                                quests;
		std::vector<Stage>                                             set;       // what the queue set, in order
		std::vector<Stage>                                             commands;  // not run yet
		std::function<void(std::uint32_t, std::uint16_t)>              onStage;
		bool                                                           busy = false;
		std::set<std::uint32_t>                                        startable;  // not running, and a friend's stage starts them (Quest::kStart)
		std::map<std::pair<std::uint32_t, std::uint16_t>, StageLog::Set> verdicts;  // kSet if not here
		std::map<std::pair<std::uint32_t, std::uint16_t>, Clock::duration> holds;

		bool Running(std::uint32_t a_quest) { return (quests[a_quest].flags & StageLog::QUEST_RUNNING) != 0; }

		void SetStage(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			auto& quest = quests[a_quest];
			if (!(quest.flags & StageLog::QUEST_RUNNING) && startable.contains(a_quest)) {
				quest.flags |= StageLog::QUEST_RUNNING;  // SetStage starts a quest that isn't running
			}
			if (!(quest.flags & StageLog::QUEST_RUNNING) || quest.missing.contains(a_stage) || quest.done.contains(a_stage)) {
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

		// The queue's view of this game.
		StageLog::Quest Quest(std::uint32_t a_quest)
		{
			const auto it = quests.find(a_quest);
			if (it == quests.end() || StageLog::Finished(it->second.flags)) {
				return StageLog::Quest::kGone;
			}
			if (!(it->second.flags & StageLog::QUEST_RUNNING) && startable.contains(a_quest)) {
				return busy ? StageLog::Quest::kWait : StageLog::Quest::kStart;
			}
			return !(it->second.flags & StageLog::QUEST_RUNNING) || busy ? StageLog::Quest::kWait : StageLog::Quest::kReady;
		}

		bool Settled(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto& quest = quests.at(a_quest);
			return quest.done.contains(a_stage) || a_stage <= quest.current;
		}

		StageLog::Set Set(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			if (const auto it = verdicts.find({ a_quest, a_stage }); it != verdicts.end() && it->second != StageLog::Set::kSet) {
				return it->second;
			}
			set.push_back({ a_quest, a_stage });
			commands.push_back({ a_quest, a_stage });
			return StageLog::Set::kSet;
		}

		Clock::duration Hold(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			const auto it = holds.find({ a_quest, a_stage });
			return it != holds.end() ? it->second : Clock::duration::zero();
		}

		std::vector<Stage> Poll(StageLog::Queue& a_queue, Clock::time_point a_now, bool a_paused = false) { return a_queue.Poll(a_now, *this, a_paused); }

		// Polls and runs the console until nothing is waiting (or a_polls run out).
		void Run(StageLog::Queue& a_queue, Clock::time_point& a_now, int a_polls = 50)
		{
			for (int i = 0; i < a_polls && a_queue.Size() > 0; ++i) {
				Poll(a_queue, a_now);
				Tick();
				a_now += 500ms;
			}
		}
	};

	Clock::time_point Start()
	{
		return Clock::time_point{} + std::chrono::hours(1);
	}
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

TEST("stage log: a quest is finished only once it is completed and stopped")
{
	CHECK(!StageLog::Finished(0));
	CHECK(!StageLog::Finished(StageLog::QUEST_RUNNING));
	CHECK(!StageLog::Finished(StageLog::QUEST_RUNNING | StageLog::QUEST_COMPLETED));
	CHECK(StageLog::Finished(StageLog::QUEST_COMPLETED));
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
	CHECK(queue.Contains(SIDE, 20));
	CHECK(!queue.Contains(SIDE, 40));

	auto now = Start();
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
	CHECK(queue.Setting() == 1);
	// Not done yet (the console runs it a moment later): nothing more is set.
	game.Poll(queue, now + 500ms);
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
	game.quests[MQ105].flags = 0;
	// MQ104's stage 210: starts MQ105 at 10, then MQ104 is done.
	game.onStage = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		if (a_quest == MQ104 && a_stage == 210) {
			game.quests[MQ105].flags = StageLog::QUEST_RUNNING;
			game.SetStage(MQ105, 10);
			game.SetStage(MQ104, 1000);
			game.quests[MQ104].flags = StageLog::QUEST_COMPLETED;
		}
	};
	StageLog::Queue queue;
	// As the other game reported them: MQ104's last stages, and MQ105's that its 210 started.
	for (const auto& [quest, stage] : std::vector<Stage>{ { MQ104, 210 }, { MQ104, 1000 }, { MQ105, 10 }, { MQ105, 20 }, { MQ105, 30 } }) {
		CHECK(queue.Add(quest, stage));
	}

	auto now = Start();
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

TEST("stage queue: a stage that completes its quest but leaves it running doesn't drop the stages after it (MQ102 65)")
{
	FakeGame game;
	game.quests[MQ102].current = 60;
	game.quests[MQ103].flags = 0;
	game.onStage = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		if (a_quest == MQ102 && a_stage == 65) {
			game.quests[MQ102].flags |= StageLog::QUEST_COMPLETED;  // the Complete Quest log entry: still running
		} else if (a_quest == MQ102 && a_stage == 150) {
			game.quests[MQ103].flags = StageLog::QUEST_RUNNING;
			game.SetStage(MQ103, 10);
		} else if (a_quest == MQ102 && a_stage == 200) {
			game.quests[MQ102].flags = StageLog::QUEST_COMPLETED;  // Stop()
		}
	};
	StageLog::Queue queue;
	for (const auto stage : { 61, 62, 65, 70, 90, 150, 200 }) {
		queue.Add(MQ102, static_cast<std::uint16_t>(stage));
	}
	queue.Add(MQ103, 10);
	queue.Add(MQ103, 20);

	auto now = Start();
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { MQ102, 61 }, { MQ102, 62 }, { MQ102, 65 }, { MQ102, 70 }, { MQ102, 90 }, { MQ102, 150 }, { MQ102, 200 }, { MQ103, 20 } }));
	CHECK(queue.Size() == 0);
	CHECK(game.quests[MQ103].current == 20);
	// Once it is stopped too, whatever else comes for it is dropped.
	queue.Add(MQ102, 300);
	game.Poll(queue, now);
	CHECK(queue.Size() == 0);
}

TEST("stage queue: a stage that never reads done is given up on after the timeout, then the next goes")
{
	FakeGame game;
	game.quests[SIDE].missing = { 50 };
	StageLog::Queue queue;
	queue.Add(SIDE, 50);
	queue.Add(SIDE, 60);

	const auto start = Start();
	CHECK(game.Poll(queue, start).empty());
	game.Tick();
	CHECK(game.Poll(queue, start + 4s).empty());
	CHECK(game.set.size() == 1);
	const auto gaveUp = game.Poll(queue, start + StageLog::Queue::TIMEOUT);
	CHECK((gaveUp == std::vector<Stage>{ { SIDE, 50 } }));
	CHECK((game.set == std::vector<Stage>{ { SIDE, 50 }, { SIDE, 60 } }));
	game.Tick();
	game.Poll(queue, start + StageLog::Queue::TIMEOUT + 500ms);
	CHECK(queue.Size() == 0);
}

TEST("stage queue: stages done here or already passed are dropped, and a finished quest's all")
{
	FakeGame game;
	game.quests[SIDE].current = 40;
	game.quests[SIDE].done = { 10, 40, 45 };
	game.quests[MQ104].flags = StageLog::QUEST_COMPLETED;
	StageLog::Queue queue;
	for (const auto stage : StageLog::Stages{ 20, 30, 40, 45, 50 }) {
		queue.Add(SIDE, stage);
	}
	queue.Add(MQ104, 300);
	queue.Add(0x00ABCDEF, 10);  // a quest this game doesn't have
	CHECK(queue.Size() == 7);

	game.Poll(queue, Start());
	CHECK((game.set == std::vector<Stage>{ { SIDE, 50 } }));
	CHECK(queue.Size() == 1);
	CHECK(queue.Waiting(MQ104).empty());
}

TEST("stage queue: a quest not started here keeps its stages; what it reached by itself is dropped once it starts")
{
	FakeGame game;
	game.quests[SIDE].flags = 0;
	StageLog::Queue queue;
	for (const auto stage : StageLog::Stages{ 10, 20, 30 }) {
		queue.Add(SIDE, stage);
	}
	auto now = Start();
	for (int i = 0; i < 10; ++i) {
		game.Poll(queue, now);
		now += 1s;
	}
	CHECK(game.set.empty());
	CHECK(queue.Size() == 3);
	// The player starts it and gets to 20 on their own.
	game.quests[SIDE].flags = StageLog::QUEST_RUNNING;
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
	auto now = Start();
	for (int i = 0; i < 20; ++i) {
		game.Poll(queue, now);
		now += 1s;
	}
	CHECK(game.set.empty());
	CHECK(queue.Size() == 1);
	game.busy = false;
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
}

TEST("stage queue: stages are set in the order they came, a few per poll")
{
	FakeGame game;
	StageLog::Queue queue;
	// Reported as they were done: each quest's 10, then each quest's 20.
	for (const auto stage : std::initializer_list<std::uint16_t>{ 10, 20 }) {
		for (std::uint32_t quest = 6; quest >= 1; --quest) {
			game.quests[quest];
			queue.Add(quest, stage);
		}
	}
	auto now = Start();
	game.Poll(queue, now);
	CHECK(game.set.size() == StageLog::Queue::MAX_PER_POLL);
	CHECK((game.set == std::vector<Stage>{ { 6, 10 }, { 5, 10 }, { 4, 10 }, { 3, 10 } }));
	game.Tick();
	game.set.clear();
	game.Poll(queue, now + 500ms);
	// 2 and 1 came before any 20.
	CHECK((game.set == std::vector<Stage>{ { 2, 10 }, { 1, 10 }, { 6, 20 }, { 5, 20 } }));
	game.Tick();
	game.Run(queue, now);
	CHECK(queue.Size() == 0);
	for (std::uint32_t quest = 1; quest <= 6; ++quest) {
		CHECK((game.quests[quest].done == std::set<std::uint16_t>{ 10, 20 }));
	}
}

TEST("stage queue: after a stage whose script waits, nothing that came after it is set until it has run (MQ206 1000)")
{
	FakeGame game;
	game.quests[MQ206].current = 700;
	game.quests[SIDE];
	game.quests[MQ207].flags = 0;
	game.holds[{ MQ206, 1000 }] = 5s;  // Wait 4, then the player goes through the door
	game.holds[{ MQ206, 1050 }] = 3s;  // Wait 2, then 1200
	StageLog::Queue queue;
	queue.Add(MQ207, 10);  // came first; MQ207 isn't running yet, which holds nothing up
	queue.Add(MQ206, 1000);
	queue.Add(SIDE, 10);
	queue.Add(MQ206, 1050);
	queue.Add(MQ206, 1200);
	queue.Add(SIDE, 20);

	auto now = Start();
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 } }));
	game.Tick();
	// 1000 reads done at the next poll, but its script runs for 5 s more: SIDE 10 came after it and waits.
	for (int i = 1; i <= 10; ++i) {
		game.Poll(queue, now + i * 500ms);
		game.Tick();
	}
	CHECK(game.set.size() == 1);
	// Then what came next, in order: SIDE 10, and MQ206's 1050 (its 3 s hold up the rest).
	game.Poll(queue, now + 5500ms);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 }, { SIDE, 10 }, { MQ206, 1050 } }));
	game.Tick();
	for (int i = 12; i < 18; ++i) {
		game.Poll(queue, now + i * 500ms);
		game.Tick();
	}
	CHECK(game.set.size() == 3);
	auto later = now + 9s;
	game.Run(queue, later);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 }, { SIDE, 10 }, { MQ206, 1050 }, { MQ206, 1200 }, { SIDE, 20 } }));
	CHECK((queue.Waiting(MQ207) == StageLog::Stages{ 10 }));
}

TEST("stage queue: a skipped stage is dropped and the next goes; one for later holds up what came after it")
{
	FakeGame game;
	game.quests[SIDE].current = 5;
	game.quests[MQ105].current = 170;
	game.verdicts[{ MQ105, 177 }] = StageLog::Set::kSkipped;  // a bribe
	game.verdicts[{ SIDE, 10 }] = StageLog::Set::kLater;
	StageLog::Queue queue;
	queue.Add(MQ105, 177);
	queue.Add(MQ105, 180);
	queue.Add(SIDE, 10);
	queue.Add(MQ105, 185);

	auto now = Start();
	game.Run(queue, now, 10);
	// 177 is gone; 180 came before SIDE's 10, 185 after it.
	CHECK((game.set == std::vector<Stage>{ { MQ105, 180 } }));
	CHECK((queue.Waiting(MQ105) == StageLog::Stages{ 185 }));
	CHECK(!game.quests[MQ105].done.contains(177));

	game.verdicts.erase({ SIDE, 10 });
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { MQ105, 180 }, { SIDE, 10 }, { MQ105, 185 } }));
	CHECK(queue.Size() == 0);
}

TEST("stage queue: a lock whose release never comes holds up only its own quest (MQ206 500 without 1200)")
{
	FakeGame game;
	game.quests[MQ206].current = 400;
	game.quests[SIDE].current = 5;
	game.quests[MQ105].current = 170;
	game.verdicts[{ MQ206, 500 }] = StageLog::Set::kQuestWaits;  // 1200 isn't waiting here
	StageLog::Queue queue;
	queue.Add(MQ206, 500);
	queue.Add(MQ206, 1000);
	queue.Add(SIDE, 10);  // came after it
	queue.Add(MQ105, 180);

	auto now = Start();
	game.Run(queue, now, 20);
	// Everything else goes; MQ206 waits, 1000 behind its 500.
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 }, { MQ105, 180 } }));
	CHECK((queue.Waiting(MQ206) == StageLog::Stages{ 500, 1000 }));
	CHECK(queue.Size() == 2);

	// The release comes: 500 goes, then 1000, then 1200.
	queue.Add(MQ206, 1200);
	game.verdicts.erase({ MQ206, 500 });
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 }, { MQ105, 180 }, { MQ206, 500 }, { MQ206, 1000 }, { MQ206, 1200 } }));
	CHECK(queue.Size() == 0);
}

TEST("stage queue: when the session ends with the player's controls taken, only the lock's quest is kept")
{
	FakeGame game;
	game.quests[MQ206].current = 500;
	game.quests[SIDE].current = 5;
	game.quests[MQ207].flags = 0;
	StageLog::Queue queue;
	queue.Add(MQ206, 1000);
	queue.Add(SIDE, 10);
	queue.Add(MQ207, 10);
	queue.Add(MQ206, 1050);
	queue.Add(MQ206, 1200);
	queue.Add(SIDE, 20);
	CHECK(queue.Size() == 6);

	queue.Keep({ MQ206, MQ206 });
	CHECK(queue.Size() == 3);
	CHECK((queue.Waiting(MQ206) == StageLog::Stages{ 1000, 1050, 1200 }));
	CHECK(!queue.Contains(SIDE));
	CHECK(!queue.Contains(MQ207));

	auto now = Start();
	game.Run(queue, now);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 }, { MQ206, 1050 }, { MQ206, 1200 } }));
	CHECK(queue.Size() == 0);
}

TEST("stage queue: while the game is paused nothing is set, and holds and timeouts don't run down")
{
	FakeGame game;
	game.quests[MQ206].current = 700;
	game.quests[SIDE];
	game.holds[{ MQ206, 1000 }] = 5s;
	StageLog::Queue queue;
	queue.Add(MQ206, 1000);
	queue.Add(SIDE, 10);

	const auto start = Start();
	game.Poll(queue, start);
	game.Tick();
	game.Poll(queue, start + 500ms);  // 1000 reads done; its script runs on for 5 s
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 } }));
	// The Pip-Boy is open from 1 s to 4 s: the script's wait doesn't advance, so neither does the hold.
	game.Poll(queue, start + 1s, true);
	game.Poll(queue, start + 4s, true);
	game.Poll(queue, start + 5500ms);
	CHECK(game.set.size() == 1);
	game.Poll(queue, start + 8s);
	CHECK(game.set.size() == 1);
	// The half second before the first paused poll counts as paused too: the hold ends at 9 s.
	game.Poll(queue, start + 8500ms);
	CHECK(game.set.size() == 1);
	game.Poll(queue, start + 9s);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 }, { SIDE, 10 } }));

	// A stage set just before a pause isn't given up on for the time paused.
	game.quests[SIDE].missing = { 20 };
	queue.Add(SIDE, 20);
	game.Tick();
	game.Poll(queue, start + 9500ms);
	CHECK(game.set.size() == 3);
	game.Poll(queue, start + 10s, true);
	CHECK(game.Poll(queue, start + 30s, true).empty());
	CHECK(game.Poll(queue, start + 33s).empty());
	CHECK(!game.Poll(queue, start + 35s).empty());
	// Nothing is set while paused, either.
	queue.Add(SIDE, 30);
	game.Poll(queue, start + 36s, true);
	CHECK(game.set.size() == 3);
	game.Poll(queue, start + 36500ms);
	CHECK(game.set.size() == 4);
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

TEST("stage queue: a 90 s pause between a lock stage and the teleport doesn't give the player their controls back (MQ206 500, 1000)")
{
	FakeGame game;
	game.quests[MQ206].current = 400;
	game.holds[{ MQ206, 500 }] = 1100ms;
	game.holds[{ MQ206, 1000 }] = 5s;  // FireTeleporter, Wait(4), the door
	StageLog::Queue queue;
	queue.Add(MQ206, 500);
	queue.Add(MQ206, 1000);
	queue.Add(MQ206, 1050);
	queue.Add(MQ206, 1200);

	auto now = Start();
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 500 } }));
	queue.Lock(MQ206, MQ206);  // as QuestSync does once it sets 500
	game.Tick();
	now += 500ms;
	game.Poll(queue, now);  // 500 reads done; its script runs on for 1.1 s

	// The player alt-tabs (the pause menu) before 1000 is set, for 90 s.
	for (int i = 0; i < 181; ++i) {
		now += 500ms;
		game.Poll(queue, now, true);
		CHECK(!queue.LockOverdue());
	}
	CHECK((game.set == std::vector<Stage>{ { MQ206, 500 } }));

	// Back in the game: 1000 is set, and the Pip-Boy is open for 90 s while its script waits.
	bool pausedInTeleport = false;
	for (int i = 0; i < 60 && queue.Size() > 0; ++i) {
		now += 500ms;
		game.Poll(queue, now);
		CHECK(!queue.LockOverdue());
		game.Tick();
		if (!pausedInTeleport && game.quests[MQ206].done.contains(1000)) {
			pausedInTeleport = true;
			for (int j = 0; j < 181; ++j) {
				now += 500ms;
				game.Poll(queue, now, true);
				CHECK(!queue.LockOverdue());
			}
		}
	}
	CHECK(pausedInTeleport);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 500 }, { MQ206, 1000 }, { MQ206, 1050 }, { MQ206, 1200 } }));
	CHECK(queue.Size() == 0);
	CHECK(!queue.LockOverdue());
}

TEST("stage queue: a lock whose quest does nothing for 90 s of play is overdue, time paused not counted")
{
	// 1000 is set but never reads done (given up on after 5 s), and nothing else of MQ206 comes.
	FakeGame game;
	game.quests[MQ206].current = 400;
	game.quests[MQ206].missing = { 1000 };
	game.holds[{ MQ206, 500 }] = 1100ms;
	StageLog::Queue queue;
	queue.Add(MQ206, 500);
	queue.Add(MQ206, 1000);

	const auto start = Start();
	auto       now = start;
	game.Poll(queue, now);
	queue.Lock(MQ206, MQ206);
	std::optional<Clock::time_point> overdueAt;
	std::size_t                      gaveUp = 0;
	while (now < start + 200s && !overdueAt) {
		game.Tick();
		now += 500ms;
		const bool paused = now > start + 40s && now <= start + 100s;  // the pause menu for 60 s
		gaveUp += game.Poll(queue, now, paused).size();
		if (queue.LockOverdue()) {
			overdueAt = now;
		}
	}
	CHECK(gaveUp == 1);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 500 }, { MQ206, 1000 } }));
	// 1000 set at 2 s, given up on at 7 s; 90 s of play after that, plus the 60 s paused.
	CHECK(overdueAt.has_value());
	CHECK(*overdueAt >= start + 156s && *overdueAt <= start + 157s);
	queue.Unlock();
	CHECK(!queue.LockOverdue());
}

TEST("stage queue: a lock whose next stage can never be set is overdue 90 s of play after its own script ran")
{
	FakeGame game;
	game.quests[MQ206].current = 400;
	game.holds[{ MQ206, 500 }] = 1100ms;
	StageLog::Queue queue;
	queue.Add(MQ206, 500);
	queue.Add(MQ206, 1000);

	const auto start = Start();
	auto       now = start;
	game.Poll(queue, now);
	queue.Lock(MQ206, MQ206);
	game.Tick();
	game.busy = true;  // something keeps MQ206 from taking its 1000
	std::optional<Clock::time_point> overdueAt;
	while (now < start + 200s && !overdueAt) {
		now += 500ms;
		game.Poll(queue, now);
		if (queue.LockOverdue()) {
			overdueAt = now;
		}
	}
	CHECK((game.set == std::vector<Stage>{ { MQ206, 500 } }));
	CHECK((queue.Waiting(MQ206) == StageLog::Stages{ 1000 }));
	// 500's script ran on until 1.6 s.
	CHECK(overdueAt.has_value());
	CHECK(*overdueAt >= start + 91s && *overdueAt <= start + 92s);
	queue.Clear();
	CHECK(!queue.LockOverdue());
}

TEST("stage queue: while the player's controls are taken, another quest's stage that waits doesn't hold up the lock's")
{
	FakeGame game;
	game.quests[MQ206].current = 500;
	game.quests[SIDE];
	game.verdicts[{ SIDE, 10 }] = StageLog::Set::kLater;  // a combat hold, say
	StageLog::Queue queue;
	queue.Add(SIDE, 10);  // came before them
	queue.Add(MQ206, 1000);
	queue.Add(MQ206, 1200);

	auto now = Start();
	game.Run(queue, now, 10);
	CHECK(game.set.empty());  // in the order they came, without a lock

	queue.Lock(MQ206, MQ206);
	game.Run(queue, now, 10);
	CHECK((game.set == std::vector<Stage>{ { MQ206, 1000 }, { MQ206, 1200 } }));
	CHECK((queue.Waiting(SIDE) == StageLog::Stages{ 10 }));
}

namespace
{
	template <class Report>
	using Outbox = StageLog::Outbox<Report>;

	constexpr Outbox<int>::Link SENDING{ true, true, true };
	constexpr Outbox<int>::Link RECONNECTING{ false, true, false };
	constexpr Outbox<int>::Link ALONE{ false, false, false };
}

TEST("stage outbox: a stage reached while the connection is being remade is sent once it is back, in order")
{
	Outbox<int> outbox;
	auto        now = Start();
	outbox.Add(1);
	outbox.Add(2);
	CHECK(outbox.Take(now, RECONNECTING).empty());  // joining, or reconnecting
	CHECK(outbox.Size() == 2);
	outbox.Add(3);
	now += 30s;
	CHECK((outbox.Take(now, SENDING) == std::vector<int>{ 1, 2, 3 }));
	CHECK(outbox.Size() == 0);
	CHECK(outbox.Take(now, SENDING).empty());  // and not twice
}

TEST("stage outbox: the story's stages reached while the connection is down are kept, and sent only into a session that shares the story")
{
	Outbox<int> outbox;
	auto        now = Start();
	outbox.Add(210, true);  // MQ104's 210, with the story the player's own while the connection is down
	outbox.Add(10);
	CHECK(outbox.Take(now, RECONNECTING).empty());
	CHECK(outbox.Size() == 2);
	CHECK((outbox.Take(now, SENDING) == std::vector<int>{ 210, 10 }));

	outbox.Add(1000, true);
	outbox.Add(20);
	CHECK((outbox.Take(now + 2s, { true, true, false }) == std::vector<int>{ 20 }));  // each player's own story
	CHECK(outbox.Size() == 0);
}

TEST("stage outbox: progress made playing alone never reaches a session joined later")
{
	Outbox<int> outbox;
	const auto  now = Start();
	outbox.Add(1);
	CHECK(outbox.Take(now, ALONE).empty());  // no session
	CHECK(outbox.Size() == 0);
	CHECK(outbox.Take(now, SENDING).empty());  // then a join
}

TEST("stage outbox: what was kept for a host who stopped answering doesn't go to another host's session")
{
	Outbox<int> outbox;
	const auto  now = Start();
	outbox.Add(1);
	CHECK(outbox.Take(now, RECONNECTING).empty());
	outbox.NewSession();  // a friend's invite accepted
	CHECK(outbox.Take(now + 1s, SENDING).empty());
}

TEST("stage outbox: after minutes of trying a host who doesn't answer, the player is playing alone, and nothing of it is sent when the host is back")
{
	Outbox<int> outbox;
	auto        now = Start();
	outbox.Add(1);
	CHECK(outbox.Take(now, RECONNECTING).empty());
	now += Outbox<int>::RECONNECT_GRACE - 1s;
	CHECK(outbox.Take(now, RECONNECTING).empty());
	CHECK(outbox.Size() == 1);  // a slow reconnect still gets it
	now += 1s;
	outbox.Add(2);
	CHECK(outbox.Take(now, RECONNECTING).empty());
	CHECK(outbox.Size() == 0);
	outbox.Add(3);  // still alone
	CHECK(outbox.Take(now + 10min, RECONNECTING).empty());
	CHECK(outbox.Take(now + 11min, SENDING).empty());

	// Back in the session, a drop starts the grace again.
	outbox.Add(4);
	CHECK(outbox.Take(now + 12min, RECONNECTING).empty());
	CHECK((outbox.Take(now + 13min, SENDING) == std::vector<int>{ 4 }));
}

TEST("stage outbox: a long backlog goes a couple of hundred a second, oldest first, under the server's budget of events")
{
	Outbox<std::size_t> outbox;
	auto                now = Start();
	const auto          total = Outbox<std::size_t>::MAX_PER_SECOND * 2 + 10;
	for (std::size_t i = 0; i < total; ++i) {
		outbox.Add(i);
	}
	CHECK(outbox.Take(now, { false, true, false }).empty());
	std::vector<std::size_t> sent;
	for (int frame = 0; frame < 60; ++frame) {  // a second of frames
		const auto some = outbox.Take(now + frame * 16ms, { true, true, true });
		sent.insert(sent.end(), some.begin(), some.end());
	}
	CHECK(sent.size() == Outbox<std::size_t>::MAX_PER_SECOND);
	CHECK(sent.front() == 0);
	now += 1s;
	for (int second = 0; second < 2; ++second, now += 1s) {
		const auto some = outbox.Take(now, { true, true, true });
		sent.insert(sent.end(), some.begin(), some.end());
	}
	REQUIRE(sent.size() == total);
	CHECK(std::ranges::is_sorted(sent));
	static_assert(Outbox<std::size_t>::MAX_PER_SECOND <= 250, "half the server's 500 events a second, the rest for everything else");
}

TEST("stage outbox: a connection down for a long time keeps the oldest stages, without growing")
{
	Outbox<std::size_t> outbox;
	const auto          total = Outbox<std::size_t>::MAX_UNSENT + 10;
	for (std::size_t i = 0; i < total; ++i) {
		outbox.Add(i);
	}
	auto now = Start();
	CHECK(outbox.Take(now, { false, true, false }).empty());
	CHECK(outbox.Size() == Outbox<std::size_t>::MAX_UNSENT);
	std::vector<std::size_t> sent;
	for (int second = 0; second < 10; ++second, now += 1s) {
		const auto some = outbox.Take(now, { true, true, true });
		sent.insert(sent.end(), some.begin(), some.end());
	}
	REQUIRE(sent.size() == Outbox<std::size_t>::MAX_UNSENT);
	CHECK(sent.front() == 0);  // what came first starts what came after (MQ104's 210 starts MQ105)
	CHECK(sent.back() == Outbox<std::size_t>::MAX_UNSENT - 1);
}

TEST("stage outbox: a save loaded drops the progress of the game before it")
{
	Outbox<int> outbox;
	outbox.Add(1);
	outbox.Clear();
	CHECK(outbox.Take(Start(), SENDING).empty());
}

TEST("stage queue: a friend's stage is set here when the session shares its quest, or when a lock begun in the session needs it after leaving")
{
	CHECK(StageLog::Settable(true, false));
	CHECK(StageLog::Settable(false, true));   // the teleport goes on with the story the player's own again
	CHECK(StageLog::Settable(true, true));
	CHECK(!StageLog::Settable(false, false));  // the story after leaving: nothing of the session's is set
}

TEST("stage queue: a lock's stage that has to wait doesn't let a stage overtake another quest's script still running")
{
	FakeGame game;
	game.quests[SIDE];
	game.quests[MQ105];
	game.quests[MQ206].current = 500;
	game.holds[{ SIDE, 10 }] = 10s;  // its script waits
	StageLog::Queue queue;
	queue.Add(SIDE, 10);
	queue.Add(MQ105, 100);  // came after SIDE's 10: waits for its script
	queue.Add(MQ206, 1000);
	queue.Lock(MQ206, MQ206);
	game.verdicts[{ MQ206, 1000 }] = StageLog::Set::kQuestWaits;

	auto now = Start();
	game.Poll(queue, now);
	game.Tick();
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));

	game.verdicts[{ MQ206, 1000 }] = StageLog::Set::kLater;  // its warning, say
	for (int i = 0; i < 4; ++i) {
		now += 500ms;
		game.Poll(queue, now);
		game.Tick();
	}
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));  // MQ105's 100 still waits for SIDE's 10 to have run
	CHECK((queue.Waiting(MQ105) == StageLog::Stages{ 100 }));

	now += 10s;
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 }, { MQ105, 100 } }));
}

TEST("stage log: a friend's stages say why they wait: a quest not running or not started here, a conversation, a loading screen, the opening")
{
	using StageLog::Wait;
	StageLog::QuestHere quest;
	CHECK(StageLog::WaitsFor(quest) == Wait::kNotRunning);
	quest.running = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kNotStarted);  // Vault 81 at its door: running, no objective shown
	quest.started = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kNone);
	quest.busy = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kBusy);
	quest.inWorld = false;
	CHECK(StageLog::WaitsFor(quest) == Wait::kLoading);
	quest.inWorld = true;
	quest.opening = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kOpening);
}

TEST("stage log: stages reached just before a quest was started are reported with it, ones from long before aren't")
{
	StageLog::Unstarted unstarted;
	const auto          start = Clock::time_point{} + 1h;
	CHECK((unstarted.Take(SIDE, { 20 }, start) == StageLog::Stages{ 20 }));

	unstarted.Add(SIDE, { 5 }, start);  // the game moved it on in the background
	unstarted.Add(MQ105, { 10 }, start);
	unstarted.Expire(start + StageLog::Unstarted::WINDOW);
	CHECK(!unstarted.Has(SIDE));
	CHECK(unstarted.Size() == 0);

	unstarted.Add(SIDE, { 10, 15 }, start);       // the conversation that starts it
	unstarted.Add(SIDE, { 55 }, start + 30s);     // the stage whose script shows the first objective
	CHECK(unstarted.Has(SIDE));
	CHECK(unstarted.Size() == 3);
	CHECK((unstarted.Take(SIDE, {}, start + 31s) == StageLog::Stages{ 10, 15, 55 }));
	CHECK(!unstarted.Has(SIDE));

	unstarted.Add(SIDE, { 10 }, start);
	unstarted.Add(SIDE, { 50 }, start + 50s);
	CHECK((unstarted.Take(SIDE, { 20, 60 }, start + 70s) == StageLog::Stages{ 20, 50, 60 }));  // 10 is too old

	unstarted.Add(SIDE, { 10 }, start);
	unstarted.Clear();
	CHECK(!unstarted.Has(SIDE));
}

TEST("stage log: in a session, a shared quest not started here is started by friends' stages, unless it ran here and was stopped")
{
	using StageLog::Wait;
	StageLog::QuestHere quest;
	quest.inSession = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kStart);  // not running here: the first stage starts it
	quest.running = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kStart);  // Vault 81 at its door: running in the background, no objective shown
	quest.busy = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kBusy);
	quest.busy = false;
	quest.startedHere = true;  // a friend's stage started it, and none of its objectives is shown yet
	CHECK(StageLog::WaitsFor(quest) == Wait::kNone);
	quest.startedHere = false;
	quest.started = true;
	CHECK(StageLog::WaitsFor(quest) == Wait::kNone);

	StageLog::QuestHere stopped;
	stopped.inSession = true;
	stopped.ranBefore = true;  // failed, or stopped by its own script: starting it again would start it over
	CHECK(StageLog::WaitsFor(stopped) == Wait::kStopped);
	stopped.inSession = false;
	CHECK(StageLog::WaitsFor(stopped) == Wait::kNotRunning);

	StageLog::QuestHere alone;  // out of a session (a lock's quest after leaving): the player's own
	CHECK(StageLog::WaitsFor(alone) == Wait::kNotRunning);
	alone.running = true;
	CHECK(StageLog::WaitsFor(alone) == Wait::kNotStarted);
	alone.inWorld = false;
	alone.inSession = true;
	CHECK(StageLog::WaitsFor(alone) == Wait::kLoading);
	alone.inWorld = true;
	alone.opening = true;
	CHECK(StageLog::WaitsFor(alone) == Wait::kOpening);
}

TEST("stage queue: a quest not started here is started by the first of its stages, in order, once nothing before it is still to be set")
{
	FakeGame game;
	game.quests[SIDE].current = 5;
	game.quests[MQ207].flags = 0;
	game.startable = { MQ207 };
	StageLog::Queue queue;
	queue.Add(SIDE, 10);
	queue.Add(SIDE, 20);
	queue.Add(MQ207, 10);  // came after SIDE's
	queue.Add(MQ207, 20);
	queue.Add(SIDE, 30);   // came after MQ207's first: doesn't hold it up once SIDE's 10 and 20 are set

	auto now = Start();
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
	game.Run(queue, now);
	CHECK(queue.Size() == 0);
	// SIDE's first stages, then MQ207 is started (by its 10) a moment after SIDE's last read done, and goes on.
	CHECK(std::ranges::find(game.set, Stage{ MQ207, 10 }) > std::ranges::find(game.set, Stage{ SIDE, 20 }));
	CHECK(std::ranges::find(game.set, Stage{ MQ207, 20 }) > std::ranges::find(game.set, Stage{ MQ207, 10 }));
	CHECK(game.quests[MQ207].current == 20);
	CHECK(game.quests[SIDE].current == 30);
}

TEST("stage queue: a quest another quest's stage starts isn't started again by the queue (MQ104 210 starts MQ105), even when it could start itself")
{
	FakeGame game;
	game.quests[MQ104].flags = 0;
	game.quests[MQ105].flags = 0;
	game.startable = { MQ104, MQ105 };  // the friend hasn't started either
	std::vector<Stage> started;  // by MQ104's 210
	game.onStage = [&](std::uint32_t a_quest, std::uint16_t a_stage) {
		if (a_quest == MQ104 && a_stage == 210) {
			started.push_back({ MQ105, 10 });
			game.SetStage(MQ105, 10);
			game.SetStage(MQ104, 1000);
		}
	};
	StageLog::Queue queue;
	for (const auto& [quest, stage] : std::vector<Stage>{ { MQ104, 200 }, { MQ104, 210 }, { MQ104, 1000 }, { MQ105, 10 }, { MQ105, 20 } }) {
		CHECK(queue.Add(quest, stage));
	}

	auto now = Start();
	game.Run(queue, now);
	CHECK(queue.Size() == 0);
	// The queue started MQ104 with its 200; MQ105 was started by 210's script, never by the queue.
	CHECK((game.set == std::vector<Stage>{ { MQ104, 200 }, { MQ104, 210 }, { MQ105, 20 } }));
	CHECK((started == std::vector<Stage>{ { MQ105, 10 } }));
	CHECK(game.quests[MQ105].current == 20);
}

TEST("stage queue: a quest not started here waits a moment after the last stage set here read done, its script may start it")
{
	FakeGame game;
	game.quests[SIDE].current = 5;
	game.quests[MQ207].flags = 0;
	game.startable = { MQ207 };
	StageLog::Queue queue;
	queue.Add(SIDE, 10);
	queue.Add(MQ207, 10);

	auto now = Start();
	game.Poll(queue, now);  // SIDE's 10 set
	game.Tick();
	now += 500ms;
	game.Poll(queue, now);  // it reads done: MQ207 waits START_GRACE
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
	now += StageLog::Queue::START_GRACE - 500ms;
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 } }));
	now += 500ms;
	game.Poll(queue, now);
	CHECK((game.set == std::vector<Stage>{ { SIDE, 10 }, { MQ207, 10 } }));
}

TEST("stage queue: a quest waiting for good, or a lock waiting for its release, holds up no start")
{
	FakeGame game;
	game.quests[SIDE].flags = 0;  // stopped here before: kWait for good
	game.quests[MQ206].current = 400;
	game.verdicts[{ MQ206, 500 }] = StageLog::Set::kQuestWaits;  // 1200 isn't coming
	game.quests[MQ207].flags = 0;
	game.startable = { MQ207 };
	StageLog::Queue queue;
	queue.Add(SIDE, 10);
	queue.Add(MQ206, 500);
	queue.Add(MQ207, 10);

	auto now = Start();
	game.Run(queue, now, 10);
	CHECK((game.set == std::vector<Stage>{ { MQ207, 10 } }));
	CHECK((queue.Waiting(SIDE) == StageLog::Stages{ 10 }));
	CHECK((queue.Waiting(MQ206) == StageLog::Stages{ 500 }));
}

TEST("stage queue: a quest not started here isn't started while the player is busy or the game is paused")
{
	FakeGame game;
	game.quests[MQ207].flags = 0;
	game.startable = { MQ207 };
	StageLog::Queue queue;
	queue.Add(MQ207, 10);

	auto now = Start();
	game.busy = true;
	game.Run(queue, now, 10);
	CHECK(game.set.empty());
	game.busy = false;
	game.Poll(queue, now, true);
	CHECK(game.set.empty());
	game.Run(queue, now, 10);
	CHECK((game.set == std::vector<Stage>{ { MQ207, 10 } }));
	CHECK(game.Running(MQ207));
}
