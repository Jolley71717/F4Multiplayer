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
}
