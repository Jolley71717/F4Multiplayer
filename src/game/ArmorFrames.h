#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "Protocol.h"

// A friend in power armor, as their stand-in shows it. A copy of their frame (the base they sent) is placed at the
// stand-in, and the stand-in is switched into it the way the game's own SwitchToPowerArmor switches an actor in
// (PowerArmor's switch-in, Address Library 2219442 in 1.11.240): at once, with no climb and no AI. The engine moves the
// frame's items onto the actor, puts the frame on it, gives it the power armor race, queues the rebuild of its 3D (a
// task of the frame after), keeps the frame persistent for it and disables the frame. Their pieces go on only once
// that rebuilt 3D is in, built on the power armor skeleton: on the stand-in's own body they are a suit nobody wears, and
// the body it has as the switch-in returns is still its own.
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
	// this many times for one friend's wear in all (a stand-in placed again for one that took but never got its power
	// armor skeleton counts too); then it is shown out of its armor for the rest of this wear: their clothes, the copy
	// hidden under it, never the pieces on its own body.
	constexpr int  MAX_SWITCH_TRIES = 3;
	constexpr auto SWITCH_RETRY = std::chrono::seconds(2);
	// How long a stand-in the engine has in its frame may stay on its own skeleton. The switch-in only queues the rebuild
	// of its 3D (TaskQueueInterface::QueueActorUpdateModel), which the task queue runs a frame or so later, longer when
	// the skeleton and frame come from disk; and it queues nothing when the model loader already holds a load for the
	// actor. One still on its own body this long after is placed again (Switch::kStuck), as a switch that never took.
	constexpr auto SKELETON_LIMIT = std::chrono::seconds(5);

	struct SwitchFacts
	{
		bool     worn = false;     // they are in power armor, and the frame they sent is a frame here
		bool     in = false;       // the engine has the stand-in in power armor (ExtraPowerArmor)
		bool     rebuilt = false;  // its 3D is built on the power armor skeleton
		Duration sinceIn{};        // since the switch-in took (when in)
		bool     ready = false;    // the stand-in can be switched now: its 3D in, its process high (the engine queues its 3D rebuild only then)
		bool     held = false;     // its copy is held under it this update (Copy::kHold): loaded, in its place, nobody here in it
		int      tries = 0;        // switch-ins tried for this wear that did not take
		Duration sinceTried{};     // since the last of them
	};

	enum class Switch
	{
		kNone,    // as it is
		kIn,      // switched into its copy now
		kOut,     // they are out and it is still in: replaced, a copy of the frame left where it stood
		kStuck,   // in, but still on its own skeleton too long (see SKELETON_LIMIT): replaced, counted as a try
		kGaveUp,  // the switch-in never took: shown out of its armor (see MAX_SWITCH_TRIES)
	};

	constexpr Switch ChooseSwitch(const SwitchFacts& a_facts)
	{
		if (a_facts.in) {
			if (!a_facts.worn) {
				return Switch::kOut;
			}
			return !a_facts.rebuilt && a_facts.sinceIn > SKELETON_LIMIT ? Switch::kStuck : Switch::kNone;
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
	// its frame it is a body of the stand-in's own race, and it keeps that body until the rebuild the switch-in queued
	// has run (the pieces are modelled for the power armor skeleton: on the other body they hang in the air).
	enum class Dress
	{
		kClothes,  // out of power armor: their gear put on afresh (Equipment::Apply empties its inventory first)
		kWait,     // in its frame, its 3D not rebuilt yet: nothing put on or looked at
		kArmor,    // in its frame on the power armor skeleton: their pieces, weapons and frame put on, its inventory left
		           // as it is (emptied, the frame the engine put on it would go too)
	};

	constexpr Dress ChooseDress(bool a_in, bool a_rebuilt)
	{
		if (!a_in) {
			return Dress::kClothes;
		}
		return a_rebuilt ? Dress::kArmor : Dress::kWait;
	}

	enum class Item
	{
		kOther,   // armor that is not power armor: clothes, hats, glasses
		kWeapon,
		kPiece,   // a power armor piece (ArmorTypePower)
		kFrame,   // the frame itself as an item, which the engine puts on an actor in a frame and takes off it
	};

	// Whether the stand-in wears an item of theirs, dressed so. In its frame, clothes put on would go under the armor at
	// best, or take the frame's slots; out of it, neither a piece nor the frame ever goes on its own body; waiting for
	// the power armor skeleton, nothing at all.
	// In its frame their frame item goes on in place of the one the engine picked. The engine picks it by what the actor
	// wears (PowerArmor's frame pick, 2219424 in 1.11.240): the first frame armor whose other keywords are on an item it
	// has equipped, else the default frame (PowerArmorDefaultFrameArmor_DO, 001909D6, the AtomCat frame). The plain
	// frame (0003E577) goes to an actor in a vault suit, which a friend in a frame no longer wears (the frame takes its
	// body slot), so their stand-in got the AtomCat frame on every wear while they wore the plain one. And once their
	// pieces are on, every pick (the engine picks again as it puts an actor's outfit back on, 2230445) finds the AtomCat
	// frame first: its ArmorTypePower keyword is on every piece.
	constexpr bool Wears(Dress a_dress, Item a_item)
	{
		switch (a_dress) {
		case Dress::kArmor:
			return a_item == Item::kPiece || a_item == Item::kWeapon || a_item == Item::kFrame;
		case Dress::kWait:
			return false;
		case Dress::kClothes:
			return a_item != Item::kPiece && a_item != Item::kFrame;
		}
		return false;
	}

	// The items of a list a stand-in dressed so wears (Wears), in their order; a_kindOf(item) says what each is. a_theirs
	// is what they have on: in its frame, a frame item counts only when theirs names one this game has (a_kindOf says
	// kFrame of it). They name none for a moment as they get in, and one from a plugin this game lacks is no frame here:
	// the frame the engine picked then stays on it, and is not looked at, or the gear check would find the stand-in in
	// something else than it was dressed in for as long as they wear that frame.
	template <class KindOf>
	std::vector<std::uint32_t> Kept(const std::vector<std::uint32_t>& a_items, const std::vector<std::uint32_t>& a_theirs, Dress a_dress, KindOf&& a_kindOf)
	{
		const bool frameNamed = std::ranges::any_of(a_theirs, [&](std::uint32_t a_item) { return a_kindOf(a_item) == Item::kFrame; });
		std::vector<std::uint32_t> out;
		for (const auto item : a_items) {
			const auto kind = a_kindOf(item);
			if (Wears(a_dress, kind) && (kind != Item::kFrame || frameNamed)) {
				out.push_back(item);
			}
		}
		return out;
	}

	// Where they were as they got into a pre-placed frame (see ChooseOriginal) is taken from their newest state only
	// if it came this recently; else from the next one. An older one (their states had stopped coming, as through a
	// load) can be anywhere.
	constexpr auto WORN_AT_MAX_AGE = std::chrono::milliseconds(500);

	constexpr bool FreshEnough(Duration a_age)
	{
		return a_age <= WORN_AT_MAX_AGE;
	}

	// A wear whose frame is not known: ours when what we sat in as the armor went on is no frame (friends hear we are
	// in power armor with no frame, and their stand-in of us wears the pieces with no frame copy), a friend's when the
	// frame their stand-in would go into is no frame here (they named none, or one from a plugin this game lacks).
	// Told in the log once a wear, once it has stayed unknown this long: the armor goes on a moment before the
	// furniture is known, so the first status of every wear names no frame and the next one names it.
	constexpr auto UNKNOWN_FRAME_AFTER = std::chrono::seconds(1);

	class UnknownFrame
	{
	public:
		// a_on: in power armor; a_known: its frame is known (a friend's: and is a frame here). True the one update it
		// is to be told.
		bool Update(bool a_on, bool a_known, std::chrono::steady_clock::time_point a_now)
		{
			if (!a_on) {
				since.reset();
				told = false;
				return false;
			}
			if (a_known) {
				since.reset();
				return false;
			}
			if (!since) {
				since = a_now;
			}
			if (told || a_now - *since < UNKNOWN_FRAME_AFTER) {
				return false;
			}
			told = true;
			return true;
		}

	private:
		std::optional<std::chrono::steady_clock::time_point> since;  // unknown since
		bool                                                 told = false;
	};

	// What our player sits in now (Party): the reference and its base (0 when none), and whether the base is a frame.
	struct Seat
	{
		std::uint32_t ref = 0;
		std::uint32_t base = 0;
		bool          frame = false;
	};

	// Our own wear as friends are told it (Party::SampleStatus). The frame is the furniture we sat in as the armor
	// went on, remembered for as long as it stays on: the occupied furniture changes with every terminal or chair
	// used in the armor (a terminal was once sent as the frame, and friends placed a terminal copy and sat the
	// stand-in at it). The reference goes too when every game has it (a frame placed in the world by a plugin, such
	// as the museum's suit in Concord): friends hide their own copy of it while we wear ours, else they saw two suits.
	// One made in this game (0xFF......) is another reference, or none, in theirs.
	class OwnWear
	{
	public:
		struct Told
		{
			std::uint32_t frame = 0;     // PlayerStatus::powerArmorFrame
			std::uint32_t frameRef = 0;  // PlayerStatus::powerArmorFrameRef
			bool          unknown = false;  // this update is the one to log the wear's frame as unknown (see UnknownFrame)
			std::uint32_t seat = 0;         // then, the base of what we sit in
		};

		// a_on: in power armor. a_seat() gives what we sit in; it is asked only while the frame is not known yet.
		template <class F>
		Told Update(bool a_on, F&& a_seat, std::chrono::steady_clock::time_point a_now)
		{
			std::uint32_t seat = 0;
			if (!a_on) {
				frame = 0;
				frameRef = 0;
			} else if (frame == 0) {
				const Seat s = a_seat();
				if (s.frame && s.base != 0) {
					frame = s.base;
					frameRef = Protocol::IsShareableRef(s.ref) ? s.ref : 0;
				} else {
					seat = s.base;
				}
			}
			const bool unknown = unknownFrame.Update(a_on, frame != 0, a_now);
			return { .frame = frame, .frameRef = frameRef, .unknown = unknown, .seat = seat };
		}

		// A load: the loaded save may have our player in another frame, and the old one was sent for as long as the
		// armor stayed on.
		void Forget()
		{
			frame = 0;
			frameRef = 0;
		}

	private:
		std::uint32_t frame = 0;
		std::uint32_t frameRef = 0;
		UnknownFrame  unknownFrame;
	};

	// A friend's wear as their stand-in keeps it (RemotePlayers::SetPowerArmor), from each status they send. The frame
	// base they told is kept until they tell another: out of the armor, and into the start of the next wear, whose
	// first status names none (see OwnWear), the stand-in goes on with the one it had. The reference goes as they get
	// out: the next frame is told with its base.
	struct Heard
	{
		bool          on = false;
		std::uint32_t frame = 0;
		std::uint32_t frameRef = 0;
	};

	constexpr Heard Hear(Heard a_kept, bool a_on, std::uint32_t a_frame, std::uint32_t a_frameRef)
	{
		a_kept.on = a_on;
		if (a_frame) {
			a_kept.frame = a_frame;
			a_kept.frameRef = a_frameRef;
		} else if (!a_on) {
			a_kept.frameRef = 0;
		}
		return a_kept;
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
