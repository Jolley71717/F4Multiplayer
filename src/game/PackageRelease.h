#pragma once

#include <cstdint>

// Which package an NPC we stop mirroring is let go of. A mirror is given the do-nothing package
// (Actor::InitiateDoNothingPackage, Puppets::Register): a package the engine makes on the spot, a form FFxxxxxx of
// procedure DoNothing flagged must-complete, put in the AI process's current package slot. Package evaluation (evp) keeps
// a must-complete package until its procedure is done, and DoNothing never is, so an NPC handed over to our game kept
// standing there. Only that package is let go of (Puppets::ReleaseNpc): one from the game's data (a quest's, a scene's)
// is the NPC's own. Pure, so it can be tested without the game.
namespace PackageRelease
{
	// PACKAGE_PROCEDURE_TYPE's DoNothing (RE::PTYPE::kDoNothing; `mirror <ref>` shows it as packType=27).
	constexpr std::int8_t DO_NOTHING = 0x1B;

	// A form made while the game runs (none from a plugin or a save's own records has this index).
	constexpr bool IsMadeForm(std::uint32_t a_formId)
	{
		return (a_formId >> 24) == 0xFF;
	}

	// The package an NPC runs (its type and form id) is the do-nothing one a mirror is given: let go of on release.
	constexpr bool IsMirrorsDoNothing(std::int8_t a_packType, std::uint32_t a_formId)
	{
		return a_packType == DO_NOTHING && IsMadeForm(a_formId);
	}
}
