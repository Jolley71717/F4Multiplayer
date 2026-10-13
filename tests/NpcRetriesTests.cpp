// Fix-ups tried on an NPC a friend's game runs, when its owner's states say it is needed: switching on a copy that is
// off here, bringing one in from a cell we have not loaded (NpcSync::SwitchOn, BringIn). Each is a console command,
// and the states come many times a second; one that never takes used to be tried forever. The tests play the owner
// against a real server and feed what it relays into the other games' budgets, as NpcSync does.

#include "ServerFixture.h"

#include "game/RetryBudget.h"

#include <unordered_map>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using RetryBudget::Clock;
	using RetryBudget::Verdict;

	constexpr std::uint32_t MOLE_RAT = 0x0001F201;
	constexpr std::uint32_t MOLE_RAT2 = 0x0001F202;
	constexpr std::uint32_t WORLD = 0x0000003C;
	constexpr auto          SWITCH_ON_RETRY = 5s;  // NpcSync's switchOns
	constexpr auto          BRING_IN_RETRY = 10s;  // and bringIns

	void Sync(Fixture& a_f, PeerId a_peer)
	{
		static std::uint32_t token = 0;
		a_f.fake->Deliver(a_peer, Protocol::Encode(Protocol::Heartbeat{ ++token }, MT::kHeartbeat));
		REQUIRE(a_f.fake->WaitFor(a_peer, Type(MT::kHeartbeatAck)));
	}

	// The NPC states the server relayed to a_peer so far.
	std::vector<Protocol::ActorState> Relayed(Fixture& a_f, PeerId a_peer)
	{
		Sync(a_f, a_peer);
		std::vector<Protocol::ActorState> out;
		for (const auto& packet : a_f.fake->Take(a_peer)) {
			if (!packet.empty() && packet[0] == Type(MT::kActorStatesRelay)) {
				const auto batch = Protocol::DecodeActorStates(packet);
				REQUIRE(batch);
				out.insert(out.end(), batch->begin(), batch->end());
			}
		}
		return out;
	}

	void Claim(Fixture& a_f, PeerId a_peer, std::uint32_t a_ref, Protocol::ClaimReason a_reason = Protocol::ClaimReason::kUnowned)
	{
		a_f.fake->Deliver(a_peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { a_ref, a_reason } }));
		Sync(a_f, a_peer);
	}

	// A game sends a batch of states for a_refs, as NpcSync::SendOwnedStates does each tick.
	void SendStates(Fixture& a_f, PeerId a_peer, std::initializer_list<std::uint32_t> a_refs, int a_times = 1)
	{
		std::vector<Protocol::ActorState> states;
		for (const auto ref : a_refs) {
			Protocol::ActorState state;
			state.refId = ref;
			state.worldspace = WORLD;
			state.x = 100.0f;
			states.push_back(state);
		}
		for (int i = 0; i < a_times; ++i) {
			a_f.fake->Deliver(a_peer, Protocol::Encode(states, MT::kActorStates));
		}
	}

	// One NPC as a game that copies it holds it: off (or out of reach) until a fix-up takes. A stuck one (a quest
	// keeps it off in this game, a cell it can't be moved from) never takes.
	struct Copy
	{
		bool needsFixing = true;
		bool stuck = false;
	};

	// One game copying the owner's NPCs: what NpcSync::SwitchOn and BringIn do with each relayed state.
	struct Game
	{
		RetryBudget::Budget                     budget;
		std::unordered_map<std::uint32_t, Copy> copies;
		std::uint32_t                           commands = 0;  // fix-ups run
		std::uint32_t                           logLines = 0;

		explicit Game(Clock::duration a_interval) :
			budget(a_interval)
		{}

		void Apply(const Protocol::ActorState& a_state, Clock::time_point a_now)
		{
			auto& copy = copies[a_state.refId];
			if (!copy.needsFixing) {
				budget.Forget(a_state.refId);  // it is on (or here): a later need starts afresh
				return;
			}
			const auto verdict = budget.Ask(a_state.refId, a_now);
			if (!RetryBudget::Go(verdict)) {
				return;
			}
			++commands;
			if (verdict == Verdict::kFirst || verdict == Verdict::kLast) {
				++logLines;
			}
			if (!copy.stuck) {
				copy.needsFixing = false;
			}
		}

		// Applies states received together as if they had arrived a_spacing apart from a_start; returns when the
		// last one would have.
		Clock::time_point ApplyAll(const std::vector<Protocol::ActorState>& a_states, Clock::time_point a_start, Clock::duration a_spacing)
		{
			auto now = a_start;
			for (const auto& state : a_states) {
				Apply(state, now);
				now += a_spacing;
			}
			return now;
		}
	};
}

TEST("npc retries: a mole rat a quest keeps off in our game is switched on three times at most, however long its owner runs it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Claim(f, 1, MOLE_RAT);

	// Alice's game runs the mole rat for a minute and a half; Bob's copy is held off by his quest.
	SendStates(f, 1, { MOLE_RAT }, 90);
	const auto states = Relayed(f, 2);
	CHECK(states.size() == 90);

	Game bob{ SWITCH_ON_RETRY };
	bob.copies[MOLE_RAT].stuck = true;
	bob.ApplyAll(states, Clock::now(), 1s);
	CHECK(bob.commands == RetryBudget::TRIES);
	CHECK(bob.logLines == 2);  // the first try and the last, not one a try
	CHECK(bob.budget.GaveUp() == 1);
}

TEST("npc retries: a burst of states switches a copy on once, not once a state")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Claim(f, 1, MOLE_RAT);
	SendStates(f, 1, { MOLE_RAT }, 40);
	const auto states = Relayed(f, 2);
	REQUIRE(states.size() == 40);

	// Two seconds of states at 20 a second: inside one retry interval.
	Game bob{ SWITCH_ON_RETRY };
	bob.copies[MOLE_RAT].stuck = true;
	bob.ApplyAll(states, Clock::now(), 50ms);
	CHECK(bob.commands == 1);
}

TEST("npc retries: a copy that comes on with the first try is left alone, and an ambush later in the session gets fresh tries")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Claim(f, 1, MOLE_RAT);

	SendStates(f, 1, { MOLE_RAT }, 30);
	Game       bob{ SWITCH_ON_RETRY };
	const auto now = bob.ApplyAll(Relayed(f, 2), Clock::now(), 1s);
	CHECK(bob.commands == 1);
	CHECK(!bob.copies[MOLE_RAT].needsFixing);

	// A script in Bob's game switches it off again for a later ambush, and this time it stays off: the try it used
	// before doesn't count against it.
	bob.copies[MOLE_RAT] = { .needsFixing = true, .stuck = true };
	SendStates(f, 1, { MOLE_RAT }, 30);
	bob.ApplyAll(Relayed(f, 2), now + 1s, 1s);
	CHECK(bob.commands == 1 + RetryBudget::TRIES);
}

TEST("npc retries: someone who doesn't run an NPC can't make the others switch it on or move it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Carol")));
	Claim(f, 1, MOLE_RAT);
	Relayed(f, 3);

	// Bob sends states for Alice's mole rat and for one nobody runs: neither reaches Carol.
	SendStates(f, 2, { MOLE_RAT, MOLE_RAT2 }, 10);
	Game carol{ SWITCH_ON_RETRY };
	carol.ApplyAll(Relayed(f, 3), Clock::now(), 1s);
	CHECK(carol.commands == 0);

	// Alice's do, and only for hers.
	SendStates(f, 1, { MOLE_RAT, MOLE_RAT2 });
	const auto fromAlice = Relayed(f, 3);
	REQUIRE(fromAlice.size() == 1);
	CHECK(fromAlice[0].refId == MOLE_RAT);
}

TEST("npc retries: a friend's companion stuck where we can't move it is brought in three times, and again once it has been here")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Claim(f, 1, MOLE_RAT, Protocol::ClaimReason::kCompanion);

	SendStates(f, 1, { MOLE_RAT }, 60);
	Game bob{ BRING_IN_RETRY };
	bob.copies[MOLE_RAT].stuck = true;
	auto now = bob.ApplyAll(Relayed(f, 2), Clock::now(), 1s);
	CHECK(bob.commands == RetryBudget::TRIES);

	// Bob walks over: the cell loads and the companion is here after all. When it is away again it can be brought in.
	bob.copies[MOLE_RAT].needsFixing = false;
	SendStates(f, 1, { MOLE_RAT });
	now = bob.ApplyAll(Relayed(f, 2), now, 1s);
	bob.copies[MOLE_RAT] = { .needsFixing = true, .stuck = false };
	SendStates(f, 1, { MOLE_RAT });
	bob.ApplyAll(Relayed(f, 2), now, 1s);
	CHECK(bob.commands == RetryBudget::TRIES + 1);
	CHECK(!bob.copies[MOLE_RAT].needsFixing);
}

TEST("retry budget: three tries an interval apart, then nothing, until forgotten or cleared")
{
	RetryBudget::Budget budget{ 5s };
	const auto          t0 = Clock::now();
	CHECK(budget.Ask(1, t0) == Verdict::kFirst);
	CHECK(budget.Ask(1, t0 + 4s) == Verdict::kWait);
	CHECK(budget.Ask(1, t0 + 5s) == Verdict::kAgain);
	CHECK(budget.Ask(1, t0 + 10s) == Verdict::kLast);
	CHECK(budget.Ask(1, t0 + 1h) == Verdict::kWait);
	CHECK(budget.Ask(2, t0 + 1h) == Verdict::kFirst);  // each NPC its own
	CHECK(budget.GaveUp() == 1);

	budget.Forget(1);
	CHECK(budget.Ask(1, t0 + 1h) == Verdict::kFirst);
	budget.Clear();
	CHECK(budget.Ask(2, t0 + 1h) == Verdict::kFirst);
	CHECK(!RetryBudget::Go(Verdict::kWait));
	CHECK(RetryBudget::Go(Verdict::kLast));
}

TEST("retry budget: past the NPCs it remembers, it starts over rather than growing")
{
	RetryBudget::Budget budget{ 5s };
	const auto          t0 = Clock::now();
	for (std::uint32_t id = 1; id <= RetryBudget::MAX_TRACKED; ++id) {
		CHECK(budget.Ask(id, t0) == Verdict::kFirst);
	}
	CHECK(budget.Ask(1, t0) == Verdict::kWait);
	CHECK(budget.Ask(RetryBudget::MAX_TRACKED + 1, t0) == Verdict::kFirst);  // the guard emptied it
	CHECK(budget.Ask(1, t0) == Verdict::kFirst);
}
