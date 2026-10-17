#pragma once

#include "Protocol.h"

// A player's look: read from their character's base NPC and rebuilt on a stand-in's own copy of
// its base NPC, so friends see each other's faces, hair and body shape. Main thread only.
namespace Face
{
	// The pieces of a look (a mask): what MakeBase writes onto the copy.
	inline constexpr std::uint32_t kHeadParts = 1;   // head, eyes, hair, beard, scars...: the copy gets its own head
	inline constexpr std::uint32_t kHairColors = 2;  // hair and beard colours, and the complexion
	inline constexpr std::uint32_t kBody = 4;        // the body shape triangle
	inline constexpr std::uint32_t kSliders = 8;     // sculpting sliders, by region and by morph
	inline constexpr std::uint32_t kBones = 16;      // bone sculpting
	inline constexpr std::uint32_t kTints = 32;      // skin tone, make-up, face paint, dirt, scars (tint layers)
	inline constexpr std::uint32_t kBodyTint = 64;
	inline constexpr std::uint32_t ALL = 0x7F;
	// Sex and body shape on the settler's own face (how 0.8.7 showed every friend): nothing of the face shows without
	// the copy's own head, since a settler's look comes baked with its template's FaceGen files.
	inline constexpr std::uint32_t BODY_ONLY = kBody;

	// The look of this NPC (the local player's) as the game stores it.
	[[nodiscard]] Protocol::Face Read(RE::TESNPC* a_npc);

	// The pieces MakeBase applies unless told otherwise: all of them (bFriendFaces), or the body shape only (off, or the
	// dev's facemask).
	[[nodiscard]] std::uint32_t Parts();
	// Dev: which pieces MakeBase applies (bits as above).
	void SetParts(std::uint32_t a_mask);
	// Whether a copy made with these pieces has its own head (so its head is the engine's to build: FaceWatch.h).
	[[nodiscard]] inline bool OwnHead(std::uint32_t a_parts) { return (a_parts & kHeadParts) != 0; }

	// A fresh base NPC for a stand-in: a copy of a_base (a settler of the right sex) wearing these pieces of a_face.
	// Null if the race differs from a_base's or the game can't make the copy.
	[[nodiscard]] RE::TESNPC* MakeBase(RE::TESNPC* a_base, const Protocol::Face& a_face, std::uint32_t a_parts);

	// Whether an actor's loaded 3D has a face (head) on it.
	[[nodiscard]] bool HasHead(RE::TESObjectREFR* a_actor);
	// Asks the engine to build a stand-in's 3D again, head included, the way it does after an NPC's face is edited.
	void Rebuild(RE::Actor* a_actor);
	// A friend's own head was given up here (FaceWatch), for the log and the status.
	void GaveUp(std::uint32_t a_playerId, bool a_ours);
	// Asks the engine to build our own player's head again (after a stand-in's took it).
	void RepairOurs();

	std::string Describe();
}
