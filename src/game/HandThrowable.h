#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

// A friend's stand-in plays their throw with an empty hand (bead 2r3, docs/GRENADE-IN-HAND.md). The engine lists the
// grenade as equipped and hangs its model under the stand-in's LArm_Hand in a child named WeaponLeft, as on its own
// throws, but keeps that node (and the model in it) culled: on a throw it did not start itself it never draws them
// (seen on the rig, 2026-10-18). So at the replayed throw's weaponAttach(2) that culled WeaponLeft is drawn, and culled
// again at the release and on every other way out (WeaponFire.cpp does the scene graph work). These are the stand-in's
// own nodes, made for it by the engine: nothing is hung, cloned or shared. On by default (bGrenadeInHand); the dev's
// `handthrowable off` turns it off for the session.
// Only with the dev's `handthrowable model` does a hand with no WeaponLeft get the model the engine's model database
// keeps for the grenade, hung there itself (never a clone: round 3 cloned it and crashed in BSGeometry::ProcessClone);
// a node has one parent, so that model hangs on one stand-in at a time. The rig never showed a hand without a
// WeaponLeft, and the engine never draws the database's own node itself, so that stays an experiment. This is the
// bookkeeping, pure so it can be tested without the game.
namespace HandThrowable
{
	using Clock = std::chrono::steady_clock;

	// The node the engine hangs a grenade under on its own throws, and the name it gives what it hangs there.
	constexpr std::string_view HAND = "LArm_Hand";
	constexpr std::string_view ENGINE_NODE = "WeaponLeft";

	// Longest a model stays in the hand after the wind-up began: a release that never comes (the graph cut off by a
	// stagger, an event missed) leaves no grenade in the hand. ThrowReplays waits five seconds for the release; the
	// wind-up itself is about a second.
	constexpr Clock::duration MAX_HOLD = std::chrono::seconds{ 3 };
	constexpr std::size_t     MAX_HOLDS = 8;  // stand-ins holding one at once (a guard)

	// What one of the actor's animation events means for the model in its hand.
	enum class Cue : std::uint8_t
	{
		kNone,
		kAttach,   // weaponAttach(2): the wind-up puts the throwable (equip index 2) in the hand
		kRelease,  // WeaponFire(2), the grenade leaving the hand, or weaponDetach(2), the graph taking it away
	};

	constexpr Cue CueOf(std::string_view a_tag, std::string_view a_payload)
	{
		if (a_payload != "2") {
			return Cue::kNone;
		}
		if (a_tag == "weaponAttach") {
			return Cue::kAttach;
		}
		return a_tag == "WeaponFire" || a_tag == "weaponDetach" ? Cue::kRelease : Cue::kNone;
	}

	// Why the model is not hung at a weaponAttach(2) (kNone: it is).
	enum class Skip : std::uint8_t
	{
		kNone,
		kOff,           // turned off (bGrenadeInHand = false, or the dev's handthrowable off)
		kNotReplayed,   // not a friend's throw replayed here (an NPC's own throw, or one already let go of)
		kNotLoaded,     // the actor is gone, dead or has no 3D
		kNoHand,        // its 3D has no LArm_Hand node
		kEngineShows,   // the engine hung its own WeaponLeft there: nothing of ours is needed
		kNoThrowable,   // the engine lists no grenade or mine as equipped on it
		kHolding,       // it holds ours already
		kNoEngineNode,  // no culled WeaponLeft holding the grenade to draw, and the database's node is not allowed
		kModelBusy,     // that model hangs on another stand-in (one parent per node)
		kFull,          // MAX_HOLDS stand-ins hold one already
	};

	constexpr std::string_view Name(Skip a_skip)
	{
		switch (a_skip) {
		case Skip::kNone:
			return "hung";
		case Skip::kOff:
			return "off";
		case Skip::kNotReplayed:
			return "not a replayed throw";
		case Skip::kNotLoaded:
			return "not loaded";
		case Skip::kNoHand:
			return "no LArm_Hand";
		case Skip::kEngineShows:
			return "the engine shows its own";
		case Skip::kNoThrowable:
			return "no throwable equipped";
		case Skip::kHolding:
			return "holds ours already";
		case Skip::kNoEngineNode:
			return "no WeaponLeft of the engine's to draw";
		case Skip::kModelBusy:
			return "the model hangs elsewhere";
		case Skip::kFull:
			return "too many held";
		}
		return "?";
	}

	// Why a model in the hand comes off (kKeep: it stays).
	enum class Drop : std::uint8_t
	{
		kKeep,
		kOff,          // turned off (the dev's handthrowable off), or the database's node no longer allowed
		kGone,         // the actor is gone or dead, or its 3D was rebuilt (a cell change, a face or power armor rebuild)
		kMoved,        // the model is no longer under the hand it was hung under
		kEngineShows,  // the engine hung its own WeaponLeft meanwhile
		kGearChanged,  // the engine no longer lists that throwable as equipped on it
		kReleased,     // the graph let go of the grenade (or took it away)
		kTimedOut,     // MAX_HOLD passed with no release
		kDeleted,      // the stand-in is about to be deleted, or a save, a load or the session's end is coming
	};

	constexpr std::string_view Name(Drop a_drop)
	{
		switch (a_drop) {
		case Drop::kKeep:
			return "kept";
		case Drop::kOff:
			return "turned off";
		case Drop::kGone:
			return "3D gone or rebuilt";
		case Drop::kMoved:
			return "moved off the hand";
		case Drop::kEngineShows:
			return "the engine shows its own";
		case Drop::kGearChanged:
			return "throwable no longer equipped";
		case Drop::kReleased:
			return "released";
		case Drop::kTimedOut:
			return "timed out";
		case Drop::kDeleted:
			return "stand-in deleted, save, load or session end";
		}
		return "?";
	}

	// What the game shows of an actor at its weaponAttach(2), read on the main thread.
	struct AtAttach
	{
		bool          enabled = false;
		bool          replayed = false;     // a friend's throw replayed on it, not let go of yet (ThrowReplays)
		bool          loaded = false;       // the actor exists, lives and has its 3D
		bool          handFound = false;    // ... with an LArm_Hand node
		bool          engineShows = false;  // ... which has a child named WeaponLeft that is drawn
		bool          engineHides = false;  // ... or one holding the grenade's model, culled (a stand-in's: seen on the rig)
		std::uint32_t throwable = 0;        // the grenade or mine the engine lists as equipped on it (0: none)
		bool          modelFallback = false;  // the dev's handthrowable model: the database's node, for a hand with no WeaponLeft
	};

	// What the game shows of an actor holding our model, each frame.
	struct WhileHeld
	{
		bool          enabled = false;
		bool          loaded = false;         // the actor exists, lives, and its 3D is the one the model was hung in
		bool          underHand = false;      // the model's parent is the LArm_Hand it was hung under, still in that 3D
		bool          engineShows = false;    // that hand has a child named WeaponLeft that is drawn (not one we drew)
		std::uint32_t throwable = 0;          // the grenade or mine the engine lists as equipped on it (0: none)
	};

	struct Hold
	{
		std::uint32_t     throwable = 0;  // the weapon whose model hangs (its model's one node in the database)
		Clock::time_point since{};
		bool              released = false;
		bool              shared = true;  // the database's own node hangs (false: the stand-in's own WeaponLeft is drawn)
	};

	class Book
	{
	public:
		// Whether to hang the model on this actor now.
		[[nodiscard]] Skip ShouldHang(std::uint32_t a_actor, const AtAttach& a_seen) const
		{
			if (!a_seen.enabled) {
				return Skip::kOff;
			}
			if (!a_seen.replayed) {
				return Skip::kNotReplayed;
			}
			if (!a_seen.loaded) {
				return Skip::kNotLoaded;
			}
			if (!a_seen.handFound) {
				return Skip::kNoHand;
			}
			if (a_seen.engineShows) {
				return Skip::kEngineShows;
			}
			if (a_seen.throwable == 0) {
				return Skip::kNoThrowable;
			}
			if (holds.contains(a_actor)) {
				return Skip::kHolding;
			}
			if (!a_seen.engineHides && !a_seen.modelFallback) {
				return Skip::kNoEngineNode;
			}
			// Drawing a stand-in's own WeaponLeft shares nothing: only the database's node is one for every stand-in.
			if (!a_seen.engineHides && ModelBusy(a_seen.throwable)) {
				return Skip::kModelBusy;
			}
			if (holds.size() >= MAX_HOLDS) {
				return Skip::kFull;
			}
			return Skip::kNone;
		}

		// The model of this throwable is in the actor's hand from now: the database's own node hung there (a_shared), or
		// the engine's own WeaponLeft, culled until now, drawn.
		void Hung(std::uint32_t a_actor, std::uint32_t a_throwable, Clock::time_point a_now, bool a_shared = true) { holds[a_actor] = { a_throwable, a_now, false, a_shared }; }

		// The actor's graph let go of the grenade: its model comes off at the next check.
		void Released(std::uint32_t a_actor)
		{
			if (const auto it = holds.find(a_actor); it != holds.end()) {
				it->second.released = true;
			}
		}

		// Whether the model of this throwable hangs on some actor.
		[[nodiscard]] bool ModelBusy(std::uint32_t a_throwable) const
		{
			for (const auto& [actor, hold] : holds) {
				if (hold.shared && hold.throwable == a_throwable) {
					return true;
				}
			}
			return false;
		}

		// Whether the model in the actor's hand comes off now, and why. A way out of the hand is taken as soon as
		// any is seen: the toggle, the actor or its 3D going, the model moved, the engine's own showing, the gear
		// changing, the release, and last the time limit.
		[[nodiscard]] Drop Check(std::uint32_t a_actor, const WhileHeld& a_seen, Clock::time_point a_now) const
		{
			const auto it = holds.find(a_actor);
			if (it == holds.end()) {
				return Drop::kKeep;
			}
			const auto& hold = it->second;
			if (!a_seen.enabled) {
				return Drop::kOff;
			}
			if (!a_seen.loaded) {
				return Drop::kGone;
			}
			if (!a_seen.underHand) {
				return Drop::kMoved;
			}
			if (a_seen.engineShows) {
				return Drop::kEngineShows;
			}
			if (a_seen.throwable != hold.throwable) {
				return Drop::kGearChanged;
			}
			if (hold.released) {
				return Drop::kReleased;
			}
			return a_now - hold.since >= MAX_HOLD ? Drop::kTimedOut : Drop::kKeep;
		}

		void Forget(std::uint32_t a_actor) { holds.erase(a_actor); }

		[[nodiscard]] bool Holds(std::uint32_t a_actor) const { return holds.contains(a_actor); }

		[[nodiscard]] std::vector<std::uint32_t> Actors() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(holds.size());
			for (const auto& [actor, hold] : holds) {
				out.push_back(actor);
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return holds.size(); }

	private:
		std::unordered_map<std::uint32_t, Hold> holds;  // actor -> what hangs in its hand
	};
}
