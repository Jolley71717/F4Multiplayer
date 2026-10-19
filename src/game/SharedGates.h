#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

// Security gates and other two-state activators whose open state is shared like a door's (docs/script-doors-survey.md,
// "What is not shared today", 1 to 3). A security gate is an activator, not a door, run by the game's standard two-state
// script, Default2StateActivator: its property isOpen says whether it is open, its function SetOpen(abOpen) plays the
// open or close animation and turns the gate's collision off or on (DisableLinkChain/EnableLinkChain along
// TwoStateCollisionKeyword), and SetOpenNoWait(abOpen) does the same a moment later, on a timer. The game opens these
// gates from terminals (TERM_nativeSwitchDoorTermina: SetOpenNoWait(true) along the chain of gates linked with
// LinkTerminalSwitchDoor, 96 gates), ID card readers (IDCardReaderScript activates its linked gate, for the player only,
// 28 readers) and buttons. All of it ran only in the game of whoever used it, and DoorShare leaves an activator's open
// state to its script, so in a friend's game the gate stayed shut, behind a terminal they may not be able to hack or a
// reader they have no card for.
//
// So a gate that runs that script, and nothing else, has its open state shared as a door's is (kReportRefState, kept in
// the server's world state, replayed to whoever joins later, loads a save or comes by later): reported by the game of
// whoever used the terminal, the reader, the button or the gate (all watched by WorldSync, with what they link to, and
// the gates linked from those), and applied in a friend's game through the script itself (SetOpenNoWait), never the
// engine's open state, so that the animation, the collision and the script's own isOpen agree. The gate's own press is
// no longer replayed by the presser's stand-in (WorldSync::ApplyRemoteActivation): its state says where it ends up,
// while a replayed press toggles, the wrong way if the two games' gates disagree.
//
// Only the base script itself: every script that extends it (DN017_PrototypeLabDoorScript, DN066_LabDoorScript and
// DN066_SubLabDoorScript, DN070_TreasureDoorScript, DN084_AssaultronDoorScript, DN103_SetStageOnOpen) also sets a quest
// stage, sounds a klaxon or opens shutters when opened, and a gate with another script beside it
// (dn133_SecurityDoorScript, DN009_SecurityDoor, DN015_ActivateOnOpen, dn143RemoveFromCaptiveFaction,
// DefaultRefEnableLinkedRefOnActivate...) may do something of its own when it opens. Those are left as they were. So
// are the few bases below that run the script but open no way, and the Mass Fusion gates the survey marks NEVER.
//
// What isn't shared, on purpose (the survey's gap 3): a button, lever or switch pressed while the friend was elsewhere
// is not pressed again in their game later. Every other switch script toggles, or does its thing once, with effects of
// its own (lights, turrets, a klaxon, enemies let out, a lift's car, a breaker): a press remembered and replayed later,
// in a game where that switch has since been pressed by the friend or loaded from a save, would undo it or do it twice.
// Where a press's lasting effect is a gate, the gate's state covers it; where it is a plain door's lock (DoorChain,
// securityBarScript), DoorShare does. Lifts stay each player's own (their cars are moved by each game's script).
//
// Kept free of the game so the rules can be tested.
namespace SharedGates
{
	using Clock = std::chrono::steady_clock;

	// The script, by the name the game's VM knows it by.
	inline constexpr std::string_view SCRIPT = "Default2StateActivator";

	struct FileForm
	{
		std::string_view file;  // the plugin
		std::uint32_t    id;    // the form's ID in it (no load-order byte)
		std::string_view what;  // for the log and the reader
	};

	// Bases that run the script but open no way: a lid, or a show. Sharing them would only send traffic.
	inline constexpr std::array NOT_A_WAY{
		FileForm{ "Fallout4.esm", 0x001C5E6F, "Jukebox01" },
		FileForm{ "Fallout4.esm", 0x001C0BE7, "Jukebox01Clean" },
		FileForm{ "Fallout4.esm", 0x0011254D, "InstituteToilet01TankCover" },
		FileForm{ "Fallout4.esm", 0x001651E4, "DeconArch01 (the Institute's decontamination arches)" },
	};

	// References the survey marks NEVER: Mass Fusion's gates (DN084), part of the Brotherhood's and the Institute's story
	// there (BoS303, InstMassFusion), each player's own. Its other gates have a script of their own beside this one
	// (DN084_AssaultronDoorScript) and are left out anyway.
	inline constexpr std::array NEVER{
		FileForm{ "Fallout4.esm", 0x000F8605, "Mass Fusion's DN084_LabSecurityDoor01" },
		FileForm{ "Fallout4.esm", 0x000F850C, "Mass Fusion's DN084_AirlockDoor01" },
		FileForm{ "Fallout4.esm", 0x000F850D, "Mass Fusion's DN084_AirlockDoor02" },
		FileForm{ "Fallout4.esm", 0x000F9CDE, "Mass Fusion's DN084_ControlRoomDoor" },
		FileForm{ "Fallout4.esm", 0x0015FAFD, "Mass Fusion's DN084_LabsUpperSecurityDoor" },
	};

	// Whether a script's name is the gate script's (the VM's names keep the case they were written with; the game's
	// own lookups ignore it).
	constexpr bool IsGateScript(std::string_view a_name)
	{
		if (a_name.size() != SCRIPT.size()) {
			return false;
		}
		const auto lower = [](char a_c) { return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c; };
		for (std::size_t i = 0; i < a_name.size(); ++i) {
			if (lower(a_name[i]) != lower(SCRIPT[i])) {
				return false;
			}
		}
		return true;
	}

	// What the game knows of a reference.
	struct Facts
	{
		bool          activator = false;  // its base is an activator (not a door, nor a light or a moveable static)
		std::uint32_t scripts = 0;        // the scripts bound to it (a plain ObjectReference object is no script)
		std::uint32_t gateScripts = 0;    // ... of them, the gate script itself (not one that extends it)
		bool          listedOut = false;  // its base is in NOT_A_WAY, or it is in NEVER
	};

	// Whether its open state is shared: an activator that runs the gate script and nothing else, not listed out.
	constexpr bool Shares(const Facts& a_facts)
	{
		return a_facts.activator && a_facts.scripts > 0 && a_facts.gateScripts == a_facts.scripts && !a_facts.listedOut;
	}

	// What the script says of the gate: its isOpen, and whether it is in its "busy" state (SetOpen goes busy before the
	// animation and sets isOpen only once it has played, seconds later).
	struct ScriptState
	{
		bool open = false;
		bool busy = false;
	};

	// Where the gate is going: a busy gate is on its way to the other state. Read so, a gate is reported as soon as it
	// starts to move, and a friend's starts a moment after.
	constexpr bool Target(const ScriptState& a_state)
	{
		return a_state.busy ? !a_state.open : a_state.open;
	}

	// A gate just loaded runs its OnLoad, which plays the animation of the state it has (SetDefaultState) and turns its
	// collision on or off to match; a SetOpen at the same time would race it, the animation one way and the collision the
	// other. So a state is applied once the gate has been loaded, and our player in the world, this long.
	inline constexpr auto LOAD_SETTLE = std::chrono::seconds(2);

	constexpr bool Settled(std::optional<Clock::time_point> a_loadedAt, Clock::time_point a_inWorldSince, Clock::time_point a_now)
	{
		const auto since = a_loadedAt && *a_loadedAt > a_inWorldSince ? *a_loadedAt : a_inWorldSince;
		return a_now - since >= LOAD_SETTLE;
	}

	enum class Apply
	{
		kWait,     // not yet: the script can't be read, or the gate has only just loaded
		kNothing,  // it is there already, or on its way
		kSet,      // call SetOpenNoWait(open)
	};

	// What to do with the session's state of a loaded gate (a_open), given what its script says (nullopt: no script
	// object to read yet).
	constexpr Apply Decide(bool a_open, const std::optional<ScriptState>& a_script, bool a_settled)
	{
		if (!a_script || !a_settled) {
			return Apply::kWait;
		}
		return Target(*a_script) == a_open ? Apply::kNothing : Apply::kSet;
	}

	// How far from a gate a player's live report of it counts: a terminal or a card reader is often across the room or
	// down the corridor from its gate (a door's reports count from 1500 units, a button's reach).
	inline constexpr float REACH = 4000.0f;

	// How many gates deep a chain of linked gates is followed (a terminal opens a chain: each gate links the next).
	inline constexpr std::size_t CHAIN_DEPTH = 8;

	// The gates we set from another player's state, until they get there: SetOpenNoWait runs a moment later (and waits
	// out an animation already playing), and until then our own watcher, if it watches the gate (our player used its
	// terminal a minute ago), would read the old state and report it back, closing the friend's gate again.
	class Applied
	{
	public:
		// How long a gate is given to get there before what it says is taken as news again.
		static constexpr auto HOLD = std::chrono::seconds(15);

		void Set(std::uint32_t a_ref, bool a_open, Clock::time_point a_now) { applied[a_ref] = { a_open, a_now }; }

		// Whether what the watcher reads of a gate now (where it is going) is to be held back: we set it the other way
		// and it hasn't got there yet. Forgotten once it gets there, or after HOLD.
		bool Holds(std::uint32_t a_ref, bool a_target, Clock::time_point a_now)
		{
			const auto it = applied.find(a_ref);
			if (it == applied.end()) {
				return false;
			}
			if (it->second.open == a_target || a_now - it->second.at > HOLD) {
				applied.erase(it);
				return false;
			}
			return true;
		}

		void Clear() { applied.clear(); }

		[[nodiscard]] std::size_t Size() const { return applied.size(); }

	private:
		struct Entry
		{
			bool              open;
			Clock::time_point at;
		};
		std::unordered_map<std::uint32_t, Entry> applied;
	};
}
