// LooksMenu's overlays (tattoos, face and body paint) and skin on a friend's stand-in (game/LooksOverlays.h): what a read
// of our overlays becomes, how the body sliders' read carries them, and what the wire carries. Then the whole way: one
// game reads its player's overlays with their sliders and sends its face, a real server relays it, and a game with
// LooksMenu adds them to its stand-in (and takes them off again when the stand-in goes) while one without leaves them be.

#include "ServerFixture.h"

#include "game/BodyMorphs.h"
#include "game/LooksOverlays.h"

#include <cmath>
#include <limits>
#include <map>
#include <string>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using BodyMorphs::Clock;
	using BodyMorphs::Then;
	using Entries = std::vector<LooksOverlays::Entry>;
	using Overlays = std::vector<Protocol::Overlay>;

	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	Protocol::Face PlainFace()
	{
		Protocol::Face face;
		face.female = false;
		face.race = 0x00013746;  // HumanRace
		face.weight[0] = 0.4f, face.weight[1] = 0.4f, face.weight[2] = 0.2f;
		return face;
	}

	// What Overlays.GetAll answers for a character with a back tattoo and war paint: in the order its store keeps them.
	Entries Worn()
	{
		return {
			{ .templateId = "WarPaint03", .priority = 5, .tint = { 1.0f, 0.0f, 0.0f, 0.5f }, .offset = { -0.2f, 0.0f }, .scale = { 0.8f, 1.2f } },
			{ .templateId = "Tattoo_Back01", .priority = 0, .tint = { 0.08f, 0.08f, 0.12f, 0.9f }, .offset = { 0.0f, 0.1f }, .scale = { 1.0f, 1.0f } },
		};
	}
}

// ---- what a read becomes -----------------------------------------------------------------

TEST("overlays: a read is sorted by priority with its tint in bytes, so the same character reads the same")
{
	const auto collected = LooksOverlays::Collect(Worn());
	REQUIRE(collected.size() == 2);
	CHECK(collected[0].templateId == "Tattoo_Back01");
	CHECK(collected[0].priority == 0);
	CHECK(collected[0].tint[0] == 20);
	CHECK(collected[0].tint[3] == 230);
	CHECK(collected[0].offset[1] == 0.1f);
	CHECK(collected[1].templateId == "WarPaint03");
	CHECK(collected[1].tint[0] == 255);
	CHECK(collected[1].tint[1] == 0);
	CHECK(collected[1].tint[3] == 128);
	CHECK(collected[1].scale[1] == 1.2f);
	auto shuffled = Worn();
	std::ranges::reverse(shuffled);
	CHECK(LooksOverlays::Collect(shuffled) == collected);
}

TEST("overlays: a read keeps one per priority, whole and plain templates, numbers in range")
{
	Entries read = Worn();
	read.push_back({ .templateId = "Second", .priority = 5 });  // LooksMenu lays one overlay at each priority
	read.push_back({ .templateId = "", .priority = 1 });
	read.push_back({ .templateId = std::string(Protocol::MAX_OVERLAY_TEMPLATE + 1, 'x'), .priority = 2 });
	read.push_back({ .templateId = "Bad\nId", .priority = 3 });
	read.push_back({ .templateId = "Lost", .priority = 4, .offset = { NaN, 0.0f } });
	read.push_back({ .templateId = "Smeared", .priority = 6, .tint = { 7.0f, -1.0f, NaN, 0.5f }, .offset = { 99.0f, -99.0f }, .scale = { 50.0f, 0.5f } });
	read.push_back({ .templateId = std::string(Protocol::MAX_OVERLAY_TEMPLATE, 'y'), .priority = -3 });
	const auto collected = LooksOverlays::Collect(read);
	REQUIRE(collected.size() == 4);
	CHECK(collected[0].templateId == std::string(Protocol::MAX_OVERLAY_TEMPLATE, 'y'));
	CHECK(collected[1].templateId == "Tattoo_Back01");
	CHECK(collected[2].templateId == "WarPaint03");
	const auto& smeared = collected[3];
	CHECK(smeared.templateId == "Smeared");
	CHECK(smeared.tint[0] == 255);
	CHECK(smeared.tint[1] == 0);
	CHECK(smeared.tint[2] == 0);
	CHECK(smeared.offset[0] == Protocol::MAX_OVERLAY_UV);
	CHECK(smeared.offset[1] == -Protocol::MAX_OVERLAY_UV);
	CHECK(smeared.scale[0] == Protocol::MAX_OVERLAY_UV);
	CHECK(smeared.scale[1] == 0.5f);
}

TEST("overlays: a read keeps at most as many as the wire carries, the lowest priorities first")
{
	Entries read;
	for (std::size_t i = 0; i < Protocol::MAX_OVERLAYS + 20; ++i) {
		read.push_back({ .templateId = std::format("Tattoo{:03}", i), .priority = static_cast<std::int32_t>(100 - i) });
	}
	const auto collected = LooksOverlays::Collect(read);
	REQUIRE(collected.size() == Protocol::MAX_OVERLAYS);
	CHECK(collected.front().priority == static_cast<std::int32_t>(100 - (Protocol::MAX_OVERLAYS + 19)));
}

TEST("overlays: a friend's overlay goes to Overlays.Add with the tint it was read with")
{
	const auto collected = LooksOverlays::Collect(Worn());
	const auto entry = LooksOverlays::ToEntry(collected[1]);
	CHECK(entry.templateId == "WarPaint03");
	CHECK(entry.priority == 5);
	CHECK(entry.tint[0] == 1.0f);
	CHECK(entry.tint[1] == 0.0f);
	CHECK(std::abs(entry.tint[3] - 0.5f) < 0.5f / 255.0f + 1e-6f);
	CHECK(entry.offset[0] == -0.2f);
	CHECK(entry.scale[0] == 0.8f);
	// Read again from that friend's stand-in, it is the same overlay.
	CHECK(LooksOverlays::Collect({ entry })[0] == collected[1]);
}

// ---- the read they share with the sliders ----------------------------------------------------

TEST("overlays: they come with the sliders' read, stay when that call goes unanswered, and go with a load")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	REQUIRE(reading.Finish(reading.Start(start), {}, start, Worn()));
	CHECK(reading.Overlays().size() == 2);
	CHECK(reading.Morphs().empty());

	// The next read: the overlays call cancelled (an older LooksMenu without the script, say). The sliders still go.
	const auto later = start + BodyMorphs::READ_INTERVAL + 1s;
	REQUIRE(reading.Finish(reading.Start(later), { { "Arms", 0.5f } }, later, std::nullopt));
	CHECK(reading.Overlays().size() == 2);
	CHECK(reading.Morphs().size() == 1);

	// Washed off at a mirror: an answer with none.
	const auto after = later + BodyMorphs::READ_INTERVAL + 1s;
	REQUIRE(reading.Finish(reading.Start(after), { { "Arms", 0.5f } }, after, Entries{}));
	CHECK(reading.Overlays().empty());

	REQUIRE(reading.Finish(reading.Start(after + 1min), {}, after + 1min, Worn()));
	reading.Reset();
	CHECK(reading.Overlays().empty());
	CHECK(!reading.Settled());
}

TEST("overlays: an answer to a read given up changes nothing")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	const auto generation = reading.Start(start);
	REQUIRE(reading.Expire(start + BodyMorphs::ANSWER_TIMEOUT));
	CHECK(!reading.Finish(generation, {}, start + 10s, Worn()));
	CHECK(reading.Overlays().empty());
}

// ---- the wire ---------------------------------------------------------------------------

TEST("protocol: a face carries its skin and overlays")
{
	auto face = PlainFace();
	face.skin = 0x0001D4B5;
	face.overlays = LooksOverlays::Collect(Worn());
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace));
	REQUIRE(decoded);
	CHECK(*decoded == face);
	CHECK(decoded->skin == 0x0001D4B5);
	CHECK(decoded->overlays.size() == 2);
}

TEST("protocol: a face's overlay placement is brought into range, and one that is not a number refuses the face")
{
	auto face = PlainFace();
	face.overlays = { { "Tattoo", 0, { 255, 255, 255, 255 }, { 400.0f, -400.0f }, { 1.0f, -50.0f } } };
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace));
	REQUIRE(decoded);
	REQUIRE(decoded->overlays.size() == 1);
	CHECK(decoded->overlays[0].offset[0] == Protocol::MAX_OVERLAY_UV);
	CHECK(decoded->overlays[0].offset[1] == -Protocol::MAX_OVERLAY_UV);
	CHECK(decoded->overlays[0].scale[1] == -Protocol::MAX_OVERLAY_UV);

	face.overlays[0].scale[0] = std::numeric_limits<float>::infinity();
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face.overlays[0].scale[0] = 1.0f;
	face.overlays[0].offset[1] = NaN;
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face.overlays = { { "\x01\x02", 0, {}, {}, { 1.0f, 1.0f } } };  // nothing left of the ID once control characters go
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
}

TEST("protocol: a face carries at most MAX_OVERLAYS overlays, and none with a template ID too long to send whole")
{
	auto face = PlainFace();
	for (std::size_t i = 0; i < Protocol::MAX_OVERLAYS + 10; ++i) {
		face.overlays.push_back({ std::format("Tattoo{:03}", i), static_cast<std::int32_t>(i), {}, {}, { 1.0f, 1.0f } });
	}
	face.overlays.insert(face.overlays.begin(), { std::string(Protocol::MAX_OVERLAY_TEMPLATE + 1, 'x'), -1, {}, {}, { 1.0f, 1.0f } });
	const auto bytes = Protocol::Encode(face, MT::kReportFace);
	const auto decoded = Protocol::DecodeFace(bytes);
	REQUIRE(decoded);
	REQUIRE(decoded->overlays.size() == Protocol::MAX_OVERLAYS);
	CHECK(decoded->overlays.front().templateId == "Tattoo000");
	CHECK(bytes.size() < 16 * 1024);  // bounded, whatever a modified game reports

	// A count past the limit, as a modified game might send, refuses the face.
	auto over = Protocol::Encode(PlainFace(), MT::kReportFace);
	REQUIRE(over.back() == 0);  // the overlay count is last
	over.back() = static_cast<std::uint8_t>(Protocol::MAX_OVERLAYS + 1);
	for (std::size_t i = 0; i <= Protocol::MAX_OVERLAYS; ++i) {
		over.insert(over.end(), { 1, 'a', 0, 0, 0, 0, 0, 0, 0, 0 });
		over.insert(over.end(), 16, 0);
	}
	CHECK(!Protocol::DecodeFace(over));
}

// ---- the whole way -------------------------------------------------------------------------

namespace
{
	// A game's side of it, as LooksMenu (game/LooksMenu.cpp) and Session drive it: its player's overlays read with their
	// sliders and its face sent once the read is in; a friend's overlays added to their stand-in when it is placed (only
	// with LooksMenu, and only those whose template this game's overlay mods have), laid on once the adds are answered,
	// and taken off when the stand-in goes.
	struct Game
	{
		bool                                         looksMenu = false;
		Entries                                      worn;       // what Overlays.GetAll would answer for our player
		std::uint32_t                                skin = 0;   // our base NPC's own skin
		std::vector<std::string>                     templates;  // the overlay templates this game has
		BodyMorphs::Reading                          reading;
		BodyMorphs::StandIns                         standIns;
		std::optional<Protocol::Face>                sent;
		std::map<std::uint32_t, Entries>             store;  // LooksMenu's own: overlays by reference
		std::uint32_t                                laid = 0;

		std::optional<std::vector<std::uint8_t>> Frame(Clock::time_point a_now)
		{
			if (looksMenu && reading.ShouldStart(false, a_now)) {
				reading.Finish(reading.Start(a_now), {}, a_now, worn);
			}
			auto face = PlainFace();
			face.skin = skin;
			face.overlays = looksMenu ? reading.Overlays() : Overlays{};
			if (!BodyMorphs::FaceReady(looksMenu, reading) || (sent && *sent == face)) {
				return std::nullopt;
			}
			sent = face;
			return Protocol::Encode(face, MT::kReportFace);
		}

		void Place(std::uint32_t a_standIn, const Protocol::Face& a_face)
		{
			if (!looksMenu || a_face.overlays.empty()) {
				return;
			}
			standIns.Setting(a_standIn);
		}

		// The adds answered a frame later: Add refuses a template this game lacks.
		void Answer(std::uint32_t a_standIn, const Protocol::Face& a_face)
		{
			if (!looksMenu) {
				return;
			}
			for (const auto& overlay : a_face.overlays) {
				if (std::ranges::contains(templates, overlay.templateId)) {
					store[a_standIn].push_back(LooksOverlays::ToEntry(overlay));
				}
			}
			switch (standIns.Answered(a_standIn)) {
			case Then::kUpdate:
				++laid;
				break;
			case Then::kClear:
				store.erase(a_standIn);
				break;
			case Then::kNothing:
				break;
			}
		}

		void Remove(std::uint32_t a_standIn)
		{
			if (standIns.Leaving(a_standIn)) {
				store.erase(a_standIn);
			}
		}
	};
}

TEST("overlays: a player's LooksMenu tattoos and skin reach a friend's stand-in with LooksMenu, and leave with it; a friend without it gets the skin only")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Take(2);
	f.fake->Take(3);

	Game aliceGame{ .looksMenu = true, .worn = Worn(), .skin = 0x0001D4B5 };
	Game bobGame{ .looksMenu = true, .templates = { "Tattoo_Back01" } };  // without the war paint's mod
	Game carolGame{ .looksMenu = false };

	const auto now = Clock::now();
	const auto report = aliceGame.Frame(now);
	REQUIRE(report);
	CHECK(!aliceGame.Frame(now + 1s));  // the same look is not sent again
	f.fake->Deliver(1, *report);

	const auto toBob = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(toBob);
	const auto bobsCopy = Protocol::DecodeFace(*toBob);
	REQUIRE(bobsCopy);
	CHECK(bobsCopy->playerId == alice->playerId);
	CHECK(bobsCopy->skin == 0x0001D4B5);
	CHECK(bobsCopy->overlays == LooksOverlays::Collect(Worn()));

	constexpr std::uint32_t BOBS_STAND_IN = 0xFF000100;
	bobGame.Place(BOBS_STAND_IN, *bobsCopy);
	bobGame.Answer(BOBS_STAND_IN, *bobsCopy);
	CHECK(bobGame.laid == 1);
	REQUIRE(bobGame.store.at(BOBS_STAND_IN).size() == 1);  // the tattoo; the war paint's mod is not in his game
	CHECK(bobGame.store.at(BOBS_STAND_IN)[0].templateId == "Tattoo_Back01");
	bobGame.Remove(BOBS_STAND_IN);
	CHECK(bobGame.store.empty());  // the next reference given that form ID has its own skin

	const auto toCarol = f.fake->WaitFor(3, Type(MT::kPlayerFace));
	REQUIRE(toCarol);
	const auto carolsCopy = Protocol::DecodeFace(*toCarol);
	REQUIRE(carolsCopy);
	CHECK(carolsCopy->skin == 0x0001D4B5);  // on the stand-in's base, LooksMenu or not
	constexpr std::uint32_t CAROLS_STAND_IN = 0xFF000200;
	carolGame.Place(CAROLS_STAND_IN, *carolsCopy);
	carolGame.Answer(CAROLS_STAND_IN, *carolsCopy);
	CHECK(carolGame.store.empty());
	CHECK(carolGame.standIns.Count() == 0);
}

TEST("overlays: a stand-in that goes while its adds are queued has them taken off once answered")
{
	Game bobGame{ .looksMenu = true, .templates = { "Tattoo_Back01", "WarPaint03" } };
	auto face = PlainFace();
	face.overlays = LooksOverlays::Collect(Worn());
	constexpr std::uint32_t STAND_IN = 0xFF000300;
	bobGame.Place(STAND_IN, face);
	bobGame.Remove(STAND_IN);  // its adds still queued: nothing to take off yet
	bobGame.Answer(STAND_IN, face);
	CHECK(bobGame.store.empty());
	CHECK(bobGame.laid == 0);
}
