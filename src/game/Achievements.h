#pragma once

#include "game/StageRules.h"

// Achievements. A stage set from a friend runs its script here as in their game, Game.AddAchievement included, and
// the game only awards one when no mods are loaded and the save was never modded. Each load logs whether this game
// can earn them and which Steam has; after stages from friends are set, newly unlocked ones are logged.
// The maintainer's rule for a friend's stage: a stage that happens live while both are in the session awards both
// players; one caught up for a joiner (done before they joined) awards nothing. The Papyrus native Game.AddAchievement
// is hooked for that (its function object's dispatch slot, found in the VM's "Game" script type once it is loaded; no
// code is patched, only that one object's vtable pointer is pointed at a copy): a call made from the stage fragment of
// a stage QuestSync set here caught up (StageSetHere, AchievementGate::Marks) is dropped, logged; every other call runs
// the game's own. Reads and the hook's install are main-thread only; the hooked call runs where the VM runs the native.
namespace Achievements
{
	// Checks at the next frame in the world; the catch-up marks are the save's now (none).
	void OnGameLoaded();

	// A stage from another player was set here, of a quest from one of the game's masters (the quest's form ID in its
	// file). a_live: it came live, or the player followed into it; its award is left alone. Not live, caught up from
	// the session's state: its fragment's award is dropped, and the one it reaches by setting another stage
	// (AchievementStages), for AchievementGate::MARK_FOR.
	void StageSetHere(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage, bool a_live);

	// A stage from another player was set: look for new achievements a moment later, once its script ran.
	void AfterStageSet(std::uint32_t a_quest, std::uint16_t a_stage);

	void Frame();

	std::string Describe();
}
