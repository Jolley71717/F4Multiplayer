#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A friend borrows a power armor frame our game made (one we found or looted, parked where they can reach it), or we
// borrow one of theirs: the rules, kept free of the game so they can be tested (FrameLoans plays them).
// The frame stays the owner's: it is a spawn their game runs (Spawns, FrameShare::SharesMade), and the loan is live
// session state the server keeps beside its spawns, never saved (Server.cpp). The owner's game reads the kit (its
// pieces and its fusion core) off the frame and sends it; the borrower's game puts copies of it into its copy of the
// frame, each named as the owner's ("Sam's T-60 Left Arm") so it never stacks with the borrower's own and can be found
// again. The borrower gets in and out through the engine's own activation. As they get out, every named item is
// gathered from the copy and from the borrower (one whose name was lost, by its base from the copy: TakeUnnamed), its
// condition read and the item removed, and the copy goes back to being the spawn's: the owner's game moves its frame
// to where they left it and wears its pieces down to what came back. A return can only lower a condition, and never
// takes anything of the owner's (AfterReturn).
namespace FrameLoan
{
	using Clock = std::chrono::steady_clock;

	// ---- The borrower's press -------------------------------------------------------------------------------

	enum class Press
	{
		kEnter,       // lent to us and filled: the engine gets our player in
		kAsk,         // ask its owner (kBorrowFrame)
		kWait,        // asked already, or lent and the kit not in yet: told so again
		kNotLending,  // its owner lends no frames (bLendPowerArmor off)
		kLent,        // lent to someone else now
		kOwnerGone,   // its owner is not in the session
		kBusy,        // we have asked for or borrowed another frame
	};

	struct PressFacts
	{
		bool ownerHere = false;    // its owner's status is in
		bool ownerLends = false;   // and says they lend
		bool ours = false;         // lent to us (kFrameLent named us)
		bool filled = false;       // our copy holds the kit
		bool asked = false;        // our ask for it is out, not answered and not timed out
		bool otherLoan = false;    // an ask or a loan of ours for another frame
		bool lentToOther = false;  // kFrameLent named someone else
	};

	constexpr Press ChoosePress(const PressFacts& a_facts)
	{
		if (a_facts.ours) {
			return a_facts.filled ? Press::kEnter : Press::kWait;
		}
		if (a_facts.asked) {
			return Press::kWait;
		}
		if (!a_facts.ownerHere) {
			return Press::kOwnerGone;
		}
		if (a_facts.lentToOther) {
			return Press::kLent;
		}
		if (!a_facts.ownerLends) {
			return Press::kNotLending;
		}
		if (a_facts.otherLoan) {
			return Press::kBusy;
		}
		return Press::kAsk;
	}

	// An ask the owner's game never answered (it was loading, say) is given up after this: the next press asks again.
	constexpr auto ASK_TIMEOUT = std::chrono::seconds(10);
	// A frame lent to us that our player never got into is given back after this, the kit taken out of it again.
	constexpr auto ENTER_TIMEOUT = std::chrono::seconds(60);
	// Out of it this long (not back in): the loan is over. The engine moves a frame to where its wearer stands as they
	// get out, and builds its 3D again, over a second or so.
	constexpr auto EXIT_SETTLE = std::chrono::seconds(2);

	// ---- The borrower's loan as it goes ---------------------------------------------------------------------

	enum class Stage
	{
		kNone,
		kAsked,  // kBorrowFrame sent
		kLent,   // kFrameLent named us: the kit is in our copy, our player not in it yet
		kWorn,   // our player is in it
		kOut,    // our player got out of it, EXIT_SETTLE not over yet
	};

	struct WearFacts
	{
		bool copyThere = false;  // our copy of the frame still exists
		bool in = false;         // our player is in our copy (seated in it, or wearing it)
		bool inArmor = false;    // our player is in power armor at all (Equipment::InPowerArmor)
	};

	enum class Step
	{
		kNone,
		kGiveUp,  // the ask timed out: forgotten
		kWorn,    // our player got in (or back in)
		kOut,     // our player got out: the settle starts
		kReturn,  // the loan is over: the kit gathered and the frame given back (or gone, ownerless)
	};

	// What the loan does next, from its stage (since a_since) and what our game shows of it. Our player in power armor
	// while our copy does not resolve (a cell changing under them) is taken as still in it: never out on that, nor given
	// back. Given back only after the settle, and only once they are out of power armor altogether (never gathered from
	// while a frame is on them).
	constexpr Step NextStep(Stage a_stage, Clock::duration a_since, const WearFacts& a_facts)
	{
		const bool unsure = a_facts.inArmor && !a_facts.copyThere;
		switch (a_stage) {
		case Stage::kAsked:
			return a_since >= ASK_TIMEOUT ? Step::kGiveUp : Step::kNone;
		case Stage::kLent:
			if (a_facts.in) {
				return Step::kWorn;
			}
			if (unsure) {
				return Step::kNone;
			}
			return !a_facts.copyThere || a_since >= ENTER_TIMEOUT ? Step::kReturn : Step::kNone;
		case Stage::kWorn:
			return a_facts.in || unsure ? Step::kNone : Step::kOut;
		case Stage::kOut:
			if (a_facts.in) {
				return Step::kWorn;
			}
			if (a_facts.inArmor) {
				return Step::kNone;  // in power armor again (ours, unseen, or another frame): given back once out of it
			}
			return a_since >= EXIT_SETTLE ? Step::kReturn : Step::kNone;
		case Stage::kNone:
			break;
		}
		return Step::kNone;
	}

	// After a load of a save made in a borrowed frame, the clean-up gathers from the copy and buries it once our player
	// has been out of it for EXIT_SETTLE (a_sinceOut; none while they are in it or in another frame): the engine's exit
	// runs on after the frame reads as empty (re-enabled, its 3D rebuilt, the player's own pieces moved back into it).
	// With no copy there is nothing to settle.
	constexpr bool CleanupDue(bool a_copy, std::optional<Clock::duration> a_sinceOut)
	{
		return !a_copy || (a_sinceOut && *a_sinceOut >= EXIT_SETTLE);
	}

	// Whether the spawn a_id is the one lent to us (a_ours, with a_stage): from the kit coming until the loan ends. Spawn id 0
	// is none, so a loan worn on past its session (which lets go of its id) holds no spawn of the next session.
	constexpr bool HoldsSpawn(Stage a_stage, std::uint32_t a_ours, std::uint32_t a_id)
	{
		return a_id != 0 && a_id == a_ours && a_stage != Stage::kNone && a_stage != Stage::kAsked;
	}

	// Whether our status tells the frame we got out of last as our parked one. Not a frame of ours lent out (it stands hidden
	// here), nor the copy lent to us (a_borrowedCopy): it is its owner's spawn, which their game and every other already
	// place and move, and a parked copy of it told as ours would stand beside it for the EXIT_SETTLE after we get out.
	constexpr bool TellsParked(bool a_lentOut, bool a_borrowedCopy)
	{
		return !a_lentOut && !a_borrowedCopy;
	}

	// One lent item as the borrower's game named it: what to gather at the return.
	struct Tag
	{
		std::uint8_t  slot = 0;
		std::uint32_t base = 0;
		std::string   name;  // the name our copy of it was given ("Sam's T-60 Left Arm")
		bool          core = false;

		bool operator==(const Tag&) const = default;
	};

	// Whether an item found in our copy or on our player is that lent item: of its base, and named as it was named.
	inline bool IsTagged(const Tag& a_tag, std::uint32_t a_base, std::string_view a_name)
	{
		return a_base == a_tag.base && !a_tag.name.empty() && a_name == a_tag.name;
	}

	// A condition as the wire carries it: 0..1, or -1 for none.
	inline float Health(float a_health)
	{
		return std::isfinite(a_health) && a_health >= 0.0f ? (std::min)(a_health, 1.0f) : -1.0f;
	}

	// One lent item found as the loan ends, with its condition then.
	struct Found
	{
		std::uint8_t slot = 0;
		float        health = -1.0f;
	};

	// The return's word on each lent item our copies were made of, in slot order: found, with its condition (the lowest,
	// should one be found twice), or not found: our game could not find it (dropped, sold, stored, a core used up, or a
	// copy that lost its name), and the owner keeps theirs as it is (AfterReturn). With our copy gone (a_copyGone:
	// unloaded with its cell, what was in it with it) one not found is left out: it went with the copy.
	inline std::vector<Protocol::ReturnedItem> ComposeReturn(const std::vector<Tag>& a_tags, const std::vector<Found>& a_found, bool a_copyGone = false)
	{
		std::vector<Protocol::ReturnedItem> out;
		for (const auto& tag : a_tags) {
			if (std::ranges::any_of(out, [&](const Protocol::ReturnedItem& a_item) { return a_item.slot == tag.slot; }) || tag.slot >= Protocol::MAX_LENT_ITEMS) {
				continue;
			}
			Protocol::ReturnedItem item{ .slot = tag.slot, .found = false, .health = -1.0f };
			for (const auto& found : a_found) {
				if (found.slot != tag.slot) {
					continue;
				}
				const float health = Health(found.health);
				if (!item.found) {
					item.found = true;
					item.health = health;
				} else if (health >= 0.0f && (item.health < 0.0f || health < item.health)) {
					item.health = health;
				}
			}
			if (!item.found && a_copyGone) {
				continue;
			}
			out.push_back(item);
		}
		std::ranges::sort(out, {}, &Protocol::ReturnedItem::slot);
		if (out.size() > Protocol::MAX_LENT_ITEMS) {
			out.resize(Protocol::MAX_LENT_ITEMS);
		}
		return out;
	}

	// The tags of a_tags of which no item was found (by slot).
	inline std::vector<Tag> NotFound(const std::vector<Tag>& a_tags, const std::vector<Found>& a_found)
	{
		std::vector<Tag> out;
		for (const auto& tag : a_tags) {
			if (std::ranges::none_of(a_found, [&](const Found& a_item) { return a_item.slot == tag.slot; })) {
				out.push_back(tag);
			}
		}
		return out;
	}

	// How many lent items of one base, not found by their names (a_unfound), are taken from our copy all the same, as items
	// of that base it still holds (a_inCopy). The copy was emptied before the kit went in, and nothing gets into it but off
	// our player: their own pieces, which may stand in for a lent one of the same base, and their own fusion cores the
	// engine moved in as the lent one ran down (a_reserved: what goes back to them, GiveRest), never taken. A copy left
	// behind would be a second of the owner's item, which they keep (AfterReturn).
	constexpr std::int32_t TakeUnnamed(std::int32_t a_unfound, std::int32_t a_inCopy, std::int32_t a_reserved)
	{
		return (std::max)(0, (std::min)(a_unfound, a_inCopy - (std::max)(a_reserved, 0)));
	}

	// ---- The owner's side -----------------------------------------------------------------------------------

	struct LendFacts
	{
		bool lends = false;     // bLendPowerArmor
		bool here = false;      // the spawn is a frame of ours, here (our game runs it)
		bool inUse = false;     // someone here is in it (our player among them)
		bool lentOut = false;   // lent to someone already
	};

	// The answer to a friend's ask (kLoanAsked): kNone lends it.
	constexpr Protocol::LoanRefusal ChooseLend(const LendFacts& a_facts)
	{
		if (!a_facts.lends) {
			return Protocol::LoanRefusal::kNotLending;
		}
		if (!a_facts.here || a_facts.inUse) {
			return Protocol::LoanRefusal::kUnavailable;
		}
		if (a_facts.lentOut) {
			return Protocol::LoanRefusal::kLent;
		}
		return Protocol::LoanRefusal::kNone;
	}

	// A stack in the frame's inventory, as the kit is picked from it.
	struct KitStack
	{
		bool piece = false;  // a power armor piece (Equipment::IsPowerArmorPiece)
		bool core = false;   // ammunition: the fusion core a frame runs on
		bool fits = false;   // its base and mods are ones every game names (shareable), and it has few enough mods for the wire
	};

	// Which stacks go with the frame (their indexes, in order): one of every piece, and one fusion core, the first. The
	// rest (anything else stored in a frame) stays in the owner's frame. Never more than the wire carries.
	inline std::vector<std::size_t> PickKit(const std::vector<KitStack>& a_stacks)
	{
		std::vector<std::size_t> out;
		bool                     core = false;
		for (std::size_t i = 0; i < a_stacks.size() && out.size() < Protocol::MAX_LENT_ITEMS; ++i) {
			const auto& stack = a_stacks[i];
			if (!stack.fits) {
				continue;
			}
			if (stack.piece) {
				out.push_back(i);
			} else if (stack.core && !core) {
				core = true;
				out.push_back(i);
			}
		}
		return out;
	}

	// The condition the owner's piece (or core) is left at after the return: the lower of what was lent and what came
	// back, so a borrower can only wear it down. None when nothing is to change: not found (see AfterReturn), no word
	// of its condition, or none lower. A lent item with no condition of its own (a_lent -1) is at its full condition.
	inline std::optional<float> HealthAfter(float a_lent, const Protocol::ReturnedItem& a_back)
	{
		if (!a_back.found || !std::isfinite(a_back.health) || a_back.health < 0.0f || a_back.health > 1.0f) {
			return std::nullopt;
		}
		const float lent = a_lent >= 0.0f ? (std::min)(a_lent, 1.0f) : 1.0f;
		return a_back.health < lent ? std::optional<float>{ a_back.health } : std::nullopt;
	}

	// What becomes of one lent item of the owner's as a return comes in: worn down when it came back lower (HealthAfter),
	// else kept as it is. Kept too when the return says nothing of it (the borrower's game never made its copy: a mod it
	// lacks) or says it was not found: "not found" is only our game failing to find a copy, never word that the borrower
	// kept one, and taking the owner's out on it lost every lent kit at its return (0 of 6 found on the rig, after the
	// borrower's copies were worn). A copy the borrower's game could not gather is theirs to give up (FrameLoans), never
	// the owner's to lose.
	enum class Back
	{
		kKeep,
		kWear,
	};

	struct AfterBack
	{
		Back  what = Back::kKeep;
		float health = -1.0f;  // kWear's

		bool operator==(const AfterBack&) const = default;
	};

	inline AfterBack AfterReturn(float a_lent, const Protocol::ReturnedItem* a_back)
	{
		if (!a_back) {
			return {};
		}
		if (const auto health = HealthAfter(a_lent, *a_back)) {
			return { .what = Back::kWear, .health = *health };
		}
		return {};
	}

	// Where the owner's frame is put as the loan ends: moved to where the borrower left it (kMove), or left where it
	// stood when lent (kStay): the loan ended with no return (the borrower left, or their game loaded), or the place
	// they left it has no form our game knows (a DLC we lack: FormTranslate gave it none).
	enum class Place
	{
		kStay,
		kMove,
	};

	constexpr Place ChoosePlace(const Protocol::FrameReturn& a_return)
	{
		return !a_return.ended && (a_return.cell != 0 || a_return.worldspace != 0) ? Place::kMove : Place::kStay;
	}

	// ---- After a load -------------------------------------------------------------------------------------

	// What a save made while our player wore a frame lent to them notes in its co-save (the LOAN record): the save holds
	// our copy of it, and the named items, and our player in it. Read back only with that save.
	struct Record
	{
		std::uint32_t    copy = 0;     // our copy's form ID
		std::uint32_t    base = 0;     // the frame's base
		std::uint32_t    spawnId = 0;  // the spawn it was a copy of
		std::uint32_t    owner = 0;    // whose it was
		std::vector<Tag> tags;

		bool operator==(const Record&) const = default;
	};

	constexpr std::size_t MAX_TAG_NAME = 255;

	inline std::vector<std::uint8_t> Encode(const Record& a_record)
	{
		std::vector<std::uint8_t> out;
		const auto word = [&](std::uint32_t a_value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<std::uint8_t>(a_value >> shift));
			}
		};
		word(a_record.copy);
		word(a_record.base);
		word(a_record.spawnId);
		word(a_record.owner);
		const auto count = (std::min)(a_record.tags.size(), Protocol::MAX_LENT_ITEMS);
		out.push_back(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& tag = a_record.tags[i];
			out.push_back(tag.slot);
			word(tag.base);
			out.push_back(static_cast<std::uint8_t>(tag.core ? 1 : 0));
			const auto length = (std::min)(tag.name.size(), MAX_TAG_NAME);
			out.push_back(static_cast<std::uint8_t>(length));
			for (std::size_t c = 0; c < length; ++c) {
				out.push_back(static_cast<std::uint8_t>(tag.name[c]));
			}
		}
		return out;
	}

	// None for bytes that are not one whole record.
	inline std::optional<Record> Decode(const std::vector<std::uint8_t>& a_bytes)
	{
		std::size_t at = 0;
		bool        ok = true;
		const auto byte = [&]() -> std::uint8_t {
			if (at >= a_bytes.size()) {
				ok = false;
				return 0;
			}
			return a_bytes[at++];
		};
		const auto word = [&]() {
			std::uint32_t value = 0;
			for (int shift = 0; shift < 32; shift += 8) {
				value |= static_cast<std::uint32_t>(byte()) << shift;
			}
			return value;
		};
		Record record;
		record.copy = word();
		record.base = word();
		record.spawnId = word();
		record.owner = word();
		const auto count = byte();
		if (!ok || count > Protocol::MAX_LENT_ITEMS) {
			return std::nullopt;
		}
		for (std::uint8_t i = 0; i < count && ok; ++i) {
			Tag tag;
			tag.slot = byte();
			tag.base = word();
			const auto core = byte();
			const auto length = byte();
			if (!ok || core > 1 || a_bytes.size() - at < length) {
				return std::nullopt;
			}
			tag.core = core == 1;
			tag.name.assign(reinterpret_cast<const char*>(a_bytes.data() + at), length);
			at += length;
			record.tags.push_back(std::move(tag));
		}
		if (!ok || at != a_bytes.size() || record.copy == 0 || record.base == 0) {
			return std::nullopt;
		}
		return record;
	}

	// What is done after loading a save with a LOAN record. The loan itself is over by then: the server ends it when our
	// game asks for the world again after a load, and the owner's frame stands at home in their game. So our player is
	// sent out of the copy, and the copy and every named item go, in or out of a session. Only the named items when the
	// form under the copy's id is no longer that frame (made at runtime, of its base): it is not ours to remove.
	enum class AfterLoad
	{
		kNothing,
		kItems,    // the named items go
		kAll,      // our player is sent out of the copy, then the copy and the named items go
	};

	constexpr AfterLoad ChooseAfterLoad(const Record& a_record, bool a_found, std::uint32_t a_base, bool a_created)
	{
		if (a_record.copy == 0) {
			return AfterLoad::kNothing;
		}
		if (!a_found || !a_created || a_base != a_record.base) {
			return a_record.tags.empty() ? AfterLoad::kNothing : AfterLoad::kItems;
		}
		return AfterLoad::kAll;
	}
}
