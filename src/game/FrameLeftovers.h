#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// A power armor frame copy of ours that a save kept switched off: the frame a friend's stand-in wore, the one left where
// they got out, the one at their parked frame, our copy of a frame their game made. Every copy goes disabled first and
// is deleted a second later (RemotePlayers' graveyard), and none is meant to reach a save. Some did all the same,
// switched off: a copy disabled as a load came that the load did not free lingered, disabled and never deleted, for the
// rest of the run (SaveGuard's Lingers), and was written into a save once it stopped being provably ours
// (SaveStrays::Remains); a copy let go of to the game as someone's while it waited to be deleted was off the books for
// the load that came then (RemotePlayers' FrameInUse); older versions left more. Each session places its copy at the
// same spot again (where the friend parks their suit, or where their stand-in gets into it), so they pile up there,
// unseen, one more each time. The engine never lets go of a reference that is only disabled, so a save keeps them.
// So right after a load each disabled frame made at runtime is looked at, and one that is provably nobody's goes the way
// every copy of ours goes (RemotePlayers::Discard: deleted, and kept out of every save). Nobody's means all of:
// - made at runtime (0xFF......): never a frame a plugin placed;
// - disabled and not deleted already: what the game shows is never touched;
// - not persistent: the engine promotes a frame to persist as an actor gets into it (Actor::SwitchToPowerArmor), so
//   the suit of an NPC whose cell is not loaded (a raider, a settler, a companion away) stays persistent while they wear
//   it, wherever they are; every copy of ours is demoted as it is buried (Equipment::ReleasePowerArmorFrame);
// - nothing in it, nor anything its base would fill it with the first time it is looked into (a loot frame's pieces);
// - no quest alias holds it, and nothing links it or enables it: no quest, workshop or script of the game's has it;
// - our player is not in it and did not leave it last, and no actor in our game is in it or getting in;
// - it has no owner of its own, or its owner is one of our stand-ins' bases (the switch-in gives a copy the stand-in's
//   base for its owner, Equipment::SwitchIntoPowerArmor): the engine gives a frame its wearer's ownership as they get in,
//   so one our player ever used is theirs, and one a settler or companion used is theirs;
// - not lent to or by a friend (FrameLoans), and not one of the session's own copies (those are already looked after).
// The process lists are not filled at once after a load, so a frame found then is judged only once it has stood so
// for LOOK_AGAIN, with no loading screen up, and only while it is still the same form (address and base).
// Kept free of the game so the rules can be tested.
namespace FrameLeftovers
{
	struct Facts
	{
		bool frame = false;        // its base is a power armor frame
		bool created = false;      // made at runtime (0xFF......)
		bool disabled = false;
		bool deleted = false;      // marked for deletion already
		bool persistent = false;   // the reference's persistent flag (a worn frame is promoted)
		bool holdsItems = false;   // anything in it, or in its base's container while it was never looked into
		bool quest = false;        // a quest alias holds it
		bool linked = false;       // it has a linked reference or an enable parent
		bool worn = false;         // our player is in it or left it last, or an actor here is in it or getting in
		bool owned = false;        // it has an owner of its own that is not one of our stand-ins' bases
		bool lent = false;         // lent to or by a friend (FrameLoans)
		bool session = false;      // one of the session's copies now: placed this run, or on its way out
	};

	constexpr bool Leftover(const Facts& a_facts)
	{
		if (!a_facts.frame || !a_facts.created || !a_facts.disabled || a_facts.deleted) {
			return false;
		}
		if (a_facts.persistent || a_facts.holdsItems || a_facts.quest || a_facts.linked) {
			return false;
		}
		return !a_facts.worn && !a_facts.owned && !a_facts.lent && !a_facts.session;
	}

	// Facts::owned. a_hasOwner: it has an owner of its own (its ownership extra data). a_owner: that owner's form ID, when
	// it is a form in our game now; none when it cannot be told (gone, such as a stand-in's face copy no save keeps, or not
	// readable), and then the frame counts as someone's: only an owner known to be a stand-in's base (a_standInBase(id))
	// leaves it nobody's.
	template <class S>
	constexpr bool Owned(bool a_hasOwner, std::optional<std::uint32_t> a_owner, S&& a_standInBase)
	{
		return a_hasOwner && !(a_owner && a_standInBase(*a_owner));
	}

	constexpr auto        LOOK_AGAIN = std::chrono::seconds(5);
	constexpr std::size_t MAX_WATCHED = 256;  // a save holding more than this has its first ones looked at, the rest at its next load

	// The disabled frames made at runtime that a load brought, until each is judged.
	class Watch
	{
	public:
		using Clock = std::chrono::steady_clock;

		struct Entry
		{
			std::uint32_t     id = 0;
			std::uint32_t     base = 0;
			const void*       ref = nullptr;  // the form's address as found: only compared, never read through
			Clock::time_point foundAt{};
		};

		// A load: what was watched before it named the old world's forms.
		void Clear() { entries.clear(); }

		void Add(std::uint32_t a_id, std::uint32_t a_base, const void* a_ref, Clock::time_point a_now)
		{
			if (entries.size() >= MAX_WATCHED || std::ranges::any_of(entries, [&](const Entry& a_entry) { return a_entry.id == a_id; })) {
				return;
			}
			entries.push_back({ a_id, a_base, a_ref, a_now });
		}

		// The ones found LOOK_AGAIN ago or more, taken off the watch to be judged; none while a loading screen is up (the
		// world it shows next is not loaded yet).
		std::vector<Entry> TakeDue(Clock::time_point a_now, bool a_loading)
		{
			std::vector<Entry> due;
			if (a_loading) {
				return due;
			}
			std::erase_if(entries, [&](const Entry& a_entry) {
				if (a_now - a_entry.foundAt < LOOK_AGAIN) {
					return false;
				}
				due.push_back(a_entry);
				return true;
			});
			return due;
		}

		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		std::vector<Entry> entries;
	};

	// Whether the form under the id when it is judged is still the one found after the load.
	constexpr bool Same(const Watch::Entry& a_found, const void* a_ref, std::uint32_t a_base)
	{
		return a_ref != nullptr && a_ref == a_found.ref && a_base == a_found.base;
	}
}
