#pragma once

// Friends on the compass: a colored shape per player in the same cell or worldspace, drawn into
// the HUD's compass widget (Scaleform), clamped to the edge when they're behind you.
namespace Compass
{
	// Works out where each friend is and updates the markers (on the UI thread). Main thread, every frame.
	void Frame();

	// What a player's marker looks like ("pink diamond"), for the player list.
	[[nodiscard]] std::string_view MarkerName(std::uint32_t a_playerId);

	[[nodiscard]] std::string Describe();
}
