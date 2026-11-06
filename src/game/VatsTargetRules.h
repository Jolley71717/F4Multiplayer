#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "game/TextureFlush.h"

// VATS never picks a friend's stand-in (bVatsSkipsFriends; your own setting, not the host's). Kept free of the game so
// it can be tested; game/VatsTargets.cpp patches the engine.
//
// How VATS picks its targets (1.11.240, read from Fallout4.exe with the game folder's Fallout4.pdb): every actor it can
// list, highlight or lock onto is first asked of one function, +B16220 (ID 2224729; called IsValidTarget here), with a
// pointer to the actor's handle and whether its 3D must be drawn. It says no for a child (TESObjectREFR::IsChild), the
// player's teammates (Actor flag 0x4000000 at +2D0, what GetPlayerTeammate reads: why a companion is never in VATS), a
// ghost, an actor with no high AI process or fading, a dead one, one whose 3D is hidden, and one with a refraction
// (a Stealth Boy). Four calls reach it, all in the VATS menu's code:
//   - the gather of potential targets (+B164B0, ID 2224731, the call at +0x18F): each high actor, by the process lists'
//     handles; a yes puts it in the potential target list (+328FE00) within fVATSMaxEngageDistance, which the menu
//     highlights and picks its first target from (the nearest first);
//   - the reference's check (+B16380, ID 2224730, +0x69), which the menu's other paths ask of a reference they have
//     (its update, its highlights, the gather of projectiles such as mines): an actor's goes to the function;
//   - the menu button's check that anything at all can be targeted (VATSMenu::HandleMenuButton, ID 2224716, +0xCB):
//     with nothing, VATS does not open and the game plays its "no target" sound;
//   - and the same check where an empty potential target list is filled again (+B15DC0, ID 2224725, +0x58): with
//     nothing to target it plays UIVATSNoTarget instead.
// Nothing else calls it (each call to it in the game, by an E8/E9 or any rel32, was looked for).
//
// The engine-honoured ways around were looked at and none fits. Teammate (the flag above) also makes the stand-in
// part of the player's team for the AI, perks and quest conditions (Lone Wanderer, GetPlayerTeammateCount). A ghost
// (ExtraGhost) cannot be hit at all, so friendly fire would stop. The VATSRestrictedTarget keyword (a default object,
// "a VATS target that will only display a full body highlight and cannot be attacked", what shows sVATSFollowerTarget)
// leaves the actor in the list, and nearest first, so the players at the back still lock onto the friend in front.
// TESForm::SetVATSTargetOverride (form flag 0x4000000 at +10, the console's SetVATSTarget) is read for destructible
// objects only (TESObjectREFR::GetVATStarget). So each of the four calls goes through VatsTargets::IsValidTarget, which
// says no for a friend's stand-in (Puppets::IsPuppet: not a mirrored NPC, which stays a target) and asks the engine
// for anyone else. Shooting a friend without VATS is untouched (friendly fire is the host's bFriendlyFire).
namespace VatsTargetRules
{
	// 1.11.240: the function each call is in (Address Library ID) and the call's offset in it.
	struct Site
	{
		std::uint64_t id = 0;
		std::uintptr_t offset = 0;
	};

	inline constexpr std::uint64_t IS_VALID_TARGET = 2224729;  // +B16220
	inline constexpr std::array<Site, 4> SITES{ {
		{ 2224731, 0x18F },  // the gather of potential targets (+B164B0): the call at +B1663F
		{ 2224730, 0x69 },   // the reference's check (+B16380): +B163E9
		{ 2224716, 0xCB },   // VATSMenu::HandleMenuButton (+B154F0): anything to target? +B155BB
		{ 2224725, 0x58 },   // the list's refill (+B15DC0): anything to target? +B15E18
	} };

	// The call sites, checked before any is patched: how many call a_target where 1.11.240 does. All or none are
	// patched: a VATS that leaves friends out of its list but locks onto one through another path would be worse.
	struct Sites
	{
		std::size_t calling = 0;
		std::size_t total = 0;

		[[nodiscard]] constexpr bool All() const { return total != 0 && calling == total; }
	};

	// a_bytes[i]: the five bytes at a_at[i].
	constexpr Sites Check(std::span<const std::array<std::uint8_t, 5>> a_bytes, std::span<const std::uintptr_t> a_at, std::uintptr_t a_target)
	{
		Sites sites;
		sites.total = a_at.size();
		if (a_bytes.size() != a_at.size()) {
			sites.total = 0;  // a caller's mistake: nothing is patched
			return sites;
		}
		for (std::size_t i = 0; i < a_at.size(); ++i) {
			if (TextureFlush::CallTarget(std::span<const std::uint8_t, 5>(a_bytes[i]), a_at[i]) == a_target) {
				++sites.calling;
			}
		}
		return sites;
	}

	enum class Verdict
	{
		kEngine,  // the engine decides, as it would alone
		kSkip,    // not a target: a friend's stand-in, with bVatsSkipsFriends on
	};

	// a_skipFriends: bVatsSkipsFriends. a_standIn: the actor asked about is a friend's stand-in (not a mirrored NPC, whose
	// owner's game runs it: that one is fought as any NPC is).
	constexpr Verdict Judge(bool a_skipFriends, bool a_standIn)
	{
		return a_skipFriends && a_standIn ? Verdict::kSkip : Verdict::kEngine;
	}
}
