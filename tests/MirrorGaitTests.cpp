#include "ServerFixture.h"

#include "game/MirrorGait.h"
#include "game/MoveSync.h"

#include <cmath>
#include <cstring>
#include <string>

// A mirrored NPC's legs as the copies play them (src/game/MirrorGait.h). The owner's game sends each NPC's states
// through a real server with what its own graph plays, as NpcSync::SendOwnedStates does; the copy's game decodes what
// the server relays and plays each state the way Puppets::DriveAnimation does, against a model of the humanoid
// locomotion graph: standing takes only "moveStart", which enters the gait iSyncLocomotionSpeed names; moving, it
// changes gait only on "Walk", "Jog" and "Run"; each clip plays at its own rate variable.
using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	constexpr std::uint32_t SETTLER = 0x0001A4D7;
	constexpr std::uint32_t WORLD = 0x0000003C;
	constexpr int           WALK = 0;
	constexpr int           JOG = 1;
	constexpr int           RUN = 2;

	const auto     t0 = MoveSync::Clock::time_point{} + 1h;
	constexpr auto FRAME = 50ms;

	struct PersonGraph
	{
		bool         moving = false;
		int          gait = WALK;  // the state it plays
		std::int32_t sync = WALK;  // iSyncLocomotionSpeed
		float        walkRate = 1.0f;
		float        runRate = 1.0f;
		int          gaitEvents = 0;

		bool Notify(const char* a_event)
		{
			if (std::strcmp(a_event, "moveStart") == 0) {
				if (moving) {
					return false;
				}
				moving = true;
				gait = sync;
				return true;
			}
			if (std::strcmp(a_event, "moveStop") == 0) {
				if (!moving) {
					return false;
				}
				moving = false;
				return true;
			}
			const int to = std::strcmp(a_event, "Run") == 0 ? RUN : std::strcmp(a_event, "Jog") == 0 ? JOG : std::strcmp(a_event, "Walk") == 0 ? WALK : -1;
			if (to < 0 || !moving) {
				return false;  // the weapon-drawn graph's events, or not in locomotion
			}
			++gaitEvents;
			gait = to;
			return true;
		}
	};

	struct Copy
	{
		bool                        moving = false;
		MoveSync::Clock::time_point nextResync{};
		Gait::Kind                  gait = Gait::Kind::kWalk;
		float                       gaitSpeed = 0.0f;
		bool                        started = false;
		MoveSync::Clock::time_point now = t0;
	};

	// One DriveAnimation frame for a mirrored person, in its order: the gait's sync variable, start or stop, the clips'
	// rates, the gait's event.
	void Frame(PersonGraph& a_graph, Copy& a_copy, const Protocol::ActorState& a_state)
	{
		const float gaitSpeed = Gait::Smooth(a_copy.gaitSpeed, a_state.speed, a_copy.started ? std::chrono::duration<float>(FRAME).count() : 0.0f);
		const bool  own = (a_state.flags & Protocol::kOwnGait) != 0;
		const auto  plan = MirrorGait::Person(own, a_state.gait, a_state.gaitRate, (a_state.flags & Protocol::kWeaponDrawn) != 0, gaitSpeed, a_copy.gait, a_state.moveMode);
		if (!a_graph.moving) {
			a_graph.sync = Gait::SyncValue(plan.gait);
		}
		const auto step = MoveSync::Drive(a_copy.moving, a_copy.nextResync, a_state.speed, a_copy.now += FRAME, [&](MoveSync::Event a_event) {
			return a_graph.Notify(a_event == MoveSync::Event::kStart ? "moveStart" : "moveStop");
		});
		a_graph.walkRate = plan.walkRate;
		a_graph.runRate = plan.runRate;
		if (Gait::Send(step.moving, a_copy.moving, plan.gait, a_copy.gait, step.resent != nullptr)) {
			a_graph.Notify(Gait::Event(plan.gait));
			a_graph.Notify(Gait::WeaponEvent(plan.gait));
		}
		a_copy.moving = step.moving;
		a_copy.nextResync = step.nextResync;
		a_copy.gait = plan.gait;
		a_copy.gaitSpeed = gaitSpeed;
		a_copy.started = true;
	}

	constexpr std::uint16_t RUNNING = 0xA6;
	constexpr std::uint16_t WALKING = 0x66;

	// The owner's settler as NpcSync::SendOwnedStates reports it: its speed, moveMode, and what its graph plays (a_gait
	// -1: a graph it could not read; a_rate 0: none read, as for the jog).
	Protocol::ActorState Owned(float a_speed, std::uint16_t a_moveMode, int a_gait, float a_rate, bool a_drawn = false)
	{
		Protocol::ActorState state;
		state.refId = SETTLER;
		state.worldspace = WORLD;
		state.speed = a_speed;
		state.moveMode = a_moveMode;
		if (a_drawn) {
			state.flags |= Protocol::kWeaponDrawn;
		}
		if (a_gait >= 0) {
			state.gait = static_cast<std::uint8_t>(a_gait);
			state.gaitRate = CreatureGait::PackRate(a_rate);
			state.flags |= Protocol::kOwnGait;
		}
		return state;
	}

	// The host (peer 1) runs the settler and sends a_states; the joiner (peer 2) gets them through the server.
	std::vector<Protocol::ActorState> ThroughServer(const std::vector<Protocol::ActorState>& a_states)
	{
		Fixture f;
		REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
		REQUIRE(f.Join(2, Fixture::MakeHello("Joiner")));
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { SETTLER, Protocol::ClaimReason::kUnowned } }));
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

	std::vector<Protocol::ActorState> Repeat(const Protocol::ActorState& a_state, int a_times)
	{
		return std::vector<Protocol::ActorState>(static_cast<std::size_t>(a_times), a_state);
	}

	bool Near(float a, float b)
	{
		return std::abs(a - b) < 0.02f;
	}
}

TEST("mirror gait: a settler the host's game has jogging jogs on the joiner's screen, entered at its start")
{
	// What the host's game reads for a jogging settler: its graph's jog (no rate of ours for it), moveMode running.
	const auto got = ThroughServer(Repeat(Owned(240.0f, RUNNING, JOG, 0.0f), 20));
	REQUIRE(got.size() == 20);
	CHECK((got[0].flags & Protocol::kOwnGait) != 0);
	CHECK(got[0].gait == JOG);

	PersonGraph graph;
	Copy        copy;
	for (const auto& state : got) {
		Frame(graph, copy, state);
		CHECK(graph.moving);
		CHECK(graph.gait == JOG);  // from the first frame
	}
	CHECK(graph.gaitEvents == 1);  // the one sent as it set off
}

TEST("mirror gait: before, the joiner's copy of a jogging settler played the run in slow motion")
{
	// The same states without the owner's gait (a graph it could not read), as every state was before.
	const auto  got = ThroughServer(Repeat(Owned(240.0f, RUNNING, -1, 0.0f), 20));
	PersonGraph graph;
	Copy        copy;
	for (const auto& state : got) {
		Frame(graph, copy, state);
	}
	CHECK(graph.gait == RUN);
	CHECK(graph.runRate < 0.7f);
}

TEST("mirror gait: the copy follows the owner's settler from a walk to a jog, a run and back, one event a change, at the owner's rates")
{
	const struct
	{
		float         speed;
		std::uint16_t moveMode;
		int           gait;
		float         rate;
	} legs[] = { { 92.0f, WALKING, WALK, 0.98f }, { 240.0f, RUNNING, JOG, 0.0f }, { 373.0f, RUNNING, RUN, 1.0f }, { 400.0f, RUNNING, RUN, 1.08f }, { 110.0f, WALKING, WALK, 1.19f } };
	std::vector<Protocol::ActorState> sent;
	for (const auto& leg : legs) {
		for (int i = 0; i < 10; ++i) {
			sent.push_back(Owned(leg.speed, leg.moveMode, leg.gait, leg.rate));
		}
	}
	const auto got = ThroughServer(sent);
	REQUIRE(got.size() == sent.size());
	PersonGraph graph;
	Copy        copy;
	for (std::size_t i = 0; i < got.size(); ++i) {
		Frame(graph, copy, got[i]);
		const auto& leg = legs[i / 10];
		CHECK(graph.gait == leg.gait);
		if (leg.gait == WALK) {
			CHECK(Near(graph.walkRate, leg.rate));
		} else if (leg.gait == RUN) {
			CHECK(Near(graph.runRate, leg.rate));
		}
	}
	CHECK(graph.gaitEvents == 4);  // set off walking, then the jog, the run and the walk again; the faster run is no change
}

TEST("mirror gait: an owner's weapon-drawn walk or run is sent as its weapon-drawn graph has it, at that clip's rate")
{
	CHECK(MirrorGait::PersonGait(true, 0) == Gait::Kind::kWalk);
	CHECK(MirrorGait::PersonGait(true, 1) == Gait::Kind::kRun);
	CHECK(!MirrorGait::PersonGait(true, 2));
	CHECK(MirrorGait::PersonGait(false, 1) == Gait::Kind::kJog);
	CHECK(!MirrorGait::PersonGait(false, 3));
	CHECK(!MirrorGait::PersonGait(false, -1));
	CHECK(std::strcmp(MirrorGait::PersonRate(true, Gait::Kind::kWalk), MirrorGait::WEAPON_WALK_RATE) == 0);
	CHECK(std::strcmp(MirrorGait::PersonRate(true, Gait::Kind::kRun), MirrorGait::WEAPON_RUN_RATE) == 0);
	CHECK(std::strcmp(MirrorGait::PersonRate(false, Gait::Kind::kRun), MirrorGait::RUN_RATE) == 0);
	CHECK(MirrorGait::PersonRate(false, Gait::Kind::kJog) == nullptr);

	// The copy plays the owner's rate on the weapon-drawn clip, the others by its speed; a jog and a run both run there.
	const auto drawnRun = MirrorGait::Person(true, RUN, CreatureGait::PackRate(1.3f), true, 373.0f, Gait::Kind::kWalk, RUNNING);
	CHECK(drawnRun.gait == Gait::Kind::kRun);
	CHECK(Near(drawnRun.weaponRunRate, 1.3f));
	CHECK(Near(drawnRun.runRate, Gait::Playback(373.0f, Gait::RUN_CLIP)));
	CHECK(Gait::WeaponState(Gait::Kind::kJog) == 1);
	CHECK(std::strcmp(Gait::WeaponEvent(Gait::Kind::kJog), "runStart") == 0);
	CHECK(std::strcmp(Gait::Event(Gait::Kind::kJog), "Jog") == 0);
	// A rate out of what still looks like the gait is held to it, as the speed's is.
	CHECK(MirrorGait::Person(true, WALK, CreatureGait::PackRate(3.5f), false, 92.0f, Gait::Kind::kWalk, WALKING).walkRate == Gait::MAX_PLAYBACK);
}

TEST("mirror gait: a stand-in, and a state without the owner's gait, go by the speed and moveMode as before")
{
	for (const float speed : { 0.0f, 92.0f, 180.0f, 373.0f, 700.0f }) {
		for (const auto current : { Gait::Kind::kWalk, Gait::Kind::kRun }) {
			for (const std::uint16_t moveMode : { std::uint16_t{ 0 }, WALKING, RUNNING }) {
				// A stand-in's flags never carry the owner's gait; garbage in gait and gaitRate changes nothing.
				const auto plan = MirrorGait::Person(false, 1, 200, false, speed, current, moveMode);
				CHECK(plan.gait == Gait::Pick(speed, current, moveMode));
				CHECK(plan.walkRate == Gait::Playback(speed, Gait::WALK_CLIP));
				CHECK(plan.runRate == Gait::Playback(speed, Gait::RUN_CLIP));
				CHECK(plan.weaponWalkRate == Gait::Playback(speed, Gait::WEAPON_WALK_CLIP));
				CHECK(plan.weaponRunRate == Gait::Playback(speed, Gait::WEAPON_RUN_CLIP));
			}
		}
	}
	// A gait the humanoid graph does not have goes by the speed too.
	CHECK(MirrorGait::Person(true, 7, 64, false, 373.0f, Gait::Kind::kWalk, 0).gait == Gait::Kind::kRun);
}

TEST("mirror gait: an owner's NPC whose standing graph creeps is sent standing; one carried further is sent the ground it covers")
{
	using MirrorGait::Family;
	// Its graph stands, its legs play nothing, and the ground covered is within a standing NPC's creep: standing.
	CHECK(MirrorGait::Reported(0.0f, 0.0f, Family::kPerson, true) == 0.0f);
	CHECK(MirrorGait::Reported(0.0f, 12.0f, Family::kPerson, true) == 0.0f);
	CHECK(MirrorGait::Reported(0.0f, MoveSync::START, Family::kPerson, true) == 0.0f);
	// Carried past that with its legs still (a lift, a vertibird, a script, a push): the ground it covers, as before.
	CHECK(MirrorGait::Reported(0.0f, MoveSync::START + 1.0f, Family::kPerson, true) == MoveSync::START + 1.0f);
	CHECK(MirrorGait::Reported(0.0f, 120.0f, Family::kPerson, true) == 120.0f);
	CHECK(MirrorGait::Reported(0.0f, 12.0f, Family::kPerson, false) == 12.0f);   // its graph not read: by the ground, as before
	CHECK(MirrorGait::Reported(0.0f, 120.0f, Family::kPerson, false) == 120.0f);
	CHECK(MirrorGait::Reported(373.0f, 0.0f, Family::kPerson, true) == 373.0f);  // legs that play go before the graph's word
	CHECK(MirrorGait::Reported(92.6f, 300.0f, Family::kPerson, false) == 92.6f);
	// A creature's graph is not taken at its word: one that never had the variable kept would slide along standing.
	CHECK(MirrorGait::Reported(0.0f, 12.0f, Family::kDog, true) == 12.0f);
	CHECK(MirrorGait::Reported(0.0f, 458.0f, Family::kDog, true) == 458.0f);
	CHECK(MirrorGait::Reported(0.0f, 300.0f, Family::kCreature, true) == 300.0f);

	// Standing with a creep of 12 units/s for two seconds: the copy's graph never sets off.
	std::vector<Protocol::ActorState> sent;
	for (int i = 0; i < 40; ++i) {
		sent.push_back(Owned(MirrorGait::Reported(0.0f, 12.0f, Family::kPerson, true), 0, WALK, 1.0f));
	}
	PersonGraph graph;
	Copy        copy;
	for (const auto& state : ThroughServer(sent)) {
		Frame(graph, copy, state);
		CHECK(!graph.moving);
	}
	// Carried at 120 units/s for as long: the copy walks along, as it did before its owner's graph was read.
	sent.clear();
	for (int i = 0; i < 40; ++i) {
		sent.push_back(Owned(MirrorGait::Reported(0.0f, 120.0f, Family::kPerson, true), 0, WALK, 1.0f));
	}
	PersonGraph carriedGraph;
	Copy        carried;
	bool        walked = false;
	for (const auto& state : ThroughServer(sent)) {
		Frame(carriedGraph, carried, state);
		walked = walked || carriedGraph.moving;
	}
	CHECK(walked);
	CHECK(carriedGraph.moving && carriedGraph.gait == WALK);
}

TEST("mirror gait: which graph an NPC has is told from the variables it answers, the same in both games")
{
	using MirrorGait::Family;
	CHECK(MirrorGait::Identify(true, false, -1) == Family::kPerson);
	CHECK(MirrorGait::Identify(true, true, 0) == Family::kPerson);  // the walk's rate goes first
	CHECK(MirrorGait::Identify(false, true, 0) == Family::kDog);
	CHECK(MirrorGait::Identify(false, false, 0) == Family::kCreature);  // a range without the dog's marker
	CHECK(MirrorGait::Identify(false, false, 1) == Family::kCreature);
	CHECK(MirrorGait::Identify(false, true, 1) == Family::kCreature);   // the marker without a range is no dog
	CHECK(MirrorGait::Identify(false, false, -1) == Family::kNone);
	CHECK(std::strcmp(MirrorGait::CREATURE_VARIABLES[0], CreatureGait::RANGE_VARIABLE) == 0);

	// A creature's state is sent as a byte, never one out of it.
	CHECK(MirrorGait::CreatureState(3) == std::uint8_t{ 3 });
	CHECK(!MirrorGait::CreatureState(-1));
	CHECK(!MirrorGait::CreatureState(256));
	const auto got = ThroughServer({ Owned(300.0f, RUNNING, 3, 0.0f) });
	REQUIRE(got.size() == 1);
	CHECK(got[0].gait == 3);
	CHECK(got[0].gaitRate == 0);
}
