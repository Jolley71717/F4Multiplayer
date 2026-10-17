#include "ServerFixture.h"
#include "Test.h"

#include "Protocol.h"
#include "game/AimPoint.h"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// Where a friend's stand-in points its gun (src/game/AimPoint.h): the owner's look pitch goes out in their
// PlayerState, through the real server to a friend, and comes back as the point the friend's game gives the
// stand-in's weapon aim, the way Session and Puppets do it.
namespace
{
	using namespace TestServer;

	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	float AngleDifference(float a_a, float a_b)
	{
		const float d = std::fmod(std::abs(a_a - a_b), 2.0f * AimPoint::PI);
		return (std::min)(d, 2.0f * AimPoint::PI - d);
	}

	float Distance(const AimPoint::Point& a_a, const AimPoint::Point& a_b)
	{
		return std::sqrt((a_a.x - a_b.x) * (a_a.x - a_b.x) + (a_a.y - a_b.y) * (a_a.y - a_b.y) + (a_a.z - a_b.z) * (a_a.z - a_b.z));
	}
}

TEST("aim point: a friend looking up or down has their stand-in aimed that way, through the server")
{
	Fixture f;
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));

	// Alice's angle x (radians, looking down is positive) and her heading; the bot's --pitch 60 and -60 too.
	struct Look
	{
		float angleX;
		float heading;
	};
	std::uint32_t sequence = 0;
	for (const auto look : { Look{ -0.6f, 1.2f }, Look{ 0.45f, 4.0f }, Look{ 0.0f, 0.0f }, Look{ -1.0472f, 2.5f }, Look{ 1.0472f, 6.0f } }) {
		Protocol::PlayerState state;
		state.sequence = ++sequence;
		state.x = 1000.0f;
		state.y = -2000.0f;
		state.z = 300.0f;
		state.heading = look.heading;
		state.pitch = Protocol::PackPitch(AimPoint::PitchFromLooking(look.angleX));
		f.fake->Take(2);
		f.fake->Deliver(1, Protocol::Encode(state));
		const auto packet = f.fake->WaitFor(2, Type(MT::kPlayerStates));
		REQUIRE(packet);
		const auto batch = Protocol::DecodePlayerStates(*packet);
		REQUIRE(batch);
		REQUIRE(batch->players.size() == 1);
		CHECK(batch->players[0].playerId == alice->playerId);
		const auto& got = batch->players[0].state;
		CHECK(got.sequence == sequence);

		// Bob's game: the stand-in stands a little off the sent place (it is interpolated) and is given the point.
		const AimPoint::Point standIn{ got.x + 12.0f, got.y - 7.0f, got.z + 1.0f };
		const auto            point = AimPoint::ForPuppet(true, standIn, got.heading, static_cast<float>(got.pitch));
		REQUIRE(point);
		const float wanted = -look.angleX * AimPoint::TO_DEGREES;
		CHECK(std::abs(AimPoint::PitchTo(standIn, *point) - wanted) < 0.6f);
		CHECK(AngleDifference(AimPoint::HeadingTo(standIn, *point), look.heading) < 0.001f);
		CHECK(std::abs(Distance(standIn, *point) - AimPoint::REACH) < 1.0f);
		// Looking up puts the point above the stand-in, down below it.
		CHECK((wanted > 1.0f) == (point->z > standIn.z + 1.0f));
		CHECK((wanted < -1.0f) == (point->z < standIn.z - 1.0f));
	}
}

TEST("aim point: the direction is the engine's: heading 0 along +y, a quarter turn along +x, up along +z")
{
	const AimPoint::Point at{ 10.0f, 20.0f, 30.0f };
	const auto            ahead = AimPoint::Along(at, 0.0f, 0.0f);
	CHECK(std::abs(ahead.x - 10.0f) < 0.01f);
	CHECK(std::abs(ahead.y - (20.0f + AimPoint::REACH)) < 0.01f);
	CHECK(std::abs(ahead.z - 30.0f) < 0.01f);
	const auto right = AimPoint::Along(at, AimPoint::PI / 2.0f, 0.0f);
	CHECK(std::abs(right.x - (10.0f + AimPoint::REACH)) < 0.1f);
	CHECK(std::abs(right.y - 20.0f) < 0.1f);
	const auto up = AimPoint::Along(at, 0.0f, 45.0f);
	CHECK(std::abs((up.z - 30.0f) - (up.y - 20.0f)) < 0.1f);
	CHECK(up.z > 30.0f);

	// Looking down is a positive angle x and a negative pitch, as `turnto` and `player.setangle x` have it.
	CHECK(AimPoint::PitchFromLooking(0.5f) < 0.0f);
	CHECK(std::abs(AimPoint::PitchFromLooking(-AimPoint::PI / 6.0f) - 30.0f) < 0.01f);

	// Straight up still says which way the stand-in faces; nonsense counts as level and ahead.
	const auto straightUp = AimPoint::Along(at, AimPoint::PI / 2.0f, 90.0f);
	CHECK(straightUp.x > at.x);
	CHECK(std::abs(AimPoint::HeadingTo(at, straightUp) - AimPoint::PI / 2.0f) < 0.001f);
	const auto nonsense = AimPoint::Along(at, NaN, NaN);
	CHECK(std::abs(nonsense.y - ahead.y) < 0.01f && std::abs(nonsense.z - ahead.z) < 0.01f);
	CHECK(Protocol::PackPitch(AimPoint::PitchFromLooking(NaN)) == 0);
	CHECK(Protocol::PackPitch(AimPoint::PitchFromLooking(-3.0f)) == 90);
}

TEST("aim point: only a friend's stand-in in a known place is given one")
{
	const AimPoint::Point at{ 1.0f, 2.0f, 3.0f };
	CHECK(AimPoint::ForPuppet(true, at, 0.3f, -20.0f).has_value());
	// A mirrored NPC is aimed by ForMirror, from its owner's copy's pitch, not by this.
	CHECK(!AimPoint::ForPuppet(false, at, 0.3f, -20.0f).has_value());
	CHECK(!AimPoint::ForPuppet(true, { NaN, 2.0f, 3.0f }, 0.3f, -20.0f).has_value());
	CHECK(!AimPoint::ForPuppet(true, { 1.0f, 2.0f, std::numeric_limits<float>::infinity() }, 0.3f, -20.0f).has_value());
}

// A mirrored NPC (src/game/AimPoint.h ForMirror): the owner's game reads the point its NPC's combat AI aims at
// (NpcSync::SendOwnedStates, AimPoint::OwnerPitch), sends the pitch in its ActorState through the real server, and
// the copy's game gives the copy, whose AI is off, a point at that pitch along its heading each frame, the way
// Puppets::Aim does, or forgets its point once the owner's copy aims at nothing.
namespace
{
	constexpr std::uint32_t RAIDER = 0x0001A4D7;

	// The look-at point of the copy in the joiner's game, as the engine keeps it, and what was done to it.
	struct CopyAim
	{
		std::optional<AimPoint::Point> point;
		int                            sets = 0;
		int                            clears = 0;

		void Frame(const Protocol::ActorState& a_state, const AimPoint::Point& a_feet)
		{
			const auto order = AimPoint::ForMirror(Protocol::UnpackAimPitch(a_state.aimPitch), point.has_value(), a_feet, a_state.heading);
			if (order.act == AimPoint::MirrorOrder::Act::kSet) {
				point = order.point;
				++sets;
			} else if (order.act == AimPoint::MirrorOrder::Act::kClear) {
				point.reset();
				++clears;
			}
		}
	};

	// The host (peer 1) runs the raider and sends a_states; returns what the joiner (peer 2) receives.
	std::vector<Protocol::ActorState> RelayRaider(const std::vector<Protocol::ActorState>& a_states)
	{
		Fixture f;
		REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
		REQUIRE(f.Join(2, Fixture::MakeHello("Joiner")));
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { RAIDER, Protocol::ClaimReason::kUnowned } }));
		Sync(f, 1);
		for (const auto& state : a_states) {
			f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorState>{ state }, MT::kActorStates));
		}
		Sync(f, 2);
		std::vector<Protocol::ActorState> got;
		for (const auto& packet : f.fake->Take(2)) {
			if (!packet.empty() && packet[0] == Type(MT::kActorStatesRelay)) {
				const auto batch = Protocol::DecodeActorStates(packet);
				REQUIRE(batch);
				got.insert(got.end(), batch->begin(), batch->end());
			}
		}
		return got;
	}

	// The raider as the host's game reports it, standing at a_feet facing a_heading, with its combat AI's aim point
	// (none: it aims at nothing).
	Protocol::ActorState OwnedRaider(const AimPoint::Point& a_feet, float a_heading, std::optional<AimPoint::Point> a_aimPoint)
	{
		Protocol::ActorState state;
		state.refId = RAIDER;
		state.worldspace = 0x0000003C;
		state.x = a_feet.x;
		state.y = a_feet.y;
		state.z = a_feet.z;
		state.heading = a_heading;
		state.flags = Protocol::kWeaponDrawn;
		state.aimPitch = Protocol::PackAimPitch(AimPoint::OwnerPitch(a_feet, a_aimPoint));
		return state;
	}

	AimPoint::Point Shoulders(const AimPoint::Point& a_feet)
	{
		return { a_feet.x, a_feet.y, a_feet.z + AimPoint::MIRROR_HEIGHT };
	}
}

TEST("aim point: a mirrored NPC aims up and down where its owner's copy aims, through the server")
{
	// The host's raider shoots at a bloatfly hovering over it, at one on a roof, at a mole rat at its feet, then at
	// a raider level with it: each the point its combat AI aims at, as Actor::SetLookAtTarget leaves it.
	const AimPoint::Point feet{ 5000.0f, -3000.0f, 200.0f };
	const float           heading = 0.8f;
	const AimPoint::Point targets[] = {
		{ feet.x + 150.0f, feet.y + 140.0f, feet.z + 420.0f },
		{ feet.x + 900.0f, feet.y + 600.0f, feet.z + 700.0f },
		{ feet.x + 120.0f, feet.y + 90.0f, feet.z + 10.0f },
		{ feet.x + 1500.0f, feet.y + 1100.0f, feet.z + AimPoint::MIRROR_HEIGHT },
	};
	std::vector<Protocol::ActorState> sent;
	for (const auto& target : targets) {
		sent.push_back(OwnedRaider(feet, heading, target));
	}
	const auto got = RelayRaider(sent);
	REQUIRE(got.size() == sent.size());

	// The joiner's copy stands a little off the host's place (it is interpolated) and faces the same way.
	const AimPoint::Point copyFeet{ feet.x + 15.0f, feet.y - 8.0f, feet.z + 2.0f };
	CopyAim               copy;
	for (std::size_t i = 0; i < got.size(); ++i) {
		const float wanted = AimPoint::PitchTo(Shoulders(feet), targets[i]);
		CHECK(got[i].aimPitch != Protocol::NO_AIM);
		copy.Frame(got[i], copyFeet);
		REQUIRE(copy.point);
		// From the copy's shoulders, its point is at the pitch the host's raider aims at from its own (rounded to a
		// degree on the way), along the way it faces, REACH away.
		CHECK(std::abs(AimPoint::PitchTo(Shoulders(copyFeet), *copy.point) - wanted) < 0.6f);
		CHECK(AngleDifference(AimPoint::HeadingTo(copyFeet, *copy.point), heading) < 0.001f);
		CHECK(std::abs(Distance(Shoulders(copyFeet), *copy.point) - AimPoint::REACH) < 1.0f);
	}
	// Up at the bloatfly (about 56 degrees from its shoulders, not the 64 from its feet), up at the roof, down at
	// the mole rat, level at the raider.
	CHECK(got[0].aimPitch > 50 && got[0].aimPitch < 58);
	CHECK(got[1].aimPitch > 20);
	CHECK(got[2].aimPitch < -20);
	CHECK(got[3].aimPitch == 0);
	CHECK(copy.sets == 4);
	CHECK(copy.clears == 0);
}

TEST("aim point: a mirrored NPC lowers its gun once its owner's copy stops aiming, and is not cleared again")
{
	const AimPoint::Point             feet{ -700.0f, 1200.0f, 50.0f };
	const AimPoint::Point             flyer{ -600.0f, 1300.0f, 600.0f };
	std::vector<Protocol::ActorState> sent;
	for (int i = 0; i < 3; ++i) {
		sent.push_back(OwnedRaider(feet, 2.0f, flyer));
	}
	// The fight is over: the host's raider has no aim point any more.
	for (int i = 0; i < 4; ++i) {
		sent.push_back(OwnedRaider(feet, 2.0f, std::nullopt));
	}
	const auto got = RelayRaider(sent);
	REQUIRE(got.size() == sent.size());
	CHECK(got[3].aimPitch == Protocol::NO_AIM);
	CHECK(!Protocol::UnpackAimPitch(got[3].aimPitch));

	CopyAim copy;
	for (std::size_t i = 0; i < 3; ++i) {
		copy.Frame(got[i], feet);
		CHECK(copy.point && copy.point->z > feet.z + 1000.0f);
	}
	copy.Frame(got[3], feet);
	CHECK(!copy.point);
	CHECK(copy.clears == 1);
	for (std::size_t i = 4; i < got.size(); ++i) {
		copy.Frame(got[i], feet);
	}
	CHECK(copy.clears == 1);  // nothing left to forget: left alone
	CHECK(copy.sets == 3);
}

TEST("aim point: a copy taken over with its own AI's aim point forgets it while the owner's copy aims at nothing")
{
	// The joiner's own AI had the raider aim at something before the host took it over.
	const AimPoint::Point feet{ 0.0f, 0.0f, 0.0f };
	const auto            got = RelayRaider({ OwnedRaider(feet, 0.0f, std::nullopt) });
	REQUIRE(got.size() == 1);
	CopyAim copy;
	copy.point = AimPoint::Point{ 100.0f, 100.0f, 500.0f };
	copy.Frame(got[0], feet);
	CHECK(!copy.point);
	CHECK(copy.clears == 1);
}

TEST("aim point: a mirrored NPC's aim when something is missing or nonsense")
{
	const AimPoint::Point feet{ 1.0f, 2.0f, 3.0f };
	// No aim point, or a nonsense one, sends none.
	CHECK(!AimPoint::OwnerPitch(feet, std::nullopt));
	CHECK(!AimPoint::OwnerPitch(feet, AimPoint::Point{ NaN, 0.0f, 0.0f }));
	CHECK(!AimPoint::OwnerPitch({ NaN, 2.0f, 3.0f }, AimPoint::Point{ 0.0f, 0.0f, 0.0f }));
	CHECK(Protocol::PackAimPitch(std::nullopt) == Protocol::NO_AIM);
	CHECK(Protocol::PackAimPitch(NaN) == Protocol::NO_AIM);
	// Straight up or down is held to a quarter turn, which still counts as an aim.
	CHECK(Protocol::PackAimPitch(200.0f) == 90);
	CHECK(Protocol::PackAimPitch(-200.0f) == -90);
	CHECK(Protocol::UnpackAimPitch(90) == 90.0f);
	CHECK(Protocol::UnpackAimPitch(-90) == -90.0f);
	CHECK(Protocol::UnpackAimPitch(0) == 0.0f);
	// A value no sender makes is no aim.
	CHECK(!Protocol::UnpackAimPitch(91));
	CHECK(!Protocol::UnpackAimPitch(-91));
	CHECK(!Protocol::UnpackAimPitch(Protocol::NO_AIM));
	// A state made without one aims at nothing.
	CHECK(Protocol::ActorState{}.aimPitch == Protocol::NO_AIM);

	// A copy whose place is not known is left as it is, aim or none.
	CHECK(AimPoint::ForMirror(30.0f, false, { NaN, 0.0f, 0.0f }, 0.0f).act == AimPoint::MirrorOrder::Act::kLeave);
	CHECK(AimPoint::ForMirror(std::nullopt, true, { 0.0f, std::numeric_limits<float>::infinity(), 0.0f }, 0.0f).act == AimPoint::MirrorOrder::Act::kLeave);
	// No aim and no point: nothing to do, every frame.
	CHECK(AimPoint::ForMirror(std::nullopt, false, feet, 0.0f).act == AimPoint::MirrorOrder::Act::kLeave);
	// A nonsense heading still aims at the pitch, straight ahead.
	const auto order = AimPoint::ForMirror(-30.0f, false, feet, NaN);
	CHECK(order.act == AimPoint::MirrorOrder::Act::kSet);
	CHECK(std::abs(AimPoint::PitchTo(Shoulders(feet), order.point) + 30.0f) < 0.01f);
}

TEST("aim point: between two states a gun sweeps from the one pitch to the other, and holds at either end")
{
	// A friend's states come ten or twenty times a second: drawn a quarter of the way from one to the next, their
	// stand-in aims a quarter of the way up.
	CHECK(std::abs(AimPoint::BlendPitch(10.0f, 30.0f, 0.25f) - 15.0f) < 0.001f);
	CHECK(std::abs(AimPoint::BlendPitch(10.0f, 30.0f, 0.5f) - 20.0f) < 0.001f);
	CHECK(std::abs(AimPoint::BlendPitch(30.0f, -30.0f, 0.5f)) < 0.001f);
	CHECK(AimPoint::BlendPitch(10.0f, 30.0f, 0.0f) == 10.0f);
	CHECK(AimPoint::BlendPitch(10.0f, 30.0f, 1.0f) == 30.0f);
	// Before the first or past the second it is held there; a nonsense way along is the later state.
	CHECK(AimPoint::BlendPitch(10.0f, 30.0f, -0.5f) == 10.0f);
	CHECK(AimPoint::BlendPitch(10.0f, 30.0f, 1.5f) == 30.0f);
	CHECK(AimPoint::BlendPitch(10.0f, 30.0f, NaN) == 30.0f);
	// A nonsense pitch counts as level.
	CHECK(std::abs(AimPoint::BlendPitch(NaN, 30.0f, 0.5f) - 15.0f) < 0.001f);
	CHECK(std::isfinite(AimPoint::BlendPitch(10.0f, std::numeric_limits<float>::infinity(), 0.5f)));
}

TEST("aim point: a mirrored NPC's aim sweeps between two aims, and comes up or goes down with the later state")
{
	CHECK(AimPoint::BlendAim(10.0f, 50.0f, 0.25f) == 20.0f);
	CHECK(AimPoint::BlendAim(-20.0f, 20.0f, 0.5f) == 0.0f);
	// The owner's copy started aiming: its copy is given the aim whole, the aim modifier eases the arms up itself.
	CHECK(AimPoint::BlendAim(std::nullopt, 40.0f, 0.1f) == 40.0f);
	// It stopped: the copy's aim is forgotten at once (ForMirror clears), not blended toward nothing.
	CHECK(!AimPoint::BlendAim(40.0f, std::nullopt, 0.9f));
	CHECK(!AimPoint::BlendAim(std::nullopt, std::nullopt, 0.5f));

	// Through the server: the host's raider tracks a bloatfly climbing from level to 40 degrees up in two states;
	// the joiner's copy, drawn halfway between them, is given a point 20 degrees up from its shoulders.
	const AimPoint::Point feet{ 100.0f, 200.0f, 50.0f };
	const auto            got = RelayRaider({
        OwnedRaider(feet, 1.0f, AimPoint::Point{ feet.x, feet.y + 1000.0f, feet.z + AimPoint::MIRROR_HEIGHT }),
        OwnedRaider(feet, 1.0f, AimPoint::Point{ feet.x, feet.y + 1000.0f, feet.z + AimPoint::MIRROR_HEIGHT + 1000.0f * std::tan(40.0f / AimPoint::TO_DEGREES) }),
    });
	REQUIRE(got.size() == 2);
	const auto between = AimPoint::BlendAim(Protocol::UnpackAimPitch(got[0].aimPitch), Protocol::UnpackAimPitch(got[1].aimPitch), 0.5f);
	REQUIRE(between);
	CHECK(std::abs(*between - 20.0f) < 0.6f);
	const auto order = AimPoint::ForMirror(between, false, feet, 1.0f);
	CHECK(order.act == AimPoint::MirrorOrder::Act::kSet);
	CHECK(std::abs(AimPoint::PitchTo(Shoulders(feet), order.point) - 20.0f) < 0.6f);
}
