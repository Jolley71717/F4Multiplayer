#pragma once

// Achievements, for the log: a stage set from a friend runs its script here as in their game,
// Game.AddAchievement included, but the game only awards one when no mods are loaded and the save
// was never modded. Each load logs whether this game can earn them and which Steam has; after
// stages from friends are set, newly unlocked ones are logged. Only reads. Main thread only.
namespace Achievements
{
	// Checks at the next frame in the world.
	void OnGameLoaded();

	// A stage from another player was set: look for new achievements a moment later, once its script ran.
	void AfterStageSet(std::uint32_t a_quest, std::uint16_t a_stage);

	void Frame();

	std::string Describe();
}
