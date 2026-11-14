#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "game/TextureFlush.h"

// Starting one chosen quest in this game, whatever its kind (game/QuestStart.cpp calls the engine). Kept free of the
// game so it can be tested.
//
// Why not the console's `startquest` (1.11.240, read from Fallout4.exe): a quest the story manager starts from an event
// (TESQuest::eventID at +F8 is not 0xFFFFFFFF: every settlement attack, every radiant job) is refused at each of the
// engine's three front doors, on purpose. The console's handler (+5C6DD0, ID 2204542) compares eventID with 0xFFFFFFFF at
// +5C6E29 and returns without a word when it differs; Papyrus's Quest.Start (+11864E0, the check at +11864EB) and
// Quest.SetStage on a stopped quest (+11862C0, +1186305) do the same and log "attempting to start event scoped quest
// outside of story manager". So a `startquest` of a friend's radiant, or of the mirror of the host's settlement attack,
// did nothing at all.
//
// Behind the doors is TESQuest::EnsureQuestStarted(bool& a_starting, bool a_now) (+66B440, ID 2207742), which has no such
// check. For a quest that isn't enabled, asked to start now: unless the quest runs once and has run (flag 0x100 and
// alreadyRun at +2B6), it fills the aliases (TESQuest::FillAliases with no event, the call at +0x4F) and, when every
// alias that must fill did, hands the quest to the story teller (BGSStoryTeller::BeginStartUpQuest, +0x62: ResetQuest,
// then StartUp, which sets the enabled flag before it returns and queues the promotion of the quest's references). When
// an alias that must fill can't, FillAliases clears what it filled and the function returns false: the quest is left
// stopped, as it was. For a quest that is enabled it starts nothing and returns true.
//
// FillAliases without an event: an alias filled from the event (a reference alias's fill type 3, a location alias's
// event data) finds nothing, since both lookups test the event for null first (+6766BC, +670F5E). So such an alias must
// be forced by the caller (RadiantSync's Force) or be optional. The quest's Papyrus scripts get no OnStoryScript event
// either: what they would have read from it (an attack's strength) is not set in this game.
//
// One trap: FillAliases returns true at once, filling nothing, while the quest still holds fills (totalRefsAliased at
// +2B0, the aliased locations at +88, the start event's data at +2C8). A quest stopped a moment ago holds them until
// the story teller has shut it down (TESQuest::ShutDown, then ClearAliases), so a start in that window would run with
// the old run's fills. Judge refuses it; the caller may ask again once the stop is over (Waits).
namespace QuestStartRules
{
	// QUEST_DATA::flags.
	inline constexpr std::uint16_t ENABLED = 0x0001;     // running, or starting up
	inline constexpr std::uint16_t STOP_START = 0x0080;  // a stop or a start of it is under way
	inline constexpr std::uint16_t RUN_ONCE = 0x0100;

	// 1.11.240, by Address Library ID, and where EnsureQuestStarted calls the other two.
	inline constexpr std::uint64_t  ENSURE_STARTED = 2207742;  // TESQuest::EnsureQuestStarted (+66B440)
	inline constexpr std::uint64_t  FILL_ALIASES = 2207786;    // TESQuest::FillAliases (+66EE80)
	inline constexpr std::uint64_t  BEGIN_START_UP = 2214209;  // BGSStoryTeller::BeginStartUpQuest (+7FF280)
	inline constexpr std::uintptr_t FILL_CALL = 0x4F;
	inline constexpr std::uintptr_t BEGIN_CALL = 0x62;
	// How EnsureQuestStarted begins: its frame, then `mov byte ptr [rdx],0` (the caller's flag cleared first).
	inline constexpr std::array<std::uint8_t, 13> BEGINS{ 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xC6, 0x02, 0x00 };

	// Whether the engine's function is the one that was read: it begins as read and calls FillAliases and
	// BeginStartUpQuest where 1.11.240 does (a_at: where each call is). Otherwise nothing is started through it.
	constexpr bool AsRead(std::span<const std::uint8_t> a_begins, std::span<const std::uint8_t, 5> a_fillCall, std::uintptr_t a_fillAt, std::uintptr_t a_fill,
		std::span<const std::uint8_t, 5> a_beginCall, std::uintptr_t a_beginAt, std::uintptr_t a_begin)
	{
		if (a_begins.size() != BEGINS.size()) {
			return false;
		}
		for (std::size_t i = 0; i < BEGINS.size(); ++i) {
			if (a_begins[i] != BEGINS[i]) {
				return false;
			}
		}
		return TextureFlush::CallTarget(a_fillCall, a_fillAt) == a_fill && TextureFlush::CallTarget(a_beginCall, a_beginAt) == a_begin;
	}

	// What the quest is before a start is asked of the engine.
	struct Facts
	{
		std::uint16_t flags = 0;          // QUEST_DATA::flags
		bool          promoting = false;  // promoteTask (+2D0) set: a start's references are still being promoted
		std::uint32_t refsAliased = 0;    // totalRefsAliased (+2B0)
		std::uint32_t locsAliased = 0;    // aliasedLocMap's size (+88)
		bool          eventData = false;  // startEventData (+2C8) set
		bool          alreadyRun = false; // +2B6
	};

	enum class Verdict : std::uint8_t
	{
		kStart,     // stopped, with nothing of an earlier run left: the engine is asked
		kRunning,   // enabled: it runs, or is starting up
		kStopping,  // a stop of it is under way (or a start that didn't get as far as enabling it)
		kFilled,    // stopped, but an earlier run's fills are still on it: the engine would start it with those
		kRanOnce,   // it runs once and has run
	};

	// Whether the engine is asked to start it. Never for a quest that runs or is stopping; never when the engine would
	// keep an earlier run's fills.
	constexpr Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.flags & ENABLED) {
			return Verdict::kRunning;
		}
		if ((a_facts.flags & STOP_START) || a_facts.promoting) {
			return Verdict::kStopping;
		}
		if (a_facts.refsAliased != 0 || a_facts.locsAliased != 0 || a_facts.eventData) {
			return Verdict::kFilled;
		}
		if ((a_facts.flags & RUN_ONCE) && a_facts.alreadyRun) {
			return Verdict::kRanOnce;
		}
		return Verdict::kStart;
	}

	// A stop of it is still under way: the same start may be asked again a moment later.
	constexpr bool Waits(Verdict a_verdict)
	{
		return a_verdict == Verdict::kStopping || a_verdict == Verdict::kFilled;
	}

	constexpr std::string_view Why(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kStart:
			return "it can be started";
		case Verdict::kRunning:
			return "it is running already";
		case Verdict::kStopping:
			return "a stop of it is still under way";
		case Verdict::kFilled:
			return "it is stopped, but its last run's aliases aren't cleared yet";
		case Verdict::kRanOnce:
			return "it runs once and has run";
		}
		return "unknown";
	}

	enum class Outcome : std::uint8_t
	{
		kStarted,     // the engine filled its aliases and started it: it is enabled now
		kNoFill,      // the engine couldn't fill an alias that must fill; it cleared what it had filled, the quest is stopped as before
		kNotEnabled,  // the engine said it started, but the quest isn't enabled: not what was read, treated as a failure
	};

	// What the engine's call did: a_said, what EnsureQuestStarted returned; a_flags, the quest's flags after.
	constexpr Outcome After(bool a_said, std::uint16_t a_flags)
	{
		if (!a_said) {
			return Outcome::kNoFill;
		}
		return (a_flags & ENABLED) ? Outcome::kStarted : Outcome::kNotEnabled;
	}

	constexpr std::string_view Why(Outcome a_outcome)
	{
		switch (a_outcome) {
		case Outcome::kStarted:
			return "started";
		case Outcome::kNoFill:
			return "the game couldn't fill an alias it needs (it cleared the rest; the quest stays stopped)";
		case Outcome::kNotEnabled:
			return "the game said it started, but it isn't running";
		}
		return "unknown";
	}

	// ---- A friend's stage of a quest the story manager starts from an event -------------------------------------------
	//
	// A friend's stage of a stopped quest is set through the console's `setstage`, which starts the quest as it sets the
	// stage, unless the quest is event-scoped (its eventID names a registered event): then the engine refuses it, and the
	// quest never starts here (FFDiamondCity01, DialogueDiamondCityEntrance, FFGoodneighbor08). Such a quest is started
	// first through EnsureQuestStarted (QuestStart::Start), with this game's own fills, when nothing it must have comes
	// from the event: an alias that must fill from the event finds nothing without one, so the start would fail anyway
	// (closed: the engine clears what it filled), and is not asked for. An optional one is left empty. Then the stage is
	// set on the running quest at the next poll, after the start-up stages the start queued, which the console passes; a
	// start-up stage the start ran itself is not set again (SetAfterStart). Not the run-once handlers of NOT_FROM_FRIEND.

	// A reference or location alias's fill type 3: from the event that starts the quest (RadiantRules::RefFill, LocFill).
	inline constexpr std::uint16_t FILL_FROM_EVENT = 3;

	// An alias of the quest, as such a start sees it.
	struct AliasFill
	{
		bool fromEvent = false;  // filled from the story manager's event (fill type 3)
		bool optional = false;   // may stay empty (BGSBaseAlias flag 0x2)
	};

	// Whether a friend's stage of this quest starts it here through the engine's start first: it isn't enabled, the story
	// manager starts it from an event (a_storyEvent: Encounters::IsStoryEvent of its eventID; without one the console's
	// setstage starts it, as always), and no alias it must fill comes from that event.
	constexpr bool StartsFromFriend(const Facts& a_facts, bool a_storyEvent, std::span<const AliasFill> a_aliases)
	{
		if ((a_facts.flags & ENABLED) || !a_storyEvent) {
			return false;
		}
		for (const auto& alias : a_aliases) {
			if (alias.fromEvent && !alias.optional) {
				return false;
			}
		}
		return true;
	}

	// Once such a start took: whether the friend's stage is still to be set on the running quest. Not when it is one of the
	// quest's start-up stages (the start runs those itself: set again, its script could run twice) or reads done already.
	constexpr bool SetAfterStart(bool a_startUpStage, bool a_doneNow)
	{
		return !a_startUpStage && !a_doneNow;
	}

	// What a friend's stage of such a quest does at this poll.
	enum class Step : std::uint8_t
	{
		kStart,   // the quest is stopped and StartsFromFriend: start it through the engine first
		kSet,     // set the stage as any (the quest runs, or isn't one to start this way)
		kGiveUp,  // a start for this stage took at an earlier poll and the quest has stopped since (a repeatable quest whose
		          // scene ended it): never started again for it, or the stage would start it, wait and start it again for good
	};

	// a_startable: StartsFromFriend now; a_startedForIt: an earlier poll's start took for this stage, which waits to be set;
	// a_running: the quest runs now.
	constexpr Step NextStep(bool a_startable, bool a_startedForIt, bool a_running)
	{
		if (a_startedForIt) {
			return a_running ? Step::kSet : Step::kGiveUp;
		}
		return a_startable && !a_running ? Step::kStart : Step::kSet;
	}

	// Event-scoped quests a friend's stage never starts here, though nothing they need comes from the event: run-once
	// handlers (flag 0x100) whose start-up stage acts on this game's world as the story manager's conditions found it (the
	// player entering a place, this game's own quest stages and endings) and then stops the quest. Started from a friend's
	// stage, the handler would run where those conditions don't hold (the player elsewhere, another ending), do its work
	// there or decide "nothing to do", and burn its one run: this game's own event could never start it again. Their stages
	// are set as before (the game refuses the console's setstage of a stopped one: "didn't start here"), and each game's own
	// event runs its own. Read from the start-up stages' fragments (Fallout4.esm, DLCCoast.esm, DLCRobot.esm, 1.11.240).
	// Sorted by (file, quest), for NotFromFriend. Hand-made.
	struct NotStarted
	{
		std::uint8_t  file;   // StageRules::File: 0 Fallout4.esm, 1 DLCCoast.esm, 2 DLCRobot.esm
		std::uint32_t quest;  // the form ID within its file
	};
	inline constexpr NotStarted NOT_FROM_FRIEND[] = {
		{ 0, 0x0423D6 },  // DN046_ChangeLocation: starts DN046 if it isn't running, stops itself
		{ 0, 0x0684D2 },  // DN123_ChangeLocation: the same for DN123
		{ 0, 0x069813 },  // DN080_ChangeLocation: a change-of-location handler (its 10 has no fragment)
		{ 0, 0x08651F },  // MinVsInstCleanUp: moves Enrico and Wallace, stops MinVsInst
		{ 0, 0x11132E },  // DN070_ChangeLocation: starts DN070 if it isn't running, stops itself
		{ 0, 0x112D0F },  // DN165_ChangeLocation: the same for DN165
		{ 0, 0x143AFF },  // BoSM01PostquestCL: stops BoSM01, starts BoSM01Postquest, stops itself
		{ 0, 0x162A3E },  // BoS303LL: sets BoS303 200/210/220 by this game's BoS201, BoS301 and BoS302
		{ 0, 0x165A54 },  // Inst305MeetingUpdate: Inst305's MeetingUpdate, stops itself
		{ 0, 0x17CA39 },  // Inst307ChangeLocation: sets Inst307 35 (can move the player), stops itself
		{ 0, 0x17D863 },  // FFDiamondCityEndGame: Diamond City's look by this game's MQWon* globals, stops itself
		{ 0, 0x215CC7 },  // MQ302Post: moves Maxson, Preston or Desdemona by this game's MQ302Faction
		{ 0, 0x224D29 },  // MS14Misc: sets its own 20 by this game's MS14 10
		{ 1, 0x00215E },  // DLC03MQ01ChangeLocationHandler: Far Harbor's radio message, stops itself
		{ 2, 0x004CD5 },  // DLC01Lair_ChangeLocation: starts DLC01Lair if it isn't running, stops itself
	};

	// Whether a friend's stage of this quest (its file, its form ID within the file) is kept from starting it here.
	constexpr bool NotFromFriend(std::uint8_t a_file, std::uint32_t a_quest)
	{
		for (const auto& row : NOT_FROM_FRIEND) {
			if (row.file == a_file && row.quest == a_quest) {
				return true;
			}
		}
		return false;
	}
}
