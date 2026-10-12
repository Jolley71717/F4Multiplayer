#include "Test.h"

#include "game/FoundDead.h"

#include <algorithm>
#include <cstdint>

using FoundDead::Npc;

TEST("found dead: with a shared story, every NPC dead in the host's save is dead for everyone")
{
	CHECK(FoundDead::Shared(true, Npc{}));
	CHECK(FoundDead::Shared(true, Npc{ .unique = true }));
	CHECK(FoundDead::Shared(true, Npc{ .essential = true }));
	CHECK(FoundDead::Shared(true, Npc{ .questAlias = true }));
}

TEST("found dead: with everyone on their own story, story characters dead in the host's save live on for the others")
{
	CHECK(FoundDead::Shared(false, Npc{}));  // a bloatfly, a raider
	CHECK(!FoundDead::Shared(false, Npc{ .unique = true }));  // a companion
	CHECK(!FoundDead::Shared(false, Npc{ .essential = true }));
	CHECK(!FoundDead::Shared(false, Npc{ .questAlias = true }));
}

namespace
{
	std::unordered_set<std::uint32_t> Filled(std::size_t a_count)
	{
		std::unordered_set<std::uint32_t> set;
		for (std::uint32_t id = 1; set.size() < a_count; ++id) {
			set.insert(id);
		}
		return set;
	}
}

TEST("found dead: the loaded set grows freely below its cap")
{
	auto loaded = Filled(10);
	int  calls = 0;
	FoundDead::TrackLoad(loaded, 0xABCDu, [&](std::uint32_t) { ++calls; return true; });
	CHECK(loaded.size() == 11);
	CHECK(calls == 0);
	FoundDead::TrackLoad(loaded, 0xABCDu, [](std::uint32_t) { return true; });  // already in
	CHECK(loaded.size() == 11);
}

TEST("found dead: a full loaded set drops what is no longer loaded first")
{
	auto loaded = Filled(FoundDead::MAX_LOADED);
	// Every other one has gone without an unload event.
	FoundDead::TrackLoad(loaded, 0xABCDu, [](std::uint32_t a_id) { return a_id % 2 == 0; });
	CHECK(loaded.size() == FoundDead::MAX_LOADED / 2 + 1);
	CHECK(loaded.contains(0xABCDu));
	CHECK(std::ranges::all_of(loaded, [](std::uint32_t a_id) { return a_id == 0xABCDu || a_id % 2 == 0; }));
}

TEST("found dead: a full loaded set is pruned once, down to a lower mark, even when all of it is loaded")
{
	auto loaded = Filled(FoundDead::MAX_LOADED);
	int  calls = 0;
	const auto allLoaded = [&](std::uint32_t) { ++calls; return true; };
	FoundDead::TrackLoad(loaded, 0xABCDu, allLoaded);
	CHECK(loaded.size() == FoundDead::PRUNED_TO + 1);
	CHECK(loaded.contains(0xABCDu));
	CHECK(calls == static_cast<int>(FoundDead::MAX_LOADED));

	// The next loads up to the cap don't prune again.
	calls = 0;
	for (std::uint32_t id = 0x10000; loaded.size() < FoundDead::MAX_LOADED; ++id) {
		FoundDead::TrackLoad(loaded, id, allLoaded);
	}
	CHECK(calls == 0);
	CHECK(loaded.size() == FoundDead::MAX_LOADED);
}
