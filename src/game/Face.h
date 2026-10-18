#pragma once

#include "Protocol.h"
#include "game/FaceHeads.h"

// A player's look: read from their character's base NPC and rebuilt on a stand-in's own copy of its base NPC, so
// friends see each other's faces, hair and body shape. Main thread only (Install aside: at plugin load).
namespace Face
{
	// The pieces of a look (a mask): what MakeBase writes onto the copy (game/FaceHeads.h).
	using FaceHeads::ALL;
	using FaceHeads::BODY_ONLY;
	using FaceHeads::kBody;
	using FaceHeads::kBodyTint;
	using FaceHeads::kBones;
	using FaceHeads::kHairColors;
	using FaceHeads::kHeadParts;
	using FaceHeads::kSliders;
	using FaceHeads::kTints;
	using FaceHeads::OwnHead;

	// Has the engine build our copies' heads at runtime, as it builds the player's (FaceHeads.h): patches the six calls
	// to its "prebuilt head?" check, or none. At plugin load.
	bool Install();
	// Whether Install patched them.
	[[nodiscard]] bool Patched();

	// The look of this NPC (the local player's) as the game stores it.
	[[nodiscard]] Protocol::Face Read(RE::TESNPC* a_npc);

	// The pieces MakeBase applies unless told otherwise (FaceHeads::Parts): all of them with bFriendFaces on and the
	// engine patched, else the body shape only; or the dev's facemask.
	[[nodiscard]] std::uint32_t Parts();
	// Dev: which pieces MakeBase applies (bits as above).
	void SetParts(std::uint32_t a_mask);

	// A fresh base NPC for a stand-in: a copy of a_base (a settler of the right sex) wearing these pieces of a_face.
	// Null if the race differs from a_base's or the game can't make the copy. a_parts comes back as the pieces it got:
	// the body shape only when the copy can't be marked for a runtime head (FaceHeads::Copies full).
	[[nodiscard]] RE::TESNPC* MakeBase(RE::TESNPC* a_base, const Protocol::Face& a_face, std::uint32_t& a_parts);
	// A copy MakeBase made with its own head, worn again after a load kept it: marked again (ForgetCopies forgot it).
	// False if it can't be (the marks are full): a stand-in must not wear it, it would have no head.
	[[nodiscard]] bool MarkCopy(RE::TESNPC* a_copy);
	// As a save loads, on the main thread: the marks are forgotten (FaceHeads::Copies).
	void ForgetCopies();

	// Whether an actor's loaded 3D has a face (head) on it.
	[[nodiscard]] bool HasHead(RE::TESObjectREFR* a_actor);
	// Asks the engine to build a stand-in's 3D again, head included, the way it does after an NPC's face is edited.
	void Rebuild(RE::Actor* a_actor);
	// A stand-in placed with its own head has one (FaceWatch), for the log and the status.
	void Built(std::uint32_t a_playerId, std::uint32_t a_actorId);
	// A friend's own head was given up here (FaceWatch), for the log and the status.
	void GaveUp(std::uint32_t a_playerId, bool a_ours);
	// Asks the engine to build our own player's head again (after a stand-in's took it).
	void RepairOurs();

	std::string Describe();
}
