#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Menus a friend's quest stage opens here by itself, which QuestSync closes again. Kept free of the game so it can be
// tested.
//
// A friend's stage set here runs its script as it ran in the friend's game, and two things such scripts do open a menu
// in front of our player, which pauses the game, and with it the catch-up, until the player answers it:
//  - Making an NPC the companion (FollowersScript.SetCompanion: Getting a Clue's 520 makes Nick Valentine ours)
//    dismisses the companion we have, and DismissCompanion asks where to send them: OpenWorkshopSettlementMenuEx, the
//    settlement list (Workshop_CaravanMenu).
//  - Giving our player an item with the FeaturedItem keyword (AddItem: the Broadsider at The Last Voyage of the U.S.S.
//    Constitution's 900, Hole in the Wall's cure at its 800; the Deliverer, the courser chip and the bobbleheads are
//    others) shows it in the engine's featured-item inspection (ExamineMenu, with VignetteMenu over it), which only
//    Enter closes.
// Neither menu is one the player asked for, and a catch-up of a progressed host met several in a row. Closing them
// keeps what the stage did: the companion still changes (the dismissed one goes home, as on Escape in the list), and
// the item is already in the inventory.
//
// A menu is the stage's when it opens within AFTER_STAGE of a friend's stage being set here, and our player has done
// nothing that opens it themselves within PLAYER_QUIET of it opening, nor since: activated something (a workbench, a
// bobblehead, a power armor station), or had a menu of their own up (a conversation, where a companion is dismissed;
// workshop mode, where a supply line is set; the Pip-Boy, whose inspect is an ExamineMenu too). Those are left to them.
// The vignette is looked at only once the inspection under it is gone (closing the inspection may take it along).
namespace CatchUpMenus
{
	using Clock = std::chrono::steady_clock;

	enum class Menu : std::uint8_t
	{
		kCaravan,   // Workshop_CaravanMenu: where a dismissed companion goes
		kExamine,   // ExamineMenu: a featured item's inspection (a workbench's too, which our player opens themselves)
		kVignette,  // VignetteMenu: the frame over the inspection
		kCount
	};

	// The game's names for them (MenuOpenCloseEvent::menuName), in Menu's order.
	constexpr std::array<std::string_view, static_cast<std::size_t>(Menu::kCount)> NAMES{ "Workshop_CaravanMenu", "ExamineMenu", "VignetteMenu" };

	// A menu that opens this soon after a friend's stage was set here is the stage's. Its script runs a moment later, and
	// a game with many mods can be seconds behind on its scripts (SceneHold::STARTED_BY_FRIEND is the same); what the
	// player did, not the time, is what keeps their own menus theirs.
	inline constexpr auto AFTER_STAGE = std::chrono::seconds(30);
	// ... unless our player activated something or had a menu of their own up this soon before it opened, or since.
	inline constexpr auto PLAYER_QUIET = std::chrono::seconds(5);
	// A menu is closed once it has been up this long: fully open, and the player's own activation that opened it heard
	// (the activation's event may come after the menu's).
	inline constexpr auto SETTLE = std::chrono::milliseconds(500);
	// A close the menu didn't take within this long is not sent again: the menu is left to the player.
	inline constexpr auto GIVE_UP = std::chrono::seconds(3);

	enum class Action : std::uint8_t
	{
		kNone,
		kClose,   // send the menu its close (once per opening)
		kGaveUp,  // it was sent a close and is still up: left to the player (once per opening)
	};

	class Guard
	{
	public:
		// A friend's stage was set here.
		void StageSet(Clock::time_point a_now) { stageAt = a_now; }

		// Our player activated something, or has a menu of their own up (call each look while one is).
		void PlayerActed(Clock::time_point a_when)
		{
			if (!playerAt || a_when > *playerAt) {
				playerAt = a_when;
			}
		}

		// Whether a_menu is up now; what to do about it. Call every frame or so for each menu.
		Action Watch(Menu a_menu, bool a_open, Clock::time_point a_now)
		{
			auto& seen = menus[static_cast<std::size_t>(a_menu)];
			if (!a_open) {
				if (seen.phase == Phase::kClosing) {
					++closed;
				}
				seen = {};
				return Action::kNone;
			}
			if (seen.phase == Phase::kShut) {
				seen = { Phase::kOpen, a_now, {} };
				return Action::kNone;
			}
			if (seen.phase == Phase::kOpen && a_now - seen.openedAt >= SETTLE) {
				if (!Stages(seen.openedAt) || Players(seen.openedAt)) {
					seen.phase = Phase::kLeft;
					++left;
					return Action::kNone;
				}
				seen.phase = Phase::kClosing;
				seen.closeAt = a_now;
				++sent;
				return Action::kClose;
			}
			if (seen.phase == Phase::kClosing && a_now - seen.closeAt >= GIVE_UP) {
				seen.phase = Phase::kLeft;
				++gaveUp;
				return Action::kGaveUp;
			}
			return Action::kNone;
		}

		// When the menu now up was seen opening (none: it isn't up).
		[[nodiscard]] std::optional<Clock::time_point> OpenedAt(Menu a_menu) const
		{
			const auto& seen = menus[static_cast<std::size_t>(a_menu)];
			return seen.phase == Phase::kShut ? std::nullopt : std::optional{ seen.openedAt };
		}

		// When the last friend's stage was set here (none: not yet).
		[[nodiscard]] std::optional<Clock::time_point> LastStage() const { return stageAt; }

		std::uint32_t sent = 0;    // closes sent
		std::uint32_t closed = 0;  // ... that the menu took
		std::uint32_t gaveUp = 0;  // ... that it didn't
		std::uint32_t left = 0;    // menus left up: not a stage's, or the player's own

	private:
		enum class Phase : std::uint8_t
		{
			kShut,     // not up
			kOpen,     // up, not judged yet (SETTLE)
			kClosing,  // a stage's: its close sent
			kLeft,     // the player's, or a close not taken
		};

		struct Seen
		{
			Phase             phase = Phase::kShut;
			Clock::time_point openedAt{};
			Clock::time_point closeAt{};
		};

		// A friend's stage was set at most AFTER_STAGE before the menu opened (or in the same moment: the menu can open
		// within the frame that set it).
		[[nodiscard]] bool Stages(Clock::time_point a_openedAt) const
		{
			return stageAt && *stageAt <= a_openedAt + SETTLE && a_openedAt - *stageAt <= AFTER_STAGE;
		}

		// Our player did something that opens such a menu within PLAYER_QUIET before it opened, or since.
		[[nodiscard]] bool Players(Clock::time_point a_openedAt) const
		{
			return playerAt && *playerAt + PLAYER_QUIET >= a_openedAt;
		}

		std::array<Seen, static_cast<std::size_t>(Menu::kCount)> menus{};
		std::optional<Clock::time_point>                         stageAt;
		std::optional<Clock::time_point>                         playerAt;
	};
}
