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
// Neither menu is one the player asked for, and a catch-up of a progressed host met several in a row.
//
// Each is closed the way it takes (Close). The inspection takes a hide message: the item is already in the inventory,
// and the hide takes the vignette along, with nothing left on (the rig, at 0.11.0). The settlement list does not: the
// script that opened it waits on it (OpenWorkshopSettlementMenuEx is latent), and only the list's own answer, a pick or
// its cancel, ends the wait. A hide closed the list and left that script suspended for good, in the save too: the new
// companion never joined and the old one never left. Its cancel (Tab) answers the wait with no settlement: the swap
// finishes, the new companion follows and the dismissed one goes home. So the list is sent its cancel, never a hide
// (MenuMessages refuses one).
//
// A menu is the stage's when it opens within AFTER_STAGE of a friend's stage being set here, and our player has done
// nothing that opens it themselves within PlayerQuiet of it opening, nor since: activated something (a workbench, a
// bobblehead, a power armor station), or had a menu of their own up (a conversation, where a companion is dismissed;
// workshop mode, where a supply line is set; the Pip-Boy, whose inspect is an ExamineMenu too). Those are left to them.
// The vignette is judged only once the inspection under it is gone (closing the inspection takes it along), and one
// over an inspection left to the player is left to them too.
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

	// How a menu is closed.
	enum class Close : std::uint8_t
	{
		kHide,    // a hide message to the menu (UIMessageQueue)
		kCancel,  // the menu's own cancel: a Tab press handed to it alone (MenuInput), as if the player pressed it
	};

	// Each menu's, in Menu's order.
	constexpr std::array<Close, static_cast<std::size_t>(Menu::kCount)> CLOSE{ Close::kCancel, Close::kHide, Close::kHide };

	[[nodiscard]] constexpr Close HowClosed(Menu a_menu) { return CLOSE[static_cast<std::size_t>(a_menu)]; }

	// A menu that opens this soon after a friend's stage was set here is the stage's. Its script runs a moment later, and
	// a game with many mods can be seconds behind on its scripts (SceneHold::STARTED_BY_FRIEND is the same); what the
	// player did, not the time, is what keeps their own menus theirs.
	inline constexpr auto AFTER_STAGE = std::chrono::seconds(30);
	// ... unless our player activated something or had a menu of their own up this soon before it opened, or since.
	// The inspection is the engine's and opens on the spot (a pickup, a workbench). The settlement list is a script's
	// even when the player opens it (their own dismissal in a conversation, a supply line in workshop mode), and those
	// scripts lag as a stage's do, so it gets the stage's window: a list after the player's own conversation or workshop
	// is left to them on a slow game too. Leaving one of the stage's costs the player a Tab; closing one of theirs sends
	// their companion home instead of where they meant.
	inline constexpr auto PLAYER_QUIET = std::chrono::seconds(5);
	inline constexpr auto PLAYER_QUIET_SCRIPTED = AFTER_STAGE;

	[[nodiscard]] constexpr Clock::duration PlayerQuiet(Menu a_menu)
	{
		return a_menu == Menu::kCaravan ? Clock::duration{ PLAYER_QUIET_SCRIPTED } : Clock::duration{ PLAYER_QUIET };
	}

	// A menu is closed once it has been up this long (a vignette: once the inspection under it has been gone this long):
	// fully open, and the player's own activation that opened it heard (the activation's event may come after the menu's).
	inline constexpr auto SETTLE = std::chrono::milliseconds(500);
	// A close the menu didn't take within this long is not sent again: the menu is left to the player.
	inline constexpr auto GIVE_UP = std::chrono::seconds(3);

	enum class Action : std::uint8_t
	{
		kNone,
		kClose,   // close the menu, as HowClosed says (once per opening)
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

		// Whether a_menu is up now; what to do about it. Call every frame or so for each menu, with the menu's own state
		// (the vignette's too while the inspection is up: it waits for it here).
		Action Watch(Menu a_menu, bool a_open, Clock::time_point a_now)
		{
			auto& seen = At(a_menu);
			if (!a_open) {
				if (seen.phase == Phase::kClosing) {
					++(HowClosed(a_menu) == Close::kCancel ? cancelled : hidden);
				}
				if (auto& vignette = At(Menu::kVignette); a_menu == Menu::kExamine && seen.phase != Phase::kShut && vignette.phase == Phase::kOpen) {
					vignette.readyAt = a_now;  // the inspection went: the vignette over it is judged SETTLE from now
				}
				seen = {};
				return Action::kNone;
			}
			if (seen.phase == Phase::kShut) {
				seen = { Phase::kOpen, a_now, a_now, {} };
			}
			if (a_menu == Menu::kVignette && seen.phase == Phase::kOpen) {
				const auto& examine = At(Menu::kExamine);
				if (examine.phase == Phase::kLeft) {
					Leave(seen);  // over an inspection left to the player: theirs too
					return Action::kNone;
				}
				if (examine.phase != Phase::kShut) {
					return Action::kNone;  // judged once the inspection is gone (closing it takes the vignette along)
				}
			}
			if (seen.phase == Phase::kOpen && a_now - seen.readyAt >= SETTLE) {
				if (!Stages(seen.openedAt) || Players(a_menu, seen.openedAt)) {
					Leave(seen);
					if (a_menu == Menu::kExamine) {
						LeaveVignette();
					}
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
				if (a_menu == Menu::kExamine) {
					LeaveVignette();
				}
				return Action::kGaveUp;
			}
			return Action::kNone;
		}

		// The close Watch asked for couldn't be sent (the menu took no input, a rule refused it): the menu is left to the
		// player, as one that didn't take its close.
		void NotSent(Menu a_menu)
		{
			auto& seen = At(a_menu);
			if (seen.phase != Phase::kClosing) {
				return;
			}
			seen.phase = Phase::kLeft;
			--sent;
			++gaveUp;
			if (a_menu == Menu::kExamine) {
				LeaveVignette();
			}
		}

		// When the menu now up was seen opening (none: it isn't up).
		[[nodiscard]] std::optional<Clock::time_point> OpenedAt(Menu a_menu) const
		{
			const auto& seen = menus[static_cast<std::size_t>(a_menu)];
			return seen.phase == Phase::kShut ? std::nullopt : std::optional{ seen.openedAt };
		}

		// When the last friend's stage was set here (none: not yet).
		[[nodiscard]] std::optional<Clock::time_point> LastStage() const { return stageAt; }

		// When our player last activated something or had a menu of their own up (none: not yet).
		[[nodiscard]] std::optional<Clock::time_point> LastPlayer() const { return playerAt; }

		std::uint32_t sent = 0;       // closes sent
		std::uint32_t cancelled = 0;  // ... cancels the menu went after (its own answer, as Tab gives it)
		std::uint32_t hidden = 0;     // ... hides the menu went after
		std::uint32_t gaveUp = 0;     // ... closes the menu didn't take, or that couldn't be sent: left to the player
		std::uint32_t left = 0;       // menus left up: not a stage's, or the player's own

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
			Clock::time_point openedAt{};  // seen opening
			Clock::time_point readyAt{};   // ... and judged SETTLE after this (a vignette: after the inspection went)
			Clock::time_point closeAt{};
		};

		Seen& At(Menu a_menu) { return menus[static_cast<std::size_t>(a_menu)]; }

		void Leave(Seen& a_seen)
		{
			a_seen.phase = Phase::kLeft;
			++left;
		}

		// The inspection is left to the player: the vignette over it, not yet judged, is theirs too.
		void LeaveVignette()
		{
			if (auto& vignette = At(Menu::kVignette); vignette.phase == Phase::kOpen) {
				Leave(vignette);
			}
		}

		// A friend's stage was set at most AFTER_STAGE before the menu opened (or in the same moment: the menu can open
		// within the frame that set it).
		[[nodiscard]] bool Stages(Clock::time_point a_openedAt) const
		{
			return stageAt && *stageAt <= a_openedAt + SETTLE && a_openedAt - *stageAt <= AFTER_STAGE;
		}

		// Our player did something that opens such a menu within its PlayerQuiet before it opened, or since.
		[[nodiscard]] bool Players(Menu a_menu, Clock::time_point a_openedAt) const
		{
			return playerAt && *playerAt + PlayerQuiet(a_menu) >= a_openedAt;
		}

		std::array<Seen, static_cast<std::size_t>(Menu::kCount)> menus{};
		std::optional<Clock::time_point>                         stageAt;
		std::optional<Clock::time_point>                         playerAt;
	};
}
