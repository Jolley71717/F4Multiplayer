#pragma once

// One player's game as far as loot goes, driven against a real Server (ServerFixture): what its
// containers hold, what the player carries, saves and loads, and the plugin's own game-free parts in
// between: WorldSync's receiving end (ContainerLedger::Receiver) and the scope rule (LootScope) on
// both ends. The test plays the player; the server's packets go through Protocol as they do in a game.

#include "ServerFixture.h"

#include "game/ContainerLedger.h"
#include "game/LootScope.h"

#include <map>
#include <set>
#include <thread>
#include <unordered_map>

namespace LootTest
{
	using namespace TestServer;
	using Clock = ContainerLedger::Clock;
	using Packets = std::vector<std::vector<std::uint8_t>>;

	// Returns once the server has handled everything a_peer sent so far (it answers a heartbeat in order).
	inline void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	// What the game knows about the forms involved (WorldSync reads these from the engine).
	struct World
	{
		std::unordered_map<std::uint32_t, LootScope::ItemFacts>   items;    // base form -> facts
		std::unordered_map<std::uint32_t, LootScope::HolderFacts> holders;  // reference -> facts (unknown: a plain container)

		LootScope::HolderFacts Holder(std::uint32_t a_ref) const
		{
			const auto it = holders.find(a_ref);
			return it != holders.end() ? it->second : LootScope::HolderFacts{ .kind = LootScope::Holder::kContainer };
		}

		LootScope::ItemFacts Item(std::uint32_t a_item) const
		{
			const auto it = items.find(a_item);
			return it != items.end() ? it->second : LootScope::ItemFacts{ .kind = LootScope::Item::kMisc };
		}
	};

	struct Game
	{
		using Stock = std::map<std::uint32_t, std::map<std::uint32_t, std::int64_t>>;  // holder -> item -> count

		PeerId                    peer = 0;
		std::uint64_t             identity = 0;
		World                     world;
		Stock                     holders;
		std::map<std::uint32_t, std::int64_t> carried;
		std::set<std::uint32_t>   dead;       // actors the session told us died (WorldSync's dead set)
		ContainerLedger::Receiver loot;
		Clock::time_point         now{};      // the game's clock: tests move it on by hand
		std::uint64_t             sessionId = 0;
		std::uint32_t             localId = 0;
		std::uint32_t             sent = 0;       // container changes reported
		std::uint32_t             notShared = 0;  // moves the scope rule kept in this game (either end)
		std::vector<Protocol::IndexedContainerChange> waiting;  // NotYetDead: tried again when the actor dies here

		struct Save
		{
			Stock                                 holders;
			std::map<std::uint32_t, std::int64_t> carried;
			Protocol::WorldRequest                point;  // the co-save record (session 0: none)
		};

		Game(PeerId a_peer, std::uint64_t a_identity) :
			peer(a_peer), identity(a_identity)
		{}

		// Connects and says hello with what our world has, as Session does; returns the player ID.
		std::uint32_t Join(Fixture& a_f, const std::string& a_name)
		{
			auto hello = Fixture::MakeHello(a_name, identity);
			hello.world = loot.Request(now);
			const auto welcome = a_f.Join(peer, hello);
			REQUIRE(welcome);
			sessionId = welcome->sessionId;
			localId = welcome->playerId;
			loot.OnWelcome(sessionId, localId);
			return localId;
		}

		// The player takes from a holder in their own game (no more than it holds), and the game reports it
		// if it is the session's.
		void Take(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int64_t a_count)
		{
			auto& have = holders[a_holder][a_item];
			const auto n = (std::min)(have, a_count);
			have -= n;
			carried[a_item] += n;
			Report(a_f, a_holder, a_item, -static_cast<std::int32_t>(n));
		}

		void Put(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int64_t a_count)
		{
			auto& have = carried[a_item];
			const auto n = (std::min)(have, a_count);
			have -= n;
			holders[a_holder][a_item] += n;
			Report(a_f, a_holder, a_item, static_cast<std::int32_t>(n));
		}

		// A build from before the scope rule: every move goes out.
		void SendRaw(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int32_t a_count)
		{
			a_f.fake->Deliver(peer, Protocol::Encode(Protocol::ContainerChange{ a_holder, a_item, a_count }, MT::kReportContainer));
			++sent;
		}

		// Everything the server has sent us so far, in order (what Session's packet loop would get).
		Packets Fetch(Fixture& a_f)
		{
			Sync(a_f, peer);
			return a_f.fake->Take(peer);
		}

		// Handles packets as Session::HandlePacket hands them to WorldSync.
		void Handle(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet.empty()) {
					continue;
				}
				switch (static_cast<MT>(packet[0])) {
				case MT::kContainerChanged:
					if (const auto msg = Protocol::DecodeIndexedContainerChange(packet)) {
						loot.Live(*msg, now, Applier());
					}
					break;
				case MT::kWorldState:
					if (const auto msg = Protocol::DecodeWorldState(packet)) {
						for (const auto id : msg->deadActors) {
							Died(id);
						}
						loot.CatchUp(msg->containerFirstIndex, msg->containerChanges, now, Applier());
					}
					break;
				case MT::kActorDied:
					if (const auto msg = Protocol::DecodeActorDeath(packet)) {
						Died(msg->refId);
					}
					break;
				default:
					break;
				}
			}
		}

		void Receive(Fixture& a_f) { Handle(Fetch(a_f)); }

		// WorldSync's frame: asks the server again when a gap has stayed open.
		bool Tick(Fixture& a_f, Clock::duration a_elapsed)
		{
			now += a_elapsed;
			if (!loot.Due(now)) {
				return false;
			}
			a_f.fake->Deliver(peer, Protocol::Encode(loot.Request(now)));
			return true;
		}

		Save SaveGame() const { return { holders, carried, loot.Point() }; }

		// Loads a save while connected, in the plugin's order: the co-save record, then (Session::OnGameLoaded)
		// a save without our session's record takes it, and the request goes out.
		void LoadGame(Fixture& a_f, const Save& a_save)
		{
			holders = a_save.holders;
			carried = a_save.carried;
			dead.clear();
			waiting.clear();
			loot.Load(a_save.point.sessionId, a_save.point.containerFrom);
			loot.Adopt(sessionId, localId);
			a_f.fake->Deliver(peer, Protocol::Encode(loot.Request(now)));
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

		std::int64_t Carried(std::uint32_t a_item) const
		{
			const auto it = carried.find(a_item);
			return it != carried.end() ? it->second : 0;
		}

	private:
		LootScope::HolderFacts Facts(std::uint32_t a_holder) const
		{
			auto facts = world.Holder(a_holder);
			facts.knownDead = dead.contains(a_holder);
			return facts;
		}

		void Report(Fixture& a_f, std::uint32_t a_holder, std::uint32_t a_item, std::int32_t a_count)
		{
			if (a_count == 0) {
				return;
			}
			const auto direction = a_count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
			if (LootScope::Of(Facts(a_holder), world.Item(a_item), direction) != LootScope::Scope::kShared) {
				++notShared;
				return;
			}
			SendRaw(a_f, a_holder, a_item, a_count);
		}

		// Another player's change, as WorldSync's TryApplyContainer makes it: removeitem takes what is there.
		bool TryApply(const Protocol::ContainerChange& a_change)
		{
			const auto direction = a_change.count < 0 ? LootScope::Direction::kTake : LootScope::Direction::kPut;
			switch (LootScope::Of(Facts(a_change.container), world.Item(a_change.item), direction)) {
			case LootScope::Scope::kShared:
				break;
			case LootScope::Scope::kNotYetDead:
				return false;
			default:
				++notShared;
				return true;
			}
			auto& have = holders[a_change.container][a_change.item];
			have = a_change.count < 0 ? have - (std::min<std::int64_t>)(have, -static_cast<std::int64_t>(a_change.count)) : have + a_change.count;
			return true;
		}

		ContainerLedger::Receiver::Apply Applier()
		{
			return [this](const Protocol::IndexedContainerChange& a_change) {
				if (!TryApply(a_change.change)) {
					waiting.push_back(a_change);
				}
			};
		}

		// The actor died here: its kill came. What waited for it goes in (WorldSync's pending retry).
		void Died(std::uint32_t a_actor)
		{
			dead.insert(a_actor);
			std::erase_if(waiting, [&](const Protocol::IndexedContainerChange& a_change) { return TryApply(a_change.change); });
		}
	};
}
