#pragma once

#include "Protocol.h"
#include "game/Encounters.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
// own. A run that names the owner's live actors starts only once a copy of one of them is placed here (never with the
// friend's own actors removed and none of the owner's to stand in their place). The owner's run that ends sends its last
// stages and then RUN_OVER under it: a friend's game still running it stops it RUN_END_WAIT later (its last stage most
// likely stopped it already), as it does when the owner leaves. A followed run's aliases that name the owner's actors are
// forced again onto our copies whenever a copy is placed anew (a save deletes them and places them again).
// Kept free of the game so it can be tested (tests/EncounterTests.cpp).
namespace Encounters
{
	// A friend's run waits this long for our player to come near it; then it is theirs alone.
	inline constexpr auto LISTEN_FOR = std::chrono::minutes(5);
	// Once near, a run waits this long for every copy it names to be placed; then it starts without the rest (the
	// game makes its own for their aliases, removed as they come).
	inline constexpr auto FILL_WAIT = std::chrono::seconds(5);
	// A start that has not made the quest run in this long failed.
	inline constexpr auto RUN_START_WAIT = std::chrono::seconds(10);
	// The owner's run ended (RUN_OVER) or the owner left: a friend's game still running it stops it this much later (its
	// last stages, sent first, have time to end it the quest's own way).
	inline constexpr auto RUN_END_WAIT = std::chrono::seconds(5);
	// ... and longer while one of those last stages still waits in the quest queue (a combat hold keeps a friend's stage
	// back: QuestSync), so a clean-up stage is set the quest's own way before the stop; but no longer than this.
	inline constexpr auto RUN_END_LIMIT = std::chrono::seconds(60);
	// Our run of a quest with none of the actors its start created (hollow: a save's of a run we followed, whose copies were
	// never saved), shared by nobody, is stopped once it has run so this long with no friend's run of the quest heard (one
	// heard replaces it once our player is near): nobody's actors will stand for it, and its objective would stay for good.
	inline constexpr auto HOLLOW_WAIT = LISTEN_FOR;
	// A followed run's alias forced onto a copy is asked again on later polls when the ask was not taken (the script VM
	// would not take the call), when it was taken but the alias does not hold the copy FORCE_READ_BACK later (the call ran
	// a frame or so after it was queued, or not at all: the rig, 2026-11-11 and 12, radiant-rr's alias 5 kept the friend's
	// own removed courser, read seconds after a force the VM took), or when the alias held the copy and holds something
	// else since (the quest's own script put another in): this many asks in all for one copy, then it is given up on.
	inline constexpr std::uint32_t MAX_FORCE_TRIES = 3;
	inline constexpr auto          FORCE_READ_BACK = std::chrono::seconds(2);
	// A stage no quest has (the editor's stages go to 9999): sent under a run as its owner's quest stops, after its last
	// stages. No new message type: an older game drops an encounter's run stage unread.
	inline constexpr std::uint16_t RUN_OVER = 0xFFFF;
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

	// Whether our game's run of a quest that is hollow (a_hollow: every alias its start created is empty, as a save's of a
	// run we followed is after the load) and that no run in the book shares is to be stopped now: it has run so for
	// HOLLOW_WAIT (a_running: since it was first seen running after the load) with no friend's run of the quest heard
	// (a_heard: one heard makes way for theirs once our player is near, Verdict::kStopOurs). In a session or alone.
	inline bool EndsHollow(bool a_hollow, bool a_heard, Clock::duration a_running)
	{
		return a_hollow && !a_heard && a_running >= HOLLOW_WAIT;
	}

	// Where a followed run's alias stands with the copy our game forces into it (AliasForce).
	enum class ForceState : std::uint8_t
	{
		kAsking,   // the last ask was not taken: asked again next poll
		kTaken,    // the last ask was taken: read back FORCE_READ_BACK after it
		kHeld,     // read back holding the copy
		kGivenUp,  // MAX_FORCE_TRIES asks made: left as it is until another copy is placed
	};

	// A followed run's alias that names one of the owner's actors, as our game forces it onto its copies (RefillsOf,
	// NoteForced, ReadBack, GiveUp): the copy it is forced onto (the last placed), the asks made for that copy, when the
	// last taken one was made, and where it stands. Used for an encounter's run (RunBook) and a radiant's with actors made
	// at runtime (RadiantRules::Entry::spawned, RadiantSync) alike.
	struct AliasForce
	{
		std::uint32_t     alias = 0;
		std::uint32_t     copy = 0;
		std::uint32_t     tries = 0;
		Clock::time_point asked{};
		ForceState        state = ForceState::kAsking;
	};

	// Why an alias is (or is no longer) forced onto our copy now (Refill).
	enum class RefillWhy : std::uint8_t
	{
		kNew,       // a copy placed now, never asked for in this alias
		kNotTaken,  // the last ask was not taken by the script VM
		kNotHeld,   // the last ask was taken, but the alias holds something else FORCE_READ_BACK later
		kLost,      // the alias held the copy and holds another reference since (an emptied one is left so)
		kGiveUp,    // one of the last three, with MAX_FORCE_TRIES asks made: not asked again (GiveUp)
	};

	struct Refill
	{
		std::uint32_t alias = 0;
		std::uint32_t copy = 0;
		RefillWhy     why = RefillWhy::kNew;
		std::uint32_t held = 0;  // what the alias holds here now (0: nothing)

		bool operator==(const Refill&) const = default;
	};

	// A followed run's aliases that name the owner's actors (a_fills: the run's, as their game sent them) and do not hold
	// our copy of the actor placed now: each to force in now (then NoteForced, with whether the ask was taken), or to give
	// up on (then GiveUp). A copy is asked for once when it is placed (a copy placed again after a save, or after our player
	// came back, is a new reference), and again on a later poll when the ask was not taken, when it was taken but the alias
	// does not hold the copy FORCE_READ_BACK later, or when the alias held it and holds something else since: up to
	// MAX_FORCE_TRIES asks for one copy. a_copy(spawn id): our copy placed now (0: none); a_here(alias): what fills it here
	// (0: none). Call ReadBack first, so an alias that holds its copy now is known to.
	template <class CopyFn, class HereFn>
	std::vector<Refill> RefillsOf(std::span<const Protocol::RadiantFill> a_fills, std::span<const AliasForce> a_forced, CopyFn&& a_copy, HereFn&& a_here, Clock::time_point a_now)
	{
		std::vector<Refill> out;
		for (const auto& fill : a_fills) {
			if (fill.kind != 0 || fill.form == Protocol::RADIANT_UNSHARED_REF || !Protocol::IsSpawnRef(fill.form)) {
				continue;
			}
			const std::uint32_t local = a_copy(fill.form);
			if (local == 0) {
				continue;
			}
			const std::uint32_t held = a_here(fill.alias);
			if (held == local) {
				continue;
			}
			const auto it = std::ranges::find(a_forced, fill.alias, &AliasForce::alias);
			if (it == a_forced.end() || it->copy != local) {
				out.push_back({ fill.alias, local, RefillWhy::kNew, held });
				continue;
			}
			RefillWhy why;
			switch (it->state) {
			case ForceState::kAsking:
				why = RefillWhy::kNotTaken;
				break;
			case ForceState::kTaken:
				if (a_now - it->asked < FORCE_READ_BACK) {
					continue;  // the call may not have run yet
				}
				why = RefillWhy::kNotHeld;
				break;
			case ForceState::kHeld:
				if (held == 0) {
					continue;  // emptied since: the quest's own Clear (a target dead), left so
				}
				why = RefillWhy::kLost;
				break;
			default:
				continue;  // given up on
			}
			out.push_back({ fill.alias, local, it->tries >= MAX_FORCE_TRIES ? RefillWhy::kGiveUp : why, held });
		}
		return out;
	}

	// The aliases whose last taken ask is read back holding its copy now (a_here(alias): what fills it here), each marked
	// held and returned with the asks it took (for the log: more than one, a retry did it).
	template <class HereFn>
	std::vector<std::pair<std::uint32_t, std::uint32_t>> ReadBack(std::vector<AliasForce>& a_forced, HereFn&& a_here)
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
		for (auto& force : a_forced) {
			if ((force.state == ForceState::kTaken || force.state == ForceState::kAsking) && force.copy != 0 && a_here(force.alias) == force.copy) {
				force.state = ForceState::kHeld;
				out.emplace_back(force.alias, force.tries);
			}
		}
		return out;
	}

	// The outcome of an ask RefillsOf gave: taken (a_ok), it is read back FORCE_READ_BACK later; not taken, asked again
	// next poll. Either way one of the MAX_FORCE_TRIES asks for the copy; a copy not asked for before starts the count.
	inline void NoteForced(std::vector<AliasForce>& a_forced, std::uint32_t a_alias, std::uint32_t a_copy, bool a_ok, Clock::time_point a_now)
	{
		auto it = std::ranges::find(a_forced, a_alias, &AliasForce::alias);
		if (it == a_forced.end()) {
			a_forced.push_back({ a_alias });
			it = std::prev(a_forced.end());
		}
		if (it->copy != a_copy) {
			it->copy = a_copy;
			it->tries = 0;
		}
		++it->tries;
		if (a_ok) {
			it->state = ForceState::kTaken;
			it->asked = a_now;
		} else {
			it->state = it->tries >= MAX_FORCE_TRIES ? ForceState::kGivenUp : ForceState::kAsking;
		}
	}

	// How an ask is made (ForceWayOf). The first ask for a copy, and one the script VM did not take, goes to the script VM
	// (ReferenceAlias.ForceRefTo, run on one of its threads). An ask made again because the alias does not hold the copy
	// the script VM took, or lost it since, calls the engine's TESQuest::ForceRefIntoAlias on the game's thread instead,
	// as the console's ForceRefIntoAlias does: it is the function ForceRefTo's native ends in (1.11.240), with nothing of
	// the script VM between (the call's two arguments as handles, its queue, its threads). The rig's radiant-rr (RRR07's
	// CourserTarget, 2026-11-11 and 12) never held the copy after three script asks the VM ran.
	enum class ForceWay : std::uint8_t
	{
		kScript,
		kEngine,
	};

	inline ForceWay ForceWayOf(RefillWhy a_why)
	{
		return a_why == RefillWhy::kNotHeld || a_why == RefillWhy::kLost ? ForceWay::kEngine : ForceWay::kScript;
	}

	// QUEST_DATA::flags the engine's own forces test (QuestStartRules.h has them too).
	inline constexpr std::uint16_t QUEST_ENABLED = 0x0001;
	inline constexpr std::uint16_t QUEST_STOP_START = 0x0080;

	// Whether a force into one of a quest's aliases is made now: the test the console's ForceRefIntoAlias makes before it
	// calls TESQuest::ForceRefIntoAlias (1.11.240, +5D9B2F to +5D9B4F): the quest enabled, no stop or start of it under
	// way, and no promotion of its references pending (TESQuest::promoteTask, +2D0: the task its start queued to make
	// the references its aliases hold persistent, which writes them back as it ends). Not now: the ask waits for a later
	// poll, and is not one of the MAX_FORCE_TRIES.
	inline bool ForceOpen(std::uint16_t a_flags, bool a_promoting)
	{
		return (a_flags & QUEST_ENABLED) != 0 && (a_flags & QUEST_STOP_START) == 0 && !a_promoting;
	}

	// Our own actor of a friend's run our game follows (an alias the owner's game left unshared, made by our start of the
	// run: Spawns' follow removals) is removed only once the start is done with it: no stop or start of the quest under
	// way and no promotion of its references pending (as ForceOpen, the quest running or not), and once it has stood
	// loaded SHARE_SETTLE (a ShareSettle wait of its own). Removed 284 ms after our start made it (a checkpoint's raider,
	// made beside our player), the game ended in abort() a moment later (the rig, 2026-11-12); every earlier removal was of
	// an actor that had stood 16 s or more. One that waits FOLLOW_REMOVE_LIMIT goes all the same, as before (a start that
	// never settles, or an actor whose 3D never comes: the graveyard waits for its 3D).
	inline constexpr auto FOLLOW_REMOVE_LIMIT = std::chrono::seconds(30);

	inline bool StartDone(std::uint16_t a_flags, bool a_promoting)
	{
		return (a_flags & QUEST_STOP_START) == 0 && !a_promoting;
	}

	enum class FollowRemoval : std::uint8_t
	{
		kWait,
		kNow,
	};

	// a_startDone: StartDone for the quest (true when the quest is gone). a_stood: loaded and standing, seen so for
	// SHARE_SETTLE by the settle wait. a_waited: since the removal was first asked.
	inline FollowRemoval JudgeFollowRemoval(bool a_startDone, bool a_stood, Clock::duration a_waited)
	{
		if (a_waited >= FOLLOW_REMOVE_LIMIT) {
			return FollowRemoval::kNow;
		}
		return a_startDone && a_stood ? FollowRemoval::kNow : FollowRemoval::kWait;
	}

	// An alias RefillsOf gave as kGiveUp: never asked for that copy again (left as it is until another copy is placed).
	inline void GiveUp(std::vector<AliasForce>& a_forced, std::uint32_t a_alias)
	{
		if (const auto it = std::ranges::find(a_forced, a_alias, &AliasForce::alias); it != a_forced.end()) {
			it->state = ForceState::kGivenUp;
		}
	}

	// For the log.
	inline std::string_view WhyText(RefillWhy a_why)
	{
		switch (a_why) {
		case RefillWhy::kNew:
			return "placed now";
		case RefillWhy::kNotTaken:
			return "the last ask was not taken by the script VM";
		case RefillWhy::kNotHeld:
			return "the last ask was taken, but the alias does not hold it when read back";
		case RefillWhy::kLost:
			return "the alias held it and holds something else since";
		default:
			return "given up on";
		}
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
		bool        oursHollow = false;     // ... one with none of its actors (a save's of a run we followed: its copies were not saved)
		bool        near = false;           // our player is near the run: a copy of its actors placed, or its trigger close
		bool        namesActors = false;    // the run names actors of the owner's (spawn ids): it starts here only with one of them
		std::size_t placed = 0;             // copies it names that are placed here now
		std::size_t waiting = 0;            // copies it names that are not placed yet but may be
	};

	enum class Verdict : std::uint8_t
	{
		kWait,
		kStart,
		kDrop,
		kStopOurs,  // our game runs a hollow run of the quest: stopped, so theirs starts next time
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
			std::optional<Clock::time_point>   ended;    // a friend's: when its owner's run ended (RUN_OVER) or the owner left
			std::vector<AliasForce>            forced;   // a friend's followed: its aliases forced onto our copies (Refills, NoteForced)
			bool                               resend = false;  // ours: to be sent again (someone joined or reloaded)
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
			if (!run || run->quest != a_quest || a_stage == RUN_OVER || std::ranges::contains(run->done, a_stage)) {
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
			if (a_stage == RUN_OVER) {
				return out;  // the end marker's value is no stage of a run's (a quest's own 65535, had one such, would read as its end)
			}
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
				if (a_facts.oursGivingWay) {
					return { Verdict::kWait, "our own run of it is giving way" };
				}
				if (!a_facts.oursHollow) {
					return { Verdict::kDrop, "our game runs it as its own" };
				}
				// Ours has none of its actors (a save's of a run we followed): it gives way once our player is where theirs is.
				return Near(a_facts) ? Judgement{ Verdict::kStopOurs, "our game's run of it has none of its actors" } : Judgement{ Verdict::kWait, "our player is not near it" };
			}
			if (!Near(a_facts)) {
				return { Verdict::kWait, a_facts.namesActors && a_facts.near ? "none of the owner's actors is placed here yet" : "our player is not near it" };
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
		// set here (each once). a_doneHere: the stages done here already, by our quest's own start: taken as done under
		// the run, never reported back (the owner's game could take another branch of the quest from one).
		std::vector<std::uint16_t> Following(RunKey a_key, std::span<const std::uint16_t> a_doneHere = {})
		{
			const auto run = Find(a_key);
			if (!run || run->state != State::kStarting) {
				return {};
			}
			run->state = State::kFollowing;
			AddDone(*run, a_doneHere);
			return std::exchange(run->toSet, {});
		}

		// The owner's run ended (RUN_OVER under it): a friend's we follow or are starting is to end here RUN_END_WAIT from
		// now (true); one only heard is dropped, never started. False for ours, or one unknown.
		bool OwnerEnded(std::uint32_t a_quest, RunKey a_key, Clock::time_point a_now)
		{
			const auto run = Find(a_key);
			if (!run || run->quest != a_quest || run->state == State::kOurs) {
				return false;
			}
			if (run->state == State::kHeard) {
				Drop(a_key);
				return false;
			}
			if (!run->ended) {
				run->ended = a_now;
			}
			return true;
		}

		// A friend left the session, or their game loaded a save (its book is cleared by the load, so no RUN_OVER comes for
		// the runs it had; what it runs after the load is announced anew): their runs we follow end as their RUN_OVER
		// would end them; those heard are dropped.
		void OwnerLeft(std::uint32_t a_origin, Clock::time_point a_now)
		{
			std::vector<RunKey> heard;
			for (auto& run : runs) {
				if (run.key.origin != a_origin || run.state == State::kOurs) {
					continue;
				}
				if (run.state == State::kHeard) {
					heard.push_back(run.key);
				} else if (!run.ended) {
					run.ended = a_now;
				}
			}
			for (const auto key : heard) {
				Drop(key);
			}
		}

		// Friends' runs whose owner's ended RUN_END_WAIT ago: their quests to stop here if still running (then Drop). One
		// whose quest still has a friend's stage waiting to be set here (a_queued(quest): the owner's last stages, held
		// back by a combat hold) waits for it, up to RUN_END_LIMIT after the end: the stop would drop the stage, whose
		// fragment may be the quest's own clean-up.
		template <class QueuedFn>
		[[nodiscard]] std::vector<RunKey> ToEnd(Clock::time_point a_now, QueuedFn&& a_queued) const
		{
			std::vector<RunKey> out;
			for (const auto& run : runs) {
				if (!run.ended || a_now - *run.ended < RUN_END_WAIT) {
					continue;
				}
				if (a_now - *run.ended < RUN_END_LIMIT && a_queued(run.quest)) {
					continue;
				}
				out.push_back(run.key);
			}
			return out;
		}

		[[nodiscard]] std::vector<RunKey> ToEnd(Clock::time_point a_now) const
		{
			return ToEnd(a_now, [](std::uint32_t) { return false; });
		}

		// Someone joined, or a friend's game loaded a save: our runs are to be sent again, each once its actors are shared
		// (TakeResend).
		void ResendOurs()
		{
			for (auto& run : runs) {
				run.resend = run.resend || run.state == State::kOurs;
			}
		}

		// Whether our run is to be sent again now (a_ready: every live actor of it is shared): once.
		static bool TakeResend(Run& a_run, bool a_ready)
		{
			if (!a_run.resend || a_run.state != State::kOurs || !a_ready) {
				return false;
			}
			a_run.resend = false;
			return true;
		}

		// A followed run's aliases that name the owner's actors and do not hold our copy of the actor placed now: each
		// (alias, copy) to force in, then NoteForced with the outcome (Encounters::RefillsOf, for a run followed here).
		template <class CopyFn, class HereFn>
		static std::vector<Refill> Refills(Run& a_run, CopyFn&& a_copy, HereFn&& a_here, Clock::time_point a_now)
		{
			if (a_run.state != State::kFollowing) {
				return {};
			}
			return RefillsOf(a_run.fills, a_run.forced, std::forward<CopyFn>(a_copy), std::forward<HereFn>(a_here), a_now);
		}

		// The aliases read back holding their copies now (Encounters::ReadBack), before Refills.
		template <class HereFn>
		static std::vector<std::pair<std::uint32_t, std::uint32_t>> ReadBack(Run& a_run, HereFn&& a_here)
		{
			return Encounters::ReadBack(a_run.forced, std::forward<HereFn>(a_here));
		}

		static void NoteForced(Run& a_run, std::uint32_t a_alias, std::uint32_t a_copy, bool a_ok, Clock::time_point a_now)
		{
			Encounters::NoteForced(a_run.forced, a_alias, a_copy, a_ok, a_now);
		}

		static void GiveUp(Run& a_run, std::uint32_t a_alias)
		{
			Encounters::GiveUp(a_run.forced, a_alias);
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
		// Near enough to start: a run that names the owner's actors only with a copy of one placed here; one that names
		// none (a treasure hunt's corpse) by its trigger too.
		static bool Near(const HeardFacts& a_facts)
		{
			return a_facts.namesActors ? a_facts.placed > 0 : a_facts.near;
		}

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
