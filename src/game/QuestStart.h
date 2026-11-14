#pragma once

#include "game/QuestStartRules.h"

#include <string>

// Starts one chosen quest in this game through the engine's own start (TESQuest::EnsureQuestStarted), not the console's
// `startquest`, which the engine refuses for every quest the story manager starts from an event: a radiant job, a
// settlement attack (QuestStartRules.h has what was read, and what the engine does). RadiantSync starts a friend's run
// with it, and with it the mirror of the host's settlement attack. Main thread only.
namespace QuestStart
{
	struct Result
	{
		bool        started = false;  // the quest is enabled now, its aliases filled by this start
		bool        wait = false;     // not started because a stop of it is still under way: worth asking again shortly
		std::string why;              // when not started: why, for the log
	};

	// Starts a_quest now, its aliases filled as its definitions say (the caller forces those the story manager's event
	// would fill). Never a quest that runs or is stopping, nor one the engine would start with an earlier run's fills.
	// Fails closed: when the engine can't fill an alias that must fill, it clears the rest itself and the quest stays
	// stopped; when the engine's function isn't the one that was read (another game version, another plugin's patch),
	// nothing is called at all.
	Result Start(RE::TESQuest* a_quest);

	// What Start would find now, without starting anything.
	[[nodiscard]] QuestStartRules::Verdict Judge(const RE::TESQuest* a_quest);

	// Whether the engine's start is the one that was read, so Start may start anything at all (looked at once; false on
	// another game version or another plugin's patch of it, where every Start is refused). Asked before anything is
	// stopped for a start (RadiantSync stops this game's own run for a friend's only when this holds).
	[[nodiscard]] bool Usable();

	// For `net status`: whether the engine's start is usable, and how many starts were asked, made and refused.
	[[nodiscard]] std::string Describe();
}
