#pragma once

// A friend's game as far as loot goes, for tests that feed it what a real server sent: WorldSync's glue
// around the loot engine (LootSync::Loot: each change live or in a world state; its own changes skipped;
// the record a save keeps) and RetrySchedule (what loaded, a loading screen, the sweep), against a world in
// which only some containers are loaded. Every holder is a plain container and every item shared (the
// scope rule has tests of its own). The game itself is the part left out.

#include "ServerFixture.h"

#include "game/LootSync.h"
#include "game/PendingContainers.h"

#include <map>
#include <set>
#include <utility>

namespace TestServer
{
	// Containers as the game keeps them: taking what isn't there takes nothing, so the order of a
	// container's changes matters (take one, then put one back, leaves one; the other way round, none).
	struct Inventories
	{
		std::map<std::pair<std::uint32_t, std::uint32_t>, int> items;

		void Change(const Protocol::ContainerChange& a_change)
		{
			auto& count = items[{ a_change.container, a_change.item }];
			count = (std::max)(0, count + a_change.count);
		}

		[[nodiscard]] std::map<std::pair<std::uint32_t, std::uint32_t>, int> Of(std::uint32_t a_container) const
		{
			std::map<std::pair<std::uint32_t, std::uint32_t>, int> out;
			for (const auto& [key, count] : items) {
				if (key.first == a_container && count != 0) {
					out[key] = count;
				}
			}
			return out;
		}

		// The session as a game that has every change, in the server's order, would have it.
		static Inventories FromEchoes(std::vector<Protocol::IndexedContainerChange> a_echoes)
		{
			std::ranges::sort(a_echoes, {}, &Protocol::IndexedContainerChange::index);
			Inventories session;
			for (std::uint32_t i = 0; i < a_echoes.size(); ++i) {
				REQUIRE(a_echoes[i].index == i);
				session.Change(a_echoes[i].change);
			}
			return session;
		}
	};

	struct FriendGame final :
		LootSync::World
	{
		using Change = Protocol::ContainerChange;
		using Clock = RetrySchedule::Clock;

		std::uint32_t           me = 0;
		std::set<std::uint32_t> loaded;
		Inventories             world;
		LootSync::Loot          loot;
		std::size_t             worldStates = 0;
		std::size_t             welcomes = 0;
		// Containers whose changes came live before this game's Welcome (a game takes them as they come).
		std::size_t             beforeWelcome = 0;
		// The frame loop: in the world or in a loading screen, the cell, what the game said loaded.
		RetrySchedule              schedule{ std::chrono::seconds(2), 256 };
		bool                       inWorld = true;
		std::uint32_t              cell = 0x0000003C;
		std::vector<std::uint32_t> loadInbox;
		Clock::time_point          now = Clock::now();

		// ---- LootSync::World: a container can be changed while we are in the world and it is loaded ----
		bool                   Loaded(std::uint32_t a_holder) override { return inWorld && loaded.contains(a_holder); }
		LootScope::HolderFacts Holder(std::uint32_t) override { return { .kind = LootScope::Holder::kContainer }; }
		LootScope::ItemFacts   Item(std::uint32_t) override { return { .kind = LootScope::Item::kMisc }; }
		bool                   QuestAlias(std::uint32_t, std::uint32_t) override { return false; }
		void                   Apply(const Change& a_change) override { world.Change(a_change); }

		// Whether something waits for a_container.
		[[nodiscard]] bool Waits(std::uint32_t a_container) const
		{
			return std::ranges::any_of(loot.Waiting(), [&](const LootSync::Pending& a_pending) { return a_pending.change.container == a_container; });
		}

		// The first change of the session our world lacks.
		[[nodiscard]] std::uint32_t Next() const { return loot.Point().containerFrom; }

		// A change that came live (WorldSync::ApplyContainerChange).
		void Offer(const Protocol::IndexedContainerChange& a_change)
		{
			loot.Live(a_change, now, *this);
		}

		// The server welcomed this game (Session hands the Welcome to WorldSync's loot through SessionSteps).
		void Welcome(const Protocol::Welcome& a_welcome)
		{
			me = a_welcome.playerId;
			loot.OnWelcome(a_welcome.sessionId, a_welcome.playerId);
		}

		// Everything the server sent, in the order it came (Session hands each packet to WorldSync).
		void Take(const std::vector<std::vector<std::uint8_t>>& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet.empty()) {
					continue;
				}
				if (packet[0] == Type(MT::kWelcome)) {
					const auto welcome = Protocol::DecodeWelcome(packet);
					REQUIRE(welcome);
					Welcome(*welcome);
					++welcomes;
				} else if (packet[0] == Type(MT::kWorldState)) {
					const auto state = Protocol::DecodeWorldState(packet);
					REQUIRE(state);
					++worldStates;
					loot.CatchUp(state->containerFirstIndex, state->containerChanges, now, *this);
				} else if (packet[0] == Type(MT::kContainerChanged)) {
					const auto change = Protocol::DecodeIndexedContainerChange(packet);
					REQUIRE(change);
					beforeWelcome += welcomes == 0;
					Offer(*change);
				}
			}
		}

		// One frame of WorldSync::Frame's retry part, a_step after the last.
		RetrySchedule::Plan Frame(Clock::duration a_step = std::chrono::milliseconds(16))
		{
			now += a_step;
			auto plan = schedule.Frame(now, inWorld, cell, std::exchange(loadInbox, {}), !loot.Waiting().empty(), [&] { return loot.Containers(); });
			for (const auto& ids : { plan.loaded, plan.others }) {
				for (const auto id : ids) {
					loot.Retry(id, *this);
				}
			}
			return plan;
		}

		// The game says the container's 3D loaded (WorldSync's refLoadInbox); the next frame tries it.
		void Load(std::uint32_t a_container)
		{
			loaded.insert(a_container);
			loadInbox.push_back(a_container);
		}

		// The player walks up to the container: it loads, and a frame goes by.
		void Arrive(std::uint32_t a_container)
		{
			Load(a_container);
			Frame();
		}

		// The player takes or puts something themselves: in their world at once, and reported.
		void Loot(Fixture& a_f, PeerId a_peer, const Change& a_change)
		{
			world.Change(a_change);
			a_f.fake->Deliver(a_peer, Protocol::Encode(a_change, MT::kReportContainer));
		}

		// The game loads a save made in this session: its co-save record (WorldSync::Load).
		void LoadRecord(std::uint32_t a_next, std::vector<LootSync::Pending> a_entries)
		{
			const auto session = loot.Point().sessionId;
			loot.LoadRecord(0, 0, {});  // F4SE reverts first
			loot.LoadRecord(session, a_next, std::move(a_entries));
		}

		// The player loads a save: what it kept.
		void LoadSave(const Inventories& a_world, std::uint32_t a_next, std::vector<LootSync::Pending> a_entries)
		{
			world = a_world;
			LoadRecord(a_next, std::move(a_entries));
		}

		// The game restarted and loaded a save: a new game, not yet welcomed, with nothing of the last run's frames
		// (nor of its loot engine: which player IDs are its own comes with the next Welcome).
		void Restart(const Inventories& a_world, std::uint32_t a_next, std::vector<LootSync::Pending> a_entries)
		{
			const auto session = loot.Point().sessionId;
			loot = LootSync::Loot{};
			loot.LoadRecord(session, a_next, std::move(a_entries));
			world = a_world;
			welcomes = 0;
			beforeWelcome = 0;
			schedule = RetrySchedule{ std::chrono::seconds(2), 256 };
			loadInbox.clear();
		}
	};
}
