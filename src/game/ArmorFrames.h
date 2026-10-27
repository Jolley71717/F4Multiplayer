#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
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

	// The limits below go by the time the game ran, not by the clock: a menu that stops the game (the Pip-Boy, the pause
	// menu, a container), a load, or a frame the game took far too long over (one in the background, its window left)
	// is not counted. Counted by the clock, a friend who got into their armor as we opened the Pip-Boy was found still
	// on its own skeleton as we closed it, placed again as a switch-in that never took, and after a few such shown out of
	// their armor for the rest of the wear. A time of the game's run, not of the clock: the two cannot be mixed up.
	struct PlayClock
	{
		using duration = Duration;
		using rep = duration::rep;
		using period = duration::period;
		using time_point = std::chrono::time_point<PlayClock>;
		static constexpr bool is_steady = true;
	};
	using PlayTime = PlayClock::time_point;
	// The most one frame counts for.
	constexpr auto LONGEST_FRAME = std::chrono::milliseconds(250);

	class Played
	{
	public:
		// Once a frame. a_paused: a menu has the game stopped, or a load is on. Returns the time the game ran until a_now.
		PlayTime Advance(std::chrono::steady_clock::time_point a_now, bool a_paused)
		{
			if (last && !a_paused) {
				played += (std::min)(Duration{ a_now - *last }, Duration{ LONGEST_FRAME });
			}
			last = a_now;
			return played;
		}

		[[nodiscard]] PlayTime Now() const { return played; }

	private:
		std::optional<std::chrono::steady_clock::time_point> last;
		// It starts an hour in: a time never set ({}) is long ago.
		PlayTime played{ std::chrono::hours(1) };
	};

	// The copy, once they are out: drawn where they got out for this long, then deleted; unless their status says where
	// their frame stands by then, and it is moved there and kept as their parked frame (see ChoosePark).
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
		bool                sameBase = true;   // it is a copy of the frame they wear (when worn)
	};

	enum class Copy
	{
		kNone,     // nothing to do: no copy wanted, its 3D is still coming, or the stand-in is in it
		kPlace,    // worn and no copy: one is placed at the stand-in
		kHold,     // worn, the stand-in not in it yet: hidden, without collision, under the stand-in
		kReplace,  // worn, but it is another frame, in another place, too far off, or never got its 3D: deleted, placed again next update
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
		// The copy left where they got out is the one they get back into, when it is theirs: another frame got into
		// within KEEP_AFTER_OUT of leaving one (a station with a few suits) is not dressed in the frame they left.
		if (!a_facts.sameBase || (a_facts.samePlace && !*a_facts.samePlace) || a_facts.distance > MOVE_LIMIT) {
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
	// How long a stand-in on the power armor skeleton may keep its legs still while its friend walks on theirs, in our
	// sight, before it is placed again (Switch::kStill), counted as a try. Live (2026-10-15) a stand-in switched in a
	// moment after it was dressed got the skeleton and all six pieces and then glided after its friend in a standing
	// pose for the rest of the wear: nothing in its graph played (the dev command could read none of its variables, not
	// even Speed and Direction), while a fresh stand-in switched into the next wear walked and ran. Over the 0.8.7-rc1
	// soak (2026-10-16, 35 wears) every stand-in that was already there when its friend got in glided, and one in four
	// of the fresh ones placed for it did too: the graph the rebuild left them answered its variables and played an
	// idle, but refused "moveStart" and "moveStop" both (see Puppets::ReleaseIdle, which now frees a stand-in's graph as
	// it frees a mirror's). Every one of them had been dressed, or given an item, 40 to 80 ms before the switch-in: the
	// biped the engine was still building for that overlapped the rebuild of its skeleton (see DRESS_SETTLE). A fresh
	// stand-in stays the cure of last resort. The cause (2026-10-19): a graph rebuilt before its power armor subgraphs are
	// loaded is left uninitialised, and only the engine's own actor update set it up once they were; a stand-in's update
	// did not (Puppets.cpp, SetUpRebuiltGraph, now does). Nor is the last try spent on a replacement shown out of its
	// armor beside an empty frame while its friend is in theirs: that one is kept in its frame as it is (Switch::kKept).
	constexpr auto STILL_LIMIT = std::chrono::seconds(2);
	// The switch-in waits this long after anything was last put on the stand-in (its clothes, the item that steers the
	// engine's pick): what is equipped goes on its body a frame or more later (equipped 30 ms before a switch-in, a vault
	// suit was not on yet to the engine's pick), and a skeleton rebuilt while that still loads is what the glides above
	// had in common.
	constexpr auto DRESS_SETTLE = std::chrono::milliseconds(400);

	// Before a stand-in whose legs stay still is replaced, it is tried in place, each try given STILL_LIMIT again (live
	// 2026-10-19: 12 of 13 switch-ins played no walk, each of the three replacements of a wear exactly like the last):
	// first its graph is set up afresh whatever the engine says of its subgraphs (Remedy::kSetUp: the actor's own
	// ModifyInitialAnimationStateImpl, then told again whether it moves); then the engine runs its own update, AI and
	// all, for OWN_UPDATE (Remedy::kOwnUpdate), as it did around every switch-in before 0.8.10, when the stand-ins in
	// their frames walked. Only then is it replaced.
	enum class Remedy
	{
		kSetUp,
		kOwnUpdate,
	};
	constexpr int  MAX_REMEDIES = 2;
	constexpr auto OWN_UPDATE = std::chrono::milliseconds(1000);
	constexpr Remedy RemedyOf(int a_tried)
	{
		return a_tried == 0 ? Remedy::kSetUp : Remedy::kOwnUpdate;
	}

	struct SwitchFacts
	{
		bool     worn = false;     // they are in power armor, and the frame they sent is a frame here
		bool     in = false;       // the engine has the stand-in in power armor (ExtraPowerArmor)
		bool     rebuilt = false;  // its 3D is built on the power armor skeleton
		Duration still{};          // its legs have not moved for this long while they walked in our sight (see Stride)
		Duration sinceIn{};        // since the switch-in took (when in)
		bool     ready = false;    // the stand-in can be switched now: its 3D in, its process high (the engine queues its 3D rebuild only then)
		bool     held = false;     // its copy is held under it this update (Copy::kHold): loaded, in its place, nobody here in it
		Duration sinceDressed = Duration::max();  // since anything was last put on it (see DRESS_SETTLE)
		int      tries = 0;        // switch-ins tried for this wear that did not take
		Duration sinceTried{};     // since the last of them
		int      remedies = 0;     // remedies tried on this stand-in in its frame (see Remedy)
	};

	enum class Switch
	{
		kNone,    // as it is
		kIn,      // switched into its copy now
		kOut,     // they are out and it is still in: replaced, a copy of the frame left where it stood
		kStuck,   // in, but still on its own skeleton too long (see SKELETON_LIMIT): replaced, counted as a try
		kStill,   // in and on the power armor skeleton, but its legs stay still while they walk (see STILL_LIMIT): replaced, counted as a try
		kGaveUp,  // the switch-in never took: shown out of its armor (see MAX_SWITCH_TRIES)
		kKept,    // as kStill, but with no try left after it: kept in its frame as it is, never shown out of its armor for it
		kRemedy,  // as kStill, with a remedy left for this stand-in: tried in place (see Remedy), not counted as a try
	};

	constexpr Switch ChooseSwitch(const SwitchFacts& a_facts)
	{
		if (a_facts.in) {
			if (!a_facts.worn) {
				return Switch::kOut;
			}
			if (!a_facts.rebuilt) {
				return a_facts.sinceIn > SKELETON_LIMIT ? Switch::kStuck : Switch::kNone;
			}
			if (a_facts.still < STILL_LIMIT) {
				return Switch::kNone;
			}
			if (a_facts.remedies < MAX_REMEDIES) {
				return Switch::kRemedy;
			}
			return a_facts.tries + 1 >= MAX_SWITCH_TRIES ? Switch::kKept : Switch::kStill;
		}
		if (!a_facts.worn) {
			return Switch::kNone;
		}
		if (a_facts.tries >= MAX_SWITCH_TRIES) {
			return Switch::kGaveUp;
		}
		if (!a_facts.ready || !a_facts.held || a_facts.sinceDressed < DRESS_SETTLE || (a_facts.tries > 0 && a_facts.sinceTried < SWITCH_RETRY)) {
			return Switch::kNone;
		}
		return Switch::kIn;
	}

	// Whether a stand-in's legs move, for Switch::kStill: a leg bone's rotation (its local transform, which the graph
	// writes every frame it plays something), and whether it changed by more than STRIDE_MOVED in any entry since it was
	// last seen to move. A walk swings the thigh by tens of degrees; a dead graph leaves it exactly where it was.
	struct LegPose
	{
		float m[9]{};
	};
	constexpr float STRIDE_MOVED = 0.05f;  // about 3 degrees

	constexpr bool Moved(const LegPose& a_from, const LegPose& a_to)
	{
		for (int i = 0; i < 9; ++i) {
			const float d = a_to.m[i] - a_from.m[i];
			if (d > STRIDE_MOVED || d < -STRIDE_MOVED) {
				return true;
			}
		}
		return false;
	}

	// Their game's own moveMode bits (Gait.h's MOVE_WALKING, MOVE_RUNNING, MOVE_SPRINTING): set only while their own
	// legs carry them. Not while carried by a lift or a vehicle (none set, whatever the speed), nor off the ground.
	constexpr std::uint16_t OWN_LEGS = 0x40 | 0x80 | 0x100;
	constexpr float         WALK_SPEED = 25.0f;  // MoveSync::START: the stand-in's graph is told to move above it

	constexpr bool Walks(std::uint16_t a_moveMode, std::uint8_t a_flags, float a_speed)
	{
		return (a_moveMode & OWN_LEGS) != 0 && (a_flags & Protocol::kInAir) == 0 && a_speed > WALK_SPEED;
	}

	// In our sight, as far as the stillness goes: near enough, and ahead of our player. Off screen the engine may not
	// play an actor's graph, and legs still there are no fault of it.
	constexpr float SIGHT_DISTANCE = 3000.0f;
	constexpr float SIGHT_CLOSE = 150.0f;    // this near it counts whichever way we face
	constexpr float SIGHT_COS = 0.34f;       // cos 70 degrees, either side of our heading

	// a_dx, a_dy: from our player to the stand-in; a_heading: our player's (0 along +y, clockwise).
	inline bool InSight(float a_dx, float a_dy, float a_heading)
	{
		const float distance = std::sqrt(a_dx * a_dx + a_dy * a_dy);
		if (distance > SIGHT_DISTANCE) {
			return false;
		}
		if (distance < SIGHT_CLOSE) {
			return true;
		}
		return (a_dx * std::sin(a_heading) + a_dy * std::cos(a_heading)) / distance >= SIGHT_COS;
	}

	// How long a stand-in's legs have stayed still while its friend walked in our sight (play time). Once a frame:
	// a_root, its 3D (only compared: a new one starts over); a_pose, its leg bone's rotation (none: no such bone, or it
	// is not in its frame on the power armor skeleton, and nothing is counted); a_walks, they walk in our sight now.
	class Stride
	{
	public:
		Duration Update(const void* a_root, const std::optional<LegPose>& a_pose, bool a_walks, PlayTime a_now)
		{
			if (a_root != root) {
				root = a_root;
				anchor.reset();
				still = {};
				last.reset();
			}
			const Duration elapsed = last ? (std::min)(Duration{ a_now - *last }, Duration{ LONGEST_FRAME }) : Duration{};
			last = a_now;
			if (!a_pose) {
				anchor.reset();
				still = {};
				return still;
			}
			if (!anchor || Moved(*anchor, *a_pose)) {
				anchor = a_pose;  // moving (or first seen): still since now
				still = {};
				return still;
			}
			if (a_walks) {
				still += elapsed;
			}
			return still;
		}

		[[nodiscard]] Duration Still() const { return still; }

	private:
		const void*             root = nullptr;
		std::optional<LegPose>  anchor;  // the pose last seen moving to
		Duration                still{};
		std::optional<PlayTime> last;
	};

	// What the stand-in wears goes by what the engine says of it, never by their status alone: until the engine has it in
	// its frame it is a body of the stand-in's own race, and it keeps that body until the rebuild the switch-in queued
	// has run (the pieces are modelled for the power armor skeleton: on the other body they hang in the air).
	enum class Dress
	{
		kClothes,  // out of power armor: their gear put on afresh (Equipment::Apply empties its inventory first)
		kWait,     // in its frame, its 3D not rebuilt yet: nothing put on or looked at
		kArmor,    // in its frame on the power armor skeleton: their pieces and weapons put on, its inventory left as it is
		           // (emptied, the frame the engine put on it would go too)
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
	// the power armor skeleton, nothing at all. The frame item is never put on or looked at: the engine puts its own pick
	// on the stand-in as it switches it in (see ChoosePick), and one swapped for theirs afterwards did not stay (the
	// engine picked again within seconds, the AtomCat frame and then the Wastelander one).
	constexpr bool Wears(Dress a_dress, Item a_item)
	{
		switch (a_dress) {
		case Dress::kArmor:
			return a_item == Item::kPiece || a_item == Item::kWeapon;
		case Dress::kWait:
			return false;
		case Dress::kClothes:
			return a_item != Item::kPiece && a_item != Item::kFrame;
		}
		return false;
	}

	// The items of a list a stand-in dressed so wears (Wears), in their order; a_kindOf(item) says what each is.
	template <class KindOf>
	std::vector<std::uint32_t> Kept(const std::vector<std::uint32_t>& a_items, Dress a_dress, KindOf&& a_kindOf)
	{
		std::vector<std::uint32_t> out;
		std::ranges::copy_if(a_items, std::back_inserter(out), [&](std::uint32_t a_item) { return Wears(a_dress, a_kindOf(a_item)); });
		return out;
	}

	// The frame item the stand-in wears in its frame is the engine's pick, made as the switch-in puts it in (PowerArmor's
	// frame pick, 2219424 in 1.11.240), by what it has equipped then: the first frame item one of whose own keywords is on
	// an item it has on, else the default frame (PowerArmorDefaultFrameArmor_DO, 001909D6, the AtomCat frame). The plain
	// frame (0003E577) is the vault suit's (usePowerArmorFrameVault). Their game picked theirs by what they had on as they
	// got in, so the stand-in is switched in only once the engine's pick for it is theirs:
	// - their gear names their frame item: an item that makes the engine pick it (one with that frame's own keyword, a
	//   vault suit for the plain frame) is put on, if the pick is not theirs already, and the switch-in waits for it;
	// - it names none yet (it is told a moment after they get in, or later): the switch-in waits until the stand-in has
	//   on what it was dressed in (their clothes, dressed a moment ago), so the pick is made by what they had on. Equipped
	//   30 ms before the switch-in, their vault suit was not on yet to the engine, and the stand-in got the AtomCat frame.
	// Neither waits longer than PICK_WAIT: switched in all the same then, in whatever the engine picks. The item put on
	// for the pick settles for DRESS_SETTLE like any other before the switch-in (the clothes it was dressed in settle
	// through SwitchFacts::sinceDressed).
	constexpr auto PICK_WAIT = std::chrono::milliseconds(1500);
	// An item put on for the pick that is not on again this long after (a gear check or a change took it off) is put on
	// again.
	constexpr auto KEY_GAP = std::chrono::milliseconds(300);

	struct PickFacts
	{
		bool     named = false;        // their gear names a frame item this game has
		bool     picksTheirs = false;  // the engine's pick for the stand-in now is that frame item
		bool     key = false;          // an item is known that makes the engine pick theirs
		bool     keyOn = false;        // the stand-in has it on
		bool     dressed = false;      // the stand-in has on what it was dressed in
		Duration waited{};             // since the switch-in was first due in this wear
		Duration sincePut = Duration::max();  // since the item was put on for the pick
	};

	enum class Pick
	{
		kSwitch,  // switched in now: the engine will pick theirs, or the clothes they got in with are on
		kPutOn,   // the item that makes the engine pick theirs is put on; switched in later
		kWait,    // switched in later
		kGiveUp,  // switched in now, the engine's pick not theirs (nothing makes it pick theirs, or PICK_WAIT is over)
	};

	constexpr Pick ChoosePick(const PickFacts& a_facts)
	{
		if (a_facts.named) {
			if (a_facts.picksTheirs) {
				return a_facts.sincePut >= DRESS_SETTLE ? Pick::kSwitch : Pick::kWait;
			}
			if (!a_facts.key || a_facts.waited >= PICK_WAIT) {
				return Pick::kGiveUp;
			}
			return !a_facts.keyOn && a_facts.sincePut >= KEY_GAP ? Pick::kPutOn : Pick::kWait;
		}
		if (a_facts.dressed) {
			return Pick::kSwitch;
		}
		return a_facts.waited >= PICK_WAIT ? Pick::kGiveUp : Pick::kWait;
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
	// in power armor with no frame, and their stand-in of us goes into a copy of STAND_IN_FRAME: see ChooseWornBase), a
	// friend's when the frame their stand-in would go into is no frame here (they named none, or one from a plugin this
	// game lacks). Told in the log once a wear, once it has stayed unknown this long: the armor goes on a moment before
	// the furniture is known, so the first status of every wear names no frame and the next one names it.
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

		// In power armor with the frame unknown for UNKNOWN_FRAME_AFTER or longer, as of the last Update.
		[[nodiscard]] bool Overdue(std::chrono::steady_clock::time_point a_now) const { return since && a_now - *since >= UNKNOWN_FRAME_AFTER; }

	private:
		std::optional<std::chrono::steady_clock::time_point> since;  // unknown since
		bool                                                 told = false;
	};

	// The frame a friend's stand-in goes into while their status says they are in power armor (RemotePlayers' WornFrame).
	// What the stand-in wears goes by what the engine has it in (ChooseDress): out of a frame, never a piece. So a friend
	// in power armor whose stand-in had no frame copy to get into was shown in the clothes under their suit, out of it to
	// every friend, until they got out and in again and their status named a frame. It came to that whenever the frame
	// they told was none (a game loaded with them already in the armor, before 0.9.2 read the worn frame: see WornSeat)
	// or one from a plugin this game lacks. The stand-in goes into a copy of a frame every game has instead: at once for a
	// frame they named that is none here (no wait makes it one), and for none once that has lasted UNKNOWN_FRAME_AFTER
	// (the first status of every wear names none, and the next one names it). Waiting a second on a named one too, a
	// friend who got out of a frame here and into one this game lacks was shown out of their armor for that second,
	// beside the copy of the frame kept from the last wear (Hear), drawn as an empty suit at their stand-in. Worn, a
	// frame's own base shows nothing (the engine disables it; the suit seen is the frame item and the pieces on the
	// actor), and a copy is emptied as it is placed. A frame they name later takes over as any change of frame does
	// (ChooseCopy).
	// The base of the museum's T-45 in Concord (Fallout4.esm), a frame in every game. Should a mod have made it no frame,
	// there is none to fall back on, as before.
	constexpr std::uint32_t STAND_IN_FRAME = 0x0019DED9;

	enum class WornBase
	{
		kNone,     // out of power armor, or in it with no frame known yet: no copy
		kTold,     // the frame they told, a frame here
		kStandIn,  // STAND_IN_FRAME: theirs named but none here, or none named for UNKNOWN_FRAME_AFTER
	};

	// a_on: their status says they are in power armor; a_named: they told a frame (kept, see Hear), not none;
	// a_toldHere: it is a frame here; a_overdue: theirs has not been one here for UNKNOWN_FRAME_AFTER in this wear
	// (UnknownFrame::Overdue); a_standInHere: STAND_IN_FRAME is a frame here.
	constexpr WornBase ChooseWornBase(bool a_on, bool a_named, bool a_toldHere, bool a_overdue, bool a_standInHere)
	{
		if (!a_on) {
			return WornBase::kNone;
		}
		if (a_toldHere) {
			return WornBase::kTold;
		}
		return (a_named || a_overdue) && a_standInHere ? WornBase::kStandIn : WornBase::kNone;
	}

	// What our player sits in now (Party): the reference and its base (0 when none), and whether the base is a frame.
	struct Seat
	{
		std::uint32_t ref = 0;
		std::uint32_t base = 0;
		bool          frame = false;
	};

	// The frame our player wears, as OwnWear learns it: the furniture they sit in (a_occupied) while that is a frame, as
	// it is when the armor goes on, else the frame the engine keeps as worn (a_held: the actor's ExtraPowerArmor,
	// which a save keeps). A game loaded with the player already in power armor (a save made in the armor, a reload, a
	// relaunch before hosting or joining) has them sitting in nothing: the frame was learnt only from what they sat in
	// as they got in, so friends were told a wear with no frame, and saw them out of their suit until they got out and
	// in again. Neither a frame: what they sit in, for the log.
	constexpr Seat WornSeat(const Seat& a_occupied, const Seat& a_held)
	{
		if (a_occupied.frame && a_occupied.base != 0) {
			return a_occupied;
		}
		if (a_held.frame && a_held.base != 0) {
			return a_held;
		}
		return a_occupied;
	}

	// Our own wear as friends are told it (Party::SampleStatus). The frame is the furniture we sat in as the armor
	// went on (loaded in the armor, the engine's worn frame: see WornSeat), remembered for as long as it stays on: the occupied furniture changes with every terminal or chair
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
			return Update(a_on, std::forward<F>(a_seat), [](std::uint32_t) { return 0u; }, a_now);
		}

		// a_spawnOf(ref) names a frame made at runtime that is a copy of a friend's spawn (Spawns::CopyIdOf): the
		// spawn's id, which every game resolves to its own copy, or the owner's game to its own frame (a frame lent to
		// us, FrameLoans). 0 for any other: a frame of our own making is none in another game.
		template <class F, class G>
		Told Update(bool a_on, F&& a_seat, G&& a_spawnOf, std::chrono::steady_clock::time_point a_now)
		{
			std::uint32_t seat = 0;
			if (!a_on) {
				frame = 0;
				frameRef = 0;
			} else if (frame == 0) {
				const Seat s = a_seat();
				if (s.frame && s.base != 0) {
					frame = s.base;
					frameRef = Protocol::IsShareableRef(s.ref) ? s.ref : static_cast<std::uint32_t>(a_spawnOf(s.ref));
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
		bool                 away = false;     // it was hidden as they got in: theirs stood parked somewhere else, and they got into it there
		float                fromStart = 0.0f; // how far it stands from where it stood before it was first moved here (0: never moved)
		std::optional<float> fromSpot;         // how far it stands from where they got in; none when that is another place
		// Another player's game runs it (NpcSync, FrameShare.h): it stands here where their game has it, however it got
		// there. Neither our player's last use of it nor the move that brought it here makes it our own suit.
		bool                 synced = false;
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
		kThere,      // (parked) ours stands where they left theirs: it stands for theirs, shown, and no copy is placed
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
		// A frame another game runs stands here where that game has it (synced): it is theirs wherever our player last
		// left it, and the move that brought it here is no use of it by our player. Where they get in is where their
		// game has it, so it is looked at afresh each time rather than left alone for the wear (moved), and it is hidden
		// wherever it stands, as the suit they wear: ours is moved to where their game has it as they got in, or is on
		// its way there (NpcSync brings it once that is in view), which is not where their stand-in was as their status
		// came for one who joined mid-wear (where they got in, in their own world, against their stand-in's first spot).
		// Compared with that, ours stood there shown for the whole wear, a second suit for one they wear.
		if (!a_facts.synced && a_facts.ours) {
			return Original::kOurs;
		}
		if (a_facts.inUse) {
			return Original::kInUse;
		}
		if (a_facts.quest) {
			return Original::kQuest;
		}
		if (!a_facts.synced && !(a_facts.fromStart <= START_TOLERANCE)) {
			return Original::kUsed;
		}
		// They took theirs away and left it parked elsewhere (ChooseParkedOriginal hid ours), and got in again there:
		// it is still the suit we hid, wherever they got in.
		if (a_facts.away) {
			return a_facts.loaded ? Original::kHide : Original::kNotLoaded;
		}
		if (!a_facts.synced && a_facts.moved) {
			return Original::kElsewhere;
		}
		if (!a_facts.loaded) {
			return Original::kNotLoaded;
		}
		if (!a_facts.synced && (!a_facts.fromSpot || !(*a_facts.fromSpot <= ORIGINAL_RADIUS))) {
			return Original::kElsewhere;
		}
		return Original::kHide;
	}

	// A friend wears a frame one game made and lent them (FrameLoans): their status names it by its spawn id, which our
	// game resolves to its own reference of it (Spawns::LocalOf): the owner's game to its real frame, every other game
	// to its copy of the spawn. That is hidden while they wear it, wherever it stands: it is the very suit they wear, and
	// the stand-in's copy shows it on them. None of the rules for a pre-placed one hold: it is no reference of our world
	// a player of ours could have found, moved or be sent for by a quest, and the owner's player, whose suit it is, is
	// the one who lent it. Only someone here in it (it was lent while nobody was) keeps it shown.
	constexpr Original ChooseLentOriginal(std::uint32_t a_frameBase, const OriginalFacts& a_facts)
	{
		if (!a_facts.found || a_frameBase == 0 || a_facts.base != a_frameBase) {
			return Original::kMissing;
		}
		if (!a_facts.frame) {
			return Original::kNotFrame;
		}
		if (a_facts.inUse) {
			return Original::kInUse;
		}
		return a_facts.loaded ? Original::kHide : Original::kNotLoaded;
	}

	// A frame a friend left standing: the one they got out of last, where their game has it now (Protocol::ParkedFrame).
	// A pre-placed frame is a reference every game has, and is shared as an NPC is (NpcSync, FrameShare.h): whoever
	// runs it says where it stands, and ours is moved there, so it stands on their rack in our game too, whether they
	// parked it before the session or got out of it in front of us. One made in their game is none in ours, and is
	// shared as a runtime NPC is (Spawns, FrameShare::SharesMade): our copy of it stands where their game has it. The
	// parked frame is for the time until either stands there: one their game has not reported yet, or one made there in
	// the seconds after they got out, before it is shared again. A copy is shown in its place then: in a power armor
	// station they parked it in, or wherever they got out and walked off. It used to be shown only while they wore it and
	// for a few seconds after, at the spot their stand-in stood as they got out (the exit steps the wearer out of the
	// frame, so that spot was some 76 units off the frame); then it was gone, and a friend's suit parked in a station
	// stood only in their game.

	// Our player's last frame, as Party::SampleStatus finds it (PlayerCharacter::lastUsedPowerArmor).
	struct LastFrame
	{
		bool          found = false;     // it exists and is not deleted
		bool          disabled = false;  // the engine disables a frame while it is worn
		bool          frame = false;     // its base is a power armor frame
		std::uint32_t ref = 0;
		std::uint32_t base = 0;
		std::uint32_t cell = 0;          // interior cell, or 0 outside
		std::uint32_t worldspace = 0;    // or 0 inside
		float         x = 0, y = 0, z = 0;
		float         heading = 0;
	};

	// What friends are told of it: only while we are out of power armor (in it, they show the frame with our stand-in),
	// and only once the engine has it standing in the world again (enabled where we got out).
	// Its place is told to the unit and its heading to a hundredth of a radian: a frame the physics nudges by a hair is not
	// news to send.
	inline std::optional<Protocol::ParkedFrame> TellParked(bool a_on, const LastFrame& a_last)
	{
		if (a_on || !a_last.found || a_last.disabled || !a_last.frame) {
			return std::nullopt;
		}
		const Protocol::ParkedFrame parked{
			.base = a_last.base,
			.ref = Protocol::IsShareableRef(a_last.ref) ? a_last.ref : 0,
			.cell = a_last.cell,
			.worldspace = a_last.worldspace,
			.x = std::round(a_last.x),
			.y = std::round(a_last.y),
			.z = std::round(a_last.z),
			.heading = std::round(a_last.heading * 100.0f) / 100.0f,
		};
		if (!parked.Valid()) {
			return std::nullopt;
		}
		return parked;
	}

	// A parked copy further than this from where they parked theirs, or turned further than PARK_TURN, is moved there;
	// one more than MOVE_LIMIT off is placed again.
	constexpr float PARK_FOLLOW = 1.0f;
	constexpr float PARK_TURN = 0.02f;  // radians

	struct ParkFacts
	{
		bool                told = false;       // they are out of power armor and tell a parked frame whose base is a frame here
		bool                inView = false;     // where it stands is in the area the engine draws around our player
		bool                oursThere = false;  // our own reference of that ID (a pre-placed frame) of that base stands there (ORIGINAL_RADIUS),
		                                        // or, for one made in their game, our copy of it that their game shares (Spawns)
		bool                taken = false;      // somebody here got into the copy we showed of this very frame there
		bool                exists = false;     // a parked copy of ours stands
		bool                sameBase = false;   // it is a copy of the base they tell
		bool                occupied = false;   // somebody here got into it
		bool                loaded = false;     // its 3D is in
		std::optional<bool> samePlace;          // it is in the place they parked theirs; none while it is still attaching
		float               distance = 0.0f;    // from where they parked theirs
		float               turn = 0.0f;        // from the way theirs faces, radians
		Duration            sincePlaced{};
		bool                leftCopy = false;   // the copy their stand-in left as they got out stands, of that base, nobody in it
	};

	enum class Park
	{
		kNone,     // nothing to do: none wanted, or its 3D is still coming
		kPlace,    // a copy is placed where they parked theirs
		kAdopt,    // the copy their stand-in left as they got out becomes the parked copy (moved there next)
		kShow,     // drawn where they parked theirs, solid once nobody stands in it
		kMove,     // moved onto where they parked theirs (they parked it a little way off, or it was the left copy)
		kReplace,  // in another place, far off, of another base or never loaded: deleted, placed again next update
		kDelete,   // no longer wanted (they got in, parked none, it is out of our sight, or ours stands there): deleted
		kAbandon,  // somebody here is in it: theirs now, neither moved nor deleted
		kDropLeft, // ours stands where they parked theirs: the copy their stand-in left as they got out goes at once
	};

	constexpr Park ChoosePark(const ParkFacts& a_facts)
	{
		if (a_facts.exists) {
			if (a_facts.occupied) {
				return Park::kAbandon;
			}
			if (!a_facts.told || !a_facts.inView || a_facts.oursThere) {
				return Park::kDelete;
			}
			if (!a_facts.sameBase || (a_facts.samePlace && !*a_facts.samePlace) || a_facts.distance > MOVE_LIMIT) {
				return Park::kReplace;
			}
			if (!a_facts.loaded) {
				return a_facts.sincePlaced > LOAD_LIMIT ? Park::kReplace : Park::kNone;
			}
			return a_facts.distance > PARK_FOLLOW || a_facts.turn > PARK_TURN ? Park::kMove : Park::kShow;
		}
		if (!a_facts.told || !a_facts.inView || a_facts.taken) {
			return Park::kNone;
		}
		// Ours stands for theirs (got in and out on the spot, as on the museum roof): the copy left where the stand-in
		// stepped out to, some 76 units off, would be a second, empty suit beside it for a few seconds.
		if (a_facts.oursThere) {
			return a_facts.leftCopy ? Park::kDropLeft : Park::kNone;
		}
		return a_facts.leftCopy ? Park::kAdopt : Park::kPlace;
	}

	// Our copy of a pre-placed frame a friend left parked (their ParkedFrame names it): the same suit, standing in two
	// places. Ours is hidden while theirs stands parked elsewhere, by the rules of a worn one (ChooseOriginal): not when
	// it is our player's own, someone here is in it, a quest of ours holds it, or it has been moved in our world. When
	// ours stands where they parked theirs, it stands for theirs (kThere): shown, and no copy is placed on it.
	// a_facts.fromSpot: how far ours stands from where they parked theirs.
	constexpr Original ChooseParkedOriginal(std::uint32_t a_ref, std::uint32_t a_frameBase, const OriginalFacts& a_facts)
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
		if (a_facts.fromSpot && *a_facts.fromSpot <= ORIGINAL_RADIUS) {
			return Original::kThere;
		}
		// Synced (see ChooseOriginal): theirs, wherever our player left it, and however it got where it stands.
		if (!a_facts.synced && a_facts.ours) {
			return Original::kOurs;
		}
		if (a_facts.inUse) {
			return Original::kInUse;
		}
		if (a_facts.quest) {
			return Original::kQuest;
		}
		if (!a_facts.synced && !(a_facts.fromStart <= START_TOLERANCE)) {
			return Original::kUsed;
		}
		if (!a_facts.loaded) {
			return Original::kNotLoaded;
		}
		return Original::kHide;
	}
}
