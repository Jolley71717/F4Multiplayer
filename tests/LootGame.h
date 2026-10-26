#pragma once

// One player's game as far as loot goes, driven against a real Server (ServerFixture). The plugin's own
// loot code runs in it: LootSync::Loot, as WorldSync owns it (the scope rule on both ends, the ledger, the
// queue of changes waiting for a container or a death, the save's record). What stands in for the engine
// is only what WorldSync reads from it (LootSync::World): what the containers hold, what is loaded, what
// is alive. The calls come in the plugin's order (Session's packet loop, WorldSync::Frame, FlushLoot
// before a save or a load, the co-save record, Session::OnGameLoaded); the test plays the player, and the
// server's packets go through Protocol as they do in a game.

#include "ServerFixture.h"

#include "game/LootSync.h"
#include "game/StorageView.h"

#include <map>
#include <set>
#include <unordered_map>

namespace LootTest
{
	using namespace TestServer;
	using Clock = LootSync::Clock;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr Clock::duration APPLY_INTERVAL = std::chrono::milliseconds{ 250 };  // WorldSync's retry interval

	inline bool IsType(const std::vector<std::uint8_t>& a_packet, MT a_type)
	{
		return !a_packet.empty() && a_packet[0] == Type(a_type);
	}

	struct Game final :
		LootSync::World
	{
		using Stock = std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>>;  // holder -> item -> count

		PeerId        peer = 0;
		std::uint64_t identity = 0;
		// What the engine knows about the forms (WorldSync reads these): unknown items are junk, unknown
		// holders plain containers.
		std::unordered_map<std::uint32_t, LootScope::ItemFacts>   items;
		std::unordered_map<std::uint32_t, LootScope::HolderFacts> holderFacts;
		std::set<std::pair<std::uint32_t, std::uint32_t>>         questStacks;  // holder, item
		std::set<std::uint32_t>                                   away;         // holders not loaded here
		Stock                                 holders;
		std::map<std::uint32_t, std::int64_t> carried;
		std::set<std::uint32_t>               dead;  // actors dead here (WorldSync's dead set; the kill is applied)
		LootSync::Loot                        loot;
		Clock::time_point                     now{};  // the game's clock: tests move it on
		std::uint64_t                         sessionId = 0;
		std::uint32_t                         localId = 0;
		std::vector<LootSync::Move>           moves;  // since the last frame (WorldSync's inbox)
		std::uint32_t                         applied = 0;   // other players' changes put into this world
		std::uint32_t                         requests = 0;  // world requests sent (after Hello)
		// Shared storage (StorageView.h), as Storage.cpp runs it: our game is the host's (its server's own game: the test sets the
		// server's hostIdentity to ours and marks our peer local).
		bool                                  host = false;
		StorageView::Side                     storage;
		std::int64_t                          givenBackShort = 0;  // a refused take our player no longer had all of (used up)

		struct Save
		{
			Stock                                 holders;
			std::map<std::uint32_t, std::int64_t> carried;
			Protocol::WorldRequest                point;  // the co-save record (session 0: none)
			std::vector<LootSync::Pending>        waiting;
		};

		Game(PeerId a_peer, std::uint64_t a_identity) :
			peer(a_peer), identity(a_identity)
		{}

		// Connects and says hello with what our world has, as Session does; returns the player ID.
		std::uint32_t Join(Fixture& a_f, const std::string& a_name)
		{
			auto hello = Fixture::MakeHello(a_name, identity);
			hello.world = loot.Hello(now);
			const auto welcome = a_f.Join(peer, hello);
			REQUIRE(welcome);
			sessionId = welcome->sessionId;
			localId = welcome->playerId;
			loot.OnWelcome(sessionId, localId);
			storage.OnWelcome(welcome->rules.sharedStorage, host);
			return localId;
		}

		// The player takes from a holder in their own game (no more than it holds). The game sees the move
		// at once; a frame reports it (a_frame false: the frame never comes before the next save or load).
		void Take(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int64_t a_count, bool a_frame = true)
		{
			auto&      have = holders[a_holder][a_item];
			const auto n = (std::min)(have, a_count);
			have -= n;
			carried[a_item] += n;
			Moved(a_f, { a_holder, a_item, -static_cast<std::int32_t>(n) }, a_frame);
		}

		void Put(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int64_t a_count, bool a_frame = true)
		{
			auto&      have = carried[a_item];
			const auto n = (std::min)(have, a_count);
			have -= n;
			holders[a_holder][a_item] += n;
			Moved(a_f, { a_holder, a_item, static_cast<std::int32_t>(n) }, a_frame);
		}

		// The player kills an actor in their game: WorldSync reports the death ahead of the frame's moves.
		void Kill(Fixture& a_f, std::uint32_t a_actor)
		{
			dead.insert(a_actor);
			a_f.fake->Deliver(peer, Protocol::Encode(Protocol::ActorDeath{ a_actor, 0, true }, MT::kReportDeath));
		}

		// A build from before the scope rule, or a changed one: the move goes out as it is.
		void SendRaw(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int32_t a_count)
		{
			a_f.fake->Deliver(peer, Protocol::Encode(Protocol::ContainerChange{ a_holder, a_item, a_count }, MT::kReportContainer));
		}

		// Everything the server has sent us so far, in order (what Session's packet loop would get).
		Packets Fetch(Fixture& a_f)
		{
			Sync(a_f, peer);
			return a_f.fake->Take(peer);
		}

		// Session's frame: the packets as Session::HandlePacket hands them to WorldSync, then WorldSync::Frame.
		void Handle(Fixture* a_f, const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (IsType(packet, MT::kContainerChanged)) {
					if (const auto msg = Protocol::DecodeIndexedContainerChange(packet)) {
						OwnNumbered(*msg);
						loot.Live(*msg, now, *this);
					}
				} else if (IsType(packet, MT::kWorldState)) {
					// WorldSync::ApplyWorldState: the deaths, then the container changes.
					if (const auto msg = Protocol::DecodeWorldState(packet)) {
						dead.insert(msg->deadActors.begin(), msg->deadActors.end());
						for (const auto& change : msg->containerChanges) {
							OwnNumbered(change);
						}
						loot.CatchUp(msg->containerFirstIndex, msg->containerChanges, now, *this);
					}
				} else if (IsType(packet, MT::kActorDied)) {
					if (const auto msg = Protocol::DecodeActorDeath(packet)) {
						dead.insert(msg->refId);
					}
				} else if (IsType(packet, MT::kStorageOwners)) {
					if (const auto msg = Protocol::DecodeStorageOwners(packet)) {
						storage.Owners(*msg);
					}
				} else if (IsType(packet, MT::kStorageContents)) {
					if (const auto msg = Protocol::DecodeStorageListing(packet)) {
						ShowContents(*msg);
					}
				} else if (IsType(packet, MT::kStorageRefused)) {
					if (const auto msg = Protocol::DecodeStorageRefusal(packet)) {
						Refused(*msg);
					}
				}
			}
			if (a_f) {
				Frame(*a_f);
			}
		}

		void Handle(Fixture& a_f, const Packets& a_packets) { Handle(&a_f, a_packets); }

		void Receive(Fixture& a_f) { Handle(a_f, Fetch(a_f)); }

		// WorldSync::Frame (and Session sending what it queued): the moves since the last frame are reported,
		// a gap that stayed open is asked for again, and what waits is tried again.
		void Frame(Fixture& a_f)
		{
			Send(a_f, loot.Report(moves, *this));
			moves.clear();
			if (const auto request = loot.Due(now, *this)) {
				a_f.fake->Deliver(peer, Protocol::Encode(*request));
				++requests;
				storage.UnlistAll();  // the server holds our listings stale from the request on
			}
			loot.Retry(*this, APPLY_INTERVAL);
			ListStorage();
			Send(a_f, std::exchange(storageOut, {}));
		}

		// ---- shared storage (Storage.cpp) ----------------------------------------------------------------------

		Packets storageOut;  // what Storage queued, sent with the frame's

		// The host's game lists the storage it has loaded that it hasn't listed since its world last loaded (Storage's scan).
		void ListStorage()
		{
			if (!host || !storage.Rule()) {
				return;
			}
			for (const auto& [holder, facts] : holderFacts) {
				if (!facts.storage || facts.workshop || away.contains(holder) || storage.Listed(holder)) {
					continue;
				}
				if (const auto listing = loot.Listing(holder, *this)) {
					storageOut.push_back(Protocol::Encode(*listing, MT::kReportStorage));
					storage.MarkListed(holder);
				}
			}
		}

		// What our copy of a container holds of what the session shares.
		std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_holder)
		{
			std::map<std::uint32_t, std::int64_t> out;
			const auto                            holder = Holder(a_holder);
			for (const auto& stack : Stacks(a_holder)) {
				auto item = Item(stack.item);
				item.questAlias = stack.questAlias;
				if (LootScope::Of(holder, item, LootScope::Direction::kTake) == LootScope::Scope::kShared) {
					out[stack.item] += stack.count;
				}
			}
			return out;
		}

		// The player opens the menu on a container (the activation, or the menu's show): a friend's storage moves the shared
		// things our copy held to our player, and asks the server for the true copy.
		void OpenMenu(Fixture& a_f, std::uint32_t a_holder)
		{
			Frame(a_f);  // the moves before it go first
			if (Keeper(a_holder) != StorageView::Keeper::kFriend) {
				return;
			}
			if (storage.Viewing() && *storage.Viewing() != a_holder) {
				CloseStorage();
			}
			if (!storage.Open(a_holder)) {
				return;
			}
			for (const auto& [item, count] : SharedIn(a_holder)) {
				holders[a_holder][item] -= count;
				carried[item] += count;
			}
			storageOut.push_back(Protocol::Encode(Protocol::StorageView{ a_holder, true }));
			Send(a_f, std::exchange(storageOut, {}));
		}

		// The menu closes: the moves made in it go first, then the session's things leave our copy.
		void CloseMenu(Fixture& a_f)
		{
			Frame(a_f);
			CloseStorage();
			Send(a_f, std::exchange(storageOut, {}));
		}

		void CloseStorage()
		{
			if (const auto container = storage.Close()) {
				for (const auto& [item, count] : SharedIn(*container)) {
					holders[*container][item] -= count;
				}
				storageOut.push_back(Protocol::Encode(Protocol::StorageView{ *container, false }));
			}
		}

		void ShowContents(const Protocol::StorageListing& a_contents)
		{
			if (const auto want = storage.Contents(a_contents)) {
				for (const auto& adjust : StorageView::Reconcile(*want, SharedIn(a_contents.container))) {
					holders[a_contents.container][adjust.item] += adjust.count;
				}
			}
		}

		void OwnNumbered(const Protocol::IndexedContainerChange& a_change)
		{
			if (loot.Ours(a_change.playerId)) {
				storage.OwnNumbered(a_change);
			}
		}

		void Refused(const Protocol::StorageRefusal& a_refusal)
		{
			const auto undo = storage.Refused(a_refusal);
			if (undo.closeView) {
				CloseStorage();
				return;
			}
			if (!undo.matched) {
				return;
			}
			auto& have = carried[undo.item];
			if (undo.player < 0 && have < -undo.player) {
				givenBackShort += -undo.player - have;  // used up meanwhile: what is left goes
				have = 0;
			} else {
				have += undo.player;
			}
			holders[undo.container][undo.item] += undo.copy;
			if (undo.refresh && storage.Refresh(undo.container)) {
				storageOut.push_back(Protocol::Encode(Protocol::StorageView{ undo.container, true }));
			}
		}

		// ---- LootSync::World: storage ------------------------------------------------------------------------

		StorageView::Keeper Keeper(std::uint32_t a_holder) override
		{
			return storage.KeeperOf(a_holder, Holder(a_holder).storage, [&](std::uint32_t a_id) { return loot.Ours(a_id); });
		}

		std::optional<std::uint32_t> ViewFrom(std::uint32_t a_holder) override { return storage.ViewFrom(a_holder); }

		void Reporting(const Protocol::ContainerChange& a_change) override { storage.Reported(a_change); }

		void Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing) override
		{
			storageOut.push_back(Protocol::Encode(Protocol::StorageShort{ a_index, a_missing }));
			storage.Unlist(a_change.container);  // listed again, as it is now
			storage.NoteShort();
		}

		// Time passes in the game, one frame every APPLY_INTERVAL. Returns whether a request went out.
		bool Wait(Fixture& a_f, Clock::duration a_time)
		{
			const auto before = requests;
			for (Clock::duration passed{}; passed < a_time; passed += APPLY_INTERVAL) {
				now += APPLY_INTERVAL;
				Frame(a_f);
			}
			return requests != before;
		}

		// Saves: Session::OnBeforeSaveOrLoad (the last moves go out), then the co-save record is written.
		Save SaveGame(Fixture& a_f)
		{
			BeforeSaveOrLoad(a_f);
			return { holders, carried, loot.Point(), loot.Waiting() };
		}

		// Loads a save while connected: Session::OnBeforeSaveOrLoad, the co-save record (F4SE reverts it
		// first), then Session::OnGameLoaded asks for what the loaded world lacks.
		void LoadGame(Fixture& a_f, const Save& a_save)
		{
			BeforeSaveOrLoad(a_f);
			Restore(a_save);
			a_f.fake->Deliver(peer, Protocol::Encode(loot.AfterLoad(sessionId, localId, now)));
			++requests;
		}

		// The game started again and loaded a_save before connecting (Join says hello from its record).
		void Restart(const Save& a_save)
		{
			moves.clear();
			Restore(a_save);
		}

		std::int64_t In(std::uint32_t a_holder, std::uint32_t a_item) const
		{
			const auto h = holders.find(a_holder);
			if (h == holders.end()) {
				return 0;
			}
			const auto i = h->second.find(a_item);
			return i != h->second.end() ? i->second : 0;
		}

		// The player's moves this game kept to itself, and other players' changes it left out.
		std::uint32_t KeptHere() const
		{
			std::uint32_t n = 0;
			for (std::size_t i = 0; i < LootScope::SCOPES; ++i) {
				n += loot.Stats().keptSent[i];
			}
			return n;
		}

		std::uint32_t LeftOut() const
		{
			std::uint32_t n = 0;
			for (std::size_t i = 0; i < LootScope::SCOPES; ++i) {
				n += loot.Stats().keptApplied[i];
			}
			return n;
		}

		std::int64_t Carried(std::uint32_t a_item) const
		{
			const auto it = carried.find(a_item);
			return it != carried.end() ? it->second : 0;
		}

		// ---- LootSync::World: what the engine says -------------------------------------------------------

		bool Loaded(std::uint32_t a_holder) override { return !away.contains(a_holder); }

		LootScope::HolderFacts Holder(std::uint32_t a_holder) override
		{
			const auto it = holderFacts.find(a_holder);
			auto       facts = it != holderFacts.end() ? it->second : LootScope::HolderFacts{ .kind = LootScope::Holder::kContainer };
			if (dead.contains(a_holder)) {
				facts.alive = false;
				facts.knownDead = true;
			}
			return facts;
		}

		LootScope::ItemFacts Item(std::uint32_t a_item) override
		{
			const auto it = items.find(a_item);
			return it != items.end() ? it->second : LootScope::ItemFacts{ .kind = LootScope::Item::kMisc };
		}

		bool QuestAlias(std::uint32_t a_holder, std::uint32_t a_item) override { return questStacks.contains({ a_holder, a_item }); }

		// The console's removeitem takes what is there; additem adds.
		void Apply(const Protocol::ContainerChange& a_change) override
		{
			auto& have = holders[a_change.container][a_change.item];
			have = a_change.count < 0 ? have - (std::min<std::int64_t>)(have, -static_cast<std::int64_t>(a_change.count)) : have + a_change.count;
			++applied;
		}

		std::vector<LootSync::Stack> Stacks(std::uint32_t a_holder) override
		{
			std::vector<LootSync::Stack> stacks;
			if (const auto h = holders.find(a_holder); h != holders.end()) {
				for (const auto& [item, count] : h->second) {
					if (count > 0) {
						stacks.push_back({ item, static_cast<std::int32_t>(count), questStacks.contains({ a_holder, item }) });
					}
				}
			}
			return stacks;
		}

		// A holder whose inventory this game hasn't made yet (never loaded here).
		std::set<std::uint32_t> unstocked;

		bool Stocked(std::uint32_t a_holder) override { return !unstocked.contains(a_holder); }

	private:
		void Moved(Fixture& a_f, const LootSync::Move& a_move, bool a_frame)
		{
			if (a_move.count != 0) {
				moves.push_back(a_move);
			}
			if (a_frame) {
				Frame(a_f);
			}
		}

		void Send(Fixture& a_f, const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				a_f.fake->Deliver(peer, packet);
			}
		}

		void BeforeSaveOrLoad(Fixture& a_f)
		{
			Send(a_f, loot.Report(moves, *this));  // WorldSync::FlushLoot
			moves.clear();
			CloseStorage();  // Storage::BeforeSaveOrLoad: no save holds the session's things in a friend's copy
			Send(a_f, std::exchange(storageOut, {}));
		}

		void Restore(const Save& a_save)
		{
			holders = a_save.holders;
			carried = a_save.carried;
			dead.clear();  // WorldSync::Reset
			loot.LoadRecord(0, 0, {});
			loot.LoadRecord(a_save.point.sessionId, a_save.point.containerFrom, a_save.waiting);
			storage.OnLoad();
		}
	};
}
