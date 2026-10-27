#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Doors only a quest's script opens, shared by the stage that opens them. A door a player opens is shared by its
// open state (DoorShare), but an activator's open state is its script's and isn't shared, and some doors are opened
// by a quest stage that nothing a friend does sets in their game. Vault 114's gear door at Park Street station
// (2026-10-19): the host pulled the lever, its console (VaultDoorConsoleScript) set DN142's stage 50 in the host's
// game, and that stage opened the door. In the friend's game DN142 stayed at 0 and the door shut. The console's
// press, replayed by the host's stand-in (WorldSync::ApplyRemoteActivation), does nothing there: the console's
// script works only for the player. Playing the door's open animation in the friend's game wouldn't let them in
// either: the stage also disables an invisible wall in the doorway (DN142QuestScript::OpenGearDoor disables
// VDoorCollision, a CollisionMarker, then lights the klaxons).
//
// So the stage itself is shared, through the session's quest stages (kReportQuestStage, kept by the server and
// replayed to a player who joins later or loads a save). QuestSync never shares these quests: they are dungeon
// quests, filed under no type with no objectives (QuestFilter). Only stages listed here, each read off the game's
// files and found to do nothing but open the way: no item, no XP, no scene, no other quest, nothing that moves
// anyone. A stage of a quest the session shares otherwise is QuestSync's and isn't listed. Left out on purpose:
//   - Vault 81's console (VaultDoorConsoleScript_V81) sets V81_00_Intro's 15, which starts the intercom talk; that
//     quest is a side quest with objectives, QuestSync's.
//   - Vault 111's gear door: the opening (MQ101, MQ102), each player's own.
//   - Vault 75's and Vault 95's gear doors are statics, already open; DLCCoast.esm (Vault 118) has no such console.
//   - The console's own lever animation: cosmetic, and the friend's console still works (once the stage is done,
//     pulling it only plays the lever).
//   - Every other stage that opens a way. docs/script-doors-survey.md read the whole game for them (2026-10-19)
//     and found none that only opens it: the rest also start scenes, send NPCs out, set objectives or more stages
//     (DN050, DN102, DN048...), are the story's (DN084, DN136...), are set by a quest QuestSync shares (DN017's
//     200, DN130's 20), or are set by nothing placed in the world (DN066's 10 and 20; its terminals open the doors).
//     A door a terminal or a card reader opens is an activator's open state (below), not a stage.
//   - Any activator's open state (an elevator's doors and car, a call panel: DoorShare), and the open state of the
//     containers, furniture and markers a player uses (their lids and the like), which WorldSync logs as "opened by
//     a script (not a door)".
//
// A friend's stage is set here only once the door it opens is loaded, and has been for a moment: the stage plays
// the door's open animation, which an unloaded door doesn't take, and a stage is done once (DN142 doesn't allow
// repeated stages), so a stage set while the player was elsewhere would leave the wall gone and the door shut for
// good. Not over a stage done here already (the player opened it, or the session's stage came twice), nor while the
// quest doesn't run here (setting it would start the quest over). Kept free of the game so it can be tested.
//
// The same list shares the Prydwen (2026-10-27, a Nexus report on 0.9.2: a friend didn't see it arrive though both
// left the place together). Read off Fallout4.esm and its scripts: the airship's arrival is the story's. Out of
// Time's 130 (set when "Sanctuary" starts, Min01's 0, Preston's talk after the museum fight) enables a flyover
// effect (MQ201FXPrydFlyoverRef 0019537C, persistent, full LOD, parked in the sky far west), whose script
// (FXPrydwenFlyoverScript) plays once its 3D loads, flies the airship across the Commonwealth to Boston Airport
// over some minutes, and at the end of the flight (or when the effect unloads, the player waits or travels) sets
// BoSEnable's 15, which enables the airship moored at the airport (BoSPrydwen 00184FD9, a persistent, initially
// disabled marker whose enable-parent children are the moored airship's parts, perches and markers), then disables
// itself. With a shared story a friend's game sets Out of Time's 130 too (QuestSync) and plays its own flyover once
// its player is outdoors; with each player's own story it doesn't; and either way the moored airship appears in a
// game only when that game's own flight ends. So BoSEnable's 15 is shared here: BoSEnable is start-game-enabled and
// internal (type 0, no objectives, never QuestSync's), 15 does nothing but enable the marker, and a persistent
// marker is enabled from anywhere (Door::loaded false: no wait for it to load, no settle). A friend whose story is
// behind sees the airship moored before their own Brotherhood quests, as the game itself orders it: the flyover
// follows Concord, long before Shadow of Steel (BoS201), whose scene and vertibird ride take the airship as there
// already; no script reads BoSEnable's stages (every script naming it sets them: the Brotherhood's, the Institute's
// and the Railroad's quests, the destruction script's 200; DN165Exterior's fragment asks only whether it runs), the
// marker is held only by its own 15 (enabled), RR304's fragment (enabled) and PrydwenDestructionScript (disabled),
// and the only conditions on BoSEnable are Liberty Prime's scribes' packages, which read its stage for Prime's 20
// to 40 (every pex and every record of the plugins, read again at review). Their own flight later sets 15 again,
// which BoSEnable (no repeated stages) ignores. A game whose own flyover is in the air (the effect enabled, with
// its 3D: Door::effect, Look::busy) waits for it instead: its script sets 15 itself at the end of the flight, or
// when the player waits, travels or goes indoors, and set sooner the moored airship would stand at the airport
// while the flying one is still crossing (both players leaving Concord together, the report's case). The flyover
// itself isn't shared: it is Out of Time's (its 130 also sends Preston's group off to Sanctuary), and the effect
// plays only once in a game (its script never resets): enabled again after its flight it would hang, in full LOD,
// where it stopped over the airport.
namespace ScriptDoors
{
	using Clock = std::chrono::steady_clock;

	struct Door
	{
		std::string_view file;   // the plugin that has the quest and the door
		std::uint32_t    quest;  // the quest, its form ID in that file (no load-order byte)
		std::uint16_t    stage;  // the stage that opens the door
		std::uint32_t    door;   // the door the stage opens, its form ID in that file: the stage waits for it to load
		std::string_view what;   // which door, for the log
		std::string_view state;  // what the stage leaves it as, for the log ("open")
		bool             loaded = true;  // the stage animates it: set only once it is loaded (an enable needs no 3D)
		std::uint32_t    effect = 0;     // a reference whose loaded 3D means this game's own script is on its way to the stage (the flyover in the air): the session's stage waits for it
	};

	// Fallout4.esm's DN142 (Vault 114) 50: Fragment_Stage_0050 calls DN142QuestScript::OpenGearDoor, which plays
	// "Stage2" on the GearDoor alias (the reference 000B25D1 of the activator VltGearDoor114 000C01CA in the Vault114
	// cell, filled by its location ref type AutofillGeneric20), disables VDoorCollision (00178F07) and enables and
	// activates the klaxon lights. Set by the console at 00145DA1 (VaultDoorConsole01, StageToSet 50), which the player
	// uses through the furniture it links to at 00145DA2 (NPCVaultConsoleActivate, LinkVaultDoorConsoleFurniture): the
	// lever's animation, each player's own, which WorldSync logs as opened by a script (a FURN, not a door). DN142 is
	// start-game-enabled (DNAM 0x0111), so it runs in every game from the start and the stage can be set in any.
	inline constexpr std::array DOORS{
		Door{ "Fallout4.esm", 0x0013B747, 50, 0x000B25D1, "Vault 114's gear door", "open" },
		// BoSEnable (001851A0) 15: Fragment_Stage_0015 enables BoSPrydwen (00184FD9), the marker whose children are the
		// airship moored at Boston Airport, and nothing else (see above). Set by the flyover effect's script
		// (MQ201FXPrydFlyoverRef 0019537C) at the end of its flight: while that effect is in the air here, waited for.
		Door{ "Fallout4.esm", 0x001851A0, 15, 0x00184FD9, "the Prydwen", "moored at Boston Airport", false, 0x0019537C },
	};

	// The door a quest's stage opens (by the IDs as this game has them: a_quests[i] is DOORS[i]'s quest), if it is one.
	inline std::optional<std::size_t> Find(const std::vector<std::uint32_t>& a_quests, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		for (std::size_t i = 0; i < DOORS.size() && i < a_quests.size(); ++i) {
			if (a_quests[i] != 0 && a_quests[i] == a_quest && DOORS[i].stage == a_stage) {
				return i;
			}
		}
		return std::nullopt;
	}

	// What the game says of one door at a look.
	struct Look
	{
		bool running = false;     // its quest is in this game and running
		bool done = false;        // the stage is done here
		bool doorLoaded = false;  // the door is loaded (has its 3D), so its open animation plays
		bool canApply = false;    // in the world, out of the opening, no menu pausing scripts
		bool busy = false;        // this game's own script is on its way to the stage (Door::effect enabled and loaded): it sets it itself
	};

	enum class Act
	{
		kNone,
		kReport,  // done here, by this game's player: tell the session
		kSet,     // the session's stage: set it here now
		kHold,    // the session's stage, but this game's own script is about to set it (Look::busy): waiting, said once
		kGiveUp,  // set here, and still not done after TIMEOUT: left alone
	};

	// What this game knows of each door's stage: whether it was done here at the last look (reported when a look
	// finds it newly done), and the session's stages to set here.
	class Book
	{
	public:
		static constexpr auto SETTLE = std::chrono::seconds(2);    // the door loaded this long before its stage is set
		static constexpr auto TIMEOUT = std::chrono::seconds(10);  // set, and not done this long after: given up on

		// The session has this door's stage (a friend did it, now or before we joined).
		void Take(std::size_t a_door)
		{
			if (a_door < DOORS.size()) {
				doors[a_door].pending = true;
			}
		}

		// The host's game, welcomed by its own server: what its world has open is the session's (reported at the
		// next look of each door, as a joiner's own before joining is not).
		void ReportOpen()
		{
			for (auto& door : doors) {
				door.reportOpen = true;
			}
		}

		// One look at a door, and what to do about it.
		Act See(std::size_t a_door, const Look& a_look, Clock::time_point a_now)
		{
			if (a_door >= DOORS.size()) {
				return Act::kNone;
			}
			auto&      door = doors[a_door];
			// The first look (after a load) is where the world stands: a stage done in the save is no news.
			const bool newlyDone = door.seenDone.has_value() && !*door.seenDone && a_look.done;
			door.seenDone = a_look.done;
			const bool reportOpen = std::exchange(door.reportOpen, false);
			if (a_look.done) {
				door.pending = false;
				door.held = false;
				door.setAt.reset();
				door.loadedSince.reset();
				// One this game set is the session's already.
				return (newlyDone || reportOpen) && !door.setHere ? Act::kReport : Act::kNone;
			}
			if (!door.pending) {
				return Act::kNone;
			}
			if (door.setAt) {
				if (a_now - *door.setAt < TIMEOUT) {
					return Act::kNone;
				}
				door.pending = false;
				door.setAt.reset();
				return Act::kGiveUp;
			}
			if (!a_look.running || !a_look.canApply || (DOORS[a_door].loaded && !a_look.doorLoaded)) {
				door.loadedSince.reset();
				return Act::kNone;
			}
			// This game's own script is on its way to the stage (the flyover in the air): it sets it itself, within
			// minutes or at the player's next wait, travel or door. Said once.
			if (a_look.busy) {
				door.loadedSince.reset();
				return std::exchange(door.held, true) ? Act::kNone : Act::kHold;
			}
			// One that needs no 3D (the Prydwen's enable) is set at once.
			if (DOORS[a_door].loaded) {
				if (!door.loadedSince) {
					door.loadedSince = a_now;
				}
				if (a_now - *door.loadedSince < SETTLE) {
					return Act::kNone;
				}
			}
			door.setAt = a_now;
			door.setHere = true;
			door.held = false;
			return Act::kSet;
		}

		// Whether a session's stage waits to be set here.
		[[nodiscard]] bool Pending(std::size_t a_door) const { return a_door < DOORS.size() && doors[a_door].pending; }

		[[nodiscard]] std::size_t PendingCount() const
		{
			std::size_t n = 0;
			for (const auto& door : doors) {
				n += door.pending ? 1 : 0;
			}
			return n;
		}

		// A save loaded (its doors are as it has them) or the session ended (nothing more of it is set here).
		void Clear() { doors = {}; }

	private:
		struct State
		{
			std::optional<bool>              seenDone;     // at the last look; none before the first
			bool                             pending = false;
			bool                             reportOpen = false;
			bool                             setHere = false;  // this game set it as the session's
			bool                             held = false;     // waiting for this game's own script (Look::busy), logged
			std::optional<Clock::time_point> loadedSince;
			std::optional<Clock::time_point> setAt;
		};
		std::array<State, DOORS.size()> doors{};
	};
}
