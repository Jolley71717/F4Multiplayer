#pragma once

#include "Protocol.h"
#include "game/ScriptNames.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <unordered_map>

// Which open and lock states a game shares, and which of the session's it takes. Only a door a player
// opens is shared as open or closed, and one an NPC our game runs opens (below). What a script or the game moves is
// left to them: an elevator (its doors, its car, the call panel), a vault's gear door, a load elevator, a door that
// opens by itself. Forcing those open in a friend's game opened the elevator onto an empty shaft, as its car was
// elsewhere (Vault 81, 2026-10-14). Locks are shared on everything that has one. Kept free of the game
// so the rules can be tested.
namespace DoorShare
{
	using RefState = Protocol::RefState;

	// DOOR record flags (TESObjectDOOR::flags).
	inline constexpr std::uint8_t DOOR_AUTOMATIC = 0x02;  // opens and closes by itself as someone comes near

	// A script on a door that doesn't make its open state a script's (docs/script-doors-survey.md, "Doors with a script
	// on them"). A friend's door is opened through the engine's open state, as a plain door's is: that raises no
	// OnActivate (nor a lock change), so what these scripts do when someone activates the door happens in the game of
	// whoever did, and only there. None of them opens or closes the door itself, blocks its activation, or answers
	// OnOpen or OnClose.
	struct QuietScript
	{
		std::string_view script;
		std::string_view why;
	};
	inline constexpr std::array QUIET_SCRIPTS{
		QuietScript{ "ChainedDoor", "a door chained shut from one side (53 doors): activating it only says it is chained; the chain, an activator, unlocks it, and the lock is shared" },
		QuietScript{ "DefaultRefOnActivate", "sets a quest stage when the door is activated (Parsons State, the Memory Den, Libertalia, Covenant, Nuka-World): the stage stays the activator's game's, or QuestSync's" },
		QuietScript{ "DefaultRefEnableLinkedRefOnActivate", "enables or disables its linked reference when activated (College Square, Faneuil Hall, Wilson Atomatoys): in the activator's game only" },
		QuietScript{ "DefaultActivateLinkOnUnlock", "activates its linked reference when unlocked (Boston Public Library): on the lock, which is shared already" },
		QuietScript{ "MS10CellDoorScript", "sets Trinity Tower's stages when the cell is unlocked: on the lock, which is shared already" },
		QuietScript{ "DefaultMusicActivateScript", "plays music when activated" },
		QuietScript{ "PlaySoundOnActivate", "plays a sound when activated" },
		QuietScript{ "DefaultBlockFollowerActivation", "keeps companions from being sent to it (on load)" },
	};

	// Doors with only such scripts that are still not shared: the story's (docs/script-doors-survey.md, NEVER).
	struct FileForm
	{
		std::string_view file;
		std::uint32_t    id;  // in that file, no load-order byte
		std::string_view what;
	};
	inline constexpr std::array NEVER{
		FileForm{ "Fallout4.esm", 0x0009D568, "Mass Fusion's security area door (DN084: the Brotherhood's and the Institute's story)" },
	};

	// What the game knows of a reference that has an open state.
	struct Facts
	{
		bool          door = false;       // its base is a door, not an activator, a container or a light
		std::uint8_t  doorFlags = 0;      // the door's record flags
		std::uint32_t scripts = 0;        // the scripts bound to it (from its base, or of its own; a plain ObjectReference object is no script)
		std::uint32_t quietScripts = 0;   // ... of them, ones on QUIET_SCRIPTS
		bool          listedOut = false;  // it is in NEVER
	};

	// Whether a script's name is on QUIET_SCRIPTS.
	constexpr bool IsQuiet(std::string_view a_script)
	{
		return ScriptNames::Find(QUIET_SCRIPTS, a_script).has_value();
	}

	// Whether a reference's open state is a player's to share. An activator's open state is always a
	// script's (an elevator, its car, a gear door, a two-state switch: the security gates among those, run by the
	// game's standard two-state script, are shared through that script instead, SharedGates); so is a door's
	// with a script on it other than the quiet ones, and an automatic door's is the game's.
	constexpr bool SharesOpen(const Facts& a_facts)
	{
		return a_facts.door && (a_facts.doorFlags & DOOR_AUTOMATIC) == 0 && a_facts.scripts == a_facts.quietScripts && !a_facts.listedOut;
	}

	// An NPC opening a door, as our game sees it (TESActivateEvent names the NPC). An NPC another game runs is a copy
	// here, its AI off: it walks through its owner's doors without opening ours, and in a friend's game ours walk through
	// theirs, shut. So a door an NPC our game runs opens is watched as one our player opened, and its state goes to the
	// others. Their copy of the NPC opens no doors, so nothing there fights it; an NPC their game runs that uses the same
	// door reports it the same way, the latest word standing, as with two players. Only plain doors (SharesOpen; never an
	// automatic one, which each game opens for whoever comes near) and no load door (the NPC goes through it to another
	// cell: nothing to see). Only near our player: a friend takes a live report of a door only from a player near it.
	struct NpcOpen
	{
		bool  runsHere = false;    // our game runs the NPC (NpcSync), and it isn't a friend's stand-in nor our player
		bool  sharesOpen = false;  // the door's open state is shared (SharesOpen)
		bool  loadDoor = false;    // it leads to another cell
		float playerDistance = 0;  // from our player to the door
	};

	// A friend refuses a live report from a player further than 1500 from the thing (WorldSync's ACTIVATION_REACH); a
	// little less allows for where the friend last saw us.
	inline constexpr float NPC_REACH = 1200.0f;

	// How long a door an NPC opened is watched: it walks through, and the door closes behind it in moments.
	inline constexpr auto NPC_WATCH = std::chrono::seconds(30);

	constexpr bool WatchesNpcOpen(const NpcOpen& a_open)
	{
		return a_open.runsHere && a_open.sharesOpen && !a_open.loadDoor && a_open.playerDistance <= NPC_REACH;
	}

	// The part of a reference's state that is shared: the lock always, the open state if it is a player's.
	constexpr RefState Shared(RefState a_state, bool a_sharesOpen)
	{
		if (!a_sharesOpen) {
			a_state.open = RefState::kUnknown;
		}
		return a_state;
	}

	// Whether a state says anything to share or apply.
	constexpr bool Says(const RefState& a_state)
	{
		return a_state.open != RefState::kUnknown || a_state.locked != RefState::kUnknown;
	}

	// What the session last said of each reference, as far as this game knows: what it reported itself
	// and what it took from the others. A world state repeats every door the session knows each time it
	// is sent, which is more than once: a game that misses a container change asks again every minute and
	// gets the whole world state each time. Taken again each time, a door an NPC or a script had since
	// closed was forced back open, an elevator's doors with it, once a minute (140 changes in an evening
	// of play). A live report from another player is always news; a world state only where it differs
	// from what we know. Cleared when a save loads: its doors are as the save had them, and the next
	// world state is applied in full.
	class Book
	{
	public:
		// We reported this state.
		void Sent(const RefState& a_state) { known[a_state.refId] = a_state; }

		// Whether a state from the server should be applied: always for a live report (one that names
		// its reporter), and for a world state when it isn't what we know already. Either way it is what
		// the session says now.
		bool Take(const RefState& a_state)
		{
			auto [it, added] = known.try_emplace(a_state.refId, a_state);
			const bool news = a_state.playerId != 0 || added || !it->second.SameState(a_state);
			it->second = a_state;
			return news;
		}

		void Clear() { known.clear(); }

		[[nodiscard]] std::size_t Size() const { return known.size(); }

	private:
		std::unordered_map<std::uint32_t, RefState> known;
	};
}
