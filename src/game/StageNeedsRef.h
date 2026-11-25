#pragma once

#include "game/StageRules.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

// Friends' stages whose script acts on a reference in the world, and acts only while that reference's 3D is loaded: it
// plays an animation on it, and an unloaded reference takes none. A stage is done once, so one set while the player was
// elsewhere (a save loaded or a session joined away from the place, the friend's talk done while this player was out of
// the cell) leaves the quest done and the world as it was. Vault 81 (GitHub report, 2026-11): the intercom talk's last
// stage, V81_00_Intro 55, opens the vault's gear door (the quest script's OpenGearDoor plays the door's open animation,
// and extends the walkway). Caught up to it with the cave not loaded (the session replays it to a game that loads a save
// or joins), the stage was done, the first objective shown, and the door stayed shut for good: the talk can't be had
// again (its stages are done) and the way into the vault is closed. Seen on the rig: the door's node stood at its closed
// pose after the catch-up, and at its open pose when the same stage was set with the cave loaded.
//
// So the stage waits (its quest with it, other quests go on: QuestSync's kQuestWaits) until the reference is loaded and has
// been for SETTLE, as ScriptDoors does for the doors it shares, and is set then. A reference this game doesn't have (a
// plugin missing, deleted, disabled) doesn't hold it. The same list serves a stage a friend does live while this player
// is elsewhere. Kept free of the game so it can be tested.
//
// A row is added only for a stage read off the game's scripts to act on a loaded reference. The other stages that open a
// way (DN142's 50, ScriptDoors) are not here: ScriptDoors holds them by its own rule.
namespace StageNeedsRef
{
	using Clock = std::chrono::steady_clock;

	struct Row
	{
		StageRules::File file;
		std::uint32_t    quest;  // in its file
		std::uint16_t    stage;
		std::uint32_t    ref;  // in the same file
		std::string_view what;
	};

	inline constexpr std::array ROWS{
		// V81_00_Intro 55: the intercom talk's end opens Vault 81's gear door (alias VaultDoor, 000A9B71 in the vault's entrance).
		Row{ StageRules::File::kFallout4, 0x000B8464, 55, 0x000A9B71, "Vault 81's gear door" },
	};

	// How long the reference has been loaded before the stage is set: its cell's objects have settled and the animation takes.
	inline constexpr auto SETTLE = std::chrono::seconds(2);

	inline const Row* Find(StageRules::File a_file, std::uint32_t a_quest, std::uint16_t a_stage)
	{
		for (const auto& row : ROWS) {
			if (row.file == a_file && row.quest == a_quest && row.stage == a_stage) {
				return &row;
			}
		}
		return nullptr;
	}

	// The loads seen, one per stage waiting: since when each reference has been loaded without a break.
	class Settling
	{
	public:
		// One look at the reference a stage waits for. True when the stage can be set now: loaded for SETTLE.
		bool Ready(std::uint64_t a_key, bool a_loaded, Clock::time_point a_now)
		{
			if (!a_loaded) {
				since.erase(a_key);
				return false;
			}
			const auto at = since.try_emplace(a_key, a_now).first->second;
			return a_now - at >= SETTLE;
		}

		// The stage was set (or dropped): forgotten.
		void Done(std::uint64_t a_key) { since.erase(a_key); }

		void Clear() { since.clear(); }

		[[nodiscard]] std::size_t Size() const { return since.size(); }

	private:
		std::map<std::uint64_t, Clock::time_point> since;
	};
}
