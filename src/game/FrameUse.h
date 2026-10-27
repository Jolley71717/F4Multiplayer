#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Why the engine turns our player away from a power armor frame with "You cannot use this at this time." (the game
// setting sFailedActivation). Read from disk in 1.11.240, where the setting's value is read in two places only:
// - TESFurniture::Activate (2198054) refuses a frame without a word when another actor uses or reserves its marker
//   (TESObjectREFR::HasUsersOtherThan with the player, 2202574). Past that, it calls the player's furniture use
//   (2232956, at +0x384) and returns true whatever that answers, so a refusal is seen at that call, not in Activate's
//   answer. The use asks TESObjectREFR::HasFreeMarker(true) (2202569: GetFirstFreeMarkerIndex with no actor and
//   reservations ignored, 2202570). When it finds none it shows sFailedActivation for anything that is not a workbench
//   or a terminal (a frame is neither), unless the frame is the one the engine last told the player was in use, which
//   gets sObjectInUse instead. It finds none when:
//   - the frame is not upright: the z of its up axis (TESObjectREFR::GetOrientation, from its angle for a non-actor)
//     is under InteractionUtils::QUprightAngleCosine (4796566), the cosine of the game setting fFurnitureUprightAngle
//     (10 degrees). A frame leaning more than 10 degrees, or with an angle that is not a number, cannot be got into;
//   - every enabled marker is used (ExtraUsedMarkers) by an actor that still exists, whoever it is: with no actor
//     asking, our own player counts too. A marker the engine still has as used by our player once they are out of the
//     frame is what HasUsersOtherThan lets through and HasFreeMarker refuses.
// - The player's furniture enter (2233039) shows it when no enter animation fits the player and the furniture, on
//   their race nor the power armor race (sPARequred or sPADisallowed when one fits the other race). For a human player
//   and a frame, Fallout4.esm's FurnitureInteractionEnter tree always has one (FurnitureEnterFromStand and its combat
//   twin between them cover in and out of combat), so this one is only logged (ENTER_MESSAGE_CALL).
// Kept free of the game so it can be tested; Equipment::FrameUseOf reads the facts.
namespace FrameUse
{
	// TESFurniture::Activate's call into the player's furniture use: where our player's refusals are seen, and where
	// what would refuse them is mended first.
	inline constexpr std::size_t USE_CALL = 0x384;

	// The player's furniture enter's one HUD message call: sFailedActivation, sPARequred or sPADisallowed.
	inline constexpr std::size_t ENTER_MESSAGE_CALL = 0x25F;

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

	// Upright as the engine asks it (comiss and jae): an up that is not a number is not upright.
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

	// What is done for our player pressing E on a frame the engine would turn them away from (Equipment's hook on the
	// call into the use), before the engine asks for a free marker: a frame the engine finds not upright is stood up
	// where it is, turned as it was; a seat the engine still has as our own player's while they are not in it is given
	// back, once they are in no furniture and no frame. Nobody else's seat is ever touched: theirs is refused without a
	// word before this, and is theirs. Anything else is left to the engine.
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

	// What else is in our player's way, besides what the engine asks of the frame's markers (Judge): the session's own
	// doings and the engine's silent states. None of these makes the game say "You cannot use this at this time.": the
	// prompt shows and E does nothing (the gate says whose the frame is on the HUD instead). Equipment::DescribeFrameUse
	// reads a frame's, Equipment::DescribePlayerUse our player's.
	struct Holds
	{
		bool keptOut = false;            // the session's gate keeps our player out: a copy of a friend's frame (RemotePlayers::KeepOutOfFrame)
		bool activationBlocked = false;  // its activation is blocked (ExtraFlags, as Papyrus's BlockActivation sets): Activate does nothing
		bool disabled = false;           // disabled: no prompt, nothing to press E on
		bool no3d = false;               // its 3D is not in
		bool activateOff = false;        // the Activate user event is off in an input layer: E does nothing; the console's activate still works
		bool inputBlocked = false;       // PlayerControls::blockPlayerInput: no input reaches the player
		bool playerBusy = false;         // our player is in furniture, getting in or out, or wears a frame: the engine takes no E then

		constexpr bool Any() const { return keptOut || activationBlocked || disabled || no3d || activateOff || inputBlocked || playerBusy; }
	};

	// Those that hold, by name, for the log and paframes.
	inline std::string Describe(const Holds& a_holds)
	{
		std::string out;
		const auto  add = [&](bool a_held, const char* a_what) {
			if (a_held) {
				out += out.empty() ? "" : ", ";
				out += a_what;
			}
		};
		add(a_holds.keptOut, "kept out by the session (a friend's frame)");
		add(a_holds.activationBlocked, "activation blocked");
		add(a_holds.disabled, "disabled");
		add(a_holds.no3d, "no 3D");
		add(a_holds.activateOff, "Activate input off");
		add(a_holds.inputBlocked, "player input blocked");
		add(a_holds.playerBusy, "our player busy in furniture or a frame");
		return out.empty() ? "nothing else holds" : out;
	}
}
