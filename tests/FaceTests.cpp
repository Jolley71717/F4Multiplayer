#include "ServerFixture.h"

using namespace TestServer;

// A player's face: reported once, relayed to the others, told to newcomers.
namespace
{
	Protocol::Face SampleFace()
	{
		Protocol::Face face;
		face.female = true;
		face.race = 0x00013746;  // HumanRace
		face.headParts = { 0x0004DA7A, 0x00047B2B, 0x0005A1D7 };
		face.hairColor = 0x0005D7A1;
		face.weight[0] = 0.2f, face.weight[1] = 0.5f, face.weight[2] = 0.3f;
		face.sliders = { 0.0f, 0.25f, 1.0f, -0.5f };
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
	// Truncated.
	const auto bytes = Protocol::Encode(SampleFace(), MT::kReportFace);
	CHECK(!Protocol::DecodeFace(std::span(bytes).first(bytes.size() - 1)));
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
