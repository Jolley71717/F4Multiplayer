#include "ServerFixture.h"

#include "game/FaceWatch.h"

using namespace TestServer;
using namespace std::chrono_literals;

// A player's face: reported once, relayed to the others, told to newcomers. Then a friend's own head on their stand-in
// (game/FaceWatch.h): when the engine is asked to build it again, and when it is given up for the settler's.
namespace
{
	Protocol::Face SampleFace()
	{
		Protocol::Face face;
		face.female = true;
		face.race = 0x00013746;  // HumanRace
		face.headParts = { 0x0004DA7A, 0x00047B2B, 0x0005A1D7 };
		face.hairColor = 0x0005D7A1;
		face.faceDetails = 0x000A5B2C;  // a complexion texture set
		face.weight[0] = 0.2f, face.weight[1] = 0.5f, face.weight[2] = 0.3f;
		face.sliders = { 0.0f, 0.25f, 1.0f, -0.5f };
		face.morphs = { { 0x15F, 0.6f }, { 0x1A2, -0.35f }, { 0x203, 1.0f } };
		face.bones = { { 7, { 0.1f, 0.0f, -0.2f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.1f } } };
		face.tints = { { 1001, 1, 90, 0xFFC08060, 3 }, { 1002, 0, 40, 0, 0 }, { 1003, 2, 100, 0, 0 } };
		face.bodyTint[0] = 200, face.bodyTint[1] = 170, face.bodyTint[2] = 140, face.bodyTint[3] = 255;
		return face;
	}
}

TEST("protocol: face round trip")
{
	auto face = SampleFace();
	face.playerId = 9;
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kPlayerFace));
	REQUIRE(decoded);
	CHECK(*decoded == face);
}

TEST("protocol: face rejects bad data")
{
	auto face = SampleFace();
	face.race = 0;
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face = SampleFace();
	face.sliders[1] = std::numeric_limits<float>::quiet_NaN();
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face = SampleFace();
	face.tints[0].type = 3;
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face = SampleFace();
	face.weight[1] = std::numeric_limits<float>::infinity();
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	face = SampleFace();
	face.morphs[1].value = std::numeric_limits<float>::quiet_NaN();
	CHECK(!Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace)));
	// Truncated.
	const auto bytes = Protocol::Encode(SampleFace(), MT::kReportFace);
	CHECK(!Protocol::DecodeFace(std::span(bytes).first(bytes.size() - 1)));
}

// Body weight is the one part of a face every receiver applies to the stand-in's base form: a modified
// game's weight of 7 would deform it in every game.
TEST("protocol: a face's body weight outside the game's range is brought back into it")
{
	auto face = SampleFace();
	face.weight[0] = 7.0f, face.weight[1] = -3.0f, face.weight[2] = 0.25f;
	const auto decoded = Protocol::DecodeFace(Protocol::Encode(face, MT::kReportFace));
	REQUIRE(decoded);
	CHECK(decoded->weight[0] == 1.0f);
	CHECK(decoded->weight[1] == 0.0f);
	CHECK(decoded->weight[2] == 0.25f);
}

TEST("face: a weight out of range reaches the others in range, and newcomers too")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	f.fake->Take(2);
	auto face = SampleFace();
	face.weight[2] = 40.0f;
	f.fake->Deliver(1, Protocol::Encode(face, MT::kReportFace));
	const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(packet);
	const auto relayed = Protocol::DecodeFace(*packet);
	REQUIRE(relayed);
	CHECK(relayed->weight[2] == 1.0f);
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	const auto told = f.fake->WaitFor(3, Type(MT::kPlayerFace));
	REQUIRE(told);
	CHECK(Protocol::DecodeFace(*told)->weight[2] == 1.0f);
}

TEST("face: the others get it with the sender's ID, the sender doesn't")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(alice);
	f.fake->Take(2);
	f.fake->Deliver(1, Protocol::Encode(SampleFace(), MT::kReportFace));
	const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(packet);
	const auto face = Protocol::DecodeFace(*packet);
	REQUIRE(face);
	CHECK(face->playerId == alice->playerId);
	auto expected = SampleFace();
	expected.playerId = alice->playerId;
	CHECK(*face == expected);
	CHECK(!f.fake->WaitFor(1, Type(MT::kPlayerFace), std::chrono::milliseconds(300)));
}

TEST("face: a newcomer is told everyone's face")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->Deliver(1, Protocol::Encode(SampleFace(), MT::kReportFace));
	// The latest report wins.
	auto changed = SampleFace();
	changed.hairColor = 0x0005D7A2;
	f.fake->Deliver(1, Protocol::Encode(changed, MT::kReportFace));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerFace));
	REQUIRE(packet);
	const auto face = Protocol::DecodeFace(*packet);
	REQUIRE(face);
	CHECK(face->playerId == alice->playerId);
	CHECK(face->hairColor == 0x0005D7A2);
}

TEST("face: nothing is sent for a player who never reported one")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kWorldState)));
	CHECK(!f.fake->WaitFor(2, Type(MT::kPlayerFace), std::chrono::milliseconds(300)));
}

// ---- a friend's own head on their stand-in (game/FaceWatch.h) -----------------------------------------------------

namespace
{
	using FaceWatch::Facts;
	using FaceWatch::HEAD_WAIT;
	using FaceWatch::OUR_FACE_GRACE;
	using FaceWatch::OUR_REPAIR_WAIT;
	using FaceWatch::Step;
	using FaceWatch::Then;
	using FaceWatch::Why;

	const auto T0 = FaceWatch::Clock::time_point{} + 100s;

	// The stand-in's 3D is loaded, with or without a head on it.
	Facts Loaded(bool a_face)
	{
		Facts facts;
		facts.has3D = true;
		facts.hasFace = a_face;
		return facts;
	}
}

TEST("face watch: a stand-in with the settler's face is never judged")
{
	FaceWatch::StandIn s;
	for (int i = 0; i < 10; ++i) {
		CHECK(FaceWatch::Frame(s, Loaded(false), T0 + i * 10s).then == Then::kNothing);
	}
	CHECK(s.step == Step::kNone);
}

TEST("face watch: a head the engine built needs nothing, however long")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	CHECK(FaceWatch::Frame(s, Facts{}, T0 + 1s).then == Then::kNothing);  // not loaded yet
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + 2s).then == Then::kNothing);
	CHECK(s.step == Step::kBuilt);
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + 60s).then == Then::kNothing);
	CHECK(s.rebuilds == 0);
}

TEST("face watch: no head after the wait asks for one rebuild, then gives their head up")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	// Its 3D takes a while to load: the wait runs from when it is loaded, not from the placement.
	CHECK(FaceWatch::Frame(s, Facts{}, T0 + 10s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 10s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 10s + HEAD_WAIT - 1ms).then == Then::kNothing);
	const auto asked = FaceWatch::Frame(s, Loaded(false), T0 + 10s + HEAD_WAIT);
	CHECK(asked.then == Then::kRebuild);
	CHECK(s.step == Step::kRebuilding);
	CHECK(s.rebuilds == 1);
	// Asked once: not again while the engine takes its time.
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 11s + HEAD_WAIT).then == Then::kNothing);
	const auto gaveUp = FaceWatch::Frame(s, Loaded(false), T0 + 10s + 2 * HEAD_WAIT);
	CHECK(gaveUp.then == Then::kGiveUp);
	CHECK(gaveUp.why == Why::kNoHead);
	CHECK(s.step == Step::kGivenUp);
	// Given up: nothing more, whatever comes.
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + 100s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 200s).then == Then::kNothing);
}

TEST("face watch: a rebuild that comes back with a head is the end of it, and a head lost later gets no second rebuild")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + HEAD_WAIT).then == Then::kRebuild);
	// The engine takes the 3D down and builds it again, this time with a head.
	CHECK(FaceWatch::Frame(s, Facts{}, T0 + HEAD_WAIT + 500ms).then == Then::kNothing);
	CHECK(s.step == Step::kLoading);
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + HEAD_WAIT + 1s).then == Then::kNothing);
	CHECK(s.step == Step::kBuilt);
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + 30s).then == Then::kNothing);
	// Its head goes later (a reload by the engine): the one rebuild was used, so it is given up after the wait.
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 60s).then == Then::kNothing);
	CHECK(s.step == Step::kLoaded);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 60s + HEAD_WAIT - 1ms).then == Then::kNothing);
	const auto gaveUp = FaceWatch::Frame(s, Loaded(false), T0 + 60s + HEAD_WAIT);
	CHECK(gaveUp.then == Then::kGiveUp);
	CHECK(gaveUp.why == Why::kNoHead);
	CHECK(s.rebuilds == 1);
}

TEST("face watch: a rebuild that unloads and reloads the 3D still without a head is not asked twice")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + HEAD_WAIT).then == Then::kRebuild);
	CHECK(FaceWatch::Frame(s, Facts{}, T0 + HEAD_WAIT + 100ms).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + HEAD_WAIT + 1s).then == Then::kNothing);
	CHECK(s.step == Step::kLoaded);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 2 * HEAD_WAIT).then == Then::kNothing);  // the wait runs from the reload
	const auto gaveUp = FaceWatch::Frame(s, Loaded(false), T0 + 2 * HEAD_WAIT + 1s);
	CHECK(gaveUp.then == Then::kGiveUp);
	CHECK(gaveUp.why == Why::kNoHead);
	CHECK(s.rebuilds == 1);
}

TEST("face watch: in power armor its head is not judged, and the wait starts over once it is its own again")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	auto busy = Loaded(false);
	busy.busy = true;
	CHECK(FaceWatch::Frame(s, busy, T0).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, busy, T0 + 10s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 10s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 10s + HEAD_WAIT - 1ms).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(false), T0 + 10s + HEAD_WAIT).then == Then::kRebuild);
}

TEST("face watch: our own head gone while it is loaded gives theirs up after the grace, not for a frame")
{
	FaceWatch::StandIn s;
	FaceWatch::Placed(s, T0);
	CHECK(FaceWatch::Frame(s, Loaded(true), T0).then == Then::kNothing);
	auto gone = Loaded(true);
	gone.oursGone = true;
	CHECK(FaceWatch::Frame(s, gone, T0 + 1s).then == Then::kNothing);
	CHECK(FaceWatch::Frame(s, Loaded(true), T0 + 1500ms).then == Then::kNothing);  // back by itself: a frame of rebuilding
	CHECK(FaceWatch::Frame(s, gone, T0 + 2s).then == Then::kNothing);              // the grace runs again from here
	CHECK(FaceWatch::Frame(s, gone, T0 + 2s + OUR_FACE_GRACE - 1ms).then == Then::kNothing);
	const auto gaveUp = FaceWatch::Frame(s, gone, T0 + 2s + OUR_FACE_GRACE);
	CHECK(gaveUp.then == Then::kGiveUp);
	CHECK(gaveUp.why == Why::kOurs);
	CHECK(s.step == Step::kGivenUp);

	// Our head gone while the stand-in is not loaded is not its doing.
	FaceWatch::StandIn unloaded;
	FaceWatch::Placed(unloaded, T0);
	Facts away;
	away.oursGone = true;
	for (int i = 0; i < 10; ++i) {
		CHECK(FaceWatch::Frame(unloaded, away, T0 + i * 10s).then == Then::kNothing);
	}
	CHECK(unloaded.step == Step::kLoading);
}

TEST("face watch: our head is asked back once, only if still gone a moment after the stand-in went")
{
	FaceWatch::Ours ours;
	CHECK(!FaceWatch::RepairDue(ours, true, T0));  // no stand-in took it
	FaceWatch::TakenBy(ours, T0);
	CHECK(!FaceWatch::RepairDue(ours, true, T0 + OUR_REPAIR_WAIT - 1ms));
	CHECK(FaceWatch::RepairDue(ours, true, T0 + OUR_REPAIR_WAIT));
	CHECK(!FaceWatch::RepairDue(ours, true, T0 + OUR_REPAIR_WAIT + 1s));  // once
	FaceWatch::TakenBy(ours, T0 + 10s);                                     // asked already: never again this session
	CHECK(!FaceWatch::RepairDue(ours, true, T0 + 10s + OUR_REPAIR_WAIT));

	// Back by itself in time: nothing asked, and the one ask is kept for a next time.
	FaceWatch::Ours fresh;
	FaceWatch::TakenBy(fresh, T0);
	CHECK(!FaceWatch::RepairDue(fresh, false, T0 + OUR_REPAIR_WAIT));
	CHECK(!FaceWatch::RepairDue(fresh, true, T0 + OUR_REPAIR_WAIT + 1s));  // disarmed
	FaceWatch::TakenBy(fresh, T0 + 20s);
	CHECK(FaceWatch::RepairDue(fresh, true, T0 + 20s + OUR_REPAIR_WAIT));
}
