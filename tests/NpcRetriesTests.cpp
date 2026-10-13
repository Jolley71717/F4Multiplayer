// Fix-ups tried on an NPC a friend's game runs, when its owner's states say it is needed: switching on a copy that is
// off here, bringing one in from a cell we have not loaded (NpcSync::SwitchOn, BringIn). Each is a console command,
// and the states come many times a second; one that never takes used to be tried forever. The tests play the owner
// against a real server and feed what it relays into the other games' budgets through RetryBudget::Plan, the decision
// NpcSync::ApplyStates makes for each state; what the copy looks like in the game (RetryBudget::Copy) is modelled.

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

	// One NPC as a game that copies it holds it (what NpcSync::CopyFor reads from the game). A stuck one never takes a
	// fix-up: a quest keeps it off in this game, or it is in a cell it can't be moved from.
	struct Held
	{
		RetryBudget::Copy copy;
		bool              stuck = false;
	};

	// Off here.
	Held Off(bool a_stuck) { return { .copy = { .exists = true, .off = true }, .stuck = a_stuck }; }
	// On, in a cell we have not loaded, while the owner's copy stands near us.
	Held Away(bool a_stuck) { return { .copy = { .exists = true, .movable = true }, .stuck = a_stuck }; }
	// On and here, where the owner's copy is.
	Held Here() { return { .copy = { .exists = true, .here = true } }; }

	// One game copying the owner's NPCs: RetryBudget::Plan for each relayed state, as NpcSync::ApplyStates does, and
	// the fix-ups it says to run (SwitchOn, BringIn), which take unless the copy is stuck.
	struct Game
	{
		RetryBudget::Budget                     switchOns{ 5s };  // as NpcSync's
		RetryBudget::Budget                     bringIns{ 10s };
		std::unordered_map<std::uint32_t, Held> npcs;
		std::uint32_t                           commands = 0;  // fix-ups run
		std::uint32_t                           logLines = 0;

		void Apply(const Protocol::ActorState& a_state, Clock::time_point a_now)
		{
			auto&      held = npcs[a_state.refId];
			const auto fixes = RetryBudget::Plan(switchOns, bringIns, a_state.refId, held.copy, a_now);
			for (const auto verdict : { fixes.switchOn, fixes.bringIn }) {
				if (RetryBudget::Go(verdict)) {
					++commands;
					logLines += verdict == Verdict::kFirst || verdict == Verdict::kLast ? 1 : 0;
				}
			}
			if (held.stuck) {
				return;
			}
			if (RetryBudget::Go(fixes.switchOn)) {
				held.copy.off = false;
			}
			if (RetryBudget::Go(fixes.bringIn)) {
				held.copy.here = true;
				held.copy.movable = false;
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

	Game bob;
	bob.npcs[MOLE_RAT] = Off(true);
	bob.ApplyAll(states, Clock::now(), 1s);
	CHECK(bob.commands == RetryBudget::TRIES);
	CHECK(bob.logLines == 2);  // the first try and the last, not one a try
	CHECK(bob.switchOns.GaveUp() == 1);
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
	Game bob;
	bob.npcs[MOLE_RAT] = Off(true);
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
	Game       bob;
	bob.npcs[MOLE_RAT] = Off(false);
	const auto now = bob.ApplyAll(Relayed(f, 2), Clock::now(), 1s);
	CHECK(bob.commands == 1);
	CHECK(!bob.npcs[MOLE_RAT].copy.off);

	// A script in Bob's game switches it off again for a later ambush, and this time it stays off: the try it used
	// before doesn't count against it.
	bob.npcs[MOLE_RAT] = Off(true);
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
	Game carol;
	carol.npcs[MOLE_RAT] = Off(false);
	carol.npcs[MOLE_RAT2] = Away(false);
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
	Game bob;
	bob.npcs[MOLE_RAT] = Away(true);
	auto now = bob.ApplyAll(Relayed(f, 2), Clock::now(), 1s);
	CHECK(bob.commands == RetryBudget::TRIES);

	// Bob walks over: the cell loads and the companion is here after all. When it is away again it can be brought in.
	bob.npcs[MOLE_RAT] = Here();
	SendStates(f, 1, { MOLE_RAT });
	now = bob.ApplyAll(Relayed(f, 2), now, 1s);
	bob.npcs[MOLE_RAT] = Away(false);
	SendStates(f, 1, { MOLE_RAT });
	bob.ApplyAll(Relayed(f, 2), now, 1s);
	CHECK(bob.commands == RetryBudget::TRIES + 1);
	CHECK(bob.npcs[MOLE_RAT].copy.here);
	CHECK(bob.bringIns.GaveUp() == 1);
	CHECK(bob.switchOns.GaveUp() == 0);
}

TEST("npc retries: a copy that is dead here is never switched on, however long its owner's game runs it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Alice")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bob")));
	Claim(f, 1, MOLE_RAT);
	SendStates(f, 1, { MOLE_RAT }, 30);

	// Bob killed his copy, and the corpse was switched off (a script cleaned it up): it stays as it is.
	Game bob;
	bob.npcs[MOLE_RAT] = { .copy = { .exists = true, .off = true, .unusable = true } };
	bob.ApplyAll(Relayed(f, 2), Clock::now(), 1s);
	CHECK(bob.commands == 0);
	CHECK(bob.switchOns.GaveUp() == 0);
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
