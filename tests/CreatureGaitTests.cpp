#include "ServerFixture.h"

#include "game/CreatureGait.h"
#include "game/MoveSync.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>

// A friend's Dogmeat as the other games' copies play him (src/game/CreatureGait.h). The owner's game sends its
// dog's states through a real server, as NpcSync::SendOwnedStates does with the gait and clip rate its graph has;
// the copy's game decodes what the server relays and plays each state through CreatureGait and MoveSync the way
// Puppets::DriveAnimation does, against a model of the dog's locomotion graph (DogmeatDefault.hkx): standing takes
// only "moveStart", which enters the state iSyncLocomotionRangeID names; moving, its MovingStateMachine changes
// state only on "ToWalkForward", "ToTrotForward", "ToTrotFastForward", "ToRunForward" and "ToWalkBackwards" (taken
// from the state it is in too, which starts the clip over); each clip plays at its own rate variable; nothing in it
// goes by Speed.
using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	constexpr std::uint32_t DOGMEAT = 0x0001D162;
	constexpr std::uint32_t WORLD = 0x0000003C;

	const auto     t0 = MoveSync::Clock::time_point{} + 1h;
	constexpr auto FRAME = 50ms;

	struct DogGraph
	{
		bool                         moving = false;
		int                          state = CreatureGait::WALK;  // the MovingStateMachine state it plays
		std::int32_t                 sync = CreatureGait::WALK;   // iSyncLocomotionRangeID
		std::map<std::string, float> rates{ { "fWalkClipMult", 1.0f }, { "fTrotClipMult", 1.0f }, { "fTrotFastClipMult", 1.0f }, { "fRunClipMult", 1.0f } };
		int                          gaitEvents = 0;
		int                          restarts = 0;  // a gait event to the state it was in: its clip started over

		bool Notify(const char* a_event)
		{
			if (std::strcmp(a_event, "moveStart") == 0) {
				if (moving) {
					return false;
				}
				moving = true;
				state = CreatureGait::Find(sync) ? sync : CreatureGait::WALK;
				return true;
			}
			if (std::strcmp(a_event, "moveStop") == 0) {
				if (!moving) {
					return false;
				}
				moving = false;
				return true;
			}
			for (const auto& range : CreatureGait::RANGES) {
				if (std::strcmp(a_event, range.event) == 0) {
					if (!moving) {
						return false;  // the standing state has no such transition
					}
					++gaitEvents;
					restarts += state == range.id;
					state = range.id;
					return true;
				}
			}
			return false;  // the humanoid graph's events ("Walk", "Run", "runStart"): a dog's has none
		}

		// The rate the state it plays runs at.
		float Rate() const
		{
			return rates.at(CreatureGait::Find(state)->rateVariable);
		}
	};

	struct Copy
	{
		bool                        moving = false;
		MoveSync::Clock::time_point nextResync{};
		std::int32_t                range = -1;
	};

	// One DriveAnimation frame for a dog, in its order: the gait's state, start or stop, the clip's rate, the event.
	void Frame(DogGraph& a_graph, Copy& a_copy, const Protocol::ActorState& a_state, MoveSync::Clock::time_point a_now)
	{
		const auto plan = CreatureGait::Choose((a_state.flags & Protocol::kOwnGait) != 0, a_state.gait, a_state.gaitRate, a_state.speed);
		a_graph.sync = plan.range->id;
		const auto step = MoveSync::Drive(a_copy.moving, a_copy.nextResync, a_state.speed, a_now, [&](MoveSync::Event a_event) {
			return a_graph.Notify(a_event == MoveSync::Event::kStart ? "moveStart" : "moveStop");
		});
		a_graph.rates[plan.range->rateVariable] = plan.rate;
		if (CreatureGait::Send(step.moving, a_copy.moving, plan.range->id, a_copy.range, step.resent != nullptr)) {
			a_graph.Notify(plan.range->event);
		}
		a_copy.moving = step.moving;
		a_copy.nextResync = step.nextResync;
		a_copy.range = step.moving ? plan.range->id : -1;
	}

	// The owner's dog as NpcSync::SendOwnedStates reports it: its animation speed, and its graph's gait and the rate
	// that gait's clip plays at. a_gait -1: a graph it could not read them from.
	Protocol::ActorState Owned(float a_speed, int a_gait, float a_rate)
	{
		Protocol::ActorState state;
		state.refId = DOGMEAT;
		state.worldspace = WORLD;
		state.speed = a_speed;
		if (a_gait >= 0) {
			state.gait = static_cast<std::uint8_t>(a_gait);
			state.gaitRate = CreatureGait::PackRate(a_rate);
			state.flags |= Protocol::kOwnGait;
		}
		return state;
	}

	// The host (peer 1) runs Dogmeat and sends a_states, one a frame; the joiner (peer 2) gets them through the
	// server and its copy plays each. Returns what the joiner's game received.
	std::vector<Protocol::ActorState> ThroughServer(const std::vector<Protocol::ActorState>& a_states)
	{
		Fixture f;
		REQUIRE(f.Join(1, Fixture::MakeHello("Host")));
		REQUIRE(f.Join(2, Fixture::MakeHello("Joiner")));
		f.fake->Deliver(1, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { DOGMEAT, Protocol::ClaimReason::kUnowned } }));
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

TEST("creature gait: the host's Dogmeat running is shown running on the joiner, at the run's own rate")
{
	// What the host's game reads for a running dog: its run (state 3) at its own pace, the clip at 1.03.
	const auto got = ThroughServer(Repeat(Owned(458.0f, CreatureGait::RUN, 1.03f), 20));
	REQUIRE(got.size() == 20);
	CHECK((got[0].flags & Protocol::kOwnGait) != 0);
	CHECK(got[0].gait == CreatureGait::RUN);

	DogGraph graph;
	Copy     copy;
	auto     now = t0;
	for (const auto& state : got) {
		Frame(graph, copy, state, now += FRAME);
		CHECK(graph.moving);
		CHECK(graph.state == CreatureGait::RUN);  // from the first frame: entered in the run, no walk first
	}
	CHECK(Near(graph.Rate(), 1.03f));
	CHECK(graph.gaitEvents == 1);  // the one sent as it set off, not one a frame
}

TEST("creature gait: before, the joiner's copy of a running dog walked: nothing told its graph the gait")
{
	// The copy as it was driven: only "moveStart"/"moveStop" and Speed, which this graph does not go by.
	const auto got = ThroughServer(Repeat(Owned(458.0f, CreatureGait::RUN, 1.03f), 20));
	DogGraph   graph;
	bool       moving = false;
	auto       resync = MoveSync::Clock::time_point{};
	auto       now = t0;
	for (const auto& state : got) {
		const auto step = MoveSync::Drive(moving, resync, state.speed, now += FRAME, [&](MoveSync::Event a_event) {
			return graph.Notify(a_event == MoveSync::Event::kStart ? "moveStart" : "moveStop");
		});
		moving = step.moving;
		resync = step.nextResync;
	}
	CHECK(graph.moving);
	CHECK(graph.state == CreatureGait::WALK);
	// Carried along at the run's pace while its legs walk at the walk's: about five times too slow.
	CHECK(458.0f / (CreatureGait::Find(CreatureGait::WALK)->pace * graph.Rate()) > 4.5f);
}

TEST("creature gait: the copy follows the owner's dog from a walk to a trot, a fast trot, a run and back, one event a change")
{
	std::vector<Protocol::ActorState> sent;
	const struct
	{
		float speed;
		int   gait;
		float rate;
	} legs[] = {
		{ 96.0f, CreatureGait::WALK, 1.01f },
		{ 230.0f, CreatureGait::TROT, 1.05f },
		{ 377.0f, CreatureGait::TROT_FAST, 1.0f },
		{ 470.0f, CreatureGait::RUN, 1.03f },
		{ 94.0f, CreatureGait::WALK, 0.99f },
	};
	for (const auto& leg : legs) {
		const auto part = Repeat(Owned(leg.speed, leg.gait, leg.rate), 8);
		sent.insert(sent.end(), part.begin(), part.end());
	}
	const auto got = ThroughServer(sent);
	REQUIRE(got.size() == sent.size());

	DogGraph graph;
	Copy     copy;
	auto     now = t0;
	for (std::size_t i = 0; i < got.size(); ++i) {
		Frame(graph, copy, got[i], now += FRAME);
		const auto& leg = legs[i / 8];
		CHECK(graph.state == leg.gait);  // on the state that says so, not later
		CHECK(Near(graph.Rate(), leg.rate));
	}
	CHECK(graph.gaitEvents == 5);  // setting off, then the four changes
	CHECK(graph.restarts == 1);    // only the one as it set off, into the state moveStart had just entered

	// The owner's dog stops: the copy stands, and sets off again straight into the gait the owner has.
	for (const auto& state : ThroughServer(Repeat(Owned(0.0f, CreatureGait::WALK, 0.0f), 4))) {
		Frame(graph, copy, state, now += FRAME);
	}
	CHECK(!graph.moving);
	for (const auto& state : ThroughServer(Repeat(Owned(458.0f, CreatureGait::RUN, 1.0f), 4))) {
		Frame(graph, copy, state, now += FRAME);
		CHECK(graph.state == CreatureGait::RUN);
	}
	CHECK(graph.moving);
}

TEST("creature gait: a dog its owner backs up backs up, at the walk's rate variable")
{
	const auto got = ThroughServer(Repeat(Owned(70.0f, CreatureGait::WALK_BACK, 0.92f), 10));
	DogGraph   graph;
	Copy       copy;
	auto       now = t0;
	for (const auto& state : got) {
		Frame(graph, copy, state, now += FRAME);
	}
	CHECK(graph.state == CreatureGait::WALK_BACK);
	CHECK(Near(graph.rates["fWalkClipMult"], 0.92f));
}

TEST("creature gait: a graph put back in its walk mid-run (a stagger, a landing) is told to run again")
{
	const auto got = ThroughServer(Repeat(Owned(458.0f, CreatureGait::RUN, 1.0f), 40));
	DogGraph   graph;
	Copy       copy;
	auto       now = t0;
	for (std::size_t i = 0; i < 10; ++i) {
		Frame(graph, copy, got[i], now += FRAME);
	}
	REQUIRE(graph.state == CreatureGait::RUN);
	// Standing in its walk again, and its iSyncLocomotionRangeID with it.
	graph.moving = false;
	graph.state = CreatureGait::WALK;
	graph.sync = CreatureGait::WALK;
	for (std::size_t i = 10; i < got.size(); ++i) {
		Frame(graph, copy, got[i], now += FRAME);
	}
	CHECK(graph.moving);
	CHECK(graph.state == CreatureGait::RUN);
}

TEST("creature gait: a state without the owner's gait goes by the speed, and the rates stay sane")
{
	// A state from a dog whose graph its owner could not read: the nearest clip's pace, forward.
	CHECK(CreatureGait::Choose(false, 0, 0, 458.0f).range->id == CreatureGait::RUN);
	CHECK(CreatureGait::Choose(false, 0, 0, 95.0f).range->id == CreatureGait::WALK);
	CHECK(CreatureGait::Choose(false, 0, 0, 225.0f).range->id == CreatureGait::TROT);
	CHECK(CreatureGait::Choose(false, 0, 0, 360.0f).range->id == CreatureGait::TROT_FAST);
	CHECK(CreatureGait::Choose(false, 0, 0, 0.0f).range->id == CreatureGait::WALK);
	CHECK(Near(CreatureGait::Choose(false, 0, 0, 458.0f).rate, 1.0f));
	// The owner's gait goes before the speed, which may lag it.
	CHECK(CreatureGait::Choose(true, CreatureGait::RUN, CreatureGait::PackRate(1.0f), 95.0f).range->id == CreatureGait::RUN);
	// A gait the dog graph has no state for, or no rate yet: the speed's.
	CHECK(CreatureGait::Choose(true, 7, CreatureGait::PackRate(1.0f), 458.0f).range->id == CreatureGait::RUN);
	CHECK(Near(CreatureGait::Choose(true, CreatureGait::RUN, 0, 229.0f).rate, 0.5f));
	// Never a frozen clip, nor a blur.
	CHECK(CreatureGait::Choose(true, CreatureGait::RUN, CreatureGait::PackRate(0.01f), 458.0f).rate == CreatureGait::MIN_RATE);
	CHECK(CreatureGait::Choose(false, 0, 0, 5000.0f).rate == CreatureGait::MAX_RATE);

	CHECK(CreatureGait::PackRate(1.0f) == 64);
	CHECK(CreatureGait::UnpackRate(CreatureGait::PackRate(1.03f)) > 1.02f);
	CHECK(CreatureGait::UnpackRate(CreatureGait::PackRate(1.03f)) < 1.04f);
	CHECK(CreatureGait::PackRate(-1.0f) == 0);
	CHECK(CreatureGait::PackRate(std::nanf("")) == 0);
	CHECK(CreatureGait::PackRate(100.0f) == 255);

	CHECK(!CreatureGait::Send(false, true, CreatureGait::RUN, CreatureGait::WALK, false));
	CHECK(!CreatureGait::Send(true, true, CreatureGait::RUN, CreatureGait::RUN, false));
	CHECK(CreatureGait::Send(true, true, CreatureGait::RUN, CreatureGait::RUN, true));
	CHECK(CreatureGait::Send(true, false, CreatureGait::RUN, CreatureGait::RUN, false));
	CHECK(CreatureGait::Find(4) == nullptr);
	CHECK(std::strcmp(CreatureGait::Find(CreatureGait::RUN)->event, "ToRunForward") == 0);
}
