#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

	// The body's sculpting regions (head, upper torso, arms, lower torso, legs): the NPC's region slider values
	// (TESNPC::morphRegionSliderValues) hold one value per region, by region index.
	inline constexpr std::size_t BODY_REGIONS = 5;

	// The region slider values a copy gets from a friend's (kSliders). The engine reads them by region index without
	// looking at the array's size: as it scales a body's bones while loading its 3D (1.11.240, Fallout4.exe+664D87,
	// on the background loader thread), a bone in regions 0-4 reads values[region] whenever the array pointer is set;
	// only a null pointer reads as 0. A friend fresh out of the game's face editor (a preset picked) reported none,
	// the copy got an empty array (data null) and the first bone, in region 4, read address 0x10: the host's crash,
	// 2026-10-18. So none stays none (empty: the copy gets no array, and the engine reads 0 for every region), and
	// fewer than the regions are filled with 0 up to them, so every region the engine can ask for is there.
	inline std::vector<float> RegionSliders(std::span<const float> a_sent)
	{
		std::vector<float> values(a_sent.begin(), a_sent.end());
		if (!values.empty() && values.size() < BODY_REGIONS) {
			values.resize(BODY_REGIONS, 0.0f);
		}
		return values;
	}

	// A face's sculpting entries (morphs, bone regions), in one order and no more than a_max: sorted by a_key first and
	// cut after, so what a full face leaves out is always its highest keys, not whichever the game's map listed last.
	// Returns how many were left out.
	template <class T, class Key>
	std::size_t SortAndCap(std::vector<T>& a_entries, std::size_t a_max, Key a_key)
	{
		std::ranges::sort(a_entries, {}, a_key);
		if (a_entries.size() <= a_max) {
			return 0;
		}
		const auto left = a_entries.size() - a_max;
		a_entries.resize(a_max);
		return left;
	}

	// The class of a race's tint template entry, by its vtable (BGSCharacterTint::Template::Mask, Palette, TextureSet).
	enum class TintClass : std::uint8_t
	{
		kMask = 0,
		kPalette = 1,
		kTextureSet = 2,
		kOther,  // none of the three (not read)
	};

	// Whether a friend's tint layer (Protocol::FaceTint::type: 0 a mask, 1 a palette, 2 a texture set) may be made on
	// the template it names: only one of the template's own class. The layer's class is the friend's word; the template
	// is found by its id alone, and one of another class (another mod's race, a bad client) would be read as the wrong
	// class when the engine builds the head on its loader thread, a crash. Such a layer is left out.
	constexpr bool TintFits(std::uint8_t a_type, TintClass a_template)
	{
		return a_type <= 2 && a_template != TintClass::kOther && static_cast<std::uint8_t>(a_template) == a_type;
	}

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
