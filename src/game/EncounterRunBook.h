#pragma once

#include "Protocol.h"
#include "game/Encounters.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Random encounters that show an objective, shared by their run (EncounterRuns.h has the game's side): a checkpoint
// under attack (RECheckpoint01 to 16: "Help the defenders"), a treasure hunt's note (RETreasureHuntSC01 to 06), a few
// scenes (RESceneKMK01 and 02) and the add-ons' like them. Their actors are shared as any encounter's (Encounters.h), one
// run per trigger; the run that stands there is shared as a radiant's run is (RadiantSync, the same messages): the
// friends' games that come near it start the same quest with its aliases forced to what the owner's game filled them
// with (their copies of its actors, the same trigger and places), and from then on each stage of it set in one game is
// set in the other, once. The friend's own actors of the quest (an alias the owner left unshared: a reinforcement still
// disabled, an actor not shared yet) are removed as they come, as a stopped run's are: the owner's are what stand there.
// A run is shared only once it stands (Encounters::Book::Stands: it can't give way to anyone's any more); a friend's game
// follows it only while the owner's game still shares a live actor of it, and only once its player comes near it (a
// copy of one of the run's actors placed, or its trigger within COLLIDE_RANGE), and never while it runs that quest as its
// own. Kept free of the game so it can be tested (tests/EncounterTests.cpp).
namespace Encounters
{
	// A friend's run waits this long for our player to come near it; then it is theirs alone.
	inline constexpr auto LISTEN_FOR = std::chrono::minutes(5);
	// Once near, a run waits this long for every copy it names to be placed; then it starts without the rest (the
	// game makes its own for their aliases, removed as they come).
	inline constexpr auto FILL_WAIT = std::chrono::seconds(5);
	// A start that has not made the quest run in this long failed.
	inline constexpr auto RUN_START_WAIT = std::chrono::seconds(10);
	// Runs kept at once, ours and friends', and runs remembered as finished (never followed again).
	inline constexpr std::size_t MAX_RUNS = 16;
	inline constexpr std::size_t MAX_FINISHED = 64;

	// Whether an encounter is shared by its run as well as by its actors: one with an objective to show.
	inline bool SharesRun(bool a_encounter, std::size_t a_objectives)
	{
		return a_encounter && a_objectives > 0;
	}

	// Whether our run stands, to be shared: Book::Stands when the book knows the run (its actors seen); a run with no
	// live actor (a treasure hunt's corpse and note) is known only by its quest running, and stands once it has run for
	// STOP_WINDOW (a_running: since it was first seen running).
	inline bool RunStands(std::optional<bool> a_bookStands, Clock::duration a_running)
	{
		return a_bookStands ? *a_bookStands : a_running >= Clock::duration{ STOP_WINDOW };
	}

	// One run: the player whose game rolled it and that game's number for it (as RadiantRules::Run).
	struct RunKey
	{
		std::uint32_t origin = 0;
		std::uint32_t serial = 0;

		bool operator==(const RunKey&) const = default;
	};

	// What the game knows of a friend's run it may follow (RunBook::Judge).
	struct HeardFacts
	{
		bool        ownerLive = true;       // the owner's game still shares a live actor of the encounter (or the run names none)
		bool        runningHere = false;    // our game runs the quest now
		bool        oursGivingWay = false;  // ... our own run of it, which is being stopped (or may yet be, within its window)
		bool        near = false;           // our player is near the run: a copy of its actors placed, or its trigger close
		std::size_t waiting = 0;            // copies it names that are not placed yet but may be
	};

	enum class Verdict : std::uint8_t
	{
		kWait,
		kStart,
		kDrop,
	};

	struct Judgement
	{
		Verdict          verdict = Verdict::kWait;
		std::string_view why;
	};

	class RunBook
	{
	public:
		enum class State : std::uint8_t
		{
			kOurs,       // our run, shared with the friends
			kHeard,      // a friend's, waiting for our player to come near
			kStarting,   // a friend's, started here with its fills: not yet seen running
			kFollowing,  // a friend's, running here: its stages are set both ways
		};

		struct Run
		{
			std::uint32_t                      quest = 0;
			RunKey                             key;
			State                              state = State::kHeard;
			std::vector<Protocol::RadiantFill> fills;    // a friend's run's, as their game had them
			std::vector<std::uint16_t>         done;     // stages done under the run in any game: each passed on and set once
			std::vector<std::uint16_t>         toSet;    // a friend's stages to set here once its run runs here
			Clock::time_point                  since{};  // heard, announced or started
			std::optional<Clock::time_point>   near;     // when our player first came near it
		};

		// a_seed: where this game's run numbers start (RadiantRules::SerialSeed).
		explicit RunBook(std::uint32_t a_seed) :
			serial(a_seed)
		{}

		void SetSelf(std::uint32_t a_self) { self = a_self; }
		[[nodiscard]] std::uint32_t Self() const { return self; }

		// Our run of a_quest stands: shared as a new run, once, with the stages done so far. Nothing when we have no id in
		// the session yet, the quest has a run of ours already, or the book is full.
		std::optional<RunKey> Announce(std::uint32_t a_quest, std::span<const std::uint16_t> a_done, Clock::time_point a_now)
		{
			if (self == 0 || runs.size() >= MAX_RUNS || std::ranges::any_of(runs, [&](const Run& a_run) { return a_run.quest == a_quest && a_run.state == State::kOurs; })) {
				return std::nullopt;
			}
			Run run{ .quest = a_quest, .key = { self, ++serial }, .state = State::kOurs, .since = a_now };
			AddDone(run, a_done);
			runs.push_back(std::move(run));
			return runs.back().key;
		}

		// A friend's run heard: kept until it is followed or dropped. False for our own, one known or finished, a run
		// with no origin, or no room.
		bool Hear(const Protocol::RadiantStart& a_start, Clock::time_point a_now)
		{
			const RunKey key{ a_start.playerId, a_start.serial };
			if (key.origin == 0 || key.origin == self || Find(key) || std::ranges::contains(finished, key) || runs.size() >= MAX_RUNS) {
				return false;
			}
			Run run{ .quest = a_start.quest, .key = key, .state = State::kHeard, .fills = a_start.fills, .since = a_now };
			AddDone(run, a_start.stages);
			run.toSet = run.done;
			runs.push_back(std::move(run));
			return true;
		}

		// A stage of a run heard from a friend: true when it is to be set here now (our run, or a friend's we follow, and
		// not done under it yet). One of a friend's run not running here yet waits for it (toSet). Marked done either way,
		// so it is never passed back.
		bool TakeStage(std::uint32_t a_quest, RunKey a_key, std::uint16_t a_stage)
		{
			const auto run = Find(a_key);
			if (!run || run->quest != a_quest || std::ranges::contains(run->done, a_stage)) {
				return false;
			}
			AddDone(*run, std::span{ &a_stage, 1 });
			if (run->state == State::kOurs || run->state == State::kFollowing) {
				return true;
			}
			run->toSet.push_back(a_stage);
			return false;
		}

		// Stages done here of a_quest: the runs each is to be reported under (ours, or a friend's we follow), once per run.
		std::vector<RunKey> Report(std::uint32_t a_quest, std::uint16_t a_stage)
		{
			std::vector<RunKey> out;
			for (auto& run : runs) {
				if (run.quest == a_quest && (run.state == State::kOurs || run.state == State::kFollowing) && !std::ranges::contains(run.done, a_stage)) {
					AddDone(run, std::span{ &a_stage, 1 });
					out.push_back(run.key);
				}
			}
			return out;
		}

		// Whether a friend's run heard here is ours to start now, to wait for, or to drop, by what the game knows of it
		// (a_facts). Waiting runs are judged again; the first time our player is near one is kept.
		Judgement Judge(Run& a_run, const HeardFacts& a_facts, Clock::time_point a_now)
		{
			if (a_now - a_run.since >= LISTEN_FOR) {
				return { Verdict::kDrop, "our player never came near it" };
			}
			if (!a_facts.ownerLive) {
				return { Verdict::kDrop, "its actors are gone in their game" };
			}
			if (a_facts.runningHere) {
				return a_facts.oursGivingWay ? Judgement{ Verdict::kWait, "our own run of it is giving way" } : Judgement{ Verdict::kDrop, "our game runs it as its own" };
			}
			if (!a_facts.near) {
				return { Verdict::kWait, "our player is not near it" };
			}
			if (!a_run.near) {
				a_run.near = a_now;
			}
			if (a_facts.waiting > 0 && a_now - *a_run.near < FILL_WAIT) {
				return { Verdict::kWait, "a copy it names is not placed yet" };
			}
			return { Verdict::kStart, {} };
		}

		// A friend's run was started here (its forced fills): running soon.
		void Started(RunKey a_key, Clock::time_point a_now)
		{
			if (const auto run = Find(a_key); run && run->state == State::kHeard) {
				run->state = State::kStarting;
				run->since = a_now;
			}
		}

		// A friend's run started here runs: followed from now on. The stages it had and those that came meanwhile, to
		// set here (each once).
		std::vector<std::uint16_t> Following(RunKey a_key)
		{
			const auto run = Find(a_key);
			if (!run || run->state != State::kStarting) {
				return {};
			}
			run->state = State::kFollowing;
			return std::exchange(run->toSet, {});
		}

		// A run is done with: forgotten, and a friend's never followed again (its origin may send it again).
		void Drop(RunKey a_key)
		{
			const auto it = std::ranges::find(runs, a_key, &Run::key);
			if (it == runs.end()) {
				return;
			}
			if (it->key.origin != self) {
				if (finished.size() >= MAX_FINISHED) {
					finished.erase(finished.begin());
				}
				finished.push_back(it->key);
			}
			runs.erase(it);
		}

		// The quest no longer runs here: our run of it and a friend's we follow are over. (One starting does not run yet:
		// RUN_START_WAIT ends it if it never does.)
		std::vector<RunKey> Over(std::uint32_t a_quest)
		{
			std::vector<RunKey> out;
			for (const auto& run : runs) {
				if (run.quest == a_quest && (run.state == State::kOurs || run.state == State::kFollowing)) {
					out.push_back(run.key);
				}
			}
			for (const auto key : out) {
				Drop(key);
			}
			return out;
		}

		// Whether a friend's run of the quest is followed here, or starting: its stages are set here, and our game's own
		// actors of the quest are not what stands there (removed).
		[[nodiscard]] bool Follows(std::uint32_t a_quest) const
		{
			return std::ranges::any_of(runs, [&](const Run& a_run) { return a_run.quest == a_quest && (a_run.state == State::kStarting || a_run.state == State::kFollowing); });
		}

		// Whether the quest's stages go by a run now: ours, or a friend's we follow.
		[[nodiscard]] bool Shares(std::uint32_t a_quest) const
		{
			return std::ranges::any_of(runs, [&](const Run& a_run) { return a_run.quest == a_quest && a_run.state != State::kHeard; });
		}

		[[nodiscard]] Run* Find(RunKey a_key)
		{
			const auto it = std::ranges::find(runs, a_key, &Run::key);
			return it != runs.end() ? &*it : nullptr;
		}

		[[nodiscard]] std::vector<Run>&       Runs() { return runs; }
		[[nodiscard]] const std::vector<Run>& Runs() const { return runs; }

		// The session ended, another server, a save loaded: runs are of another world. The finished are kept (a friend's
		// run is never followed twice), and so are the run numbers.
		void Clear() { runs.clear(); }

	private:
		static void AddDone(Run& a_run, std::span<const std::uint16_t> a_stages)
		{
			for (const auto stage : a_stages) {
				if (a_run.done.size() < Protocol::MAX_RADIANT_STAGES && !std::ranges::contains(a_run.done, stage)) {
					a_run.done.insert(std::ranges::upper_bound(a_run.done, stage), stage);
				}
			}
		}

		std::vector<Run>    runs;
		std::vector<RunKey> finished;
		std::uint32_t       serial = 0;
		std::uint32_t       self = 0;
	};
}
