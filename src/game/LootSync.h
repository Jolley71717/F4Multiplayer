#pragma once

#include "Protocol.h"
#include "game/ContainerLedger.h"
#include "game/LootScope.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

// Loot between one player's game and the session, as WorldSync runs it: which of the player's own item
// moves are reported, and how other players' container changes go into our world (in session order,
// each container's in its order, waiting while a container isn't loaded or an NPC still lives here).
// Kept free of the game so it can be tested against a real server; WorldSync reads the engine for it
// (World), and the tests make a world up.
namespace LootSync
{
	using Clock = ContainerLedger::Clock;

	// An item the player moved between their inventory and a container or an NPC.
	struct Move
	{
		std::uint32_t holder = 0;
		std::uint32_t item = 0;   // base form
		std::int32_t  count = 0;  // < 0: taken from the holder
	};

	// What the game says about the references and forms involved, and how a change goes into it.
	class World
	{
	public:
		virtual ~World() = default;

		// The holder can be changed now: we are in the world (out of the opening) and its reference is here.
		virtual bool                   Loaded(std::uint32_t a_holder) = 0;
		virtual LootScope::HolderFacts Holder(std::uint32_t a_holder) = 0;
		// The item's own facts (questAlias is the holder's: see QuestAlias).
		virtual LootScope::ItemFacts   Item(std::uint32_t a_item) = 0;
		// The holder's stack of the item fills a quest alias.
		virtual bool                   QuestAlias(std::uint32_t a_holder, std::uint32_t a_item) = 0;
		// Puts another player's change into the world (removeitem takes what is there).
		virtual void                   Apply(const Protocol::ContainerChange& a_change) = 0;
	};

	// A change that waits to go into our world. Kept in the save (not aliveFor or alive).
	struct Pending
	{
		std::uint32_t             index = 0;
		Protocol::ContainerChange change;
		Clock::duration           aliveFor{};     // time in the world spent waiting for a living NPC to die
		bool                      alive = false;  // a living NPC holds it up (the last try found one, or one ahead of it did)
	};

	class Loot
	{
	public:
		// A change for a living NPC waits for it to die here (its kill may be on its way). One that never dies
		// (a companion's trade a joiner reported anyway) isn't kept for ever: after this long in the world.
		static constexpr Clock::duration NOT_YET_DEAD_LIMIT = std::chrono::minutes{ 10 };
		// Changes waiting, at most. Past it one waiting for a living NPC makes room; with none, the new change
		// is let go.
		static constexpr std::size_t MAX_PENDING = 4096;

		struct Counts
		{
			std::array<std::uint32_t, LootScope::SCOPES> keptSent{};     // the player's moves kept in this game, by scope
			std::array<std::uint32_t, LootScope::SCOPES> keptApplied{};  // other players' changes left out here, by scope
			std::uint32_t sent = 0;            // the player's moves reported
			std::uint32_t pendingDropped = 0;  // waiting changes given up (a living NPC that never died, or no room)
			std::uint32_t pendingFull = 0;     // of those, for want of room
		};

		// ---- the player's own moves --------------------------------------------------------------------

		// The player's moves since the last frame (or, before a save or a load, the frame that didn't come):
		// those that are the session's, encoded to go to the server in order.
		std::vector<std::vector<std::uint8_t>> Report(std::span<const Move> a_moves, World& a_world)
		{
			std::vector<std::vector<std::uint8_t>> packets;
			for (const auto& move : a_moves) {
				const auto direction = move.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
				// Here a living NPC is pickpocketing or a companion's trade: never the session's.
				const auto scope = LootScope::Of(a_world.Holder(move.holder), a_world.Item(move.item), direction);
				if (scope != LootScope::Scope::kShared) {
					++counts.keptSent[static_cast<std::size_t>(scope)];
					continue;
				}
				const Protocol::ContainerChange change{ move.holder, move.item, move.count };
				receiver.Reported(change);
				packets.push_back(Protocol::Encode(change, Protocol::MessageType::kReportContainer));
				++counts.sent;
			}
			return packets;
		}

		// ---- which session our world is in -------------------------------------------------------------

		// The server welcomed us into a_session.
		void OnWelcome(std::uint64_t a_session, std::uint32_t a_localId)
		{
			live = a_session;
			liveId = a_localId;
			if (receiver.OnWelcome(a_session, a_localId)) {
				pending.clear();  // a different session: none of its changes are in our world yet
			}
		}

		// What to put in Hello: everything our world lacks.
		Protocol::WorldRequest Hello(Clock::time_point a_now) { return receiver.Request(a_now); }

		// A save's record was read, before the game says the save has loaded (a_session 0: it has none).
		void LoadRecord(std::uint64_t a_session, std::uint32_t a_next, std::vector<Pending> a_pending)
		{
			receiver.Load(a_session, a_next);
			pending = std::move(a_pending);
			// In session order, whatever order the record holds them in: a container's changes go in in that order.
			std::ranges::stable_sort(pending, {}, &Pending::index);
		}

		// The save has loaded while we are in session a_session: a save from no session (made alone, or
		// another character's) or another one takes ours from its first change, so the saves made from it
		// on carry a record. Returns the request for what our world lacks, to send now.
		Protocol::WorldRequest AfterLoad(std::uint64_t a_session, std::uint32_t a_localId, Clock::time_point a_now)
		{
			if (receiver.Adopt(a_session, a_localId)) {
				pending.clear();
			}
			return receiver.Request(a_now);
		}

		// The same, in the session we were last welcomed into (SessionSteps::OnGameLoaded, once the game has
		// left the session if the save isn't its world): nothing while we aren't in one.
		std::optional<Protocol::WorldRequest> AfterLoad(Clock::time_point a_now)
		{
			if (live == 0) {
				return std::nullopt;
			}
			return AfterLoad(live, liveId, a_now);
		}

		// The connection is gone. Our world's record stays: a reconnect carries on from it.
		void OnDisconnected() { live = 0; }

		// ---- other players' changes --------------------------------------------------------------------

		void Live(const Protocol::IndexedContainerChange& a_change, Clock::time_point a_now, World& a_world)
		{
			receiver.Live(a_change, a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); });
		}

		// One WorldState's container changes. Returns how many numbers the server's log lost (see
		// ContainerLedger::Ledger::OnChunk).
		std::uint32_t CatchUp(std::uint32_t a_firstIndex, const std::vector<Protocol::IndexedContainerChange>& a_changes, Clock::time_point a_now, World& a_world)
		{
			const auto lost = receiver.CatchUp(a_firstIndex, a_changes, a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); });
			if (lost) {
				// Changes from the lost part of the server's log that still wait for their container go too:
				// the server gives their numbers to new changes. (A reply that rewinds has no changes, so
				// none of these came in it.)
				std::erase_if(pending, [&](const Pending& a_pending) { return a_pending.index >= a_firstIndex; });
			}
			return lost;
		}

		// Tries the waiting changes again (every a_elapsed while in the world). Changes to one container
		// stay in order: the first that can't go in holds back the rest of its container's. Time an NPC
		// lives here counts for all its changes, so the wait for one that never dies ends for all of them
		// together.
		void Retry(World& a_world, Clock::duration a_elapsed)
		{
			std::unordered_set<std::uint32_t> blocked, alive;
			std::erase_if(pending, [&](Pending& a_pending) {
				const auto container = a_pending.change.container;
				if (blocked.contains(container)) {
					if (alive.contains(container)) {
						a_pending.alive = true;
						a_pending.aliveFor += a_elapsed;
					}
					return false;
				}
				switch (TryApply(a_pending.change, a_world)) {
				case Applied::kDone:
					return true;
				case Applied::kNotYetDead:
					a_pending.alive = true;
					a_pending.aliveFor += a_elapsed;
					alive.insert(container);
					if (a_pending.aliveFor >= NOT_YET_DEAD_LIMIT) {
						++counts.keptApplied[static_cast<std::size_t>(LootScope::Scope::kNotYetDead)];
						++counts.pendingDropped;
						return true;  // it never died here: what waits behind it goes on
					}
					break;
				case Applied::kNotLoaded:
					break;
				}
				blocked.insert(container);
				return false;
			});
		}

		// A request to send now when a gap has stayed open with nothing arriving (see Ledger::Due).
		// Tries again what waits for a_container alone (WorldSync tries a container as it loads, and the rest
		// a few at a time: see RetrySchedule), in order, until one can't go in. Time spent waiting for a living
		// NPC is counted by Retry(World&, elapsed), not here. Returns how many went in (or were left out for
		// good by the rule).
		std::size_t Retry(std::uint32_t a_container, World& a_world)
		{
			std::size_t applied = 0;
			bool        blocked = false;
			bool        alive = false;
			std::erase_if(pending, [&](Pending& a_pending) {
				if (a_pending.change.container != a_container) {
					return false;
				}
				if (blocked) {
					a_pending.alive = a_pending.alive || alive;
					return false;
				}
				switch (TryApply(a_pending.change, a_world)) {
				case Applied::kDone:
					++applied;
					return true;
				case Applied::kNotYetDead:
					a_pending.alive = alive = true;
					break;
				case Applied::kNotLoaded:
					break;
				}
				blocked = true;
				return false;
			});
			return applied;
		}

		// Every container something waits for, once each.
		[[nodiscard]] std::vector<std::uint32_t> Containers() const
		{
			std::vector<std::uint32_t> containers;
			containers.reserve(pending.size());
			for (const auto& entry : pending) {
				containers.push_back(entry.change.container);
			}
			std::ranges::sort(containers);
			containers.erase(std::unique(containers.begin(), containers.end()), containers.end());
			return containers;
		}

		// Whether something waits for a living NPC (its wait runs out: see NOT_YET_DEAD_LIMIT).
		[[nodiscard]] bool WaitsForTheLiving() const { return std::ranges::any_of(pending, &Pending::alive); }

		// A gap asked for long enough is skipped instead, what waited behind it going into a_world (see
		// Ledger::Skip). Neither while we aren't in a session: nobody would answer, and the hello that
		// reconnects asks for everything our world lacks.
		std::optional<Protocol::WorldRequest> Due(Clock::time_point a_now, World& a_world)
		{
			if (live == 0 || !receiver.Due(a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); })) {
				return std::nullopt;
			}
			return receiver.Request(a_now);
		}

		// ---- what is known ---------------------------------------------------------------------------

		// For the save's record: which session, the first change our world lacks, and what waits.
		[[nodiscard]] Protocol::WorldRequest              Point() const { return receiver.Point(); }
		[[nodiscard]] const std::vector<Pending>&         Waiting() const { return pending; }
		[[nodiscard]] const ContainerLedger::Receiver&    Numbers() const { return receiver; }
		[[nodiscard]] const Counts&                       Stats() const { return counts; }
		[[nodiscard]] bool                                Ours(std::uint32_t a_playerId) const { return receiver.Ours(a_playerId); }

		[[nodiscard]] std::uint32_t KeptSent(LootScope::Scope a_scope) const { return counts.keptSent[static_cast<std::size_t>(a_scope)]; }
		[[nodiscard]] std::uint32_t KeptApplied(LootScope::Scope a_scope) const { return counts.keptApplied[static_cast<std::size_t>(a_scope)]; }

	private:
		enum class Applied
		{
			kDone,        // in our world, or never will be
			kNotLoaded,   // try again when the container is here
			kNotYetDead,  // a living NPC here: try again once it has died
		};

		// Another player's change, judged by the same rule the sender's game used (a joiner's build from
		// before the rule, or a changed one, may report what isn't the session's).
		Applied TryApply(const Protocol::ContainerChange& a_change, World& a_world)
		{
			if (!a_world.Loaded(a_change.container)) {
				return Applied::kNotLoaded;
			}
			const auto direction = a_change.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
			auto       item = a_world.Item(a_change.item);
			// Everyone takes their own quest item (and doesn't get a copy of one someone else put in).
			item.questAlias = direction == LootScope::Direction::kTake && LootScope::IsInventory(item.kind) && a_world.QuestAlias(a_change.container, a_change.item);
			const auto scope = LootScope::Of(a_world.Holder(a_change.container), item, direction);
			if (scope == LootScope::Scope::kNotYetDead) {
				return Applied::kNotYetDead;
			}
			if (scope != LootScope::Scope::kShared) {
				++counts.keptApplied[static_cast<std::size_t>(scope)];
				return Applied::kDone;
			}
			a_world.Apply(a_change);
			return Applied::kDone;
		}

		// Another player's change, next in session order (the ledger keeps the order). Changes to one
		// container stay in order; different containers don't wait for each other.
		void ApplyInOrder(const Protocol::IndexedContainerChange& a_change, World& a_world)
		{
			const auto& change = a_change.change;
			const auto  first = std::ranges::find(pending, change.container, [](const Pending& a_pending) { return a_pending.change.container; });
			if (first != pending.end()) {
				Hold({ .index = a_change.index, .change = change, .alive = first->alive });  // waits for the same NPC
				return;
			}
			switch (TryApply(change, a_world)) {
			case Applied::kDone:
				return;
			case Applied::kNotLoaded:
				Hold({ a_change.index, change });
				return;
			case Applied::kNotYetDead:
				Hold({ .index = a_change.index, .change = change, .alive = true });
				return;
			}
		}

		void Hold(const Pending& a_change)
		{
			if (pending.size() >= MAX_PENDING) {
				++counts.pendingDropped;
				++counts.pendingFull;
				// What waits for a living NPC is the likeliest never to go in (one a joiner's game reported
				// anyway, maybe many): it makes room before an ordinary change for a container not loaded is lost.
				const auto alive = std::ranges::find_if(pending, &Pending::alive);
				if (alive == pending.end()) {
					return;
				}
				++counts.keptApplied[static_cast<std::size_t>(LootScope::Scope::kNotYetDead)];
				pending.erase(alive);
			}
			pending.push_back(a_change);
		}

		ContainerLedger::Receiver receiver;
		std::uint64_t             live = 0;    // the session we are welcomed into (0: none, or the connection went)
		std::uint32_t             liveId = 0;  // our player ID in it
		std::vector<Pending>      pending;
		Counts                    counts;
	};
}
