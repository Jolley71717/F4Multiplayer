#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// A friend in power armor, as their stand-in shows it. A copy of their frame (the base they sent) is placed at the
// stand-in, and the stand-in is switched into it the way the game's own SwitchToPowerArmor switches an actor in
// (PowerArmor's switch-in, Address Library 2219442 in 1.11.240): at once, with no climb and no AI. The engine moves the
// frame's items onto the actor, puts the frame on it, gives it the power armor race, rebuilds its 3D there and then,
// keeps the frame persistent for it and disables the frame. Their pieces go on only once the engine has it in its
// frame: on the stand-in's own body they are a suit nobody wears.
// Out of their armor, the stand-in is replaced rather than switched out. The engine's own exit (2252033) finishes, for an
// actor near the player, over several frames as its animation graph allows, and with no room to step out it gives the
// actor an AI package to walk out with, which a stand-in never runs. Its frame goes with it, and a copy of the frame is
// left where they got out a while.
// Activating a frame starts nothing for an NPC (TESFurniture::Activate begins power armor only for the player), and a
// stand-in runs a do-nothing package: sent in that way it never got in, and while the pieces waited for it the friend
// was shown without their armor.
namespace ArmorFrames
{
	using Duration = std::chrono::steady_clock::duration;

	// The copy, once they are out: drawn where they got out for this long, then deleted.
	constexpr auto KEEP_AFTER_OUT = std::chrono::seconds(4);
	// A copy whose 3D has not come in this long after it was placed is placed again.
	constexpr auto LOAD_LIMIT = std::chrono::seconds(10);
	// A copy this far from the stand-in before it is in it (it is kept under the stand-in, so only after the stand-in has
	// jumped: placed again after a load, or their owner travelled) is placed again rather than moved there.
	constexpr float MOVE_LIMIT = 256.0f;

	struct CopyFacts
	{
		bool                worn = false;      // they are in power armor, and the frame they sent is a frame here
		bool                exists = false;    // a copy of ours stands
		bool                engaged = false;   // the stand-in is in it: the engine's while it is (disabled, kept for the stand-in)
		bool                occupied = false;  // somebody here got into it (it stands drawn and solid once they are out)
		bool                loaded = false;    // its 3D is in
		std::optional<bool> samePlace;         // it is in the stand-in's place; none while either is still attaching
		float               distance = 0.0f;   // from the stand-in
		Duration            sincePlaced{};
		Duration            sinceOut{};        // since they got out (when not worn)
	};

	enum class Copy
	{
		kNone,     // nothing to do: no copy wanted, its 3D is still coming, or the stand-in is in it
		kPlace,    // worn and no copy: one is placed at the stand-in
		kHold,     // worn, the stand-in not in it yet: hidden, without collision, under the stand-in
		kReplace,  // worn, but it is in another place, too far off, or never got its 3D: deleted, placed again next update
		kShow,     // they are out: drawn where they got out (solid once nobody stands in it)
		kDelete,   // out long enough: deleted
		kAbandon,  // somebody here is in it: theirs now, neither moved nor deleted
	};

	// One step each update.
	constexpr Copy ChooseCopy(const CopyFacts& a_facts)
	{
		if (!a_facts.exists) {
			return a_facts.worn ? Copy::kPlace : Copy::kNone;
		}
		// The engine's while the stand-in is in it: disabled, so without 3D, and left where it was placed while the
		// stand-in walks off in it. Nothing of ours moves, shows, replaces or deletes it then; it goes with the stand-in.
		if (a_facts.engaged) {
			return Copy::kNone;
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

	// A switch-in that does not take (the engine put no ExtraPowerArmor on the stand-in) is tried again this long after,
	// this many times for one stand-in in all; then it is shown out of its armor for the rest of this wear: their clothes,
	// the copy hidden under it, never the pieces on its own body.
	constexpr int  MAX_SWITCH_TRIES = 3;
	constexpr auto SWITCH_RETRY = std::chrono::seconds(2);

	struct SwitchFacts
	{
		bool     worn = false;   // they are in power armor, and the frame they sent is a frame here
		bool     in = false;     // the engine has the stand-in in power armor (ExtraPowerArmor)
		bool     ready = false;  // the stand-in can be switched now: its 3D in, its process high (the engine rebuilds its 3D at once only then)
		bool     held = false;   // its copy is held under it this update (Copy::kHold): loaded, in its place, nobody here in it
		int      tries = 0;      // switch-ins tried for this stand-in that did not take
		Duration sinceTried{};   // since the last of them
	};

	enum class Switch
	{
		kNone,    // as it is
		kIn,      // switched into its copy now
		kOut,     // they are out and it is still in: replaced, a copy of the frame left where it stood
		kGaveUp,  // the switch-in never took: shown out of its armor (see MAX_SWITCH_TRIES)
	};

	constexpr Switch ChooseSwitch(const SwitchFacts& a_facts)
	{
		if (a_facts.in) {
			return a_facts.worn ? Switch::kNone : Switch::kOut;
		}
		if (!a_facts.worn) {
			return Switch::kNone;
		}
		if (a_facts.tries >= MAX_SWITCH_TRIES) {
			return Switch::kGaveUp;
		}
		if (!a_facts.ready || !a_facts.held || (a_facts.tries > 0 && a_facts.sinceTried < SWITCH_RETRY)) {
			return Switch::kNone;
		}
		return Switch::kIn;
	}

	// What the stand-in wears goes by what the engine says of it, never by their status alone: until the engine has it in
	// its frame it is a body of the stand-in's own race.
	enum class Dress
	{
		kClothes,  // out of power armor: their gear put on afresh (Equipment::Apply empties its inventory first)
		kArmor,    // in its frame: their pieces and weapons put on, its inventory left as it is (emptied, the frame the
		           // engine put on it would go too)
	};

	constexpr Dress ChooseDress(bool a_in)
	{
		return a_in ? Dress::kArmor : Dress::kClothes;
	}

	enum class Item
	{
		kOther,   // armor that is not power armor: clothes, hats, glasses
		kWeapon,
		kPiece,   // a power armor piece (ArmorTypePower)
		kFrame,   // the frame itself as an item, which the engine puts on an actor in a frame and takes off it
	};

	// Whether the stand-in wears an item of theirs, dressed so. In its frame, clothes put on would go under the armor at
	// best, or take the frame's slots; out of it, neither a piece nor the frame ever goes on its own body.
	constexpr bool Wears(Dress a_dress, Item a_item)
	{
		if (a_dress == Dress::kArmor) {
			return a_item == Item::kPiece || a_item == Item::kWeapon;
		}
		return a_item != Item::kPiece && a_item != Item::kFrame;
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
	// power armor objective until that step is done), that has been moved in our world, or that stands anywhere but
	// where they got into theirs is a suit of our own world they never touched, and hiding it took it from under our
	// player (or out of sight for the whole wear, somewhere else).
	// How near where they got in ours must stand, in game units. An actor is lined up on a frame's spot before it is
	// in, so a friend in a pre-placed suit stands within a few units of where that suit stood; their status names the
	// frame as they begin to climb in, possibly before they are lined up, from as far as a player activates something
	// (fActivatePickLength, 180 by default in 1.11.240). A suit moved by anything a player would notice stands further
	// off.
	constexpr float ORIGINAL_RADIUS = 200.0f;
	// How far ours may stand from where it stood before it was first moved in our world. The engine notes where a frame
	// stands before it first moves it (TESObjectREFR::EnsureStartingPosition, which for furniture does so only for power
	// armor frames), and moves a frame to where its wearer stands as they get out: one our player has used stands
	// elsewhere, as at a power armor station where a friend may park theirs too, and is our player's own suit now.
	constexpr float START_TOLERANCE = 1.0f;

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
		float                fromStart = 0.0f; // how far it stands from where it stood before it was first moved here (0: never moved)
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
		kUsed,       // moved in our world since it was placed (our player used it): our own suit, left alone
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
		if (!(a_facts.fromStart <= START_TOLERANCE)) {
			return Original::kUsed;
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
