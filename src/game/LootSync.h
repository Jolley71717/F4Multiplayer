#pragma once

#include "Protocol.h"
#include "game/ContainerLedger.h"
#include "game/LootScope.h"
#include "game/StorageView.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
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

	// One stack a holder has, as the game reads it.
	struct Stack
	{
		std::uint32_t item = 0;  // base form
		std::int32_t  count = 0;
		bool          questAlias = false;  // it fills a quest alias
	};

	// What the game says about the references and forms involved, and how a change goes into it.
	class World
	{
	public:
		virtual ~World() = default;

		// What the holder has now (for an emptied holder: Protocol::CONTAINER_EMPTIED). Nothing, by default.
		virtual std::vector<Stack> Stacks(std::uint32_t) { return {}; }
		// The game has made the holder's inventory (a container rolls its leveled loot as it first loads): an emptied
		// holder waits until then, or its first load would fill it after all.
		virtual bool               Stocked(std::uint32_t) { return true; }

		// The holder can be changed now: we are in the world (out of the opening) and its reference is here.
		virtual bool                   Loaded(std::uint32_t a_holder) = 0;
		virtual LootScope::HolderFacts Holder(std::uint32_t a_holder) = 0;
		// The item's own facts (questAlias is the holder's: see QuestAlias).
		virtual LootScope::ItemFacts   Item(std::uint32_t a_item) = 0;
		// The holder's stack of the item fills a quest alias.
		virtual bool                   QuestAlias(std::uint32_t a_holder, std::uint32_t a_item) = 0;
		// Puts another player's change into the world (removeitem takes what is there).
		virtual void                   Apply(const Protocol::ContainerChange& a_change) = 0;

		// Shared storage (StorageView.h): who keeps the holder's true copy. Nobody, by default (no rule).
		virtual StorageView::Keeper           Keeper(std::uint32_t) { return StorageView::Keeper::kNone; }
		// While our menu is open on our copy of a friend's storage: the first numbered change its contents lack.
		virtual std::optional<std::uint32_t> ViewFrom(std::uint32_t) { return std::nullopt; }
		// Our move at a friend's storage is to go to the server (it may come back refused). False: it can't be followed
		// now (too many unanswered), so it isn't sent, and the world has undone it.
		virtual bool                         Reporting(const Protocol::ContainerChange&) { return true; }
		// One of our own moves at a friend's storage came back numbered, in session order (live, or in a catch-up).
		// a_lacking: our world doesn't have it (the save we loaded is from before it). Our moves there numbered before it
		// was kept (KeptSince) don't come: they were ordinary loot, and its claim took our puts back out of the host's copy.
		virtual void                         OwnStorage(const Protocol::IndexedContainerChange&, bool) {}
		// A friend's numbered take (a_index) our storage held a_missing fewer of than taken.
		virtual void                         Short(std::uint32_t, const Protocol::ContainerChange&, std::int32_t) {}
		// The first change numbered while a friend's storage was kept: our takes before it were ordinary loot, never undone.
		virtual std::uint32_t                KeptSince(std::uint32_t) { return 0; }
		// While our menu shows a friend's storage: an item that stays our own there for the view (a quest alias holds a stack
		// of it in our copy: StorageView::Side::Open).
		virtual bool                         Aliased(std::uint32_t, std::uint32_t) { return false; }
	};

	// What a holder holds of what the session shares (LootScope's: ammo, aid, junk, and no stack a quest alias holds), item
	// by item.
	inline std::map<std::uint32_t, std::int64_t> SharedIn(World& a_world, std::uint32_t a_holder)
	{
		std::map<std::uint32_t, std::int64_t> out;
		const auto                            holder = a_world.Holder(a_holder);
		for (const auto& stack : a_world.Stacks(a_holder)) {
			auto item = a_world.Item(stack.item);
			item.questAlias = stack.questAlias;
			if (LootScope::Of(holder, item, LootScope::Direction::kTake) == LootScope::Scope::kShared) {
				out[stack.item] += stack.count;
			}
		}
		return out;
	}

	// The items the session would share that a holder holds a stack of filling a quest alias (StorageView::Side::Open).
	inline std::set<std::uint32_t> Aliased(World& a_world, std::uint32_t a_holder)
	{
		std::set<std::uint32_t> out;
		const auto              holder = a_world.Holder(a_holder);
		for (const auto& stack : a_world.Stacks(a_holder)) {
			if (stack.questAlias && LootScope::Of(holder, a_world.Item(stack.item), LootScope::Direction::kTake) == LootScope::Scope::kShared) {
				out.insert(stack.item);
			}
		}
		return out;
	}

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
			std::uint32_t emptiedSent = 0;     // holders the player took the last shared stack from (CONTAINER_EMPTIED)
			std::uint32_t emptiedApplied = 0;  // other players' emptied holders emptied here
			std::uint32_t emptiedStacks = 0;   // stacks those took out of our world
			std::uint32_t emptiedKept = 0;     // items those left: the session put them in and nobody took them out since
			std::uint32_t emptiedOwn = 0;      // stacks those left: our copy's own (no friend took that item from the holder)
			std::uint32_t storageKept = 0;     // the player's moves at storage another game keeps, made with no view of it (their copy's own)
			std::uint32_t storageSkipped = 0;  // other players' changes to storage another game keeps, not shown here (no view of it)
			std::uint32_t storageOverflow = 0; // the player's moves at a friend's storage undone unsent (too many unanswered)
		};

		// ---- the player's own moves --------------------------------------------------------------------

		// The player's moves since the last frame (or, before a save or a load, the frame that didn't come):
		// those that are the session's, encoded to go to the server in order. A holder the player took the
		// session's things from and that has none of them left goes out emptied after them (CONTAINER_EMPTIED):
		// each game rolls a chest's and an NPC's leveled loot on its own, so taking the same items can leave a friend's
		// copy holding more of them than ours did (TryEmpty says what a friend's copy gives up).
		std::vector<std::vector<std::uint8_t>> Report(std::span<const Move> a_moves, World& a_world)
		{
			std::vector<std::vector<std::uint8_t>> packets;
			std::vector<std::uint32_t>             takenFrom;
			Restocked(a_moves, a_world);
			for (const auto& move : a_moves) {
				const auto direction = move.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
				// Storage another game keeps, with our menu not open on it (StorageView: our copy's own things then), or an item
				// whose stack there fills a quest alias (our copy's own: it never leaves it as a view ends): the player's own.
				const auto keeper = a_world.Keeper(move.holder);
				if (keeper == StorageView::Keeper::kFriend && (!a_world.ViewFrom(move.holder) || a_world.QuestAlias(move.holder, move.item) ||
						a_world.Aliased(move.holder, move.item))) {
					++counts.storageKept;
					continue;
				}
				// Here a living NPC is pickpocketing or a companion's trade: never the session's.
				const auto holder = a_world.Holder(move.holder);
				const auto scope = LootScope::Of(holder, a_world.Item(move.item), direction);
				if (scope != LootScope::Scope::kShared) {
					++counts.keptSent[static_cast<std::size_t>(scope)];
					// A weapon, a key or a note taken from a chest or corpse that holds nothing else the session shares leaves it
					// bare too (its own take isn't reported): a friend's copy may still hold more of what was taken from it before
					// (TryEmpty). Once told, not again for each gun taken after (told: until something shared is put back in it).
					if (direction == LootScope::Direction::kTake && (scope == LootScope::Scope::kPersonal || scope == LootScope::Scope::kWeaponArmor) &&
						LootScope::Of(holder, { .kind = LootScope::Item::kMisc }, direction) == LootScope::Scope::kShared && !toldBare.contains(move.holder) &&
						std::ranges::find(takenFrom, move.holder) == takenFrom.end()) {
						takenFrom.push_back(move.holder);
					}
					continue;
				}
				const Protocol::ContainerChange change{ move.holder, move.item, move.count };
				// At a friend's storage it is checked by the server against the true copy, and may come back refused; one our
				// game can't follow now is undone there instead of sent.
				if (keeper == StorageView::Keeper::kFriend && !a_world.Reporting(change)) {
					++counts.storageOverflow;
					continue;
				}
				receiver.Reported(change);
				packets.push_back(Protocol::Encode(change, Protocol::MessageType::kReportContainer));
				++counts.sent;
				if (keeper == StorageView::Keeper::kFriend) {
					continue;
				}
				Track(change, CountOf(move.holder, move.item, a_world) - move.count);  // ours is made: what it held before
				toldBare.erase(move.holder);  // it holds something shared again (a put), or is about to be asked
				if (direction == LootScope::Direction::kTake && std::ranges::find(takenFrom, move.holder) == takenFrom.end()) {
					takenFrom.push_back(move.holder);
				}
			}
			for (const auto holder : takenFrom) {
				// A container that keeps what is put in it (a home's, a settlement's) may hold a friend's stash in their copy:
				// only the takes go out, and take what was taken. So may storage one game keeps for everyone.
				if (!LootScope::Rolled(a_world.Holder(holder)) || a_world.Keeper(holder) != StorageView::Keeper::kNone || HoldsShared(holder, a_world)) {
					continue;
				}
				const Protocol::ContainerChange change{ holder, Protocol::CONTAINER_EMPTIED, -1 };
				receiver.Reported(change);
				packets.push_back(Protocol::Encode(change, Protocol::MessageType::kReportContainer));
				++counts.emptiedSent;
				sessionPut.erase(holder);  // ours holds nothing shared
				auto& told = toldBare[holder];
				told.clear();
				for (const auto& stack : a_world.Stacks(holder)) {
					told[stack.item] += stack.count;
				}
			}
			return packets;
		}

		// ---- which session our world is in -------------------------------------------------------------

		// The server welcomed us into a_session.
		void OnWelcome(std::uint64_t a_session, std::uint32_t a_localId)
		{
			live = a_session;
			toldBare.clear();
			liveId = a_localId;
			if (receiver.OnWelcome(a_session, a_localId)) {
				pending.clear();  // a different session: none of its changes are in our world yet
				sessionPut.clear();
				friendTook.clear();
			}
		}

		// What to put in Hello: everything our world lacks.
		Protocol::WorldRequest Hello(Clock::time_point a_now)
		{
			asked = 1;
			return receiver.Request(a_now);
		}

		// A save's record was read, before the game says the save has loaded (a_session 0: it has none).
		void LoadRecord(std::uint64_t a_session, std::uint32_t a_next, std::vector<Pending> a_pending)
		{
			toldBare.clear();  // the world is the save's: a chest looted bare since may hold its things again
			sessionPut.clear();
			friendTook.clear();
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
			toldBare.clear();
			sessionPut.clear();
			friendTook.clear();
			if (receiver.Adopt(a_session, a_localId)) {
				pending.clear();
			}
			++asked;
			return receiver.Request(a_now);
		}

		// A request Due made that was never sent (the game held it back): no world state will answer it.
		void Unasked()
		{
			if (asked > 0) {
				--asked;
			}
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
			receiver.Live(a_change, a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); }, OwnOf(a_world));
		}

		// One WorldState's container changes. Returns how many numbers the server's log lost (see
		// ContainerLedger::Ledger::OnChunk).
		std::uint32_t CatchUp(std::uint32_t a_firstIndex, const std::vector<Protocol::IndexedContainerChange>& a_changes, Clock::time_point a_now, World& a_world)
		{
			const auto lost = receiver.CatchUp(a_firstIndex, a_changes, a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); }, OwnOf(a_world));
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
				switch (TryApply(a_pending.index, a_pending.change, a_world)) {
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
				switch (TryApply(a_pending.index, a_pending.change, a_world)) {
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
			if (live == 0 || !receiver.Due(a_now, [&](const Protocol::IndexedContainerChange& a_ready) { ApplyInOrder(a_ready, a_world); }, OwnOf(a_world))) {
				return std::nullopt;
			}
			++asked;
			return receiver.Request(a_now);
		}

		// ---- shared storage (StorageView) ------------------------------------------------------------

		// What our storage container holds of what the session shares, for the server's book (the host's game lists it, and
		// so claims it): its shared stacks, with the changes still waiting to go into it, as of the first numbered change our
		// world lacks (the server counts the friends' changes from there in). Nothing while our world still waits for an
		// answer from the server (a hello's, a load's, a gap's), nor for a container whose inventory the game hasn't made
		// (it would list as empty): listed once it has come.
		std::optional<Protocol::StorageListing> Listing(std::uint32_t a_container, World& a_world)
		{
			if (receiver.Numbers().Asking() || !a_world.Stocked(a_container)) {
				return std::nullopt;
			}
			const auto holder = a_world.Holder(a_container);
			const auto shared = [&](std::uint32_t a_item, bool a_questAlias, LootScope::Direction a_direction) {
				auto item = a_world.Item(a_item);
				item.questAlias = a_questAlias;
				return LootScope::Of(holder, item, a_direction) == LootScope::Scope::kShared;
			};
			std::map<std::uint32_t, std::int64_t> held;
			for (const auto& stack : a_world.Stacks(a_container)) {
				if (stack.count > 0 && shared(stack.item, stack.questAlias, LootScope::Direction::kTake)) {
					held[stack.item] += stack.count;
				}
			}
			for (const auto& entry : pending) {
				const auto& change = entry.change;
				if (change.container == a_container && change.item != Protocol::CONTAINER_EMPTIED &&
					shared(change.item, false, change.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut)) {
					held[change.item] += change.count;
				}
			}
			Protocol::StorageListing listing{ a_container, receiver.Numbers().Next(), {} };
			for (const auto& [item, count] : held) {
				if (count > 0 && listing.stacks.size() < Protocol::MAX_STORAGE_STACKS) {
					listing.stacks.push_back({ item, static_cast<std::int32_t>((std::min<std::int64_t>)(count, Protocol::MAX_ITEM_COUNT)) });
				}
			}
			return listing;
		}

		// Our own changes as the ledger lets them through, in order: those at a friend's storage go to a_world (OwnStorage).
		ContainerLedger::Receiver::Own OwnOf(World& a_world)
		{
			return [&a_world](const Protocol::IndexedContainerChange& a_change, bool a_lacking) {
				if (a_change.change.item != Protocol::CONTAINER_EMPTIED && a_world.Keeper(a_change.change.container) == StorageView::Keeper::kFriend &&
					a_change.index >= a_world.KeptSince(a_change.change.container)) {
					a_world.OwnStorage(a_change, a_lacking);
				}
			};
		}

		// ---- what is known ---------------------------------------------------------------------------

		// For the save's record: which session, the first change our world lacks, and what waits.
		[[nodiscard]] Protocol::WorldRequest              Point() const { return receiver.Point(); }
		[[nodiscard]] const std::vector<Pending>&         Waiting() const { return pending; }
		[[nodiscard]] const ContainerLedger::Receiver&    Numbers() const { return receiver; }
		[[nodiscard]] const Counts&                       Stats() const { return counts; }
		[[nodiscard]] bool                                Ours(std::uint32_t a_playerId) const { return receiver.Ours(a_playerId); }
		// How many world states we asked for on this connection: the hello's, and each request since (a load's, a gap's).
		// The server counts the same, and says which one each world state answers (Protocol::StorageCaughtUp).
		[[nodiscard]] std::uint32_t                       Asked() const { return asked; }

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
		// Whether the holder still has a stack the session shares (taking it would be reported).
		bool HoldsShared(std::uint32_t a_holder, World& a_world)
		{
			const auto holder = a_world.Holder(a_holder);
			return std::ranges::any_of(a_world.Stacks(a_holder), [&](const Stack& a_stack) {
				auto item = a_world.Item(a_stack.item);
				item.questAlias = a_stack.questAlias;
				return a_stack.count > 0 && LootScope::Of(holder, item, LootScope::Direction::kTake) == LootScope::Scope::kShared;
			});
		}

		// Another player emptied a holder of what the session shares. Their takes went in before this, as many of each item
		// as they took; ours gives up the rest of each item a friend took from it (our roll had more of that than theirs: the
		// holder is bare of it in their game now), and nothing else. What no friend took from it is our copy's own and stays:
		// what our game's roll put there that theirs didn't, an item only a mod of ours adds, what our player put in where
		// the session didn't see it (alone, before a join). Taking every item thus leaves a friend's copy as taking all but one
		// does, but for those leftovers. Weapons, armor, keys, notes, quest items stay, as they would for a take of each, and
		// so does what the session put in and nobody has taken since (a friend's put the sender hadn't seen yet: the server
		// may order it first, and the sender's copy gets it after). A container that keeps things (a home's, a settlement's)
		// is left alone: our copy may hold our player's stash, which the friend never saw.
		Applied TryEmpty(const Protocol::ContainerChange& a_change, World& a_world)
		{
			if (!a_world.Stocked(a_change.container)) {
				return Applied::kNotLoaded;
			}
			const auto holder = a_world.Holder(a_change.container);
			// The holder as a whole, by an item that is always the session's when its holder is (junk).
			const auto scope = LootScope::Of(holder, { .kind = LootScope::Item::kMisc }, LootScope::Direction::kTake);
			if (scope == LootScope::Scope::kNotYetDead) {
				return Applied::kNotYetDead;
			}
			if (scope != LootScope::Scope::kShared || !LootScope::Rolled(holder)) {
				++counts.keptApplied[static_cast<std::size_t>(scope != LootScope::Scope::kShared ? scope : LootScope::Scope::kPerPlayer)];
				return Applied::kDone;
			}
			const auto put = sessionPut.find(a_change.container);
			const auto took = friendTook.find(a_change.container);
			for (const auto& stack : a_world.Stacks(a_change.container)) {
				auto item = a_world.Item(stack.item);
				item.questAlias = stack.questAlias;
				if (stack.count <= 0 || LootScope::Of(holder, item, LootScope::Direction::kTake) != LootScope::Scope::kShared) {
					continue;
				}
				std::int64_t kept = 0;
				if (put != sessionPut.end()) {
					const auto it = put->second.find(stack.item);
					kept = it != put->second.end() ? (std::min<std::int64_t>)(it->second, stack.count) : 0;
				}
				if (kept > 0) {
					counts.emptiedKept += static_cast<std::uint32_t>(kept);
				}
				if (stack.count <= kept) {
					continue;
				}
				if (took == friendTook.end() || !took->second.contains(stack.item)) {
					++counts.emptiedOwn;  // no friend took it from here: their copy never had it
					continue;
				}
				a_world.Apply({ a_change.container, stack.item, -static_cast<std::int32_t>(stack.count - kept) });
				++counts.emptiedStacks;
			}
			// What friends take after this is told afresh (the holder's loot may respawn, and the next emptying is of that).
			if (took != friendTook.end()) {
				friendTook.erase(took);
			}
			++counts.emptiedApplied;
			return Applied::kDone;
		}

		// What the session put into a holder and hasn't taken out since, each item's count (another player's change put
		// in here, or our player's own reported): an emptying leaves it. a_before: how many of the item the holder had
		// before the change. A take is taken from the holder's own roll first: only what it took beyond that was a put of
		// the session's (a friend taking their copy's roll while another's put races the marker leaves the put kept).
		void Track(const Protocol::ContainerChange& a_change, std::int64_t a_before)
		{
			if (a_change.item == Protocol::CONTAINER_EMPTIED || a_change.count == 0) {
				return;
			}
			if (a_change.count > 0) {
				sessionPut[a_change.container][a_change.item] += a_change.count;
				return;
			}
			const auto holder = sessionPut.find(a_change.container);
			if (holder == sessionPut.end()) {
				return;
			}
			if (const auto it = holder->second.find(a_change.item); it != holder->second.end()) {
				const auto roll = (std::max)(std::int64_t{ 0 }, a_before - it->second);
				const auto fromPut = (std::max)(std::int64_t{ 0 }, -std::int64_t{ a_change.count } - roll);
				if ((it->second -= fromPut) <= 0) {
					holder->second.erase(it);
				}
			}
			if (holder->second.empty()) {
				sessionPut.erase(holder);
			}
		}

		static std::int64_t CountOf(std::uint32_t a_holder, std::uint32_t a_item, World& a_world)
		{
			std::int64_t count = 0;
			for (const auto& stack : a_world.Stacks(a_holder)) {
				if (stack.item == a_item) {
					count += stack.count;
				}
			}
			return count;
		}

		// A holder told bare follows our player's moves (what ours holds should be what it held then, give or take
		// them). One that holds more than that, or gave more of something than that, had its loot respawned (a cell
		// reset) and is told again when it is taken bare.
		void Restocked(std::span<const Move> a_moves, World& a_world)
		{
			std::vector<std::uint32_t> touched;
			for (const auto& move : a_moves) {
				const auto told = toldBare.find(move.holder);
				if (told == toldBare.end()) {
					continue;
				}
				told->second[move.item] += move.count;
				if (std::ranges::find(touched, move.holder) == touched.end()) {
					touched.push_back(move.holder);
				}
			}
			for (const auto holder : touched) {
				const auto& told = toldBare.at(holder);
				const auto  more = std::ranges::any_of(told, [](const auto& a_item) { return a_item.second < 0; }) ||
				                   std::ranges::any_of(a_world.Stacks(holder), [&](const Stack& a_stack) {
					const auto it = told.find(a_stack.item);
					return a_stack.count > (it != told.end() ? it->second : 0);
				});
				if (more) {
					toldBare.erase(holder);
				}
			}
		}

		// Storage another game keeps (StorageView): a change goes into our copy only while our menu shows it, from the first
		// change its contents lack; never later (our copy holds our player's own things between views). True when the change
		// is left out.
		bool SkipStorage(std::uint32_t a_index, const Protocol::ContainerChange& a_change, World& a_world)
		{
			if (a_world.Keeper(a_change.container) != StorageView::Keeper::kFriend) {
				return false;
			}
			const auto from = a_world.ViewFrom(a_change.container);
			if (from && a_index >= *from && *from != StorageView::NOT_YET && a_change.item != Protocol::CONTAINER_EMPTIED && a_world.Loaded(a_change.container) &&
				!a_world.Aliased(a_change.container, a_change.item)) {
				return false;
			}
			++counts.storageSkipped;
			return true;
		}

		Applied TryApply(std::uint32_t a_index, const Protocol::ContainerChange& a_change, World& a_world)
		{
			if (SkipStorage(a_index, a_change, a_world)) {
				return Applied::kDone;
			}
			if (!a_world.Loaded(a_change.container)) {
				return Applied::kNotLoaded;
			}
			const auto keeper = a_world.Keeper(a_change.container);
			if (a_change.item == Protocol::CONTAINER_EMPTIED) {
				if (keeper != StorageView::Keeper::kNone) {
					++counts.storageSkipped;  // one game's storage is never emptied by another's word
					return Applied::kDone;
				}
				return TryEmpty(a_change, a_world);
			}
			const auto direction = a_change.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
			if (keeper == StorageView::Keeper::kUs && direction == LootScope::Direction::kTake && !a_world.Stocked(a_change.container)) {
				// Our storage's inventory isn't made yet: it would count as empty, and the take as short. It waits.
				return Applied::kNotLoaded;
			}
			auto       item = a_world.Item(a_change.item);
			// Everyone takes their own quest item (and doesn't get a copy of one someone else put in).
			item.questAlias = direction == LootScope::Direction::kTake && LootScope::IsInventory(item.kind) && a_world.QuestAlias(a_change.container, a_change.item);
			const auto scope = LootScope::Of(a_world.Holder(a_change.container), item, direction);
			if (scope == LootScope::Scope::kNotYetDead) {
				return Applied::kNotYetDead;
			}
			if (scope != LootScope::Scope::kShared) {
				++counts.keptApplied[static_cast<std::size_t>(scope)];
				// A friend's take from our storage that our game doesn't share (another build's rule, or another mod's form):
				// the server counted it taken, our container keeps it, so the taker gives it back.
				if (keeper == StorageView::Keeper::kUs && direction == LootScope::Direction::kTake) {
					a_world.Short(a_index, a_change, -a_change.count);
				}
				return Applied::kDone;
			}
			const auto before = CountOf(a_change.container, a_change.item, a_world);
			if (keeper == StorageView::Keeper::kUs && direction == LootScope::Direction::kTake && before < -static_cast<std::int64_t>(a_change.count)) {
				// Our storage is the true copy: a friend took more than it holds (our world was reloaded since we listed it).
				// What it had goes; the rest the taker gives back.
				a_world.Short(a_index, a_change, static_cast<std::int32_t>(-static_cast<std::int64_t>(a_change.count) - before));
			}
			if (keeper == StorageView::Keeper::kNone) {
				Track(a_change, before);
				if (direction == LootScope::Direction::kTake) {
					friendTook[a_change.container].insert(a_change.item);  // their copy held it: an emptying after takes ours too
				}
			}
			a_world.Apply(a_change);
			return Applied::kDone;
		}

		// Another player's change, next in session order (the ledger keeps the order). Changes to one
		// container stay in order; different containers don't wait for each other.
		void ApplyInOrder(const Protocol::IndexedContainerChange& a_change, World& a_world)
		{
			const auto& change = a_change.change;
			if (SkipStorage(a_change.index, change, a_world)) {
				return;  // never held: it goes into our copy only while we view it
			}
			const auto  first = std::ranges::find(pending, change.container, [](const Pending& a_pending) { return a_pending.change.container; });
			if (first != pending.end()) {
				Hold({ .index = a_change.index, .change = change, .alive = first->alive });  // waits for the same NPC
				return;
			}
			switch (TryApply(a_change.index, change, a_world)) {
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
		std::uint32_t             asked = 0;   // world states asked for on this connection (the hello first): see Asked
		std::vector<Pending>      pending;
		// Holders reported emptied (CONTAINER_EMPTIED) and not stocked with anything shared since: what ours held then, each
		// item's count, moved on by our player's moves since (see Restocked).
		std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::int64_t>> toldBare;
		// What the session put into each holder and nobody took out since (see Track).
		std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::int64_t>> sessionPut;
		// The items other players took from each holder since it was last emptied here (see TryEmpty): an emptying takes
		// the rest of these from our copy, and leaves the rest of its roll.
		std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> friendTook;
		Counts                    counts;
	};
}
