#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// A friend in power armor, as their stand-in shows it. The stand-in wears the armor pieces they wear from the moment
// their status says they are in it, and a copy of their frame (the base they sent) is kept under it, hidden and
// without collision, so that the empty frame stands where they get out, as it does in their game.
// The stand-in itself is never put into the copy. On disk (1.11.240) activating a frame starts nothing for an NPC:
// TESFurniture::Activate begins power armor or furniture use only for the player; for any other actor it does nothing
// unless the actor is already using furniture, and then only gets it up. An NPC gets into a frame only through its AI
// package (PowerArmor::InitiatePowerArmorEnter is reached from the package procedures and from
// Actor::PutActorInChairBedQuick), and a stand-in runs a do-nothing package. Sent in by activating the copy, it never
// got in (in=false sit=0 for fifteen seconds, twice, in a live game), and while the pieces waited for it the friend
// was shown without their armor.
namespace ArmorFrames
{
	using Duration = std::chrono::steady_clock::duration;

	// The copy, once they are out: drawn where they got out for this long, then deleted.
	constexpr auto KEEP_AFTER_OUT = std::chrono::seconds(4);
	// A copy whose 3D has not come in this long after it was placed is placed again.
	constexpr auto LOAD_LIMIT = std::chrono::seconds(10);
	// A copy this far from the stand-in while worn (it is kept under the stand-in, so only after the stand-in has
	// jumped: placed again after a load, or their owner travelled) is placed again rather than moved there.
	constexpr float MOVE_LIMIT = 256.0f;

	struct CopyFacts
	{
		bool                worn = false;      // they are in power armor, and the frame they sent is a frame here
		bool                exists = false;    // a copy of ours stands
		bool                occupied = false;  // somebody here got into it (it stands drawn and solid once they are out)
		bool                loaded = false;    // its 3D is in
		std::optional<bool> samePlace;         // it is in the stand-in's place; none while either is still attaching
		float               distance = 0.0f;   // from the stand-in
		Duration            sincePlaced{};
		Duration            sinceOut{};        // since they got out (when not worn)
	};

	enum class Copy
	{
		kNone,     // nothing to do: no copy wanted, or its 3D is still coming
		kPlace,    // worn and no copy: one is placed at the stand-in
		kHold,     // worn: hidden, without collision, under the stand-in
		kReplace,  // worn, but it is in another place, too far off, or never got its 3D: deleted, placed again next update
		kShow,     // they are out: drawn where they got out (solid once nobody stands in it)
		kDelete,   // out long enough: deleted
		kAbandon,  // somebody here is in it: theirs now, neither moved nor deleted
	};

	// One step each update. Nothing waits for the copy: the pieces go on as soon as they are in, whatever it does.
	constexpr Copy ChooseCopy(const CopyFacts& a_facts)
	{
		if (!a_facts.exists) {
			return a_facts.worn ? Copy::kPlace : Copy::kNone;
		}
		if (a_facts.occupied) {
			return Copy::kAbandon;
		}
		if (!a_facts.worn) {
			return a_facts.sinceOut >= KEEP_AFTER_OUT ? Copy::kDelete : Copy::kShow;
		}
		if ((a_facts.samePlace && !*a_facts.samePlace) || a_facts.distance > MOVE_LIMIT) {
			return Copy::kReplace;
		}
		if (!a_facts.loaded) {
			return a_facts.sincePlaced > LOAD_LIMIT ? Copy::kReplace : Copy::kNone;
		}
		return Copy::kHold;
	}

	// Where they were as they got into a pre-placed frame (see ChooseOriginal) is taken from their newest state only
	// if it came this recently; else from the next one. An older one (their states had stopped coming, as through a
	// load) can be anywhere.
	constexpr auto WORN_AT_MAX_AGE = std::chrono::milliseconds(500);

	constexpr bool FreshEnough(Duration a_age)
	{
		return a_age <= WORN_AT_MAX_AGE;
	}

	// A friend in a frame every game has (one placed in the world by a plugin, such as the museum's suit in
	// Concord): our copy of that very frame still stands where it was placed while they wear theirs, two suits
	// where they see one. Ours is hidden (not drawn, no collision) while they wear theirs, and shown again once they
	// are out and their stand-in's copy is clear of it. It is only hidden, never used, moved or deleted: it is a
	// reference of our own world, its pieces and scripts belong to our own game and save.
	// Only when ours is the same suit in the same place, and nobody here needs it: a frame by that ID that our player
	// wears or left last, that someone here is in, that a quest of ours holds (the museum's suit is the main quest's
	// power armor objective until that step is done), or that stands anywhere but where they got into theirs is a suit
	// of our own world they never touched, and hiding it took it from under our player (or out of sight for the whole
	// wear, somewhere else).
	// How near where they got in ours must stand, in game units. An actor is lined up on a frame's spot before it is
	// in, so a friend in a pre-placed suit stands within a few units of where that suit stood; their status names the
	// frame as they begin to climb in, possibly before they are lined up, from as far as a player activates something
	// (fActivatePickLength, 180 by default in 1.11.240). A suit moved by anything a player would notice stands further
	// off.
	constexpr float ORIGINAL_RADIUS = 200.0f;

	struct OriginalFacts
	{
		bool                 found = false;    // a reference of that ID exists here and is not deleted or disabled
		bool                 created = false;  // ours is one made in this game (0xFF......): not the one they mean
		std::uint32_t        base = 0;         // its base form
		bool                 frame = false;    // its base is furniture the engine takes as a power armor frame
		bool                 quest = false;    // it fills a quest alias of ours (ExtraAliasInstanceArray)
		bool                 loaded = false;   // its 3D is in
		bool                 ours = false;     // our player is in it or last left it: our own suit, not theirs
		bool                 inUse = false;    // another actor of ours is in it
		bool                 moved = false;    // earlier in this wear it was not where they got in (or moved, or was used)
		std::optional<float> fromSpot;         // how far it stands from where they got in; none when that is another place
	};

	enum class Original
	{
		kHide,       // hide ours while they wear theirs
		kNone,       // their frame is not one every game has (or no frame)
		kMissing,    // not here, or not the same frame
		kNotLoaded,  // not loaded yet: looked at again later
		kOurs,       // our player's own suit
		kInUse,      // someone here is in it
		kNotFrame,   // not a power armor frame
		kQuest,      // a quest of ours holds it: our player may have to find it or get into it
		kElsewhere,  // not where they got in: left alone for the rest of this wear
	};

	constexpr Original ChooseOriginal(std::uint32_t a_ref, std::uint32_t a_frameBase, const OriginalFacts& a_facts)
	{
		if (a_ref == 0 || (a_ref >> 24) == 0xFF || a_frameBase == 0) {
			return Original::kNone;
		}
		if (!a_facts.found || a_facts.created || a_facts.base != a_frameBase) {
			return Original::kMissing;
		}
		if (!a_facts.frame) {
			return Original::kNotFrame;
		}
		if (a_facts.ours) {
			return Original::kOurs;
		}
		if (a_facts.inUse) {
			return Original::kInUse;
		}
		if (a_facts.quest) {
			return Original::kQuest;
		}
		if (a_facts.moved) {
			return Original::kElsewhere;
		}
		if (!a_facts.loaded) {
			return Original::kNotLoaded;
		}
		if (!a_facts.fromSpot || !(*a_facts.fromSpot <= ORIGINAL_RADIUS)) {
			return Original::kElsewhere;
		}
		return Original::kHide;
	}
}
