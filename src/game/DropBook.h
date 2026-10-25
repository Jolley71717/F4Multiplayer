#pragma once

#include "Protocol.h"
#include "game/LootScope.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

// The game-free half of giving items by dropping them (Drops): what may be given, when a dropped item has come to rest,
// the claims our player makes, and what our world holds of the session's transfers (the co-save record the server makes
// the session agree with: common/DropStore.h). Kept free of the game so it can be tested.
namespace DropBook
{
	using Clock = std::chrono::steady_clock;

	// ---- what may be given ---------------------------------------------------------------------------------

	struct ItemFacts
	{
		LootScope::Item kind = LootScope::Item::kOther;
		bool            featured = false;     // a bobblehead or a magazine
		bool            questItem = false;    // the engine marks it a quest item (it can't be dropped anyway)
		bool            runtimeBase = false;  // its base form was made at runtime (FF......): no other game has it
		bool            runtimeMod = false;   // one of its mods was (a mod made at runtime, which ModsOn leaves out)
		std::int32_t    count = 0;
	};

	enum class Givable : std::uint8_t
	{
		kGive,
		kNotItem,      // not something an inventory holds
		kPersonal,     // keys, notes, holotapes, books, bobbleheads, magazines: everyone collects their own
		kQuest,
		kNotEverywhere,  // its base or a mod exists in this game only: the friend's copy couldn't be that item
		kTooMany,      // more than one dropped reference holds
	};

	constexpr Givable Judge(const ItemFacts& a_item)
	{
		using LootScope::Item;
		if (!LootScope::IsInventory(a_item.kind)) {
			return Givable::kNotItem;
		}
		if (a_item.kind == Item::kKey || a_item.kind == Item::kNote || a_item.kind == Item::kBook || a_item.featured) {
			return Givable::kPersonal;
		}
		if (a_item.questItem) {
			return Givable::kQuest;
		}
		if (a_item.runtimeBase || a_item.runtimeMod) {
			return Givable::kNotEverywhere;
		}
		if (a_item.count < 1 || a_item.count > Protocol::MAX_DROP_COUNT) {
			return Givable::kTooMany;
		}
		return Givable::kGive;
	}

	// ---- when it has come to rest ----------------------------------------------------------------------------

	// A dropped item falls and rolls: it is reported where it comes to rest, so every copy lies where the dropper sees it.
	inline constexpr auto SETTLE_MIN = std::chrono::milliseconds(1500);   // never sooner after the drop
	inline constexpr auto SETTLE_STILL = std::chrono::milliseconds(500);  // and still for this long
	inline constexpr auto SETTLE_MAX = std::chrono::seconds(5);           // reported then, wherever it is
	inline constexpr float SETTLE_MOVE = 2.0f;                            // game units: less is resting

	constexpr bool Settled(Clock::duration a_sinceDrop, Clock::duration a_sinceMoved)
	{
		return a_sinceDrop >= SETTLE_MAX || (a_sinceDrop >= SETTLE_MIN && a_sinceMoved >= SETTLE_STILL);
	}

	// ---- which reference our player's item became ----------------------------------------------------------------

	// The engine's event names no reference for some drops, so the item is looked for near our player (Drops' scan). Only
	// when it can only have been a drop: our player's count of that base fell by just what the events said left (a_before:
	// the count a frame before them, if known), and none of it was used up (food eaten, a stim used: an equip event of
	// that base among them). Else the scan could take another item lying near them for ours, and the swap delete it.
	constexpr bool MayScan(std::optional<std::uint32_t> a_before, std::uint32_t a_now, std::int64_t a_left, bool a_used)
	{
		return !a_used && a_before && a_left > 0 && *a_before >= a_now && std::int64_t{ *a_before - a_now } == a_left;
	}

	// ---- a take being finished --------------------------------------------------------------------------------

	enum class TakeState : std::uint8_t
	{
		kTaken,    // our copy left the world and our player's count of its base rose by the drop's count
		kWaiting,  // not yet (the engine's pickup may take a frame or two): checked again, then tried again
	};

	// Our player's take of a granted drop of a_count: a_before the count of its base when it began, a_now the count now,
	// a_copyInWorld whether our copy still lies loose. Both must have happened: the count alone may rise for another reason
	// (an item of that base looted meanwhile), and our copy may leave the world without coming to us (its cell unloaded).
	constexpr TakeState JudgeTake(bool a_copyInWorld, std::uint32_t a_before, std::uint32_t a_now, std::int32_t a_count)
	{
		return !a_copyInWorld && a_count > 0 && std::int64_t{ a_now } - a_before >= a_count ? TakeState::kTaken : TakeState::kWaiting;
	}

	// ---- our player's claims ---------------------------------------------------------------------------------

	// Our player pressed Take on a copy: one claim per item until the server answers, and one at a time so a held key
	// doesn't send one a frame. An unanswered claim may go again after CLAIM_RETRY (an answer lost with a reconnect).
	class Claims
	{
	public:
		static constexpr auto CLAIM_RETRY = std::chrono::seconds(5);

		// Whether to claim a_dropId now.
		bool Start(std::uint32_t a_dropId, Clock::time_point a_now)
		{
			if (const auto it = out.find(a_dropId); it != out.end() && a_now - it->second < CLAIM_RETRY) {
				return false;
			}
			if (out.size() >= MAX_OUT) {
				std::erase_if(out, [&](const auto& a_entry) { return a_now - a_entry.second >= CLAIM_RETRY; });
				if (out.size() >= MAX_OUT) {
					return false;
				}
			}
			out[a_dropId] = a_now;
			return true;
		}

		// The server answered (the item went, to us or another).
		bool Answered(std::uint32_t a_dropId) { return out.erase(a_dropId) != 0; }

		[[nodiscard]] bool Waiting(std::uint32_t a_dropId) const { return out.contains(a_dropId); }
		[[nodiscard]] std::size_t Size() const { return out.size(); }
		void Clear() { out.clear(); }

	private:
		static constexpr std::size_t MAX_OUT = 8;
		std::unordered_map<std::uint32_t, Clock::time_point> out;
	};

	// ---- a copy scrapped in workshop mode ----------------------------------------------------------------------

	// Workshop mode scraps a loose item lying in a settlement (its pick test lets loose weapons, armor and junk through):
	// the player gets its components and the game takes the reference away. Our copy of an item lying in the session
	// went that way without a word to anyone: the server still had it lying there, every friend's copy stood on, and
	// ours was placed again as soon as its handle let go, or after the next save, load or walk away. The components and
	// the item both, as often as the player liked. A scrap is our world consuming the item, so it is our player's take.
	enum class CopyGone : std::uint8_t
	{
		kStanding,  // still there as far as we can tell (or hidden, its activation not yet blocked)
		kScrapped,  // scrapped (or stored) in workshop mode: claimed as our take (Scraps)
		kLost,      // its handle names nothing: the game took it (its cell went); placed again
	};

	// Our copy, looked at in a frame: a_resolves (its handle still names a reference), a_named (the workshop's own
	// destroyed event named it: scrapped or stored). Only that event makes it a scrap: a copy merely found gone, in
	// workshop mode or not, may have gone some other way (its cell unloading as the player walks off), and taking it for
	// our take would take the item from its owner for nothing. Gone so, it is lost if its handle went (placed again),
	// else left alone.
	constexpr CopyGone JudgeCopy(bool a_resolves, bool a_named)
	{
		if (a_named) {
			return CopyGone::kScrapped;
		}
		return a_resolves ? CopyGone::kStanding : CopyGone::kLost;
	}

	// Drops our player scrapped (their copy taken away in workshop mode), until the server says who has each. The scrap is
	// claimed as a take (kClaimDrop kTake: no new message): granted, every game removes its copy and the server keeps it as
	// ours, so nobody takes it and no list or saved session lays it down again. Our record says we took it from the moment
	// of the scrap (Record::Took), so a link lost before the answer still settles it as ours with our next record. Lost to
	// a claim that came first (someone took it a moment before), it is theirs, our record says we never had it, and what
	// the scrap put in the workbench is taken back out of it (ScrapYields).
	class Scraps
	{
	public:
		enum class Outcome : std::uint8_t
		{
			kNotScrapped,  // not one of ours: the answer is for something else
			kOurs,         // granted: the item is gone for everyone, its components are ours
			kLost,         // someone else had it first: only our copy went; the item is theirs
		};

		// a_dropId's copy was scrapped here. True the first time: claim it now (a second scrap of it is the same one). Never
		// refused for want of room: a scrap left unclaimed would leave the item lying for the others while our player has
		// its components (the set holds no more than the drops lying about).
		bool Scrapped(std::uint32_t a_dropId)
		{
			if (a_dropId == 0 || out.contains(a_dropId)) {
				return false;
			}
			out.insert(a_dropId);
			return true;
		}

		// The server says who has a_dropId now (0: nobody it remembers, its owner's world has it again).
		Outcome Answered(std::uint32_t a_dropId, std::uint32_t a_takerId, std::uint32_t a_localId)
		{
			if (!out.erase(a_dropId)) {
				return Outcome::kNotScrapped;
			}
			return a_takerId != 0 && a_takerId == a_localId ? Outcome::kOurs : Outcome::kLost;
		}

		// Not placed again while its claim waits (the server still lists it).
		[[nodiscard]] bool Waiting(std::uint32_t a_dropId) const { return out.contains(a_dropId); }
		[[nodiscard]] std::vector<std::uint32_t> Unanswered() const { return { out.begin(), out.end() }; }
		[[nodiscard]] std::size_t Size() const { return out.size(); }
		void Clear() { out.clear(); }

	private:
		std::set<std::uint32_t> out;
	};

	// What each scrap of our copies put in its workbench, so a scrap lost to a friend's claim (Scraps::Outcome::kLost)
	// can be taken back out: the item is theirs, and our player is to keep nothing of it. The workshop's destroyed event
	// names the reference and its workbench; the components (or, stored, the item itself) come into the workbench from
	// nothing (container change events) in the same moment, before or after it. Every destroyed event is noted, our
	// player's own scraps too, and each thing that came in is given to the nearest scrap of that workbench within WINDOW,
	// so our own scraps beside a copy's keep their components. A scrap's yield is known once WINDOW twice over has passed.
	class ScrapYields
	{
	public:
		static constexpr auto        WINDOW = std::chrono::milliseconds(500);
		static constexpr std::size_t MAX_ADDED = 256;   // things come in from nothing, the newest kept
		static constexpr std::size_t MAX_SCRAPS = 64;   // destroyed events waiting for their yield, the newest kept

		struct Item
		{
			std::uint32_t base = 0;
			std::int32_t  count = 0;

			bool operator==(const Item&) const = default;
		};

		struct Yield
		{
			std::uint32_t     dropId = 0;
			std::uint32_t     bench = 0;
			std::vector<Item> items;
		};

		// a_count of a_base came into a_container from nothing.
		void Added(std::uint32_t a_container, std::uint32_t a_base, std::int32_t a_count, Clock::time_point a_at)
		{
			if (a_container == 0 || a_base == 0 || a_count <= 0) {
				return;
			}
			if (added.size() >= MAX_ADDED) {
				added.erase(added.begin());
			}
			added.push_back({ a_container, a_base, a_count, a_at });
		}

		// The workshop took a reference away from a_bench: our copy of a_dropId, or (0) anything else of the settlement's.
		void Destroyed(std::uint32_t a_dropId, std::uint32_t a_bench, Clock::time_point a_at)
		{
			if (a_bench == 0) {
				return;
			}
			if (scraps.size() >= MAX_SCRAPS) {
				scraps.erase(scraps.begin());
			}
			scraps.push_back({ a_dropId, a_bench, a_at });
		}

		// The yields of our copies' scraps now known (other scraps' are let go), each with what it put in its workbench.
		// What a scrap is given is its own: never given again to a scrap after it.
		std::vector<Yield> Due(Clock::time_point a_now)
		{
			std::vector<Yield> due;
			std::vector<bool>  done(scraps.size(), false);
			std::vector<bool>  given(added.size(), false);
			for (std::size_t i = 0; i < scraps.size(); ++i) {
				if (a_now - scraps[i].at < WINDOW * 2) {
					continue;
				}
				done[i] = true;
				Yield yield{ scraps[i].dropId, scraps[i].bench, {} };
				for (std::size_t j = 0; j < added.size(); ++j) {
					if (given[j] || Nearest(added[j]) != i) {
						continue;
					}
					given[j] = true;
					const auto same = std::ranges::find(yield.items, added[j].base, &Item::base);
					if (same != yield.items.end()) {
						same->count += added[j].count;
					} else {
						yield.items.push_back({ added[j].base, added[j].count });
					}
				}
				if (yield.dropId != 0) {
					due.push_back(std::move(yield));
				}
			}
			std::vector<Scrap> waiting;
			for (std::size_t i = 0; i < scraps.size(); ++i) {
				if (!done[i]) {
					waiting.push_back(scraps[i]);
				}
			}
			scraps = std::move(waiting);
			// What was given, and what came in too long ago for any scrap still waiting, is let go.
			std::vector<In> kept;
			for (std::size_t j = 0; j < added.size(); ++j) {
				if (!given[j] && a_now - added[j].at < WINDOW * 4) {
					kept.push_back(added[j]);
				}
			}
			added = std::move(kept);
			return due;
		}

		// What of a lost scrap's yield is taken back out (Drops), and what is refused. Only a thing with no instance of its
		// own is taken (a_fungible: a component, a misc mod, ammunition, food), where any of that base in the workbench is
		// the same thing: the amount comes out right even if our player moved some out since. An item stored whole (a
		// weapon, an armor) can sit beside our player's own of the same base, modded or legendary, and the engine removes
		// by base whichever it finds first: it is refused, left where it is, and our player keeps it (the friend has the
		// drop too). Our player's own is never taken.
		struct TakeBack
		{
			std::vector<Item> out;
			std::vector<Item> refused;
		};

		template <class Fungible>
		static TakeBack PlanTakeBack(const Yield& a_yield, Fungible&& a_fungible)
		{
			TakeBack plan;
			for (const auto& item : a_yield.items) {
				if (item.base == 0 || item.count <= 0) {
					continue;
				}
				(a_fungible(item.base) ? plan.out : plan.refused).push_back(item);
			}
			return plan;
		}

		[[nodiscard]] std::size_t Waiting() const { return scraps.size(); }
		void Clear()
		{
			added.clear();
			scraps.clear();
		}

	private:
		struct In
		{
			std::uint32_t     container = 0;
			std::uint32_t     base = 0;
			std::int32_t      count = 0;
			Clock::time_point at{};
		};
		struct Scrap
		{
			std::uint32_t     dropId = 0;
			std::uint32_t     bench = 0;
			Clock::time_point at{};
		};

		// The scrap of the same workbench nearest in time to what came in, within WINDOW (SIZE_MAX: none).
		[[nodiscard]] std::size_t Nearest(const In& a_in) const
		{
			std::size_t best = SIZE_MAX;
			auto        bestGap = WINDOW + Clock::duration{ 1 };
			for (std::size_t i = 0; i < scraps.size(); ++i) {
				if (scraps[i].bench != a_in.container) {
					continue;
				}
				const auto gap = a_in.at > scraps[i].at ? a_in.at - scraps[i].at : scraps[i].at - a_in.at;
				if (gap <= WINDOW && gap < bestGap) {
					best = i;
					bestGap = gap;
				}
			}
			return best;
		}

		std::vector<In>    added;
		std::vector<Scrap> scraps;
	};

	// ---- our drops the server hasn't confirmed ---------------------------------------------------------------

	// Our drops our game swapped for a copy (kPlaced sent) that the server hasn't yet said lie there. The server forgets
	// an offer whose dropper leaves before their kPlaced reaches it, so if we leave first the item would be in no world:
	// our game gives each one left here back to our world instead (and our record no longer says it left). The server
	// tells us once it lies there (kDropPlaced, as everyone), or by its list or someone's take of it.
	class Unconfirmed
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Swapped(const Protocol::DroppedItem& a_drop)
		{
			if (a_drop.dropId != 0 && (out.size() < MAX || out.contains(a_drop.dropId))) {
				out[a_drop.dropId] = a_drop;
			}
		}

		// The server says it lies there (or lay there: someone took it).
		void Heard(std::uint32_t a_dropId) { out.erase(a_dropId); }

		// We left: the ones never heard of, in the order they were dropped, to be given back to our world.
		std::vector<Protocol::DroppedItem> TakeAll()
		{
			std::vector<Protocol::DroppedItem> all;
			for (auto& [id, drop] : out) {
				all.push_back(std::move(drop));
			}
			out.clear();
			return all;
		}

		[[nodiscard]] bool Waiting(std::uint32_t a_dropId) const { return out.contains(a_dropId); }
		[[nodiscard]] std::size_t Size() const { return out.size(); }
		void Clear() { out.clear(); }

	private:
		std::map<std::uint32_t, Protocol::DroppedItem> out;
	};

	// ---- what our world holds of the session's transfers ------------------------------------------------------

	// Per session (a save may have played in more than one: a host's and a friend's), what the transfers did to our world:
	// the drops whose item left it (by our game's nonce) and the drops it took in (by drop ID). Saved in the co-save with
	// each save, read back with it, and sent to the server after every welcome and load (Protocol::DropRecord).
	class Record
	{
	public:
		static constexpr std::size_t MAX_SESSIONS = 8;
		static constexpr std::uint16_t VERSION = 1;

		// We are in a_session now (its welcome): its entry is the one changed and sent.
		void Bind(std::uint64_t a_session)
		{
			current = a_session;
			if (a_session == 0) {
				return;
			}
			auto it = std::ranges::find(sessions, a_session, &Session::id);
			if (it == sessions.end()) {
				if (sessions.size() >= MAX_SESSIONS) {
					sessions.erase(sessions.begin());  // the one played longest ago
				}
				sessions.push_back({ a_session, {}, {} });
			} else {
				std::rotate(it, it + 1, sessions.end());  // the most recent last
			}
		}

		[[nodiscard]] std::uint64_t Current() const { return current; }

		// An item we dropped left our world (our copy of it is the session's now).
		void Made(std::uint64_t a_nonce)
		{
			if (auto* session = Find(current); session && !std::ranges::contains(session->made, a_nonce)) {
				Trim(session->made);
				session->made.push_back(a_nonce);
			}
		}

		// An item we dropped is back in our world after all (the server may never have heard we swapped it out: Unconfirmed).
		void Unmade(std::uint64_t a_nonce)
		{
			if (auto* session = Find(current)) {
				std::erase(session->made, a_nonce);
			}
		}

		// An item lying in the session is in our player's inventory now.
		void Took(std::uint32_t a_dropId)
		{
			if (auto* session = Find(current); session && !std::ranges::contains(session->taken, a_dropId)) {
				Trim(session->taken);
				session->taken.push_back(a_dropId);
			}
		}

		// An item we recorded as taken is not in our world after all: our copy was scrapped, but someone else was granted it
		// (DropBook::Scraps).
		void Untook(std::uint32_t a_dropId)
		{
			if (auto* session = Find(current)) {
				std::erase(session->taken, a_dropId);
			}
		}

		[[nodiscard]] bool HasMade(std::uint64_t a_nonce) const
		{
			const auto* session = Find(current);
			return session && std::ranges::contains(session->made, a_nonce);
		}

		[[nodiscard]] bool HasTaken(std::uint32_t a_dropId) const
		{
			const auto* session = Find(current);
			return session && std::ranges::contains(session->taken, a_dropId);
		}

		// What to tell the server of the session we are in.
		[[nodiscard]] Protocol::DropRecord ForServer() const
		{
			Protocol::DropRecord out;
			out.sessionId = current;
			if (const auto* session = Find(current)) {
				out.made = session->made;
				out.taken = session->taken;
			}
			return out;
		}

		[[nodiscard]] bool Empty() const { return sessions.empty(); }

		// The co-save's bytes: the version, then each session's entry.
		[[nodiscard]] std::vector<std::uint8_t> Save() const
		{
			std::vector<std::uint8_t> out;
			const auto                put = [&](const auto& a_value) {
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
				out.insert(out.end(), bytes, bytes + sizeof(a_value));
			};
			put(VERSION);
			put(static_cast<std::uint8_t>(sessions.size()));
			for (const auto& session : sessions) {
				put(session.id);
				put(static_cast<std::uint16_t>(session.made.size()));
				for (const auto nonce : session.made) {
					put(nonce);
				}
				put(static_cast<std::uint16_t>(session.taken.size()));
				for (const auto id : session.taken) {
					put(id);
				}
			}
			return out;
		}

		// A save's record (nothing if it can't be read: a damaged record holds no transfers). The session we are in stays.
		static std::optional<Record> Load(std::span<const std::uint8_t> a_bytes)
		{
			std::size_t offset = 0;
			const auto  take = [&](auto& a_value) {
				if (offset + sizeof(a_value) > a_bytes.size()) {
					return false;
				}
				std::memcpy(&a_value, a_bytes.data() + offset, sizeof(a_value));
				offset += sizeof(a_value);
				return true;
			};
			std::uint16_t version = 0;
			std::uint8_t  count = 0;
			if (!take(version) || version != VERSION || !take(count) || count > MAX_SESSIONS) {
				return std::nullopt;
			}
			Record record;
			for (std::uint8_t i = 0; i < count; ++i) {
				Session       session;
				std::uint16_t made = 0;
				if (!take(session.id) || !take(made) || made > Protocol::MAX_DROP_RECORD) {
					return std::nullopt;
				}
				session.made.resize(made);
				for (auto& nonce : session.made) {
					if (!take(nonce)) {
						return std::nullopt;
					}
				}
				std::uint16_t taken = 0;
				if (!take(taken) || taken > Protocol::MAX_DROP_RECORD) {
					return std::nullopt;
				}
				session.taken.resize(taken);
				for (auto& id : session.taken) {
					if (!take(id)) {
						return std::nullopt;
					}
				}
				record.sessions.push_back(std::move(session));
			}
			if (offset != a_bytes.size()) {
				return std::nullopt;
			}
			return record;
		}

		// A loaded save's record replaces ours (the world it describes is the save's; a revert's is empty); we are still in
		// the session we were, bound again (Bind): a save from before it has no entry for it, and our transfers from now on
		// go into a fresh one rather than nowhere.
		void Replace(Record a_loaded)
		{
			const auto session = current;
			*this = std::move(a_loaded);
			current = 0;
			Bind(session);
		}

	private:
		struct Session
		{
			std::uint64_t              id = 0;
			std::vector<std::uint64_t> made;
			std::vector<std::uint32_t> taken;
		};

		Session* Find(std::uint64_t a_session)
		{
			const auto it = std::ranges::find(sessions, a_session, &Session::id);
			return a_session != 0 && it != sessions.end() ? &*it : nullptr;
		}

		const Session* Find(std::uint64_t a_session) const
		{
			const auto it = std::ranges::find(sessions, a_session, &Session::id);
			return a_session != 0 && it != sessions.end() ? &*it : nullptr;
		}

		// The oldest goes past the wire's cap: the server forgets its oldest takes too (DropStore::Limits::taken).
		template <class T>
		static void Trim(std::vector<T>& a_list)
		{
			if (a_list.size() >= Protocol::MAX_DROP_RECORD) {
				a_list.erase(a_list.begin());
			}
		}

		std::vector<Session> sessions;
		std::uint64_t        current = 0;
	};
}
