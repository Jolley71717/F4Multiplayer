// LooksMenu's body sliders on a friend's stand-in (game/BodyMorphs.h): what a read of our sliders becomes, when reads go,
// what the wire carries, and what becomes of a stand-in's sliders. Then the whole way: one game reads its player's
// sliders and sends its face once the read is in, a real server relays it, and a game with LooksMenu sets them on its
// stand-in (and takes them off again when the stand-in goes) while a game without it leaves them be.

#include "ServerFixture.h"

#include "game/BodyMorphs.h"

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
	using Morphs = std::vector<Protocol::BodyMorph>;

	Protocol::Face PlainFace()
	{
		Protocol::Face face;
		face.female = true;
		face.race = 0x00013746;  // HumanRace
		face.weight[0] = 0.2f, face.weight[1] = 0.5f, face.weight[2] = 0.3f;
		return face;
	}

	// What BodyGen answers for a body shaped in LooksMenu: in the order its store keeps them, one slider moved back to zero.
	Morphs Answered()
	{
		return { { "Waist", -0.3f }, { "Breasts", 0.6f }, { "Butt", 0.0f }, { "Arms", 1.0f } };
	}
}

// ---- what a read becomes ------------------------------------------------------------

TEST("body morphs: a read is sorted by name, without sliders at zero, so the same body reads the same")
{
	const auto collected = BodyMorphs::Collect(Answered());
	CHECK(collected == (Morphs{ { "Arms", 1.0f }, { "Breasts", 0.6f }, { "Waist", -0.3f } }));
	auto shuffled = Answered();
	std::ranges::reverse(shuffled);
	CHECK(BodyMorphs::Collect(shuffled) == collected);
}

TEST("body morphs: a read keeps each name once, in range, whole and plain")
{
	Morphs read = { { "Arms", 0.5f }, { "Arms", 0.9f }, { "Legs", 40.0f }, { "Hips", -9.0f }, { "Nan", std::numeric_limits<float>::quiet_NaN() },
		{ std::string(Protocol::MAX_BODY_MORPH_NAME + 1, 'x'), 0.5f }, { std::string(Protocol::MAX_BODY_MORPH_NAME, 'y'), 0.5f }, { "Bad\nName", 0.5f }, { "", 0.5f } };
	const auto collected = BodyMorphs::Collect(read);
	CHECK(collected == (Morphs{ { "Arms", 0.5f }, { "Hips", -Protocol::MAX_BODY_MORPH_VALUE }, { "Legs", Protocol::MAX_BODY_MORPH_VALUE }, { std::string(Protocol::MAX_BODY_MORPH_NAME, 'y'), 0.5f } }));
}

TEST("body morphs: a read keeps at most as many sliders as the wire carries")
{
	Morphs read;
	for (std::size_t i = 0; i < Protocol::MAX_BODY_MORPHS + 40; ++i) {
		read.push_back({ std::format("Slider{:03}", i), 0.5f });
	}
	const auto collected = BodyMorphs::Collect(read);
	REQUIRE(collected.size() == Protocol::MAX_BODY_MORPHS);
	CHECK(collected.front().name == "Slider000");
}

// ---- when reads go -------------------------------------------------------------------

TEST("body morphs: the first read goes at once, menu or not, and the face waits for it")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	CHECK(!BodyMorphs::FaceReady(true, reading));
	CHECK(BodyMorphs::FaceReady(false, reading));  // without LooksMenu the face never waits
	REQUIRE(reading.ShouldStart(true, start));
	const auto generation = reading.Start(start);
	CHECK(!reading.ShouldStart(false, start + 1min));  // one at a time
	CHECK(!BodyMorphs::FaceReady(true, reading));
	CHECK(reading.Finish(generation, Answered(), start + 50ms));
	CHECK(BodyMorphs::FaceReady(true, reading));
	CHECK(reading.Morphs().size() == 3);
}

TEST("body morphs: later reads wait for the interval and for the menus to close")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	REQUIRE(reading.Finish(reading.Start(start), Answered(), start));
	CHECK(!reading.ShouldStart(false, start + BodyMorphs::READ_INTERVAL - 1s));
	CHECK(!reading.ShouldStart(true, start + BodyMorphs::READ_INTERVAL + 1s));  // at the plastic surgeon
	CHECK(reading.ShouldStart(false, start + BodyMorphs::READ_INTERVAL + 1s));
}

TEST("body morphs: a read the VM never answers is given up, the face goes with what was read before, and a late answer changes nothing")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	REQUIRE(reading.Finish(reading.Start(start), Answered(), start));
	const auto before = reading.Morphs();
	const auto later = start + BodyMorphs::READ_INTERVAL + 1s;
	const auto generation = reading.Start(later);
	CHECK(!reading.Expire(later + BodyMorphs::ANSWER_TIMEOUT - 1s));
	CHECK(reading.Expire(later + BodyMorphs::ANSWER_TIMEOUT));
	CHECK(!reading.Pending());
	CHECK(!reading.Finish(generation, { { "Arms", 2.0f } }, later + 10s));
	CHECK(reading.Morphs() == before);
}

TEST("body morphs: a first read that fails or is never answered still lets the face go, without sliders")
{
	BodyMorphs::Reading failed;
	const auto start = Clock::now();
	failed.Fail(failed.Start(start), start);
	CHECK(BodyMorphs::FaceReady(true, failed));
	CHECK(failed.Morphs().empty());

	BodyMorphs::Reading silent;
	silent.Start(start);
	CHECK(!BodyMorphs::FaceReady(true, silent));
	CHECK(silent.Expire(start + BodyMorphs::ANSWER_TIMEOUT));
	CHECK(BodyMorphs::FaceReady(true, silent));
}

TEST("body morphs: after a load the sliders are read afresh and an answer from before it is ignored")
{
	BodyMorphs::Reading reading;
	const auto start = Clock::now();
	REQUIRE(reading.Finish(reading.Start(start), Answered(), start));
	const auto old = reading.Start(start + 20s);
	reading.Reset();
	CHECK(reading.Morphs().empty());
	CHECK(!BodyMorphs::FaceReady(true, reading));
	CHECK(!reading.Finish(old, Answered(), start + 21s));
	CHECK(reading.ShouldStart(true, start + 21s));
}

// ---- a stand-in's sliders -------------------------------------------------------------

TEST("body morphs: a stand-in's body is rebuilt once its sets are answered, and its sliders go with it")
{
	BodyMorphs::StandIns standIns;
	constexpr std::uint32_t STAND_IN = 0xFF001234;
	standIns.Setting(STAND_IN);
	CHECK(standIns.Answered(STAND_IN) == Then::kUpdate);
	CHECK(standIns.Answered(STAND_IN) == Then::kNothing);  // once
	CHECK(standIns.Leaving(STAND_IN));
	CHECK(!standIns.Has(STAND_IN));
	CHECK(!standIns.Leaving(STAND_IN));  // taken off once
}

TEST("body morphs: a stand-in that goes while its sets are queued has its sliders taken off once they are answered")
{
	BodyMorphs::StandIns standIns;
	constexpr std::uint32_t STAND_IN = 0xFF001234;
	standIns.Setting(STAND_IN);
	CHECK(!standIns.Leaving(STAND_IN));  // not now: a set answered after it would put them back
	CHECK(standIns.Answered(STAND_IN) == Then::kClear);
	CHECK(standIns.Count() == 0);
}

TEST("body morphs: nothing is taken off a stand-in we set none on, and nothing is kept past a load")
{
	BodyMorphs::StandIns standIns;
	CHECK(!standIns.Leaving(0xFF000AAA));
	CHECK(standIns.Answered(0xFF000AAA) == Then::kNothing);
	standIns.Setting(0xFF000BBB);
	standIns.Reset();
	CHECK(standIns.Count() == 0);
	CHECK(standIns.Answered(0xFF000BBB) == Then::kNothing);
}

// ---- on the wire -------------------------------------------------------------------------

TEST("protocol: a face carries its body sliders, and one without any still decodes")
{
	auto face = PlainFace();
	face.playerId = 3;
	CHECK(Protocol::DecodeFace(Protocol::Encode(face, MT::kPlayerFace)) == face);
	face.bodyMorphs = BodyMorphs::Collect(Answered());
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kPlayerFace));
	REQUIRE(decoded);
	CHECK(*decoded == face);
}

TEST("protocol: a face's body sliders out of range arrive in range, and bad ones refuse the face")
{
	auto face = PlainFace();
	face.bodyMorphs = { { "Arms", 50.0f }, { "Legs", -50.0f } };
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace));
	REQUIRE(decoded);
	CHECK(decoded->bodyMorphs == (Morphs{ { "Arms", Protocol::MAX_BODY_MORPH_VALUE }, { "Legs", -Protocol::MAX_BODY_MORPH_VALUE } }));

	face.bodyMorphs = { { "Arms", std::numeric_limits<float>::infinity() } };
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face.bodyMorphs = { { "\x01\x02", 0.5f } };  // nothing left of the name once control characters go
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
}

TEST("protocol: a face carries at most MAX_BODY_MORPHS sliders, and none with a name too long to send whole")
{
	auto face = PlainFace();
	for (std::size_t i = 0; i < Protocol::MAX_BODY_MORPHS + 10; ++i) {
		face.bodyMorphs.push_back({ std::format("Slider{:03}", i), 0.5f });
	}
	face.bodyMorphs.insert(face.bodyMorphs.begin(), { std::string(Protocol::MAX_BODY_MORPH_NAME + 1, 'x'), 0.5f });
	const auto bytes = Protocol::Encode(face, MT::kReportFace);
	const auto decoded = Protocol::DecodeFace(bytes);
	REQUIRE(decoded);
	REQUIRE(decoded->bodyMorphs.size() == Protocol::MAX_BODY_MORPHS);
	CHECK(decoded->bodyMorphs.front().name == "Slider000");
	CHECK(bytes.size() < 16 * 1024);  // bounded, whatever a modified game reports

	// A count past the limit, as a modified game might send, refuses the face.
	auto over = Protocol::Encode(PlainFace(), MT::kReportFace);
	over.resize(over.size() - 5);  // the skin and the overlay count follow the sliders
	REQUIRE(over.back() == 0);
	over.back() = static_cast<std::uint8_t>(Protocol::MAX_BODY_MORPHS + 1);
	for (std::size_t i = 0; i <= Protocol::MAX_BODY_MORPHS; ++i) {
		over.insert(over.end(), { 1, 'a', 0, 0, 0, 0x3F });
	}
	over.insert(over.end(), { 0, 0, 0, 0, 0 });
	CHECK(!Protocol::DecodeFace(over));
}

// ---- the whole way ---------------------------------------------------------------------------

namespace
{
	// A game's side of it, as LooksMenu (game/LooksMenu.cpp) and Session drive it: its player's sliders read when due and
	// its face sent once the first read is in; a friend's sliders set on their stand-in when it is placed (only with
	// LooksMenu), its body rebuilt once the sets are answered, and the sliders taken off when the stand-in goes.
	struct Game
	{
		bool                     looksMenu = false;
		Morphs                   body;  // what BodyGen would answer for our player
		BodyMorphs::Reading      reading;
		BodyMorphs::StandIns     standIns;
		std::optional<Protocol::Face> sent;
		std::map<std::uint32_t, Morphs> store;  // LooksMenu's own: sliders by reference
		std::uint32_t            rebuilt = 0;

		// One frame: a read when due (answered at once here), then our face if it differs from what went last.
		std::optional<std::vector<std::uint8_t>> Frame(Clock::time_point a_now)
		{
			if (looksMenu && reading.ShouldStart(false, a_now)) {
				reading.Finish(reading.Start(a_now), body, a_now);
			}
			auto face = PlainFace();
			face.bodyMorphs = looksMenu ? reading.Morphs() : Morphs{};
			if (!BodyMorphs::FaceReady(looksMenu, reading) || (sent && *sent == face)) {
				return std::nullopt;
			}
			sent = face;
			return Protocol::Encode(face, MT::kReportFace);
		}

		// A friend's stand-in placed with their face: sets queued (only with LooksMenu), answered a frame later.
		void Place(std::uint32_t a_standIn, const Protocol::Face& a_face)
		{
			if (!looksMenu || a_face.bodyMorphs.empty()) {
				return;
			}
			standIns.Setting(a_standIn);
		}

		void Answer(std::uint32_t a_standIn, const Protocol::Face& a_face)
		{
			if (!looksMenu) {
				return;
			}
			store[a_standIn] = a_face.bodyMorphs;
			switch (standIns.Answered(a_standIn)) {
			case Then::kUpdate:
				++rebuilt;
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

TEST("body morphs: a player's LooksMenu body reaches a friend's stand-in with LooksMenu, and leaves with it; a friend without it is untouched")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	f.fake->Take(2);
	f.fake->Take(3);

	Game aliceGame{ .looksMenu = true, .body = Answered() };
	Game bobGame{ .looksMenu = true };
	Game carolGame{ .looksMenu = false };

	const auto now = Clock::now();
	const auto report = aliceGame.Frame(now);
	REQUIRE(report);
	CHECK(!aliceGame.Frame(now + 1s));  // the same body is not sent again
	f.fake->Deliver(1, *report);

	const auto toBob = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(toBob);
	const auto bobsCopy = Protocol::DecodeFace(*toBob);
	REQUIRE(bobsCopy);
	CHECK(bobsCopy->playerId == alice->playerId);
	CHECK(bobsCopy->bodyMorphs == (Morphs{ { "Arms", 1.0f }, { "Breasts", 0.6f }, { "Waist", -0.3f } }));

	constexpr std::uint32_t BOBS_STAND_IN = 0xFF000100;
	bobGame.Place(BOBS_STAND_IN, *bobsCopy);
	bobGame.Answer(BOBS_STAND_IN, *bobsCopy);
	CHECK(bobGame.rebuilt == 1);
	CHECK(bobGame.store.at(BOBS_STAND_IN) == bobsCopy->bodyMorphs);
	bobGame.Remove(BOBS_STAND_IN);
	CHECK(bobGame.store.empty());  // the next reference given that form ID has its own body

	const auto toCarol = f.fake->WaitFor(3, Type(MT::kPlayerFace));
	REQUIRE(toCarol);
	const auto carolsCopy = Protocol::DecodeFace(*toCarol);
	REQUIRE(carolsCopy);  // the sliders do not stop her game reading the rest of the face
	constexpr std::uint32_t CAROLS_STAND_IN = 0xFF000200;
	carolGame.Place(CAROLS_STAND_IN, *carolsCopy);
	carolGame.Answer(CAROLS_STAND_IN, *carolsCopy);
	CHECK(carolGame.store.empty());
	CHECK(carolGame.standIns.Count() == 0);
}

TEST("body morphs: a newcomer is told the body as last read, and a player without LooksMenu sends none")
{
	Server::Options options;
	options.faceInterval = 600s;  // the test moves the server clock past it (Advance), so a busy PC can't decide when
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));

	Game aliceGame{ .looksMenu = true, .body = Answered() };
	Game bobGame{ .looksMenu = false };
	auto now = Clock::now();
	f.fake->Deliver(1, *aliceGame.Frame(now));
	const auto bobReport = bobGame.Frame(now);
	REQUIRE(bobReport);  // at once: no LooksMenu, nothing to wait for
	f.fake->Deliver(2, *bobReport);
	REQUIRE(f.fake->WaitFor(2, Type(MT::kPlayerFace)));  // her first read reaches Bob: the server's face interval starts

	// Alice moves a slider at the plastic surgeon; it goes with the next read once she is out of the chair.
	aliceGame.body.push_back({ "Thighs", 0.8f });
	CHECK(!aliceGame.Frame(now + 5s));
	f.Advance(601s);  // past the server's face interval
	now += BodyMorphs::READ_INTERVAL + 1s;
	const auto changed = aliceGame.Frame(now);
	REQUIRE(changed);
	f.fake->Deliver(1, *changed);
	const auto toBob = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(toBob);
	const auto changedFace = Protocol::DecodeFace(*toBob);
	REQUIRE(changedFace);
	CHECK(std::ranges::contains(changedFace->bodyMorphs, Protocol::BodyMorph{ "Thighs", 0.8f }));

	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	bool sawAlice = false;
	bool sawBob = false;
	for (int i = 0; i < 2; ++i) {
		const auto packet = f.fake->WaitFor(3, Type(MT::kPlayerFace));
		REQUIRE(packet);
		const auto face = Protocol::DecodeFace(*packet);
		REQUIRE(face);
		if (face->bodyMorphs.empty()) {
			sawBob = true;
		} else {
			sawAlice = true;
			CHECK(face->bodyMorphs.size() == 4);
			CHECK(std::ranges::contains(face->bodyMorphs, Protocol::BodyMorph{ "Thighs", 0.8f }));
		}
	}
	CHECK(sawAlice);
	CHECK(sawBob);
}
