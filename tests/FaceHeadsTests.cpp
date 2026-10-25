#include "Test.h"

#include "game/FaceHeads.h"

#include <thread>
#include <vector>

// A friend's own head, built by the engine at runtime (game/FaceHeads.h): which pieces a copy gets, the call sites
// checked before any is patched, and the copies the engine is told build their head.
namespace
{
	// The five bytes of a near call at a_at to a_target.
	std::array<std::uint8_t, 5> CallTo(std::uintptr_t a_at, std::uintptr_t a_target)
	{
		const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::intptr_t>(a_target) - static_cast<std::intptr_t>(a_at + 5)));
		return { 0xE8, static_cast<std::uint8_t>(rel), static_cast<std::uint8_t>(rel >> 8), static_cast<std::uint8_t>(rel >> 16), static_cast<std::uint8_t>(rel >> 24) };
	}

	constexpr std::uintptr_t BASE = 0x7FF700000000;
	constexpr std::uintptr_t TARGET = BASE + 0x6DF3A0;
	// The six sites as 1.11.240 has them.
	constexpr std::array<std::uintptr_t, 6> SITES{ BASE + 0x2CC411, BASE + 0x65594B, BASE + 0x6DEDB7, BASE + 0x6E0EB3, BASE + 0x6E2602, BASE + 0x6E2628 };

	std::array<std::array<std::uint8_t, 5>, 6> GameBytes()
	{
		std::array<std::array<std::uint8_t, 5>, 6> bytes{};
		for (std::size_t i = 0; i < SITES.size(); ++i) {
			bytes[i] = CallTo(SITES[i], TARGET);
		}
		return bytes;
	}
}

TEST("face heads: a copy gets the whole look only with bFriendFaces on and the engine patched")
{
	CHECK(FaceHeads::Parts(true, std::nullopt, true) == FaceHeads::ALL);
	CHECK(FaceHeads::OwnHead(FaceHeads::Parts(true, std::nullopt, true)));
	// Unpatched (another plugin's patch at a site, or an unknown game): a copy with its own head would stand headless.
	CHECK(FaceHeads::Parts(true, std::nullopt, false) == FaceHeads::BODY_ONLY);
	CHECK(FaceHeads::Parts(false, std::nullopt, true) == FaceHeads::BODY_ONLY);
	CHECK(FaceHeads::Parts(false, std::nullopt, false) == FaceHeads::BODY_ONLY);
	CHECK(!FaceHeads::OwnHead(FaceHeads::BODY_ONLY));
}

TEST("face heads: the dev's facemask wins whatever the ini and the patch, within the known pieces")
{
	CHECK(FaceHeads::Parts(false, 0x7F, false) == 0x7F);
	CHECK(FaceHeads::Parts(true, 0x04, true) == 0x04);
	CHECK(FaceHeads::Parts(true, 0x00, true) == 0x00);
	CHECK(FaceHeads::Parts(true, 0xFFFF, true) == FaceHeads::ALL);
}

TEST("face heads: all six sites calling the check is the only case they are patched")
{
	const auto bytes = GameBytes();
	const auto sites = FaceHeads::Check(bytes, SITES, TARGET);
	CHECK(sites.All());
	CHECK(sites.calling == 6);
	CHECK(sites.total == 6);
}

TEST("face heads: one site patched by another plugin leaves all six alone")
{
	auto bytes = GameBytes();
	bytes[3] = CallTo(SITES[3], BASE + 0x9000000);  // a call to elsewhere: someone's hook
	auto sites = FaceHeads::Check(bytes, SITES, TARGET);
	CHECK(!sites.All());
	CHECK(sites.calling == 5);

	bytes = GameBytes();
	bytes[5] = { 0xE9, bytes[5][1], bytes[5][2], bytes[5][3], bytes[5][4] };  // a jump to the same place is not a call
	CHECK(!FaceHeads::Check(bytes, SITES, TARGET).All());

	bytes = GameBytes();
	bytes[0] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
	CHECK(FaceHeads::Check(bytes, SITES, TARGET).calling == 5);
}

TEST("face heads: no sites, or bytes that don't match the sites, patch nothing")
{
	CHECK(!FaceHeads::Check({}, {}, TARGET).All());
	const auto bytes = GameBytes();
	const auto fewer = std::span<const std::uintptr_t>(SITES).first(5);
	CHECK(!FaceHeads::Check(bytes, fewer, TARGET).All());
}

TEST("face heads: a copy is ours by its address and its ID together")
{
	FaceHeads::Copies<4> copies;
	CHECK(copies.Size() == 0);
	CHECK(!copies.Contains(0x1000, 0xFF000801));
	CHECK(copies.Add(0x1000, 0xFF000801));
	CHECK(copies.Contains(0x1000, 0xFF000801));
	// The same address with another ID: a form the engine made where ours was freed, not ours.
	CHECK(!copies.Contains(0x1000, 0xFF000900));
	// Our ID at another address: not ours either (the engine passes the form itself).
	CHECK(!copies.Contains(0x2000, 0xFF000801));
	// Added twice: one entry.
	CHECK(copies.Add(0x1000, 0xFF000801));
	CHECK(copies.Size() == 1);
	// No form at address 0.
	CHECK(!copies.Add(0, 0xFF000802));
	CHECK(copies.Size() == 1);
}

TEST("face heads: a full list takes no more copies and keeps the ones it has")
{
	FaceHeads::Copies<3> copies;
	CHECK(copies.Add(0x1000, 1));
	CHECK(copies.Add(0x2000, 2));
	CHECK(copies.Add(0x3000, 3));
	CHECK(!copies.Add(0x4000, 4));
	CHECK(!copies.Contains(0x4000, 4));
	CHECK(copies.Size() == 3);
	CHECK(copies.Contains(0x1000, 1));
	CHECK(copies.Contains(0x3000, 3));
	CHECK(copies.Add(0x2000, 2));  // already there: still fine when full
}

TEST("face heads: a load forgets every copy, and a kept copy can be marked again")
{
	FaceHeads::Copies<2> copies;
	CHECK(copies.Add(0x1000, 0xFF000801));
	CHECK(copies.Add(0x2000, 0xFF000802));
	copies.Clear();
	CHECK(copies.Size() == 0);
	// The load set the runtime IDs back: another NPC made at a freed copy's address with its ID is not ours.
	CHECK(!copies.Contains(0x1000, 0xFF000801));
	CHECK(!copies.Contains(0x2000, 0xFF000802));
	// Room again: a copy the load kept is marked as a stand-in wears it.
	CHECK(copies.Add(0x2000, 0xFF000802));
	CHECK(copies.Add(0x3000, 0xFF000803));
	CHECK(copies.Contains(0x2000, 0xFF000802));
	CHECK(!copies.Contains(0x1000, 0xFF000801));
}

TEST("face heads: the loader threads read the copies while the main thread adds them")
{
	constexpr std::size_t N = 256;
	FaceHeads::Copies<N> copies;
	std::atomic<bool>    done{ false };
	std::atomic<int>     wrong{ 0 };
	std::vector<std::thread> readers;
	for (int r = 0; r < 3; ++r) {
		readers.emplace_back([&] {
			while (!done.load()) {
				// Whatever an entry a reader sees, it is whole: an address seen is seen with its own ID.
				const auto n = copies.Size();
				for (std::size_t i = 0; i < n; ++i) {
					if (!copies.Contains(0x10000 + i * 0x100, static_cast<std::uint32_t>(0xFF000000 + i))) {
						wrong.fetch_add(1);
					}
				}
			}
		});
	}
	for (std::size_t i = 0; i < N; ++i) {
		CHECK(copies.Add(0x10000 + i * 0x100, static_cast<std::uint32_t>(0xFF000000 + i)));
	}
	done = true;
	for (auto& t : readers) {
		t.join();
	}
	CHECK(wrong.load() == 0);
	CHECK(copies.Size() == N);
}

TEST("face heads: a friend with no region sliders gives the copy none, never an empty array the engine indexes")
{
	// Fresh out of the face editor with a preset: the host's crash (2026-10-18) as the copy's body loaded.
	CHECK(FaceHeads::RegionSliders({}).empty());
}

TEST("face heads: region sliders cover every body region the engine can read")
{
	const std::vector<float> all{ 0.1f, -0.2f, 0.3f, 0.0f, 1.0f };
	CHECK(FaceHeads::RegionSliders(all) == all);
	// Fewer than the regions (an older or hostile sender): filled with 0 up to them, what was sent kept.
	const std::vector<float> two{ 0.5f, -1.0f };
	const auto filled = FaceHeads::RegionSliders(two);
	REQUIRE(filled.size() == FaceHeads::BODY_REGIONS);
	CHECK(filled[0] == 0.5f);
	CHECK(filled[1] == -1.0f);
	CHECK(filled[4] == 0.0f);
	// More (another race's regions): all kept.
	const std::vector<float> seven(7, 0.25f);
	CHECK(FaceHeads::RegionSliders(seven) == seven);
}

TEST("face heads: a friend's tint layer is made only on a template of its own class; another class, or one unknown, is left out")
{
	using FaceHeads::TintClass;
	using FaceHeads::TintFits;
	CHECK(TintFits(0, TintClass::kMask));
	CHECK(TintFits(1, TintClass::kPalette));
	CHECK(TintFits(2, TintClass::kTextureSet));
	// A mask template named by a palette or texture set layer (another mod's race, a bad client): the head build would
	// read it as the wrong class.
	CHECK(!TintFits(1, TintClass::kMask));
	CHECK(!TintFits(2, TintClass::kMask));
	CHECK(!TintFits(0, TintClass::kPalette));
	CHECK(!TintFits(2, TintClass::kPalette));
	CHECK(!TintFits(0, TintClass::kTextureSet));
	CHECK(!TintFits(1, TintClass::kTextureSet));
	// A template of none of the three, and a layer type no game sends.
	for (const auto type : std::initializer_list<std::uint8_t>{ 0, 1, 2, 3, 255 }) {
		CHECK(!TintFits(type, TintClass::kOther));
	}
	CHECK(!TintFits(3, TintClass::kTextureSet));
	CHECK(!TintFits(static_cast<std::uint8_t>(TintClass::kOther), TintClass::kOther));
}
