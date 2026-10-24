#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "game/ScriptNames.h"

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
// The base script, and the few that extend it whose additions do no more than lights, sounds and a show when the gate
// opens in a friend's game too (CHILDREN). A script that extends it is called as itself (SetOpenNoWait is its own
// override, if it has one), so whatever it adds runs in every game the gate opens in. The others that extend it set a
// stage that does more (DN017_PrototypeLabDoorScript: Arcjet's, the Brotherhood's story; DN103_SetStageOnOpen:
// Poseidon Energy's protectrons wake; DLC04_GauntletGasDoorScript: Nuka-World's story), are the story's
// (DN084_AssaultronDoorScript), or move with something of their own: the Mechanist's lair doors
// (DLC01_MechanistDoorScript, 84) follow their door controls, whose red and green state is each game's own and would
// disagree with a door set from outside; the track system's doors (DLC01_TrackSystemDoor) open and close as its carts
// pass, in each game. A gate with another script beside the base one is shared only when that script does its thing on
// activation alone (SIBLINGS): SetOpenNoWait raises no OnActivate, so it happens in the presser's game only. Left out:
// dn133_SecurityDoorScript (the Castle's, a script of its own opening the gate), DN009_SecurityDoor (sets Hunter/Hunted's
// stage as the gate's animation ends), DN015_ActivateOnOpen (lets a glowing one out as the animation plays) and
// DN130_CambridgePDDoorStateToggle (sets the gate from the Brotherhood's stage on load). So are the few bases below that
// run the script but open no way, and the Mass Fusion gates the survey marks NEVER.
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

	struct Listed
	{
		std::string_view script;
		std::string_view why;
	};

	// Scripts that extend it and are shared as it is (see above): what they add when the gate opens.
	inline constexpr std::array CHILDREN{
		Listed{ "DN066_LabDoorScript", "Med-Tek Research's first lab door: sounds its klaxon and opens the lab's window shutters" },
		Listed{ "DN066_SubLabDoorScript", "Med-Tek Research's sub-lab door: sounds its klaxon and opens the sub-lab's shutters" },
		Listed{ "DN070_TreasureDoorScript", "the Jamaica Plain treasure vault's two inner gates: set DN070's stages 95, 105 and 110, lights, displays and the vault's music" },
	};

	// Scripts beside it that do their thing only when the gate is activated (in the presser's game alone).
	inline constexpr std::array SIBLINGS{
		Listed{ "dn143RemoveFromCaptiveFaction", "Vault 75's two cell gates: on activation, the captives behind them turn on the player" },
		Listed{ "DefaultRefEnableLinkedRefOnActivate", "a gate at the Boston Airport ruins, whose link it would enable is unset: nothing" },
		Listed{ "DLC01:DLC01_GarageDoorKlaxonScript", "the Mechanist's lair garage door: its klaxon lights and sounds while it moves" },
	};

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
		FileForm{ "Fallout4.esm", 0x000F8606, "Mass Fusion's lab gate (its security terminal's, before DN084_LabSecurityDoor01)" },
		FileForm{ "Fallout4.esm", 0x00098B11, "Mass Fusion's DN084_ReactorLabSecurityDoor" },
	};

	// Whether a script's name is the gate script's itself (the VM's names keep the case they were written with; the
	// game's own lookups ignore it).
	constexpr bool IsGateScript(std::string_view a_name)
	{
		return ScriptNames::Same(a_name, SCRIPT);
	}

	// The script a shared gate is run by, as this list spells it, if a_name is the gate script or one of CHILDREN: the
	// one whose isOpen is read and whose SetOpenNoWait is called.
	constexpr std::optional<std::string_view> RunningScript(std::string_view a_name)
	{
		if (IsGateScript(a_name)) {
			return SCRIPT;
		}
		return ScriptNames::Find(CHILDREN, a_name);
	}

	constexpr bool IsSibling(std::string_view a_name)
	{
		return ScriptNames::Find(SIBLINGS, a_name).has_value();
	}

	// What the game knows of a reference.
	struct Facts
	{
		bool          activator = false;  // its base is an activator (not a door, nor a light or a moveable static)
		std::uint32_t scripts = 0;        // the scripts bound to it (a plain ObjectReference object is no script)
		std::uint32_t gateScripts = 0;    // ... of them, the gate script itself
		bool          listedOut = false;  // its base is in NOT_A_WAY, or it is in NEVER
		std::uint32_t childScripts = 0;   // ... one of CHILDREN (a reference that has one has the base script removed)
		std::uint32_t siblings = 0;       // ... one of SIBLINGS
	};

	// Whether its open state is shared: an activator run by the gate script or one of CHILDREN, never both, with
	// nothing beside it but SIBLINGS, not listed out.
	constexpr bool Shares(const Facts& a_facts)
	{
		const bool runs = (a_facts.gateScripts > 0) != (a_facts.childScripts > 0) && a_facts.childScripts <= 1;
		return a_facts.activator && runs && a_facts.gateScripts + a_facts.childScripts + a_facts.siblings == a_facts.scripts && !a_facts.listedOut;
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
