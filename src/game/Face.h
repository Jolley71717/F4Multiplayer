#pragma once

#include "Protocol.h"

// A player's look: read from their character's base NPC and rebuilt on a stand-in's own copy of
// its base NPC, so friends see each other's faces, hair and body shape. Main thread only.
namespace Face
{
	// The look of this NPC (the local player's) as the game stores it.
	[[nodiscard]] Protocol::Face Read(RE::TESNPC* a_npc);

	// A fresh base NPC for a stand-in: a copy of a_base (a settler of the right sex) wearing a_face.
	// Null if the race differs from a_base's or the game can't make the copy.
	[[nodiscard]] RE::TESNPC* MakeBase(RE::TESNPC* a_base, const Protocol::Face& a_face);

	// Dev: which pieces MakeBase applies (bits: 1 head parts, 2 hair colours, 4 body shape,
	// 8 sliders, 16 bones, 32 tints, 64 body tint). Default: all.
	void SetParts(std::uint32_t a_mask);

	std::string Describe();
}
