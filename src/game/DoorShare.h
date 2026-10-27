#pragma once

#include "Protocol.h"
#include "game/ScriptNames.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

// Which open and lock states a game shares, and which of the session's it takes. Only a plain door's open
// state is shared, whoever opens it: a player, an NPC our game runs, or a script that opens or unlocks a door that
// has no script of its own (every loaded one is read, below). What a script or the game moves is left to them: an
// elevator (its doors, its car, the call panel), a vault's gear door (ScriptDoors shares the stage that opens it), a
// load elevator, a door that opens by itself. Forcing those open in a friend's game opened the elevator onto an empty
// shaft, as its car was elsewhere (Vault 81, 2026-10-14). Locks are shared on everything that has one. Kept free of
// the game so the rules can be tested.
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
	inline constexpr float REPORT_REACH = 1200.0f;
	inline constexpr float NPC_REACH = REPORT_REACH;

	// How long a door an NPC opened is watched: it walks through, and the door closes behind it in moments.
	inline constexpr auto NPC_WATCH = std::chrono::seconds(30);

	constexpr bool WatchesNpcOpen(const NpcOpen& a_open)
	{
		return a_open.runsHere && a_open.sharesOpen && !a_open.loadDoor && a_open.playerDistance <= NPC_REACH;
	}

	// Every loaded door whose open state is a player's (SharesOpen) is read this often as well, whoever or whatever
	// changes it: a quest stage that unlocks the way (Fraternal Post 115's doors, the Boston Public Library's load
	// doors), a trigger that unlocks a door as the player walks in, a script that shuts one behind them, an NPC's use
	// no event named. Before, only a door the player had used (or a terminal they had used linked to) was watched, so
	// a plain door a script opened or unlocked reached a friend only once the player touched it. An activator's open
	// state is still its script's (an elevator; a shared gate is read through its script), a quest's stage still
	// ScriptDoors'. The first read of a door is where it stands (the save's, or the session's) and is not reported:
	// reporting every door the player walks past would fill the session's world state with the save's doors. A change
	// from it is, once our player is near enough for a friend to take our word for it (REPORT_REACH); read while far,
	// it is kept and reported when we come near, if it still stands.
	inline constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);

	// How many loaded doors the poll reads in one frame: a sweep of them all is spread over as many frames as it takes
	// (a busy exterior has a few hundred, a big modded cell may have more), so no frame pays for every door at once.
	// At 60 frames a second up to 960 doors are still read every POLL_INTERVAL; past that a sweep takes longer.
	inline constexpr std::size_t POLL_PER_FRAME = 32;

	// A door just loaded is first read this long after: its scripts are bound by then, and what its cell's scripts do
	// to it as it loads (a lock set on load) is where it stands in this game, not news.
	inline constexpr auto LOAD_SETTLE = std::chrono::seconds(2);

	// The game's load event (TESObjectLoadedEvent) names few of the doors a cell loads: on the rig (2026-10-27) three
	// of the nineteen doors near the player in Corvega's plant, and none in Sanctuary or Goodneighbor on coming back.
	// So the loaded doors are found as well by a walk of the loaded cells' reference lists (the interior, or the
	// attached cells of the grid and the worldspace's persistent cell, where the load doors are), FIND_PER_FRAME
	// references a frame; a door with its 3D up found that way is polled as one the event named. A pass over every
	// cell is followed by FIND_REST before the next: an interior of 8000 references takes about half a second a pass, a
	// 5x5 grid of exterior cells about as long and the Commonwealth's persistent references a second or two more, so a
	// door in a cell that attaches later is on the poll's list within a few seconds, and read LOAD_SETTLE after.
	inline constexpr std::size_t FIND_PER_FRAME = 256;
	inline constexpr auto        FIND_REST = std::chrono::seconds(1);

	// Where the walk stands: which of the pass's cells, and which of its references, is read next. Kept free of the
	// game: the game gives the cells as a pass begins, and each cell's reference count as it reads it. A cell may gain
	// or lose references between frames (one read twice or missed in a pass is found by the next).
	class Walk
	{
	public:
		using Clock = std::chrono::steady_clock;

		// Whether a pass may begin: none is under way, and the last ended FIND_REST ago (or none has).
		[[nodiscard]] bool Due(Clock::time_point a_now) const { return cells.empty() && a_now >= restUntil; }

		// Begins a pass over the cells given, in that order. None: nothing to walk, and the rest begins.
		void Begin(std::vector<std::uint32_t> a_cells, Clock::time_point a_now)
		{
			cells = std::move(a_cells);
			at = 0;
			next = 0;
			if (cells.empty()) {
				restUntil = a_now + FIND_REST;
			}
		}

		// The cell read next (0 when no pass is under way), and the first of its references to read.
		[[nodiscard]] std::uint32_t Cell() const { return at < cells.size() ? cells[at] : 0; }
		[[nodiscard]] std::size_t   From() const { return next; }

		// How many of the cell's a_total references are read this frame, from From(): up to FIND_PER_FRAME.
		[[nodiscard]] std::size_t Chunk(std::size_t a_total) const { return next < a_total ? (std::min)(a_total - next, FIND_PER_FRAME) : 0; }

		// Records a read of a_read of the cell's a_total references. Done with the cell, the next is up; done with them
		// all, the pass ends and the rest begins.
		void Read(std::size_t a_read, std::size_t a_total, Clock::time_point a_now)
		{
			next += a_read;
			if (next >= a_total) {
				Skip(a_now);
			}
		}

		// Done with the cell (it is gone, or detached): the next is up.
		void Skip(Clock::time_point a_now)
		{
			++at;
			next = 0;
			if (at >= cells.size()) {
				cells.clear();
				at = 0;
				++passes;
				restUntil = a_now + FIND_REST;
			}
		}

		[[nodiscard]] std::uint32_t Passes() const { return passes; }

	private:
		std::vector<std::uint32_t> cells;  // the pass's, as it began
		std::size_t                at = 0, next = 0;
		std::uint32_t              passes = 0;
		Clock::time_point          restUntil{};
	};

	// How often one reference is reported at most: REPORT_BURST at once, then one every REPORT_EVERY. A door a script
	// flips over and over (every frame, or each game's script undoing what the other's did) would otherwise be reported
	// each time it is read, and the server drops a player's events past 500 a second, all kinds together (kills and
	// container changes among them). A change that may not go yet is held, and its latest state goes when it may.
	inline constexpr std::uint32_t REPORT_BURST = 8;
	inline constexpr auto          REPORT_EVERY = std::chrono::seconds(2);

	class Pace
	{
	public:
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] bool May(Clock::time_point a_now) const { return due <= a_now + (REPORT_BURST - 1) * REPORT_EVERY; }

		void Spend(Clock::time_point a_now) { due = (std::max)(due, a_now) + REPORT_EVERY; }

	private:
		Clock::time_point due{};  // when every report spent would have come back
	};

	// One look at a watched reference, as WorldSync's watcher sees it.
	struct Watch
	{
		bool quiet = false;    // on watch because it is loaded (the poll), not because the player used it
		bool seen = false;     // a state of it has been sent, or taken as its baseline
		bool changed = false;  // what it reads now differs from that
		bool near = false;     // our player is within reach of it (REPORT_REACH; a gate's is SharedGates')
		bool pending = false;  // the session's state of it waits to be applied here: what it reads now is the old one
		bool paced = false;    // reported REPORT_BURST times lately (Pace): a change waits its turn
	};

	enum class Look
	{
		kNothing,   // nothing new, or nothing to say yet
		kBaseline,  // a polled door's first read: kept as where it stands, not reported
		kHold,      // changed, but we are too far for a friend to take it: kept for when we are near
		kSend,      // report it
	};

	constexpr Look OnLook(const Watch& a_watch)
	{
		if (a_watch.pending) {
			return Look::kNothing;
		}
		if (a_watch.quiet && !a_watch.seen) {
			return Look::kBaseline;
		}
		if (!a_watch.changed) {
			return Look::kNothing;
		}
		return a_watch.near && !a_watch.paced ? Look::kSend : Look::kHold;
	}

	// What our watcher takes as already said of a reference once a friend's state of it is taken (OfferRefState), so
	// it isn't sent back: their word where they said something, ours (what we last said, or what it reads) where they
	// didn't, and no open state where we read none (a door with a script on it in our game, from a plugin the friend
	// hasn't). Taken whole, a state that leaves the open state out read as a change here and was sent back with ours,
	// which read as a change in theirs, and so on, back and forth, for as long as both games watched the door.
	constexpr RefState Heard(RefState a_heard, const RefState& a_ours, bool a_readsOpen)
	{
		if (a_heard.open == RefState::kUnknown && a_ours.refId == a_heard.refId) {
			a_heard.open = a_ours.open;
		}
		if (a_heard.locked == RefState::kUnknown && a_ours.refId == a_heard.refId) {
			a_heard.locked = a_ours.locked;
		}
		if (!a_readsOpen) {
			a_heard.open = RefState::kUnknown;
		}
		return a_heard;
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
