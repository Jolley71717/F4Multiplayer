#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The local player's own inventory and equipment are theirs alone. What a friend's game sends (their gear for their
// stand-in, the gear of an NPC their game runs, an item they picked up, a container they looted) changes only the
// thing it is for here: never the local player, whose Pip-Boy, weapons and armor nothing from the session puts on,
// takes off, gives or takes away. Every change the session makes to an inventory or what an actor wears is judged here
// at the point it is made, whatever the code that asked for it already checked (a Nexus report had a host lose their
// Pip-Boy and weapons as a friend joined; that was a story mod's quest stage replayed, see Official.h, and nothing of
// the session may ever do the same).
// Kept free of the game so it can be tested; Equipment and WorldSync read the facts from the engine.
namespace OwnGear
{
	// What a change made for a friend's game is.
	enum class Change : std::uint8_t
	{
		kDressStandIn,  // a friend's stand-in dressed as their player is (Equipment::Apply, WearOnStandIn, WearInArmor, PutOnForPick, HoldThrowable)
		kDressCopy,     // an NPC this game mirrors for a friend's game dressed as their copy is (Equipment::Wear, HoldThrowable)
		kEmptyFrame,    // a power armor frame emptied for a stand-in to get into (a copy placed for it, SwitchIntoPowerArmor)
		kTakeLoose,     // a loose item a friend picked up, taken out of this world too (WorldSync's pickups)
		kContainer,     // what a friend took from or put into a container or a corpse (the shared loot)
		kCount,
	};

	// The reference the change is for, as the engine has it.
	struct Target
	{
		bool exists = true;          // it is a reference here at all
		bool player = false;         // the local player
		bool standIn = false;        // a friend's stand-in (Puppets::IsPuppet)
		bool mirrored = false;       // an NPC this game runs as a friend's copy (a puppet of the NPC kind)
		bool madeHere = false;       // made by this game at runtime (FF......): a copy placed for a stand-in
		bool playersFrame = false;   // the power armor frame the local player is in
		bool inWorld = true;         // in a cell: a reference an inventory holds (a persistent one, picked up) has none
	};

	enum class Verdict : std::uint8_t
	{
		kGo,
		kGone,         // no such reference here
		kOurPlayer,    // the local player: theirs alone
		kWrongActor,   // not the kind of actor the change is for (a stand-in's gear on an NPC, an NPC's on a stand-in)
		kNotCopy,      // a frame this game didn't place for the stand-in: someone's own, or the world's
		kOurFrame,     // the frame the local player is in
		kInInventory,  // a picked-up item that is in an inventory here (ours, or a container's), not loose in the world
		kCount,
	};

	constexpr Verdict Judge(Change a_change, const Target& a_target)
	{
		if (!a_target.exists) {
			return Verdict::kGone;
		}
		if (a_target.player) {
			return Verdict::kOurPlayer;
		}
		switch (a_change) {
		case Change::kDressStandIn:
			return a_target.standIn ? Verdict::kGo : Verdict::kWrongActor;
		case Change::kDressCopy:
			return a_target.mirrored && !a_target.standIn ? Verdict::kGo : Verdict::kWrongActor;
		case Change::kEmptyFrame:
			if (a_target.playersFrame) {
				return Verdict::kOurFrame;
			}
			return a_target.madeHere ? Verdict::kGo : Verdict::kNotCopy;
		case Change::kTakeLoose:
			return a_target.inWorld ? Verdict::kGo : Verdict::kInInventory;
		case Change::kContainer:
			return a_target.standIn ? Verdict::kWrongActor : Verdict::kGo;
		case Change::kCount:
			break;
		}
		return Verdict::kWrongActor;
	}

	constexpr std::string_view Name(Change a_change)
	{
		switch (a_change) {
		case Change::kDressStandIn:
			return "dressStandIn";
		case Change::kDressCopy:
			return "dressCopy";
		case Change::kEmptyFrame:
			return "emptyFrame";
		case Change::kTakeLoose:
			return "takeLoose";
		case Change::kContainer:
			return "container";
		case Change::kCount:
			break;
		}
		return "?";
	}

	constexpr std::string_view Name(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kGo:
			return "go";
		case Verdict::kGone:
			return "gone";
		case Verdict::kOurPlayer:
			return "our player";
		case Verdict::kWrongActor:
			return "not the actor it is for";
		case Verdict::kNotCopy:
			return "not a copy placed here";
		case Verdict::kOurFrame:
			return "our player's frame";
		case Verdict::kInInventory:
			return "in an inventory, not loose";
		case Verdict::kCount:
			break;
		}
		return "?";
	}

	// What the session's inventory and equipment changes came to, for `net status`: refused ones by verdict.
	struct Tally
	{
		std::uint32_t refused[static_cast<std::size_t>(Verdict::kCount)]{};
		std::uint32_t ourPlayer = 0;  // refused for the local player, whatever the change (the one that must stay 0)

		// Counts a verdict; true when the change goes ahead.
		bool Count(Verdict a_verdict)
		{
			if (a_verdict == Verdict::kGo) {
				return true;
			}
			++refused[static_cast<std::size_t>(a_verdict)];
			if (a_verdict == Verdict::kOurPlayer) {
				++ourPlayer;
			}
			return false;
		}

		[[nodiscard]] std::uint32_t Refused() const
		{
			std::uint32_t total = 0;
			for (std::size_t i = 1; i < static_cast<std::size_t>(Verdict::kCount); ++i) {
				total += refused[i];
			}
			return total;
		}
	};
}
