#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Friends on the compass: a colored shape per player in the same cell or worldspace, drawn into
// the HUD's compass widget (Scaleform), clamped to the edge when they're behind you; and each player's ping
// (Party, PingRules), a ring in their color.
namespace Compass
{
	// Works out where each friend is; the markers (and the HUD messages) are written from inside the HUD's
	// own update, which this hooks the first time the HUD is up. Every frame.
	void Frame();

	// What a player's marker looks like ("pink diamond"), for the player list.
	[[nodiscard]] std::string_view MarkerName(std::uint32_t a_playerId);

	[[nodiscard]] std::string Describe();

	// Whether a menu's updates have stopped while the HUD's go on: the power armor HUD, let go when the player gets out of
	// the armor, leaves the count of markers it showed as it was, and the HUD would be touched every frame for them. Told
	// each frame how many updates each has had; stopped after FRAMES frames in a row with the HUD's moving and its not
	// (a frame it just missed is not one). Free of the game, so it is tested.
	struct Stopped
	{
		static constexpr std::uint32_t FRAMES = 3;

		std::uint32_t hudSeen = 0;
		std::uint32_t menuSeen = 0;
		std::uint32_t still = 0;

		bool Frame(std::uint32_t a_hudAdvances, std::uint32_t a_menuAdvances)
		{
			if (a_menuAdvances != menuSeen) {
				still = 0;
			} else if (a_hudAdvances != hudSeen) {
				++still;
			}
			hudSeen = a_hudAdvances;
			menuSeen = a_menuAdvances;
			return still >= FRAMES;
		}
	};
}
