#include "Test.h"

#include "game/InertImpact.h"

#include <algorithm>
#include <thread>
#include <vector>

// A friend's stand-in's plain bullet or beam passes through bodies (game/InertImpact.h): which impacts are refused, the
// four call sites checked before any is patched, and the copies' list read with no lock.
namespace
{
	// The five bytes of a near call at a_at to a_target.
	std::array<std::uint8_t, 5> CallTo(std::uintptr_t a_at, std::uintptr_t a_target)
	{
		const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(a_at + 5)));
		return { 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
	}

	constexpr std::uintptr_t BASE = 0x7FF700000000;
	constexpr std::uintptr_t TARGET = BASE + 0xE4DE70;  // Projectile::IsValidImpact
	// The four functions as 1.11.240 has them: the cast-hit loop, HandleHits, BeamProjectile's and MissileProjectile's AddImpact.
	constexpr std::array<std::uintptr_t, 4> FUNCTIONS{ BASE + 0xE48C50, BASE + 0xE4E5D0, BASE + 0xE1FB50, BASE + 0xE3CBB0 };

	std::array<std::uintptr_t, 4> Sites()
	{
		std::array<std::uintptr_t, 4> at{};
		for (std::size_t i = 0; i < at.size(); ++i) {
			at[i] = FUNCTIONS[i] + InertImpact::SITES[i].offset;
		}
		return at;
	}

	std::array<std::array<std::uint8_t, 5>, 4> GameBytes()
	{
		const auto                                  at = Sites();
		std::array<std::array<std::uint8_t, 5>, 4> bytes{};
		for (std::size_t i = 0; i < at.size(); ++i) {
			bytes[i] = CallTo(at[i], TARGET);
		}
		return bytes;
	}
}

TEST("inert impact: only a trail's impact on an actor is refused, and only one the engine found valid")
{
	using InertImpact::Refused;
	CHECK(Refused(true, true, true));
	// A trail's impact on a wall, a crate or a door: its look (no force, no damage) stays.
	CHECK(!Refused(true, true, false));
	// Anyone else's shot on an actor (ours, an NPC's, a copy's real one, a stand-in's missile that goes off): the engine's.
	CHECK(!Refused(true, false, true));
	CHECK(!Refused(true, false, false));
	// What the engine refused already (its shooter's own body, a trigger) stays refused, and is not counted as ours.
	CHECK(!Refused(false, true, true));
	CHECK(!Refused(false, false, true));
}

TEST("inert impact: the four calls of IsValidImpact are where 1.11.240 has them")
{
	// The call sites read from the game: the cast-hit loop +E49908, HandleHits +E4ED3C, BeamProjectile::AddImpact +E1FB6E
	// and MissileProjectile::AddImpact +E3CCC5.
	const auto at = Sites();
	CHECK(at[0] == BASE + 0xE49908);
	CHECK(at[1] == BASE + 0xE4ED3C);
	CHECK(at[2] == BASE + 0xE1FB6E);
	CHECK(at[3] == BASE + 0xE3CCC5);
	static_assert(InertImpact::IS_VALID_IMPACT == 2237061);
	std::array<std::uint64_t, 4> ids{};
	for (std::size_t i = 0; i < ids.size(); ++i) {
		ids[i] = InertImpact::SITES[i].id;
	}
	std::ranges::sort(ids);
	CHECK(std::ranges::adjacent_find(ids) == ids.end());
	CHECK(ids == (std::array<std::uint64_t, 4>{ 2236559, 2236887, 2237022, 2237063 }));
}

TEST("inert impact: all four sites call IsValidImpact, or none is patched")
{
	const auto at = Sites();
	auto       bytes = GameBytes();
	CHECK(InertImpact::Check(bytes, at, TARGET).All());
	CHECK(InertImpact::Check(bytes, at, TARGET).calling == 4);

	// Another plugin's patch at one site (a call elsewhere): not all.
	bytes[2] = CallTo(at[2], BASE + 0x1000);
	CHECK(!InertImpact::Check(bytes, at, TARGET).All());
	CHECK(InertImpact::Check(bytes, at, TARGET).calling == 3);

	// A jump written over a call (a detour's branch): not a call.
	bytes = GameBytes();
	bytes[0] = { 0xE9, bytes[0][1], bytes[0][2], bytes[0][3], bytes[0][4] };
	CHECK(!InertImpact::Check(bytes, at, TARGET).All());

	// Sites and bytes that don't pair up: nothing is patched.
	bytes = GameBytes();
	CHECK(!InertImpact::Check(std::span(bytes).first(3), at, TARGET).All());
	CHECK(!InertImpact::Check(std::span<const std::array<std::uint8_t, 5>>{}, std::span<const std::uintptr_t>{}, TARGET).All());
}

TEST("inert impact: the trails' copies are listed once each, up to the room there is")
{
	InertImpact::Trails<3> trails;
	int                    a = 0, b = 0, c = 0, d = 0;
	CHECK(!trails.Contains(&a));
	CHECK(!trails.Contains(nullptr));
	CHECK(!trails.Add(nullptr));
	CHECK(trails.Add(&a));
	CHECK(!trails.Add(&a));  // once
	CHECK(trails.Add(&b));
	CHECK(trails.Add(&c));
	CHECK(trails.Size() == 3);
	CHECK(!trails.Add(&d));  // full: d's shots hit as before, nothing else is lost
	CHECK(trails.Contains(&a) && trails.Contains(&b) && trails.Contains(&c));
	CHECK(!trails.Contains(&d));
	CHECK(!trails.Contains(nullptr));
}

TEST("inert impact: a copy listed on one thread is seen whole by readers on others")
{
	// One writer (under the copies' lock in the plugin), readers on the engine's threads: a reader sees either the list
	// before a copy was added or the list with it, never a count that covers a slot not yet written.
	constexpr std::size_t       N = 64;
	InertImpact::Trails<N>      trails;
	std::array<int, N>          copies{};
	std::atomic<bool>           done{ false };
	std::atomic<std::size_t>    misses{ 0 };
	std::vector<std::thread>    readers;
	for (int r = 0; r < 3; ++r) {
		readers.emplace_back([&] {
			while (!done.load()) {
				const auto n = trails.Size();
				for (std::size_t i = 0; i < n; ++i) {
					if (!trails.Contains(&copies[i])) {
						++misses;
					}
				}
			}
		});
	}
	for (auto& copy : copies) {
		CHECK(trails.Add(&copy));
	}
	done = true;
	for (auto& reader : readers) {
		reader.join();
	}
	CHECK(misses.load() == 0);
	CHECK(trails.Size() == N);
}
