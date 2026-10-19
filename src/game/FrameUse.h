#pragma once

#include <cstdint>

// Why the engine turns our player away from a power armor frame with "You cannot use this at this time." (the game
// setting sFailedActivation). Read from disk in 1.11.240: TESFurniture::Activate (2198054) refuses a frame silently
// when another actor uses or reserves its marker (TESObjectREFR::HasUsersOtherThan with the player, 2202574); past that,
// the player's own furniture use (2232956) asks TESObjectREFR::HasFreeMarker(true) (2202569, which is
// GetFirstFreeMarkerIndex(no actor, reservations ignored), 2202570) and shows that message when it finds none. That
// finds none when:
// - the frame is not upright: the up axis of its orientation (TESObjectREFR::GetOrientation, from its angle) is further
//   from straight up than InteractionUtils::QUprightAngleCosine (4796566) allows. A frame knocked onto its back or side
//   cannot be got into;
// - every enabled marker is used (ExtraUsedMarkers) by an actor that still exists, whoever it is: with no actor asking,
//   our own player counts too. A marker the engine still has as used by our player once they are out of the frame is
//   what HasUsersOtherThan lets through and HasFreeMarker refuses.
// Kept free of the game so it can be tested; Equipment::FrameUseOf reads the facts.
namespace FrameUse
{
	struct Facts
	{
		float         up = 1.0f;          // the up axis's z (cos of the tilt), from its angle
		float         upMin = 0.0f;       // the least the engine lets a frame be got into at
		int           markers = 0;        // enabled markers
		int           used = 0;           // of them, used by an actor that exists (ExtraUsedMarkers)
		std::uint32_t user = 0;           // the first such user
		int           userMarker = -1;    // the marker it uses
		bool          userIsPlayer = false;
		bool          userIn = false;     // that user is in it now: it occupies it, or the engine has it in this frame
		bool          playerBusy = false; // our player sits in furniture, gets in or out of some, or wears a frame
		bool          othersThanPlayer = false;  // HasUsersOtherThan(player): TESFurniture::Activate refuses silently then
		bool          free = true;        // HasFreeMarker(true), as the engine asks it
	};

	enum class Verdict
	{
		kFree,       // our player can get in
		kOtherUser,  // someone else uses or reserves it: refused silently (TESFurniture::Activate)
		kTilted,     // not upright: "You cannot use this at this time"
		kStaleUse,   // its marker is used by an actor that is not in it: "You cannot use this at this time"
		kInUse,      // its marker is used by an actor in it (our player, getting in or in it)
		kNoMarker,   // no enabled marker
	};

	constexpr bool Upright(const Facts& a_facts)
	{
		return a_facts.up >= a_facts.upMin;
	}

	constexpr Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.othersThanPlayer) {
			return Verdict::kOtherUser;
		}
		if (a_facts.free) {
			return Verdict::kFree;
		}
		if (!Upright(a_facts)) {
			return Verdict::kTilted;
		}
		if (a_facts.markers == 0) {
			return Verdict::kNoMarker;
		}
		if (a_facts.used > 0 && !a_facts.userIn) {
			return Verdict::kStaleUse;
		}
		return a_facts.used > 0 ? Verdict::kInUse : Verdict::kFree;
	}

	// What is done for our player pressing E on a frame the engine would turn them away from (Equipment's activation
	// hook), before the engine is asked: a seat the engine still has as our own player's while they are not in it is
	// given back, once they are in no furniture and no frame (nobody else's is ever touched: theirs is refused without a word anyway, and is theirs); a frame the
	// engine finds not upright is stood up where it is, turned as it was. Anything else is left to the engine.
	struct Mend
	{
		bool standUp = false;
		bool freeSeat = false;

		constexpr bool Any() const { return standUp || freeSeat; }
	};

	constexpr Mend ChooseMend(const Facts& a_facts)
	{
		if (a_facts.othersThanPlayer || a_facts.free) {
			return {};
		}
		return { .standUp = !Upright(a_facts), .freeSeat = a_facts.used > 0 && a_facts.userIsPlayer && !a_facts.userIn && !a_facts.playerBusy };
	}

	constexpr const char* Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kFree:
			return "free";
		case Verdict::kOtherUser:
			return "another actor uses or reserves it";
		case Verdict::kTilted:
			return "not upright";
		case Verdict::kStaleUse:
			return "its marker is used by an actor not in it";
		case Verdict::kInUse:
			return "in use";
		case Verdict::kNoMarker:
			return "no enabled marker";
		}
		return "-";
	}
}
