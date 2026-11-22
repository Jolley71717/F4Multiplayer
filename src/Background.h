#pragma once

#include <cstdint>
#include <optional>

// bRunInBackground: while a session runs, or one is on its way, the game runs on while its window is not focused (its
// own bAlwaysActive). A player who alt-tabs is still seen moving by friends, with the NPCs their game runs, and a host
// or join started at the main menu goes on loading behind another window. With the developer channel on (the test
// rig) it is on all the while, so a script driving the menu or a load with the virtual pad never stalls when someone
// clicks elsewhere. Otherwise the game's own setting is back as it was: playing alone, alt-tabbing pauses the game as
// it always did, and the setting the game may write into its own ini is the player's. No game needed, so it is tested.
//
// The same switch holds the game's bShowTutorials off in a session (bHideTutorials, game/RealTime.h).
namespace Background
{
	// Why the game runs on unfocused now (with bRunInBackground on), the first that holds.
	enum class Reason : std::uint8_t
	{
		kNone,        // playing alone, nothing pending: the game's own setting
		kSession,     // hosting, or joined to someone's
		kUnderWay,    // a join set (an invite or join-by-address accepted, connecting or trying again), or bHost waiting for our world
		kDevChannel,  // the developer channel (the rig): the menu and loads are driven from outside the window
	};

	struct Now
	{
		bool session = false;
		bool underWay = false;
		bool devChannel = false;
	};

	[[nodiscard]] constexpr Reason RunOn(const Now& a_now)
	{
		if (a_now.session) {
			return Reason::kSession;
		}
		if (a_now.underWay) {
			return Reason::kUnderWay;
		}
		return a_now.devChannel ? Reason::kDevChannel : Reason::kNone;
	}

	[[nodiscard]] constexpr const char* Describe(Reason a_reason)
	{
		switch (a_reason) {
		case Reason::kSession:
			return "in a session";
		case Reason::kUnderWay:
			return "a host or join is under way";
		case Reason::kDevChannel:
			return "the developer channel is on";
		default:
			return "no session, none under way";
		}
	}

	class Switch
	{
	public:
		// a_forced: the value the game's setting has while forced (bAlwaysActive on, bShowTutorials off).
		Switch() = default;
		explicit Switch(bool a_forced) :
			forced(a_forced)
		{}

		// a_enabled: the player's switch (bRunInBackground, bHideTutorials); a_force: force the setting now (bAlwaysActive:
		// RunOn says why; the tutorials: hosting, or joined to someone's); a_current: the game's setting now.
		// The value to set it to, or nothing to leave it as it is.
		std::optional<bool> Update(bool a_enabled, bool a_force, bool a_current)
		{
			if (a_enabled && a_force) {
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
		std::optional<bool> original;  // the game's setting as it was first forced
	};
}
