#pragma once

#include "game/QuestStartRules.h"

#include <string>

// Starts one chosen quest in this game through the engine's own start (TESQuest::EnsureQuestStarted), not the console's
// `startquest`, which the engine refuses for every quest the story manager starts from an event: a radiant job, a
// settlement attack, Goodneighbor's gate guards (QuestStartRules.h has what was read, and what the engine does).
// RadiantSync starts a friend's run and the mirror of the host's settlement attack with it, and QuestSync an
// event-scoped quest a friend's stage reaches (QuestStartRules::StartsFromFriend). Main thread only.
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

	// For `net status`: whether the engine's start is usable, and how many starts were asked, made and refused.
	[[nodiscard]] std::string Describe();
}
