#pragma once

#include "Protocol.h"
#include "game/DropBook.h"
#include "game/LootScope.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Giving items through a container (the host's bContainerGifts, Protocol::SessionRules::containerGifts): a weapon or
// armor our player puts in a container is a gift, a drop whose place is that container (Protocol::DroppedItem::holder).
// Everything the server does for a dropped item it does for a gift (common/DropStore.h): it is offered to its giver's game
// first, lies in the session once that game says the item left its world, is granted to whoever takes it first, and a
// world's record after each welcome and load makes the session agree with it (DropBook::Record, Drops' co-save record).
//
// - Giving (only under the host's rule: off, a container no gift lies in isn't even watched). A put into the container
//   whose menu is open (WorldSync's event, Moved) that left one more of a weapon or armor
//   in it than we knew of is reported, one item at a time (each with a nonce of ours). On the server's offer the item
//   leaves our world: taken out of the container, or, while the menu still shows it, kept there as our copy of the gift,
//   so nothing changes on screen. The record says it left (Made), and kPlaced goes. An item no longer there (our player
//   took it back first), or a container not loaded now, is kept (kKept): it never lay anywhere. Nothing is ever given
//   back on a timeout or a link that went: the server keeps a gift's offer until our next record says whether the item
//   left our world (DropStore::DropOffers).
// - Showing. While our player has a container's menu open, each gift lying in it shows there as our copy (one item of
//   it, with its mods, condition and name), the giver's own included. A copy goes as the menu closes, as the gift goes,
//   and before every save and load: no save holds one.
// - Taking. Fewer in the container than we knew of, of a class a copy is, that went to our player (their take's event
//   said so, or, the event lost, their count of the class rose), took that copy: it comes out of our player's inventory
//   at once (they have it only once the server says so), and is claimed (kTake, asked again until answered, never settled
//   by a timeout). Granted to us, it goes into our player's inventory and the record says we took it; granted to someone
//   else, nothing needs undoing. So the side our player can't hide an item from (the take we hold back) is the one put
//   right, never an item already in their inventory. A copy gone anywhere else (another mod's script) isn't shown again
//   until a load or a rejoin, and a take back of a class whose put waits for its offer takes that put back (its offer is
//   kept).
// - A reload settles the rest: a friend's save from before their take lacks the item, so it lies there again; ours from
//   before the put has it in the container, so the gift is withdrawn. The one case left is a giver's save from before the
//   put loaded after a friend took it: both have it (counted on the server, dupRisk), as for a dropped item.
//
// Only weapons and armor: shared storage and the container loot never share them (LootScope's kWeaponArmor), so a gift
// never meets their copies. Items of one class (base, mods, name, condition to the percent) are the same thing: a copy
// and our player's own of that class are told apart by count alone. Kept free of the game so it can be tested; Gifts.cpp
// is its engine.
namespace GiftBook
{
	using Clock = std::chrono::steady_clock;

	inline constexpr std::uint32_t PLAYER = Protocol::PLAYER_REF_ID;
	inline constexpr std::size_t   MAX_PUTS = 64;                         // gifts reported and not yet offered
	inline constexpr auto          PUT_FORGET = std::chrono::seconds(60);  // an offer later than this is kept (never lay anywhere)
	inline constexpr std::size_t   MAX_COPIES = 64;                       // copies shown in one container at once
	inline constexpr std::size_t   MAX_CLAIMS = 128;                      // takes waiting for their answer, with the copies shown
	inline constexpr auto          CLAIM_RETRY = std::chrono::seconds(5);
	inline constexpr auto          SIGNAL_KEEP = std::chrono::seconds(1);  // a move's event waits this long for its change

	// A container a gift may be put in: not a workbench (each player's own), a vendor's or a companion's, nor an actor, a
	// power armor frame or anything made at runtime (no other game has it).
	constexpr bool GiftPlace(const LootScope::HolderFacts& a_holder)
	{
		return a_holder.kind == LootScope::Holder::kContainer && !a_holder.workshop && !a_holder.vendorChest && !a_holder.companion;
	}

	constexpr bool GiftKind(LootScope::Item a_kind)
	{
		return a_kind == LootScope::Item::kWeapon || a_kind == LootScope::Item::kArmor;
	}

	// One stack of a weapon or armor in an inventory, as the engine has it.
	struct Stack
	{
		std::uint32_t                  base = 0;
		LootScope::Item                kind = LootScope::Item::kWeapon;
		std::int32_t                   count = 0;
		std::uint16_t                  templateIndex = 0xFFFF;  // StandInGear::NO_TEMPLATE: none named
		std::vector<Protocol::ItemMod> mods;
		float                          health = -1.0f;  // 0..1, or -1: none
		std::string                    name;            // the player's name for it; empty: the game's own
		bool                           quest = false;   // it fills a quest alias
		bool                           equipped = false;
		bool                           lent = false;    // a copy of a piece a friend lent our player with a frame (FrameLoans): theirs, never given
	};

	// What tells items of a base apart: two of one class are the same thing.
	struct Class
	{
		std::uint32_t              base = 0;
		std::vector<std::uint32_t> mods;  // sorted
		std::string                name;
		std::int16_t               health = -1;  // percent, or -1: none

		auto operator<=>(const Class&) const = default;
		bool operator==(const Class&) const = default;
	};

	inline std::int16_t Percent(float a_health)
	{
		if (!std::isfinite(a_health) || a_health < 0.0f) {
			return -1;
		}
		return static_cast<std::int16_t>(std::lround(std::clamp(a_health, 0.0f, 1.0f) * 100.0f));
	}

	inline Class ClassOf(const Stack& a_stack)
	{
		Class out{ .base = a_stack.base, .name = std::string(Protocol::Utf8Prefix(a_stack.name, Protocol::MAX_ITEM_NAME_LENGTH)), .health = Percent(a_stack.health) };
		for (const auto& mod : a_stack.mods) {
			out.mods.push_back(mod.mod);
		}
		std::ranges::sort(out.mods);
		return out;
	}

	using Counts = std::map<Class, std::int64_t>;

	inline Counts Count(const std::vector<Stack>& a_stacks)
	{
		Counts out;
		for (const auto& stack : a_stacks) {
			if (stack.count > 0) {
				out[ClassOf(stack)] += stack.count;
			}
		}
		return out;
	}

	inline std::int64_t CountOf(const Counts& a_counts, const Class& a_class)
	{
		const auto it = a_counts.find(a_class);
		return it != a_counts.end() ? it->second : 0;
	}

	// The stack (its place in a_stacks) to take one of a_class from: one not worn first.
	inline std::optional<std::uint32_t> Pick(const std::vector<Stack>& a_stacks, const Class& a_class)
	{
		std::optional<std::uint32_t> worn;
		for (std::uint32_t i = 0; i < a_stacks.size(); ++i) {
			if (a_stacks[i].count <= 0 || ClassOf(a_stacks[i]) != a_class) {
				continue;
			}
			if (!a_stacks[i].equipped) {
				return i;
			}
			worn = worn ? worn : std::optional<std::uint32_t>{ i };
		}
		return worn;
	}

	// Whether the item may be given: a weapon or armor every game can have as it is (DropBook::Judge), whose mods the
	// wire carries. Never a copy of a friend's lent power armor piece (Stack::lent): as for a drop (Drops), it stays in our
	// copy of the container, and its owner's game takes theirs out at the return.
	inline bool Givable(const Stack& a_stack)
	{
		if (a_stack.lent) {
			return false;
		}
		DropBook::ItemFacts facts{ .kind = a_stack.kind, .questItem = a_stack.quest, .runtimeBase = (a_stack.base >> 24) == 0xFF, .count = 1 };
		facts.runtimeMod = std::ranges::any_of(a_stack.mods, [](const Protocol::ItemMod& a_mod) { return (a_mod.mod >> 24) == 0xFF; });
		return GiftKind(a_stack.kind) && a_stack.mods.size() <= Protocol::MAX_ITEM_MODS && DropBook::Judge(facts) == DropBook::Givable::kGive;
	}

	// Where a container is, for the gift's place.
	struct Place
	{
		std::uint32_t cell = 0;        // interior cell, or 0 outside
		std::uint32_t worldspace = 0;  // or 0 inside
		float         x = 0, y = 0, z = 0;
	};

	// One item of a_stack, put in a_holder at a_place: the gift as reported (the server numbers it).
	inline Protocol::DroppedItem ItemOf(const Stack& a_stack, std::uint32_t a_holder, const Place& a_place, std::uint64_t a_nonce)
	{
		Protocol::DroppedItem item;
		item.nonce = a_nonce;
		item.base = a_stack.base;
		item.count = 1;
		item.templateIndex = a_stack.templateIndex;
		item.mods = a_stack.mods;
		item.health = a_stack.health >= 0.0f && std::isfinite(a_stack.health) ? std::clamp(a_stack.health, 0.0f, 1.0f) : -1.0f;
		item.name = Protocol::Utf8Prefix(a_stack.name, Protocol::MAX_ITEM_NAME_LENGTH);
		item.cell = a_place.cell;
		item.worldspace = a_place.worldspace;
		item.x = a_place.x;
		item.y = a_place.y;
		item.z = a_place.z;
		item.holder = a_holder;
		return item;
	}

	enum class Notice : std::uint8_t
	{
		kGiven,       // the first gift of a session lies there: friends can take it, and so can we until they do
		kNotGiven,    // a weapon or armor that can't be given stays in our copy (a quest item, a runtime mod; the session full)
		kTakenFirst,  // a friend took it first: our player has nothing
		kCount,
	};

	// What the rules need of the game: Gifts.cpp in the game, a test's world in the tests. The changes it makes are its own:
	// WorldSync's event watch leaves them out.
	class Engine
	{
	public:
		virtual ~Engine() = default;

		// The weapon and armor stacks a_ref holds (of a_base; 0: of every base), in the engine's order; nothing if a_ref
		// isn't here or its inventory isn't made. PLAYER: our player's.
		virtual std::optional<std::vector<Stack>> Stacks(std::uint32_t a_ref, std::uint32_t a_base) = 0;
		// One item out of a_ref's stack a_stack of a_base (its place as Stacks lists them): whether one went.
		virtual bool Remove(std::uint32_t a_ref, std::uint32_t a_base, std::uint32_t a_stack) = 0;
		// One a_item into a_ref (a container: a copy of a gift; PLAYER: our player's take, granted): whether it went in.
		virtual bool Add(std::uint32_t a_ref, const Protocol::DroppedItem& a_item) = 0;
		virtual LootScope::HolderFacts Holder(std::uint32_t a_ref) = 0;
		virtual std::optional<Place>   PlaceOf(std::uint32_t a_ref) = 0;
		// Our game has the item's base and every mod on it.
		virtual bool Placeable(const Protocol::DroppedItem& a_item) = 0;
		// What our game's form a_base is: a weapon or an armor, or nothing (neither, or no such form here).
		virtual std::optional<LootScope::Item> KindOf(std::uint32_t a_base) = 0;
		virtual void Send(std::vector<std::uint8_t> a_packet) = 0;
		virtual void Notify(Notice a_notice) = 0;
		virtual std::uint64_t     NewNonce() = 0;
		virtual Clock::time_point Now() = 0;

		// Our world's record of the session's transfers (Drops' co-save record: DropBook::Record).
		virtual void Made(std::uint64_t a_nonce) = 0;
		virtual bool HasMade(std::uint64_t a_nonce) = 0;
		virtual void Took(std::uint32_t a_dropId) = 0;
		virtual bool HasTaken(std::uint32_t a_dropId) = 0;
	};

	struct Stats
	{
		std::uint32_t views = 0;        // container menus watched
		std::uint32_t puts = 0;         // weapons and armor our player put in one
		std::uint32_t notGiven = 0;     // of those, not to be given (DropBook::Judge, or no place)
		std::uint32_t overflow = 0;     // not given: MAX_PUTS waited for their offers
		std::uint32_t reported = 0;
		std::uint32_t refused = 0;      // the server kept none (the session full, or the host's rule off)
		std::uint32_t given = 0;        // offers taken out of our world (kPlaced)
		std::uint32_t kept = 0;         // offers whose item was gone from the container (kKept)
		std::uint32_t lateOffers = 0;   // offers of ours we no longer waited for (kKept)
		std::uint32_t shown = 0;        // copies put in a container
		std::uint32_t unplaceable = 0;  // gifts whose copy our game can't make (a base or mod it lacks)
		std::uint32_t notGear = 0;      // gifts that aren't a weapon or armor our game has, nor one a gift may be (a modified game's): never shown
		std::uint32_t addUndone = 0;    // items an add that failed left in an inventory anyway, taken out again
		std::uint32_t copyMissing = 0;  // copies gone from their container but not to our player (not shown again until a load or a rejoin)
		std::uint32_t taken = 0;        // copies our player took (claimed)
		std::uint32_t unsignaled = 0;   // of those, seen by their count alone (the move's event never came)
		std::uint32_t unheld = 0;       // of those, not found in their inventory to hold back until the answer
		std::uint32_t received = 0;     // takes granted to us
		std::uint32_t giveFailed = 0;   // granted, but the item couldn't go into our player's inventory (it lies there again)
		std::uint32_t takenFirst = 0;   // our take lost to someone else's
		std::uint32_t givenBack = 0;    // granted to a take our world no longer makes (after a load): lies there again
		std::uint32_t takenBack = 0;    // a take lost that we couldn't hold back: taken out of our player's inventory after all
		std::uint32_t dupRisk = 0;      // ... and not there to take: both have it
	};

	// A gift as it reached us may be shown: one weapon or armor of our game's, whose mods are plugins' (a modified game
	// can report anything: the server can't tell a gun from a box of ammo).
	inline bool Receivable(const Protocol::DroppedItem& a_drop, std::optional<LootScope::Item> a_kind)
	{
		return a_kind && GiftKind(*a_kind) && a_drop.count == 1 && Protocol::IsShareableRef(a_drop.base) && a_drop.mods.size() <= Protocol::MAX_ITEM_MODS &&
		       std::ranges::none_of(a_drop.mods, [](const Protocol::ItemMod& a_mod) { return (a_mod.mod >> 24) == 0xFF; });
	}

	// Our player's moves between their inventory and a container, as the event watch saw them, until the main thread reads
	// them: summed by container and item, so a Take All of a thousand kinds is a thousand entries and no move is ever left
	// out (Gifts.cpp keeps only the moves at the container our menu shows). Not locked: Gifts.cpp holds its lock.
	class Inbox
	{
	public:
		static constexpr std::size_t MAX_KINDS = 8192;

		// False: MAX_KINDS kinds already wait (never, but for a container of that many kinds), and it is left out.
		bool Add(std::uint32_t a_holder, std::uint32_t a_base, std::int32_t a_count)
		{
			if (a_count == 0) {
				return true;
			}
			auto it = moves.find({ a_holder, a_base });
			if (it == moves.end()) {
				if (moves.size() >= MAX_KINDS) {
					++dropped;
					return false;
				}
				it = moves.emplace(std::pair{ a_holder, a_base }, Sums{}).first;
			}
			(a_count > 0 ? it->second.puts : it->second.takes) += a_count > 0 ? std::int64_t{ a_count } : -std::int64_t{ a_count };
			return true;
		}

		// Each container's and item's puts (+) and takes (-) to a_to(holder, base, count), and the inbox is empty again.
		template <class F>
		void Drain(F&& a_to)
		{
			constexpr std::int64_t MAX = INT32_MAX;
			for (const auto& [key, sums] : std::exchange(moves, {})) {
				if (sums.puts > 0) {
					a_to(key.first, key.second, static_cast<std::int32_t>((std::min)(sums.puts, MAX)));
				}
				if (sums.takes > 0) {
					a_to(key.first, key.second, -static_cast<std::int32_t>((std::min)(sums.takes, MAX)));
				}
			}
		}

		void                        Clear() { moves.clear(); }
		[[nodiscard]] std::size_t   Size() const { return moves.size(); }
		std::uint32_t               TakeDropped() { return std::exchange(dropped, 0); }

	private:
		struct Sums
		{
			std::int64_t puts = 0;
			std::int64_t takes = 0;
		};
		std::map<std::pair<std::uint32_t, std::uint32_t>, Sums> moves;
		std::uint32_t                                           dropped = 0;
	};

	class Client
	{
	public:
		explicit Client(Engine& a_engine) :
			engine(a_engine)
		{}

		// We were welcomed as a_localId; a_rule: the host's bContainerGifts (our puts are gifts).
		void Welcome(std::uint32_t a_localId, bool a_rule)
		{
			localId = a_localId;
			rule = a_rule;
			said.fill(false);
		}

		[[nodiscard]] bool Rule() const { return rule && localId != 0; }

		// ---- the engine's events --------------------------------------------------------------------------------

		// Our player moved a_count of a_base between their inventory and a_holder (+: put in, -: taken out).
		void Moved(std::uint32_t a_holder, std::uint32_t a_base, std::int32_t a_count)
		{
			if (!view || view->holder != a_holder || a_count == 0) {
				return;
			}
			auto& signal = view->signals[a_base];
			(a_count > 0 ? signal.puts : signal.takes) += a_count > 0 ? a_count : -static_cast<std::int64_t>(a_count);
			signal.at = engine.Now();
		}

		// The container menu shows a_holder.
		void Open(std::uint32_t a_holder)
		{
			if (view && view->holder == a_holder) {
				return;
			}
			Close();
			if (localId == 0 || !Protocol::IsShareableRef(a_holder)) {
				return;
			}
			// The host's rule off, a container no gift lies in isn't watched: nothing put in it is given (it costs nothing).
			if (!Rule() && std::ranges::none_of(lying, [&](const auto& a_entry) { return a_entry.second.holder == a_holder; })) {
				return;
			}
			if (!GiftPlace(engine.Holder(a_holder))) {
				return;
			}
			const auto stacks = engine.Stacks(a_holder, 0);
			if (!stacks) {
				return;
			}
			view = View{ .holder = a_holder, .real = Count(*stacks) };
			++stats.views;
			ShowCopies();
			NoteCarried();
		}

		// The menu closed (or a save or a load is about to be made, or the session ends): our copies leave the container.
		void Close()
		{
			if (!view) {
				return;
			}
			Sync();
			for (const auto& [id, cls] : view->copies) {
				if (!TakeOut(view->holder, cls)) {
					++stats.copyMissing;
				}
			}
			view.reset();
		}

		// Every frame while welcomed.
		void Frame()
		{
			const auto now = engine.Now();
			std::erase_if(puts, [&](const Put& a_put) { return now - a_put.at > PUT_FORGET; });
			for (auto& [id, claim] : claims) {
				if (now - claim.sent >= CLAIM_RETRY) {
					Claim(id, Protocol::DropClaim::kTake);  // asked again: only the server's answer settles it
					claim.sent = now;
				}
			}
			if (view) {
				Sync();
				ShowCopies();
				NoteCarried();
			}
		}

		// ---- the server's messages ----------------------------------------------------------------------------

		// A gift lies in the session (or, to us alone, the offer of one of ours). One with no holder is a loose drop's (Drops').
		void Placed(const Protocol::DroppedItem& a_drop)
		{
			if (a_drop.holder == 0) {
				return;
			}
			const bool mine = localId != 0 && a_drop.ownerId == localId;
			if (mine && view) {
				// What our player did since the last frame first: a take back of a put's class in the same tick as its
				// offer takes that put back (CancelPuts), before the offer is matched to it.
				Sync();
			}
			const auto put = mine ? std::ranges::find_if(puts, [&](const Put& a_put) { return a_put.item.nonce == a_drop.nonce; }) : puts.end();
			if (a_drop.dropId == 0) {
				// Not kept by the server: the item stays in our copy of the container, ours alone.
				if (put != puts.end()) {
					const bool cancelled = put->cancelled;
					puts.erase(put);
					if (!cancelled) {
						++stats.refused;
						Say(Notice::kNotGiven);
					}
				}
				return;
			}
			if (put != puts.end() && put->cancelled) {
				// Our player took one of its class back out before the offer came: it never lay anywhere, and what the
				// container held of that class before stays there (it was never the gift).
				puts.erase(put);
				Claim(a_drop.dropId, Protocol::DropClaim::kKept);
				++stats.kept;
				return;
			}
			if (put == puts.end()) {
				if (mine && !lying.contains(a_drop.dropId) && !engine.HasMade(a_drop.nonce)) {
					// An offer of ours we no longer wait for (forgotten, or our world loaded since): its item never left our
					// world, so it never lay anywhere. (The server ignores this for a gift that lies there.)
					Claim(a_drop.dropId, Protocol::DropClaim::kKept);
					++stats.lateOffers;
					return;
				}
				lying[a_drop.dropId] = a_drop;
				return;
			}
			const auto entry = *put;
			puts.erase(put);
			Swap(entry, a_drop);
		}

		// A drop no longer lies anywhere. False: not a gift we know of (Drops' to handle).
		bool Gone(const Protocol::DropGone& a_gone)
		{
			const auto id = a_gone.dropId;
			const auto drop = lying.find(id);
			if (drop == lying.end() && !claims.contains(id) && !(view && view->copies.contains(id))) {
				return false;
			}
			RemoveCopy(id);  // our player's take of it just now, if any, is claimed first
			lying.erase(id);
			GoneFor(id, a_gone.takerId);
			if (view) {
				NoteCarried();  // what a grant or a lost take changed of our player's isn't a take of theirs
			}
			return true;
		}

		// A packet of the server's list of what lies in the session (the gifts in it; the last packet replaces what we knew).
		void List(const Protocol::DropList& a_list)
		{
			for (const auto& drop : a_list.drops) {
				if (drop.holder != 0) {
					listing.push_back(drop);
				}
			}
			if (!a_list.last) {
				return;
			}
			std::map<std::uint32_t, Protocol::DroppedItem> now;
			for (auto& drop : listing) {
				now[drop.dropId] = std::move(drop);
			}
			listing.clear();
			for (const auto& [id, drop] : lying) {
				if (!now.contains(id)) {
					RemoveCopy(id);
				}
			}
			lying = std::move(now);
		}

		// ---- saves, loads and the session -----------------------------------------------------------------------

		// Before a save or a load: no copy is saved.
		void BeforeSaveOrLoad() { Close(); }

		// A save was loaded (or a new game started): its world is the save's, and its record says what it holds of the
		// gifts. Our takes waiting for an answer were the world's before (a grant to one now is given back), and our puts
		// waiting for their offer are in the save or not (an offer of one is kept). What lies in the session stays.
		void GameLoaded()
		{
			view.reset();
			claims.clear();
			puts.clear();
			unplaceable.clear();
			vanished.clear();
		}

		// The session ended (or our link went): copies leave the container, and everything but the record is forgotten.
		// Nothing is given back: a take waiting for its answer lies there again after our next record, and a put whose
		// item left our world waits for that record as an offer.
		void Reset()
		{
			Close();
			GameLoaded();
			lying.clear();
			listing.clear();
			localId = 0;
			rule = false;
		}

		// ---- what it knows ------------------------------------------------------------------------------------------

		[[nodiscard]] const Stats&                                           Counters() const { return stats; }
		[[nodiscard]] const std::map<std::uint32_t, Protocol::DroppedItem>& Lying() const { return lying; }
		[[nodiscard]] std::optional<std::uint32_t>                           Viewing() const { return view ? std::optional{ view->holder } : std::nullopt; }
		[[nodiscard]] std::size_t                                            Copies() const { return view ? view->copies.size() : 0; }
		[[nodiscard]] bool                                                   Shows(std::uint32_t a_dropId) const { return view && view->copies.contains(a_dropId); }
		[[nodiscard]] std::size_t                                            Claims() const { return claims.size(); }
		[[nodiscard]] bool                                                   Claiming(std::uint32_t a_dropId) const { return claims.contains(a_dropId); }
		[[nodiscard]] std::size_t                                            Puts() const { return puts.size(); }

	private:
		struct Put
		{
			Protocol::DroppedItem item;
			Class                 cls;
			std::uint32_t         holder = 0;
			Clock::time_point     at{};
			bool                  cancelled = false;  // one of its class taken back out before the offer: kept on it
		};

		struct Signal
		{
			std::int64_t      puts = 0;
			std::int64_t      takes = 0;
			Clock::time_point at{};
		};

		struct View
		{
			std::uint32_t                   holder = 0;
			Counts                          real;    // what the container holds that isn't our copy of a gift
			std::map<std::uint32_t, Class>  copies;  // drop ID -> the class of our copy of it in the container
			std::map<std::uint32_t, Signal> signals;  // by base: our player's moves not yet seen in the container
			// Our player's items of the bases our copies are, as last seen: a copy gone with no event, while their count of its
			// class rose, went to them (the event lost).
			Counts                          carried;
			std::set<std::uint32_t>         carriedBases;
		};

		struct TakeClaim
		{
			Protocol::DroppedItem item;
			Class                 cls;
			bool                  held = false;  // the item came out of our player's inventory until the answer
			Clock::time_point     sent{};
		};

		// Our claim of a gift no longer lying there, answered: granted to a_taker (0: withdrawn).
		void GoneFor(std::uint32_t id, std::uint32_t a_taker)
		{
			const auto claim = claims.find(id);
			if (localId != 0 && a_taker == localId) {
				if (claim != claims.end()) {
					const auto ours = std::move(claim->second);
					claims.erase(claim);
					if (ours.held && !AddOne(PLAYER, ours.item)) {
						Claim(id, Protocol::DropClaim::kGiveBack);  // it couldn't go into our player's inventory: it lies there again
						++stats.giveFailed;
						return;
					}
					engine.Took(id);
					++stats.received;
				} else if (!engine.HasTaken(id)) {
					// Granted to a take we no longer make (from before a load): it lies there again for anyone.
					Claim(id, Protocol::DropClaim::kGiveBack);
					++stats.givenBack;
				}
				return;
			}
			if (claim != claims.end()) {
				const auto lost = std::move(claim->second);
				claims.erase(claim);
				if (!lost.held) {
					if (TakeOut(PLAYER, lost.cls)) {
						++stats.takenBack;
					} else {
						++stats.dupRisk;
					}
				}
				if (a_taker != 0) {
					++stats.takenFirst;
					engine.Notify(Notice::kTakenFirst);
				}
			}
		}

		void Claim(std::uint32_t a_dropId, Protocol::DropClaim a_op) { engine.Send(Protocol::Encode(Protocol::ClaimDrop{ a_dropId, a_op })); }

		void Say(Notice a_notice)
		{
			if (!std::exchange(said[static_cast<std::size_t>(a_notice)], true)) {
				engine.Notify(a_notice);
			}
		}

		// One item of a_class out of a_ref.
		bool TakeOut(std::uint32_t a_ref, const Class& a_class)
		{
			const auto stacks = engine.Stacks(a_ref, a_class.base);
			const auto pick = stacks ? Pick(*stacks, a_class) : std::nullopt;
			return pick && engine.Remove(a_ref, a_class.base, *pick);
		}

		// What the container holds now against what we knew: an item more than we knew of that our player put in is a gift;
		// one fewer that went to our player (their take's event said so, or, the event lost, their count of its class rose)
		// is our copy of a gift first (so it is claimed and held back), else their own item. A copy gone anywhere else is
		// never shown again until a load or a rejoin.
		void Sync()
		{
			const auto stacks = engine.Stacks(view->holder, 0);
			if (!stacks) {
				return;
			}
			const auto now = engine.Now();
			const auto have = Count(*stacks);
			Counts     known = view->real;
			std::map<Class, std::vector<std::uint32_t>> copiesOf;
			for (const auto& [id, cls] : view->copies) {
				++known[cls];
				copiesOf[cls].push_back(id);
			}
			std::set<Class> classes;
			for (const auto& [cls, n] : have) {
				classes.insert(cls);
			}
			for (const auto& [cls, n] : known) {
				classes.insert(cls);
			}
			std::map<std::uint32_t, Counts> carriedNow;  // read once a base, before anything is held back
			const auto rise = [&](const Class& a_class) -> std::int64_t {
				if (!view->carriedBases.contains(a_class.base)) {
					return 0;
				}
				auto it = carriedNow.find(a_class.base);
				if (it == carriedNow.end()) {
					const auto mine = engine.Stacks(PLAYER, a_class.base);
					it = carriedNow.emplace(a_class.base, mine ? Count(*mine) : Counts{}).first;
				}
				return (std::max)(std::int64_t{ 0 }, CountOf(it->second, a_class) - CountOf(view->carried, a_class));
			};
			for (const auto& cls : classes) {
				const auto change = CountOf(have, cls) - CountOf(known, cls);
				auto&      signal = view->signals[cls.base];
				if (change > 0) {
					view->real[cls] += change;
					const auto gifts = (std::min)(change, signal.puts);
					signal.puts -= gifts;
					const auto stack = std::ranges::find_if(*stacks, [&](const Stack& a_stack) { return a_stack.count > 0 && ClassOf(a_stack) == cls; });
					for (std::int64_t i = 0; i < gifts && stack != stacks->end(); ++i) {
						NewGift(*stack, cls, now);
					}
				} else if (change < 0) {
					auto        missing = -change;
					const auto  ids = copiesOf[cls];
					const auto  copies = static_cast<std::int64_t>(ids.size());
					const auto  toPlayer = (std::min)(missing, (std::max)(signal.takes, rise(cls)));
					const auto  claimed = (std::min)(toPlayer, copies);
					for (std::int64_t i = 0; i < claimed; ++i) {
						if (i >= signal.takes) {
							++stats.unsignaled;
						}
						Taken(ids[static_cast<std::size_t>(i)], cls, now);
					}
					auto&      real = view->real[cls];
					const auto own = (std::min)(toPlayer - claimed, real);
					real -= own;
					CancelPuts(cls, own);
					signal.takes -= (std::min)(signal.takes, claimed + own);
					missing -= claimed + own;
					// Copies gone elsewhere (another mod's script; or the event lost and the item changed on the way): they no
					// longer show, and aren't shown again.
					for (auto i = claimed; missing > 0 && i < copies; ++i, --missing) {
						const auto id = ids[static_cast<std::size_t>(i)];
						view->copies.erase(id);
						vanished.insert(id);
						++stats.copyMissing;
					}
					real -= (std::min)(missing, real);  // our player's own, gone elsewhere
				}
			}
			std::erase_if(view->real, [](const auto& a_entry) { return a_entry.second <= 0; });
			std::erase_if(view->signals, [&](const auto& a_entry) { return now - a_entry.second.at > SIGNAL_KEEP || (a_entry.second.puts <= 0 && a_entry.second.takes <= 0); });
			NoteCarried();
		}

		// Our player took a_n of a_class's own items out of the container we view: as many of their puts of that class still
		// waiting for an offer were taken back (their offers are kept), the latest first. What the container held of that
		// class before the put is never given for one.
		void CancelPuts(const Class& a_class, std::int64_t a_n)
		{
			for (auto it = puts.rbegin(); a_n > 0 && it != puts.rend(); ++it) {
				if (!it->cancelled && it->holder == view->holder && it->cls == a_class) {
					it->cancelled = true;
					--a_n;
				}
			}
		}

		// What our player carries of the bases our copies are, for the next Sync.
		void NoteCarried()
		{
			view->carried.clear();
			view->carriedBases.clear();
			for (const auto& [id, cls] : view->copies) {
				if (!view->carriedBases.insert(cls.base).second) {
					continue;
				}
				if (const auto mine = engine.Stacks(PLAYER, cls.base)) {
					for (const auto& [c, n] : Count(*mine)) {
						view->carried[c] = n;
					}
				} else {
					view->carriedBases.erase(cls.base);
				}
			}
		}

		// One a_item into a_ref: the class it is there, or nothing, and then nothing of it is left there (an add that
		// failed after putting something in is undone).
		std::optional<Class> AddOne(std::uint32_t a_ref, const Protocol::DroppedItem& a_item)
		{
			const auto before = engine.Stacks(a_ref, a_item.base);
			if (!before) {
				return std::nullopt;
			}
			const auto         had = Count(*before);
			const bool         added = engine.Add(a_ref, a_item);
			const auto         after = engine.Stacks(a_ref, a_item.base);
			std::vector<Class> grown;
			for (const auto& [cls, n] : after ? Count(*after) : Counts{}) {
				for (auto more = n - CountOf(had, cls); more > 0; --more) {
					grown.push_back(cls);
				}
			}
			if (added && grown.size() == 1) {
				return grown.front();
			}
			for (const auto& cls : grown) {
				if (TakeOut(a_ref, cls)) {
					++stats.addUndone;
				}
			}
			return std::nullopt;
		}

		// Our player put one of a_stack's items in the container we view: reported as a gift, if it may be one.
		void NewGift(const Stack& a_stack, const Class& a_class, Clock::time_point a_now)
		{
			++stats.puts;
			if (!Rule()) {
				return;  // the host's rule is off: it stays in our copy of the container, as in 0.9.x
			}
			const auto place = engine.PlaceOf(view->holder);
			if (!Givable(a_stack) || !place) {
				++stats.notGiven;
				Say(Notice::kNotGiven);
				return;
			}
			if (puts.size() >= MAX_PUTS) {
				++stats.overflow;
				return;
			}
			auto item = ItemOf(a_stack, view->holder, *place, engine.NewNonce());
			engine.Send(Protocol::Encode(item, Protocol::MessageType::kReportDrop));
			puts.push_back({ std::move(item), a_class, view->holder, a_now });
			++stats.reported;
		}

		// The server's offer of our gift: its item leaves our world now, or never lay anywhere.
		void Swap(const Put& a_put, const Protocol::DroppedItem& a_drop)
		{
			const bool viewing = view && view->holder == a_put.holder;
			if (viewing) {
				Sync();  // what our player did meanwhile (a take of it back, say)
			}
			const auto   stacks = engine.Stacks(a_put.holder, a_put.cls.base);
			std::int64_t shown = 0;
			if (viewing) {
				shown = std::ranges::count(view->copies, a_put.cls, [](const auto& a_entry) { return a_entry.second; });
			}
			const auto there = stacks ? CountOf(Count(*stacks), a_put.cls) - shown : 0;
			bool       out = false;
			if (there >= 1 && viewing && CountOf(view->real, a_put.cls) >= 1) {
				// The menu still shows it: the item itself stays there as our copy of the gift, so nothing moves on screen.
				--view->real[a_put.cls];
				view->copies[a_drop.dropId] = a_put.cls;
				NoteCarried();
				out = true;
			} else if (there >= 1 && !viewing) {
				out = TakeOut(a_put.holder, a_put.cls);
			}
			if (!out) {
				// Our player took it back first (or the container isn't here now): it never lay anywhere.
				Claim(a_drop.dropId, Protocol::DropClaim::kKept);
				++stats.kept;
				return;
			}
			engine.Made(a_drop.nonce);
			Claim(a_drop.dropId, Protocol::DropClaim::kPlaced);
			lying[a_drop.dropId] = a_drop;
			++stats.given;
			Say(Notice::kGiven);
		}

		// Our player took our copy of a gift: held back from their inventory until the server's answer, and claimed.
		void Taken(std::uint32_t a_dropId, const Class& a_class, Clock::time_point a_now)
		{
			view->copies.erase(a_dropId);
			const bool held = TakeOut(PLAYER, a_class);
			const auto drop = lying.find(a_dropId);
			if (drop == lying.end()) {
				// Never (a copy goes before its gift does): a copy of nothing that lies there is in nobody's inventory.
				++stats.copyMissing;
				return;
			}
			claims[a_dropId] = { drop->second, a_class, held, a_now };
			Claim(a_dropId, Protocol::DropClaim::kTake);
			++stats.taken;
			if (!held) {
				++stats.unheld;
			}
		}

		// Our copy of a gift that no longer lies there leaves the container (after our player's take of it, if any, is seen).
		void RemoveCopy(std::uint32_t a_dropId)
		{
			if (!view || !view->copies.contains(a_dropId)) {
				return;
			}
			Sync();
			const auto it = view->copies.find(a_dropId);
			if (it == view->copies.end()) {
				return;  // taken just now (claimed) or gone
			}
			if (!TakeOut(view->holder, it->second)) {
				++stats.copyMissing;
			}
			view->copies.erase(it);
		}

		// The gifts lying in the container we view that show no copy yet get one.
		void ShowCopies()
		{
			const auto holder = view->holder;
			for (const auto& [id, drop] : lying) {
				if (drop.holder != holder || view->copies.contains(id) || claims.contains(id) || unplaceable.contains(id) || vanished.contains(id)) {
					continue;
				}
				if (view->copies.size() >= MAX_COPIES || view->copies.size() + claims.size() >= MAX_CLAIMS) {
					return;  // every take of a copy can be claimed
				}
				if (!engine.Placeable(drop)) {
					unplaceable.insert(id);
					++stats.unplaceable;
					continue;
				}
				if (!Receivable(drop, engine.KindOf(drop.base))) {
					// Not a weapon or armor (a modified game's report): shown, it would be an item no Sync sees, left in our
					// copy for good.
					unplaceable.insert(id);
					++stats.notGear;
					continue;
				}
				const auto grown = AddOne(holder, drop);
				if (!grown) {
					unplaceable.insert(id);  // it didn't go in (and nothing of it stayed)
					++stats.unplaceable;
					continue;
				}
				view->copies[id] = *grown;
				++stats.shown;
			}
		}

		Engine&                                        engine;
		std::uint32_t                                  localId = 0;
		bool                                           rule = false;
		std::optional<View>                            view;
		std::vector<Put>                               puts;
		std::map<std::uint32_t, TakeClaim>             claims;
		std::map<std::uint32_t, Protocol::DroppedItem> lying;
		std::vector<Protocol::DroppedItem>             listing;
		std::set<std::uint32_t>                        unplaceable;
		// Gifts whose copy went from the container we view, but not to our player (another mod's script, say): not shown here
		// again until a load or a rejoin, so a script that empties the container makes one item of each gift a load or a
		// rejoin, not one a frame.
		std::set<std::uint32_t>                        vanished;
		std::array<bool, static_cast<std::size_t>(Notice::kCount)> said{};
		Stats                                          stats;
	};
}
