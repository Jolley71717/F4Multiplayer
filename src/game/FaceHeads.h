#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "game/TextureFlush.h"

// A friend's own head on their stand-in, built by the engine as it builds the player's. Kept free of the game so it
// can be tested; game/Face.cpp patches the engine.
//
// How the engine picks a head (1.11.240, read from Fallout4.exe with the game folder's Fallout4.pdb): one function,
// +6DF3A0 (ID 2209308; called UsesPreprocessedHead here), answers whether an NPC wears a prebuilt head, the FaceGen
// files the Creation Kit bakes per NPC, or one built at runtime from its head parts, sculpting and tint layers. It
// follows the NPC's face template (faceNPC) to the root and says no for the player (form 7), the two chargen NPCs, an
// NPC with the spouse keyword and an NPC whose face was edited in game (change flag 0x800, how LooksMenu's edits show);
// every other NPC, a runtime form with no file included, says yes. With a yes the head task asks for the FaceGen nif
// (FaceGenData\FaceGeom\<plugin>\<id>.nif); a runtime copy has no plugin, so none is asked for, and the head build
// (+6E25A0) then finds no nif and attaches nothing: the stand-in stood headless (live 2026-10-17, 3 of 3) whatever
// its head parts and however often it was rebuilt. With a no it is the player's path: the head task loads the head
// parts' meshes and morphs, the head is built from them, the facial bones are sculpted on a chargen skeleton and the
// tint layers are painted into the face texture (BSFaceGenManager's customization queue).
//
// So the engine is told no for our copies: each of the six calls to that function (the head task's queueing, its
// processing, the head build's two, the head data queue's and one in TESNPC's head part update) goes through
// UsesPreprocessedHead below. Every other NPC gets the engine's own answer. The six sites are checked first; one that
// does not call the function where 1.11.240 does (another plugin's patch) leaves all six alone: half the calls told
// no would load one kind of head and build the other. Then no friend gets their own head (Parts, below).
namespace FaceHeads
{
	// The pieces of a look (a mask): what Face::MakeBase writes onto the copy.
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

	// Whether a copy made with these pieces has its own head (so its head is the engine's to build at runtime).
	constexpr bool OwnHead(std::uint32_t a_parts) { return (a_parts & kHeadParts) != 0; }

	// The pieces a copy gets: the dev's facemask when one was given (an experiment, whatever the engine does with it);
	// else their whole look with bFriendFaces on and the engine patched to build such a head; else the body shape only.
	constexpr std::uint32_t Parts(bool a_friendFaces, std::optional<std::uint32_t> a_dev, bool a_patched)
	{
		if (a_dev) {
			return *a_dev & ALL;
		}
		return a_friendFaces && a_patched ? ALL : BODY_ONLY;
	}

	// The call sites, checked before any is patched: how many call a_target where 1.11.240 does.
	struct Sites
	{
		std::size_t calling = 0;
		std::size_t total = 0;

		constexpr bool All() const { return total != 0 && calling == total; }
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

	// Our copies: the base NPCs made for stand-ins with their own head, by address and form ID. Added to on the main
	// thread only; read from any (the engine's loader threads run the head task), without a lock. Emptied as a save
	// loads (Face::ForgetCopies): the load sets the runtime form IDs back, so a form the engine makes after it at a
	// freed copy's address can come back with that copy's ID too. A copy the load kept is marked again when a stand-in
	// wears it (Face::MarkCopy).
	template <std::size_t N>
	class Copies
	{
	public:
		// False when full: that copy wears the settler's face (Face::MakeBase).
		bool Add(std::uintptr_t a_address, std::uint32_t a_id)
		{
			if (a_address == 0 || Contains(a_address, a_id)) {
				return a_address != 0;
			}
			const auto n = count.load(std::memory_order_relaxed);
			if (n >= N) {
				return false;
			}
			ids[n].store(a_id, std::memory_order_relaxed);
			addresses[n].store(a_address, std::memory_order_relaxed);
			count.store(n + 1, std::memory_order_release);  // a reader that sees the count sees the entry
			return true;
		}

		bool Contains(std::uintptr_t a_address, std::uint32_t a_id) const
		{
			const auto n = count.load(std::memory_order_acquire);
			for (std::size_t i = 0; i < n; ++i) {
				if (addresses[i].load(std::memory_order_relaxed) == a_address && ids[i].load(std::memory_order_relaxed) == a_id) {
					return true;
				}
			}
			return false;
		}

		std::size_t Size() const { return count.load(std::memory_order_acquire); }

		// Main thread only. A reader on another thread that read the count before may still compare a slot as it is
		// written again: for that one call it can miss a copy being marked, which no head task asks about yet.
		void Clear() { count.store(0, std::memory_order_release); }

	private:
		std::array<std::atomic<std::uintptr_t>, N> addresses{};
		std::array<std::atomic<std::uint32_t>, N>  ids{};
		std::atomic<std::size_t>                   count{ 0 };
	};
}
