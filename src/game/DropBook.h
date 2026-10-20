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
