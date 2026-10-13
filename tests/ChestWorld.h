#pragma once

// A world for the loot engine (LootSync::Loot) in tests about order and waiting rather than about what is
// shared: every holder is a plain container and every item junk, so the scope rule shares everything. Which
// holders are loaded, and what putting a change in does, are the test's.

#include "game/LootSync.h"

#include <functional>
#include <utility>

namespace TestServer
{
	struct ChestWorld final :
		LootSync::World
	{
		using Change = Protocol::ContainerChange;

		std::function<bool(std::uint32_t)>  loaded;
		std::function<void(const Change&)> put;

		ChestWorld(std::function<bool(std::uint32_t)> a_loaded, std::function<void(const Change&)> a_put) :
			loaded(std::move(a_loaded)), put(std::move(a_put))
		{}

		bool Loaded(std::uint32_t a_holder) override { return loaded(a_holder); }

		LootScope::HolderFacts Holder(std::uint32_t) override { return { .kind = LootScope::Holder::kContainer }; }

		LootScope::ItemFacts Item(std::uint32_t) override { return { .kind = LootScope::Item::kMisc }; }

		bool QuestAlias(std::uint32_t, std::uint32_t) override { return false; }

		void Apply(const Change& a_change) override { put(a_change); }
	};

	// A change numbered by the server, as it comes to a game.
	inline Protocol::IndexedContainerChange Numbered(std::uint32_t a_index, std::uint32_t a_playerId, const Protocol::ContainerChange& a_change)
	{
		return { .index = a_index, .playerId = a_playerId, .change = a_change };
	}
}
