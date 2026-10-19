#pragma once

#include <optional>

// bRunInBackground: while a session runs, the game runs on while its window is not focused (its own bAlwaysActive),
// so a player who alt-tabs is still seen moving by friends, with the NPCs their game runs. Out of a session the game's
// own setting is back as it was: playing alone, alt-tabbing pauses the game as it always did, and the setting the
// game may write into its own ini is the player's. No game needed, so it is tested.
//
// The same switch holds the game's bShowTutorials off in a session (bHideTutorials, game/RealTime.h).
namespace Background
{
	class Switch
	{
	public:
		// a_forced: the value the game's setting has during a session (bAlwaysActive on, bShowTutorials off).
		Switch() = default;
		explicit Switch(bool a_forced) :
			forced(a_forced)
		{}

		// a_enabled: bRunInBackground; a_session: hosting, or joined to someone's; a_current: the game's setting now.
		// The value to set it to, or nothing to leave it as it is.
		std::optional<bool> Update(bool a_enabled, bool a_session, bool a_current)
		{
			if (a_enabled && a_session) {
				if (!original) {
					original = a_current;
				}
				return a_current == forced ? std::nullopt : std::optional{ forced };
			}
			if (!original) {
				return std::nullopt;
			}
			const bool back = *original;
			original.reset();
			return a_current != back ? std::optional{ back } : std::nullopt;
		}

	private:
		bool                forced = true;
		std::optional<bool> original;  // the game's setting as the session began
	};
}
