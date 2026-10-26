#pragma once

// One player's game as far as loot goes, driven against a real Server (ServerFixture). The plugin's own
// loot code runs in it: LootSync::Loot, as WorldSync owns it (the scope rule on both ends, the ledger, the
// queue of changes waiting for a container or a death, the save's record), and StorageView::Client, as
// Storage.cpp runs it. What stands in for the engine is only what WorldSync and Storage read from it and
// change in it (LootSync::World, StorageView::Engine): what the containers hold, what is loaded, what is
// alive. The calls come in the plugin's order (Session's packet loop, WorldSync::Frame, FlushLoot before a
// save or a load, the co-save records, Session::OnGameLoaded); the test plays the player, and the server's
// packets go through Protocol as they do in a game.

#include "ServerFixture.h"

#include "game/LootSync.h"
#include "game/StorageView.h"

#include <array>
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
		LootSync::World,
		StorageView::Engine
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
		std::uint32_t                         caughtUp = 0;  // kStorageCaughtUp markers read
		// Shared storage, as Storage.cpp runs it: our game is the host's (its server's own game: the test sets the server's
		// hostIdentity to ours and marks our peer local).
		bool                                  host = false;
		StorageView::Client                   storage{ *this };
		bool                                  connected = false;
		std::optional<std::uint32_t>          menu;        // the container our menu is open on
		bool                                  hideAsked = false;
		std::array<std::uint32_t, static_cast<std::size_t>(StorageView::Notice::kCount)> notices{};
		// Items of ours that weren't moved where an engine call said (our player lacked them): see ChangePlayer.
		std::int64_t                          lacked = 0;
		// Moves the session's numbering can't carry (FormTranslate drops them at Session::SendOut): the items here.
		std::set<std::uint32_t>               untranslatable;

		struct Save
		{
			Stock                                  holders;
			std::map<std::uint32_t, std::int64_t>  carried;
			Protocol::WorldRequest                 point;  // the co-save record (session 0: none)
			std::vector<LootSync::Pending>         waiting;
			std::uint64_t                          movesSession = 0;  // Storage's record: our unanswered moves
			std::vector<Protocol::ContainerChange> unanswered;
			std::vector<std::uint32_t>             lacks;      // and our numbered moves its world lacks
			std::uint32_t                          lacksFrom = 0;
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
			connected = true;
			loot.OnWelcome(sessionId, localId);
			storage.OnWelcome(welcome->rules.sharedStorage, host, sessionId);
			return localId;
		}

		// The connection goes (Session: Storage first, then WorldSync forgets the frame's packets). What the server
		// sent and we hadn't read is lost with it.
		void Disconnect(Fixture& a_f)
		{
			storage.OnDisconnected();
			loot.OnDisconnected();
			outbox.clear();
			connected = false;
			menu.reset();
			a_f.fake->Drop(peer);
			a_f.fake->Take(peer);
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

		// Session's frame: the packets as Session::HandlePacket hands them to WorldSync and Storage, then the frame.
		void Handle(Fixture* a_f, const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (IsType(packet, MT::kContainerChanged)) {
					if (const auto msg = Protocol::DecodeIndexedContainerChange(packet)) {
						loot.Live(*msg, now, *this);
					}
				} else if (IsType(packet, MT::kWorldState)) {
					// WorldSync::ApplyWorldState: the deaths, then the container changes.
					if (const auto msg = Protocol::DecodeWorldState(packet)) {
						dead.insert(msg->deadActors.begin(), msg->deadActors.end());
						loot.CatchUp(msg->containerFirstIndex, msg->containerChanges, now, *this);
					}
				} else if (IsType(packet, MT::kActorDied)) {
					if (const auto msg = Protocol::DecodeActorDeath(packet)) {
						dead.insert(msg->refId);
					}
				} else if (IsType(packet, MT::kStorageOwners)) {
					if (const auto msg = Protocol::DecodeStorageOwners(packet)) {
						storage.State().Owners(*msg);
					}
				} else if (IsType(packet, MT::kStorageContents)) {
					if (const auto msg = Protocol::DecodeStorageListing(packet)) {
						storage.OnContents(*msg);
					}
				} else if (IsType(packet, MT::kStorageRefused)) {
					if (const auto msg = Protocol::DecodeStorageRefusal(packet)) {
						storage.OnRefused(*msg);
					}
				} else if (IsType(packet, MT::kStorageCaughtUp)) {
					// Storage::OnCaughtUp: our moves at a friend's storage settle once the world state answers our latest request.
					if (const auto msg = Protocol::DecodeStorageCaughtUp(packet)) {
						++caughtUp;
						storage.OnCaughtUp(msg->asked, loot.Asked(), loot.Numbers().Numbers().Next(), loot.Numbers().Numbers().Waiting() != 0);
					}
				}
			}
			// The menu Storage hid closes (the engine's close event, posted to the main thread).
			if (std::exchange(hideAsked, false) && menu) {
				menu.reset();
				storage.MenuClosed();
			}
			if (a_f) {
				Frame(*a_f);
			}
		}

		void Handle(Fixture& a_f, const Packets& a_packets) { Handle(&a_f, a_packets); }

		void Receive(Fixture& a_f) { Handle(a_f, Fetch(a_f)); }

		// WorldSync::Frame and Storage::Frame (and Session sending what they queued): the moves since the last frame are
		// reported, a gap that stayed open is asked for again, and what waits is tried again.
		void Frame(Fixture& a_f)
		{
			FlushLoot();
			if (const auto request = loot.Due(now, *this)) {
				outbox.push_back(Protocol::Encode(*request));
				++requests;
				storage.State().UnlistAll();  // the server holds our listings stale from the request on
				storage.Asked();               // Storage::UnlistAll: and we settle again after its answer
			}
			loot.Retry(*this, APPLY_INTERVAL);
			if (connected) {
				storage.Frame();
			}
			ListStorage();
			SendOut(a_f);
		}

		// ---- shared storage (Storage.cpp) ----------------------------------------------------------------------

		Packets outbox;  // what WorldSync and Storage queued, in order, sent with the frame (Session::SendOut)

		// The host's game lists the storage it has loaded that it hasn't listed since its world last loaded (Storage's scan).
		void ListStorage()
		{
			auto& side = storage.State();
			if (!host || !side.Rule()) {
				return;
			}
			for (const auto& [holder, facts] : holderFacts) {
				if (!facts.storage || facts.workshop || away.contains(holder) || side.Listed(holder)) {
					continue;
				}
				if (const auto listing = loot.Listing(holder, *this)) {
					outbox.push_back(Protocol::Encode(*listing, MT::kReportStorage));
					side.MarkListed(holder);
				}
			}
		}

		// The player opens the menu on a container (Equipment's show, posted): Storage::OnMenuShown.
		void OpenMenu(Fixture& a_f, std::uint32_t a_holder)
		{
			menu = a_holder;
			storage.Open(a_holder, Keeper(a_holder) == StorageView::Keeper::kFriend);
			SendOut(a_f);
		}

		// The menu closes (WorldSync's MenuWatcher, posted): Storage::OnMenuClosed.
		void CloseMenu(Fixture& a_f)
		{
			menu.reset();
			storage.MenuClosed();
			SendOut(a_f);
		}

		// What our copy of a container holds of what the session shares (WorldSync::SharedIn).
		std::map<std::uint32_t, std::int64_t> SharedIn(std::uint32_t a_holder) override { return LootSync::SharedIn(*this, a_holder); }

		std::set<std::uint32_t> Aliased(std::uint32_t a_holder) override { return LootSync::Aliased(*this, a_holder); }

		// ---- StorageView::Engine -----------------------------------------------------------------------------

		std::int64_t ChangeCopy(std::uint32_t a_container, std::uint32_t a_item, std::int64_t a_count) override
		{
			return Change(holders[a_container][a_item], a_count);
		}

		std::int64_t ChangePlayer(const StorageView::PlayerChange& a_change) override
		{
			const auto done = Change(carried[a_change.Item()], a_change.Count());
			lacked += done - a_change.Count();  // a take of more than our player had: what they no longer had
			return done;
		}

		void FlushLoot() override
		{
			for (auto& packet : loot.Report(moves, *this)) {
				outbox.push_back(std::move(packet));
			}
			moves.clear();
		}

		void Send(std::vector<std::uint8_t> a_packet) override { outbox.push_back(std::move(a_packet)); }

		void Notify(StorageView::Notice a_notice) override { ++notices[static_cast<std::size_t>(a_notice)]; }

		void HideMenu() override { hideAsked = menu.has_value(); }

		Clock::time_point Now() override { return now; }

		std::uint32_t Noticed(StorageView::Notice a_notice) const { return notices[static_cast<std::size_t>(a_notice)]; }

		// ---- LootSync::World: storage ------------------------------------------------------------------------

		StorageView::Keeper Keeper(std::uint32_t a_holder) override
		{
			return storage.State().KeeperOf(a_holder, [&](std::uint32_t a_id) { return loot.Ours(a_id); });
		}

		std::optional<std::uint32_t> ViewFrom(std::uint32_t a_holder) override { return storage.State().ViewFrom(a_holder); }

		std::uint32_t KeptSince(std::uint32_t a_holder) override { return storage.State().KeptSince(a_holder); }

		bool Aliased(std::uint32_t a_holder, std::uint32_t a_item) override { return storage.State().Aliased(a_holder, a_item); }

		bool Reporting(const Protocol::ContainerChange& a_change) override { return storage.Reporting(a_change); }

		void OwnStorage(const Protocol::IndexedContainerChange& a_change, bool a_lacking) override { storage.OwnStorage(a_change, a_lacking); }

		void Short(std::uint32_t a_index, const Protocol::ContainerChange& a_change, std::int32_t a_missing) override
		{
			outbox.push_back(Protocol::Encode(Protocol::StorageShort{ a_index, a_missing }));
			storage.State().Unlist(a_change.container);  // listed again, as it is now
			storage.State().NoteShort();
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

		// Saves: Session::OnBeforeSaveOrLoad (the last moves go out), then the co-save records are written.
		Save SaveGame(Fixture& a_f)
		{
			BeforeSaveOrLoad(a_f);
			return { holders, carried, loot.Point(), loot.Waiting(), storage.State().MovesSession(), storage.State().Unanswered(), storage.State().Lacking(), storage.State().LacksFrom() };
		}

		// Loads a save while connected: Session::OnBeforeSaveOrLoad, the co-save records (F4SE reverts them
		// first), then Session::OnGameLoaded asks for what the loaded world lacks.
		void LoadGame(Fixture& a_f, const Save& a_save)
		{
			BeforeSaveOrLoad(a_f);
			Restore(a_save);
			outbox.push_back(Protocol::Encode(loot.AfterLoad(sessionId, localId, now)));
			++requests;
			SendOut(a_f);
		}

		// The game started again and loaded a_save before connecting (Join says hello from its record).
		void Restart(const Save& a_save)
		{
			moves.clear();
			outbox.clear();
			connected = false;
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
		// Adds to or takes from a count (a take takes no more than there is): what was changed.
		static std::int64_t Change(std::int64_t& a_have, std::int64_t a_count)
		{
			const auto done = a_count < 0 ? -(std::min)(a_have, -a_count) : a_count;
			a_have += done;
			return done;
		}

		void Moved(Fixture& a_f, const LootSync::Move& a_move, bool a_frame)
		{
			if (a_move.count != 0) {
				moves.push_back(a_move);
			}
			if (a_frame) {
				Frame(a_f);
			}
		}

		// Session::SendOut: what the session's numbering can't carry is dropped, and Storage hears of it (Storage::Unsent).
		void SendOut(Fixture& a_f)
		{
			for (auto& packet : std::exchange(outbox, {})) {
				if (!connected) {
					continue;
				}
				if (IsType(packet, MT::kReportContainer)) {
					if (const auto change = Protocol::DecodeContainerChange(packet); change && untranslatable.contains(change->item)) {
						storage.Unsent(*change);
						continue;
					}
				}
				a_f.fake->Deliver(peer, std::move(packet));
			}
			// What an undo above queued (a view asked afresh) goes in the next frame.
		}

		void BeforeSaveOrLoad(Fixture& a_f)
		{
			FlushLoot();                // WorldSync::FlushLoot
			storage.BeforeSaveOrLoad();  // Storage::BeforeSaveOrLoad: no save holds the session's things in a friend's copy
			menu.reset();
			SendOut(a_f);
		}

		// The co-save records are read (F4SE reverts them first), then Session::OnGameLoaded: WorldSync forgets, and
		// Storage::OnGameLoaded takes the save's unanswered moves when the save is the session's we are in.
		void Restore(const Save& a_save)
		{
			holders = a_save.holders;
			carried = a_save.carried;
			dead.clear();  // WorldSync::Reset
			loot.LoadRecord(0, 0, {});
			loot.LoadRecord(a_save.point.sessionId, a_save.point.containerFrom, a_save.waiting);
			const auto session = connected ? sessionId : a_save.movesSession;
			const bool same = a_save.movesSession == session;
			storage.OnLoad(session, same ? a_save.unanswered : std::vector<Protocol::ContainerChange>{}, same ? a_save.lacks : std::vector<std::uint32_t>{}, same ? a_save.lacksFrom : 0);
		}
	};
}
