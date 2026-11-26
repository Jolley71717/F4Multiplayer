#pragma once

#include "game/NpcRoll.h"

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The game's side of NpcRoll.h: what a leveled NPC was rolled as, read from its leveled data, and our NPC made another
// game's roll. Main thread only.
namespace Rolls
{
	// What an NPC our game runs is, as reported under a_sessionId (a_factions: a spawn's, which the others' copies take).
	// Nothing for one with nothing to tell: not leveled, and no factions asked for.
	[[nodiscard]] std::optional<Protocol::NpcIdentity> Read(RE::Actor* a_actor, std::uint32_t a_sessionId, bool a_factions);

	// Dev and the session's end: the settler-store sync (a plugin NPC's vendor factions) on or off, what we put NPCs in taken out again, and one
	// NPC's vendor state as text.
	void SetVendorSync(bool a_on);
	[[nodiscard]] bool VendorSync();
	// The vendor factions our game's own scripts put a plugin NPC in: what it is in less what a friend's report put it in (sorted).
	[[nodiscard]] std::vector<NpcRoll::FactionRank> OwnVendorFactions(RE::Actor* a_actor);
	void ForgetVendorCache(RE::Actor* a_actor);
	void UndoVendorFactions();

	// A save made in a session keeps what we put NPCs in from a friend's report (a record in the save, read back with it): taken off
	// once the save has loaded (RemoveSavedVendorFactions), whether a session runs or not.
	[[nodiscard]] std::vector<std::uint8_t> SaveVendorRecord();
	void LoadVendorRecord(std::span<const std::uint8_t> a_bytes, const std::function<std::optional<std::uint32_t>(std::uint32_t)>& a_resolve);
	void RevertVendorRecord();
	void RemoveSavedVendorFactions();
	[[nodiscard]] std::string DescribeVendor(RE::Actor* a_actor);

	// Our NPC as Decide compares it (a_copy: our copy of a friend's spawn).
	[[nodiscard]] NpcRoll::Ours ReadOurs(RE::Actor* a_actor, bool a_copy);

	// What a roll of a_original can give (cached: the lists are the game's data).
	[[nodiscard]] const NpcRoll::Reach& ReachOf(std::uint32_t a_original);

	// Makes our NPC what a_plan says (NpcRoll::StepsFor: the roll is only written, for its next load).
	// The console commands it ran (CommandMeter).
	std::uint32_t Apply(RE::Actor* a_actor, const Protocol::NpcIdentity& a_theirs, const NpcRoll::Plan& a_plan);

	// Whether a_base is a leveled NPC here (an NPC whose traits come from a template): placing it rolls it.
	[[nodiscard]] bool IsLeveledNpc(std::uint32_t a_base);

	// Dev: our NPC's roll, level and factions, and its name.
	[[nodiscard]] std::string Describe(RE::Actor* a_actor);

	[[nodiscard]] const char* Name(NpcRoll::Roll a_roll);
}
