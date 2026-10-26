#pragma once

#include "game/RemoteEdits.h"

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <vector>

// A menu that stops our game (the Pip-Boy, a container, the pause menu: GameStop) stops the NPCs our game runs too, and
// their copies in friends' games with them. A short stop is let be: friends' copies run their own AI after
// NpcPlace::LOST_AFTER, and ours report again when the menu closes. Once our game has been stopped for AFTER, NpcSync's
// Scan offers to run nothing new, and lets go of the NPCs it runs that a friend's game can take on: a friend in the same
// cell or world, near it, and not stopped themselves. Their game claims it as nobody's (the server grants that), runs
// it on, and ours copies it from theirs once the menu closes. Released through the hand-off's own path (NpcSync's
// handedOffAt), so it is not claimed back for HANDOFF_HOLD, nor let go again for HANDOFF_RETRY. Kept free of the game
// so it can be tested.
namespace PauseHandOff
{
	using RemoteEdits::Spot;
	using Duration = std::chrono::steady_clock::duration;

	// Stopped this long, our game hands off. Longer than NpcPlace::LOST_AFTER: a quick look at the Pip-Boy, a stimpak,
	// moves nothing.
	constexpr Duration AFTER = std::chrono::seconds{ 5 };
	// A friend this near an NPC of ours can take it on (NpcSync's HANDOFF_FROM: as far as our game's AI is any use).
	constexpr float FRIEND_RANGE = 6000.0f;

	// Whether Scan offers to run NPCs (and frames) nobody runs: not once our game has been stopped for AFTER, so a
	// stopped game takes on nothing it would only stand still.
	constexpr bool Offers(Duration a_stoppedFor)
	{
		return a_stoppedFor < AFTER;
	}

	// Whether a friend's game is stopped too, by the kind of menu they are in (their status, Protocol::MenuKind):
	// nothing is handed to them. A conversation, a workbench or the looks menu leave their world running.
	constexpr bool Stopped(std::uint8_t a_menu)
	{
		using namespace Protocol::MenuKind;
		return a_menu == kPipboy || a_menu == kPaused || a_menu == kLooting || a_menu == kTerminal;
	}

	// A friend, as their latest state has them.
	struct Friend
	{
		Spot          at;
		std::uint8_t  menu = Protocol::MenuKind::kNone;  // the menu they are in (their status)
	};

	// An NPC our game runs, as Scan finds it (loaded and alive).
	struct Npc
	{
		std::uint32_t id = 0;            // its session id (a spawn of ours: Protocol::IsSpawnRef)
		Spot          at;
		bool          companion = false;    // our own companion: it follows us
		bool          talkingToUs = false;  // the NPC in our player's conversation
		bool          holdsUs = false;      // holding our player in a conversation or scene (Dialogue::Holds)
		bool          commanded = false;    // a settler of the workbench our player commands at (SettlerCommand)
		bool          fought = false;       // our player hit or talked to it lately (NpcHold::FoughtLately)
		bool          handedOffLately = false;  // let go within HANDOFF_RETRY
	};

	enum class Why
	{
		kRunning,     // our game is not stopped, or not for AFTER
		kOurs,        // our companion, in our conversation or scene, or a settler we command: it stays with us
		kFought,      // our player fought it lately: our killing blow must land where it runs (NpcHold)
		kSpawn,       // our game made it: no other game has it to run
		kLately,      // let go lately: not again so soon
		kNoFriend,    // no friend near it in its space, or none whose game runs
		kRelease,     // let go: a friend's game takes it on
	};

	// What Scan does with an NPC our game runs, our game stopped for a_stoppedFor. (A dying one never comes here: it is
	// no candidate, and NpcHold keeps it until its death is reported. Nor does a power armor frame: those are no actors.)
	inline Why Decide(Duration a_stoppedFor, const Npc& a_npc, const std::vector<Friend>& a_friends)
	{
		if (Offers(a_stoppedFor)) {
			return Why::kRunning;
		}
		if (a_npc.companion || a_npc.talkingToUs || a_npc.holdsUs || a_npc.commanded) {
			return Why::kOurs;
		}
		if (a_npc.fought) {
			return Why::kFought;
		}
		if (Protocol::IsSpawnRef(a_npc.id)) {
			return Why::kSpawn;
		}
		if (a_npc.handedOffLately) {
			return Why::kLately;
		}
		for (const auto& f : a_friends) {
			if (!Stopped(f.menu) && RemoteEdits::Within(RemoteEdits::Distance(a_npc.at, f.at), FRIEND_RANGE)) {
				return Why::kRelease;
			}
		}
		return Why::kNoFriend;
	}
}
