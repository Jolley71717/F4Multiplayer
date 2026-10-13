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

#include <map>
#include <set>
#include <unordered_map>

namespace LootTest
{
	using namespace TestServer;
	using Clock = LootSync::Clock;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	constexpr Clock::duration APPLY_INTERVAL = std::chrono::milliseconds{ 250 };  // WorldSync's retry interval

	// Returns once the server has handled everything a_peer sent so far (it answers a heartbeat in order).
	inline void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

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
			if (const auto request = loot.Due(now)) {
				a_f.fake->Deliver(peer, Protocol::Encode(*request));
				++requests;
			}
			loot.Retry(*this, APPLY_INTERVAL);
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
		}

		void Restore(const Save& a_save)
		{
			holders = a_save.holders;
			carried = a_save.carried;
			dead.clear();  // WorldSync::Reset
			loot.LoadRecord(0, 0, {});
			loot.LoadRecord(a_save.point.sessionId, a_save.point.containerFrom, a_save.waiting);
		}
	};
}
