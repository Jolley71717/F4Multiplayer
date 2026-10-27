#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The local player's own inventory and equipment are theirs alone. What a friend's game sends (their gear for their
// stand-in, the gear of an NPC their game runs, an item they picked up, a container they looted) changes only the
// thing it is for here: never the local player, whose Pip-Boy, weapons and armor nothing from the session puts on,
// takes off, gives or takes away. Every change the session makes to an inventory or what an actor wears is judged here
// at the point it is made, whatever the code that asked for it already checked (a Nexus report had a host lose their
// Pip-Boy and weapons as a friend joined; that was a story mod's quest stage replayed, see Official.h, and nothing of
// the session may ever do the same). The exception is the player's own act put right: a take or put of theirs at a
// friend's shared storage that the server refused, or a take the host's chest held less of, is undone (Change::kStorageUndo,
// never on anything but our player, and only for a StorageView::PlayerChange, which only StorageView::Client makes).
// Another is the player's take of a gift from a container (Change::kGiftTake, GiftBook.h): what they took is held back from
// their inventory until the server grants it, then given, or taken out again if a friend's take came first.
// A third is a power armor frame a friend lent: copies of its pieces go into our copy of it, and are gathered back out of
// it and out of our player as they get out (Change::kLoan; only items named as the lent ones, FrameLoans).
// A move our loaded world lacks is put right in the true copy by the server, never on our player.
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
		// An item lying in the world that a player gave by dropping it (Drops): this game's copy of it placed, taken away when
		// someone takes it, and the dropper's own item swapped for one. Only ever a reference this game made, loose in the
		// world: the local player's take of one is their own (they pressed Take), granted by the server first.
		kDropCopy,
		// Shared storage (StorageView.h): our player's own move at a friend's storage undone (the server refused it, or the
		// host's chest held less than a take: a take given back, a put returned). A move our loaded world lacks is put right
		// in the true copy by the server, never here. The one change made to the local player, and only ever to them: what they
		// did themselves, put right. Nothing else goes to them (a friend's copy's own things are set aside in the copy while it
		// shows the true one, never moved to the player).
		kStorageUndo,
		// Gifts (GiftBook.h, the host's bContainerGifts): a copy of a weapon or armor given through a container, put in or taken out
		// of this game's own copy of that container, and the giver's own item taken out of it as it is given. A container's
		// inventory only: never an actor's, never our player's.
		kGiftCopy,
		// Our player's own take of a gift: the item they took held back from their inventory until the server's answer, given
		// them when it is theirs, or taken out again when a friend's take came first. Only ever our player, and only ever what
		// they took themselves (GiftBook::Client makes it).
		kGiftTake,
		// A power armor frame lent to our player (FrameLoans): copies of its owner's pieces and core put into our copy of the
		// frame, and gathered back out of it and out of our player as they get out (only items named as the lent ones), with a
		// fusion core of our player's own the frame used up given back to them. Our player, or a frame this game placed.
		kLoan,
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
		bool heldByPlayer = false;   // in no cell, and the engine names the local player as the inventory that holds it
		bool heldHere = false;       // in no cell, and the engine names another inventory here as its holder (a container's, an NPC's)
		bool container = false;      // a container's reference (its base is one), not an actor
	};

	enum class Verdict : std::uint8_t
	{
		kGo,
		kGone,         // no such reference here
		kOurPlayer,    // the local player: theirs alone
		kWrongActor,   // not the kind of actor the change is for (a stand-in's gear on an NPC, an NPC's on a stand-in)
		kNotCopy,      // a frame this game didn't place for the stand-in: someone's own, or the world's
		kOurFrame,     // the frame the local player is in
		kInInventory,     // a picked-up item that a container or an NPC here holds, not loose in the world
		kInOurInventory,  // a picked-up item that the local player holds: theirs alone
		kNotPlaced,       // a picked-up item in no cell and in no inventory the engine names: being loaded or moved, for now
		kCount,
	};

	constexpr Verdict Judge(Change a_change, const Target& a_target)
	{
		if (!a_target.exists) {
			return Verdict::kGone;
		}
		if (a_target.player) {
			return a_change == Change::kStorageUndo || a_change == Change::kGiftTake || a_change == Change::kLoan ? Verdict::kGo : Verdict::kOurPlayer;
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
			if (a_target.inWorld) {
				return Verdict::kGo;
			}
			if (a_target.heldByPlayer) {
				return Verdict::kInOurInventory;
			}
			return a_target.heldHere ? Verdict::kInInventory : Verdict::kNotPlaced;
		case Change::kContainer:
			return a_target.standIn ? Verdict::kWrongActor : Verdict::kGo;
		case Change::kDropCopy:
			if (!a_target.inWorld) {
				if (a_target.heldByPlayer) {
					return Verdict::kInOurInventory;
				}
				return a_target.heldHere ? Verdict::kInInventory : Verdict::kNotPlaced;
			}
			return a_target.madeHere ? Verdict::kGo : Verdict::kNotCopy;
		case Change::kStorageUndo:
			return Verdict::kWrongActor;  // only ever our player
		case Change::kGiftCopy:
			return a_target.container && !a_target.standIn && !a_target.mirrored ? Verdict::kGo : Verdict::kWrongActor;
		case Change::kGiftTake:
			return Verdict::kWrongActor;  // only ever our player
		case Change::kLoan:
			if (a_target.playersFrame) {
				return Verdict::kOurFrame;  // never while our player is in it
			}
			return a_target.madeHere && !a_target.standIn && !a_target.mirrored ? Verdict::kGo : Verdict::kNotCopy;
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
		case Change::kDropCopy:
			return "dropCopy";
		case Change::kStorageUndo:
			return "storageUndo";
		case Change::kGiftCopy:
			return "giftCopy";
		case Change::kGiftTake:
			return "giftTake";
		case Change::kLoan:
			return "loan";
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
		case Verdict::kInOurInventory:
			return "in our player's inventory";
		case Verdict::kNotPlaced:
			return "in no cell";
		case Verdict::kCount:
			break;
		}
		return "?";
	}

	// A friend's pickup refused only because the item is in no cell and in no inventory the engine names (a loose item
	// can report none for a moment: being loaded, moved, its cell detaching) is kept and tried again, for this long since
	// that was first seen; past it, it is dropped. Every other refusal is final: an item an inventory holds (the local
	// player's, a container's, an NPC's) is never taken out of it for a friend.
	inline constexpr std::chrono::seconds PICKUP_WAIT{ 30 };

	constexpr bool TryAgain(Verdict a_verdict, std::chrono::steady_clock::duration a_waited)
	{
		return a_verdict == Verdict::kNotPlaced && a_waited < PICKUP_WAIT;
	}

	// Whether a refusal shows the session about to change what it must never change (the local player and what they
	// hold, a stand-in's gear on an NPC or an NPC's on a stand-in, a frame nobody placed for a stand-in), rather than a
	// change that found nothing to do (gone, already in an inventory, in no cell), which a join's catch-up can bring by
	// the hundred.
	constexpr bool Alarming(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kOurPlayer:
		case Verdict::kWrongActor:
		case Verdict::kNotCopy:
		case Verdict::kOurFrame:
		case Verdict::kInOurInventory:
			return true;
		default:
			return false;
		}
	}

	// How many refusals are logged in a session: the alarming ones on a budget of their own, so the benign ones can't
	// use it up and hide them.
	struct LogBudget
	{
		static constexpr std::uint32_t ALARMING_LIMIT = 100;
		static constexpr std::uint32_t BENIGN_LIMIT = 50;

		enum class Log : std::uint8_t
		{
			kSkip,
			kLog,
			kLogLast,  // logged, and the last of its kind this session
		};

		std::uint32_t alarming = 0;
		std::uint32_t benign = 0;

		Log Take(Verdict a_verdict)
		{
			if (a_verdict == Verdict::kGo) {
				return Log::kSkip;
			}
			const bool alarm = Alarming(a_verdict);
			auto&      used = alarm ? alarming : benign;
			const auto limit = alarm ? ALARMING_LIMIT : BENIGN_LIMIT;
			if (used >= limit) {
				return Log::kSkip;
			}
			return ++used == limit ? Log::kLogLast : Log::kLog;
		}
	};

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
