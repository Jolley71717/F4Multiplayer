// Where our copy of an NPC a friend's game runs stands, against where their game has it: the host finished Concord,
// so its Preston, Sturges and Jun stand in Sanctuary, while a joiner who has not still has theirs in the Museum of
// Freedom with their 3D loaded. The joiner's copies are moved to where the host's stand (NpcSync's BringIn) and
// mirrored from there, but never out of a cell the joiner stands in with them, nor while the joiner's game is using
// them. The tests play the host against a real server and feed what it sends the joiner (owners, the host's
// position, the NPCs' states) through NpcPlace::Decide, RetryBudget::Plan and NpcPlace::Follows, the decisions
// NpcSync::ApplyStates and UpdateMirrors make; what the joiner's copies look like in the game is modelled.

#include "ServerFixture.h"

#include "game/NpcPlace.h"

#include <map>
#include <set>
#include <thread>

using namespace TestServer;
using namespace std::chrono_literals;

namespace
{
	using Packets = std::vector<std::vector<std::uint8_t>>;
	using NpcPlace::Spot;
	using NpcPlace::Why;
	using RetryBudget::Clock;

	constexpr std::uint32_t WORLD = 0x0000003C;    // the Commonwealth
	constexpr std::uint32_t MUSEUM = 0x0001F3A1;   // an interior: the Museum of Freedom
	constexpr std::uint32_t PRESTON = 0x0001A4D7;
	constexpr std::uint32_t STURGES = 0x0001A4D8;
	constexpr std::uint32_t JUN = 0x0001A4D9;

	const Spot SANCTUARY{ 0, WORLD, -80000.0f, 90000.0f, 0.0f };  // where the host stands
	const Spot IN_THE_MUSEUM{ MUSEUM, 0, 400.0f, -1200.0f, 0.0f };
	const Spot ACROSS_THE_MAP{ 0, WORLD, 60000.0f, -80000.0f, 0.0f };

	Spot Off(const Spot& a_at, float a_dx, float a_dy = 0.0f)
	{
		return { a_at.cell, a_at.worldspace, a_at.x + a_dx, a_at.y + a_dy, a_at.z };
	}

	// A copy loaded at a_at in the joiner's game, on and alive.
	NpcPlace::Ours LoadedAt(const Spot& a_at)
	{
		return { .exists = true, .loaded = true, .at = a_at };
	}

	std::vector<std::uint8_t> StateOf(const Spot& a_at)
	{
		static std::uint32_t sequence = 0;
		Protocol::PlayerState state;
		state.sequence = ++sequence;
		state.cell = a_at.cell;
		state.worldspace = a_at.worldspace;
		state.x = a_at.x, state.y = a_at.y, state.z = a_at.z;
		return Protocol::Encode(state);
	}

	// The host's game reports where it has these NPCs, as NpcSync::SendOwnedStates does each tick.
	std::vector<std::uint8_t> NpcsAt(const std::vector<std::pair<std::uint32_t, Spot>>& a_npcs)
	{
		std::vector<Protocol::ActorState> states;
		for (const auto& [ref, at] : a_npcs) {
			Protocol::ActorState state;
			state.refId = ref;
			state.cell = at.cell;
			state.worldspace = at.worldspace;
			state.x = at.x, state.y = at.y, state.z = at.z;
			states.push_back(state);
		}
		return Protocol::Encode(states, MT::kActorStates);
	}

	// The joiner's NpcSync, as far as where its copies stand goes: who runs each NPC and where each other player is
	// (as the server says), where our player and our copies are, and for each NPC state the server passes on, the
	// fix-ups RetryBudget::Plan says to run on what NpcPlace::Decide makes of it (SwitchOn, BringIn: these take unless
	// the copy is stuck) and whether UpdateMirrors then mirrors it.
	struct Joiner
	{
		std::map<std::uint32_t, std::uint32_t>         owners;
		std::map<std::uint32_t, Protocol::PlayerState> lastStates;
		Spot                                           player;
		std::map<std::uint32_t, NpcPlace::Ours>        npcs;
		std::map<std::uint32_t, FoundDead::Npc>        story;  // characters of our own story (WorldSync::DescribeStory)
		bool                                           sharedStory = false;
		std::set<std::uint32_t>                        stuck;  // their fix-ups never take
		RetryBudget::Budget                            switchOns{ 5s };  // as NpcSync's
		RetryBudget::Budget                            bringIns{ 10s };
		std::uint32_t                                  switches = 0, moves = 0;
		std::map<std::uint32_t, Why>                   why;       // the last decision for each NPC
		std::map<std::uint32_t, bool>                  mirrored;  // whether UpdateMirrors puppets ours after it
		std::size_t                                    handled = 0;  // NPC states
		Clock::time_point                              now = Clock::now();
		Clock::duration                                spacing = 50ms;  // between the states, as they would arrive

		void Handle(const Packets& a_packets)
		{
			for (const auto& packet : a_packets) {
				if (packet[0] == Type(MT::kActorOwners)) {
					const auto batch = Protocol::DecodeOwners(packet);
					REQUIRE(batch);
					for (const auto& entry : *batch) {
						if (entry.playerId == 0) {
							owners.erase(entry.refId);
						} else {
							owners[entry.refId] = entry.playerId;
						}
					}
				} else if (packet[0] == Type(MT::kPlayerStates)) {
					const auto batch = Protocol::DecodePlayerStates(packet);
					REQUIRE(batch);
					for (const auto& entry : batch->players) {
						lastStates[entry.playerId] = entry.state;
					}
				} else if (packet[0] == Type(MT::kActorStatesRelay)) {
					const auto states = Protocol::DecodeActorStates(packet);
					REQUIRE(states);
					for (const auto& state : *states) {
						Apply(state);
					}
				}
			}
		}

		void Apply(const Protocol::ActorState& a_state)
		{
			++handled;
			const auto owner = owners.find(a_state.refId);
			if (owner == owners.end()) {
				return;  // nobody runs it, as far as we have heard
			}
			const auto lastState = [this](std::uint32_t a_player) -> std::optional<Protocol::PlayerState> {
				const auto it = lastStates.find(a_player);
				return it == lastStates.end() ? std::nullopt : std::optional{ it->second };
			};
			auto&      ours = npcs[a_state.refId];
			const auto theirs = RemoteEdits::SpotOf(a_state);
			const auto decided = NpcPlace::Decide(theirs, ours, player, RemoteEdits::OwnerBeside(lastState, owner->second, a_state));
			why[a_state.refId] = decided;
			auto copy = NpcPlace::CopyOf(ours, decided);
			if (copy.off && !copy.unusable && !RemoteEdits::MaySwitchOn(sharedStory, story[a_state.refId])) {
				copy.unusable = true;  // a character of our own story stays off (NpcSync's CopyFor)
			}
			const auto fixes = RetryBudget::Plan(switchOns, bringIns, a_state.refId, copy, now);
			const bool takes = !stuck.contains(a_state.refId);
			if (RetryBudget::Go(fixes.switchOn)) {
				++switches;
				ours.off = ours.off && !takes;
			}
			if (RetryBudget::Go(fixes.bringIn)) {
				++moves;
				if (takes) {
					ours.at = theirs;  // moveto player, then setpos to where theirs stands
					ours.loaded = true;
				}
			}
			// UpdateMirrors: talking to us, holding us in a scene or our companion, ours runs its own AI; else it follows
			// theirs when near.
			mirrored[a_state.refId] = ours.exists && ours.loaded && !ours.off && !ours.unusable && !NpcPlace::StaysWithUs(ours) &&
			                          NpcPlace::Follows(ours.at, theirs, 0s);
			now += spacing;
		}
	};

	// Hands what the server sends a_peer to the joiner, in order, until a_done() or half a minute passes.
	template <class Done>
	void Pump(Fixture& a_f, PeerId a_peer, Joiner& a_game, Done&& a_done)
	{
		const auto deadline = std::chrono::steady_clock::now() + 30s;
		while (!a_done() && std::chrono::steady_clock::now() < deadline) {
			a_game.Handle(a_f.fake->Take(a_peer));
			std::this_thread::sleep_for(10ms);
		}
		REQUIRE(a_done());
	}

	// The host (peer 1) and a joiner (peer 2) whose game is modelled.
	struct Session
	{
		Fixture       f;
		std::uint32_t host = 0;
		Joiner        joiner;

		Session()
		{
			host = JoinAs(f, 1, "Host");
			JoinAs(f, 2, "Joiner");
		}

		// The host's game runs these NPCs.
		void HostRuns(std::initializer_list<std::uint32_t> a_refs)
		{
			std::vector<Protocol::ActorClaim> claims;
			for (const auto ref : a_refs) {
				claims.push_back({ ref, Protocol::ClaimReason::kUnowned });
			}
			f.fake->Deliver(1, Protocol::Encode(claims));
			Pump(f, 2, joiner, [&] { return std::ranges::all_of(a_refs, [&](std::uint32_t a_ref) { return joiner.owners.contains(a_ref); }); });
		}

		void HostStands(const Spot& a_at)
		{
			f.fake->Deliver(1, StateOf(a_at));
			Pump(f, 2, joiner, [&] {
				const auto it = joiner.lastStates.find(host);
				return it != joiner.lastStates.end() && it->second.x == a_at.x && it->second.y == a_at.y && it->second.cell == a_at.cell;
			});
		}

		// The host's game reports its NPCs a_times; returns once the joiner has handled them all.
		void HostReports(const std::vector<std::pair<std::uint32_t, Spot>>& a_npcs, int a_times)
		{
			const auto until = joiner.handled + a_npcs.size() * static_cast<std::size_t>(a_times);
			for (int i = 0; i < a_times; ++i) {
				f.fake->Deliver(1, NpcsAt(a_npcs));
			}
			Pump(f, 2, joiner, [&] { return joiner.handled >= until; });
		}
	};

	void CheckAt(const NpcPlace::Ours& a_ours, const Spot& a_at)
	{
		CHECK(a_ours.at.cell == a_at.cell);
		CHECK(a_ours.at.worldspace == a_at.worldspace);
		CHECK(a_ours.at.x == a_at.x);
		CHECK(a_ours.at.y == a_at.y);
	}
}

TEST("npc place: the host's Preston, Sturges and Jun in Sanctuary are brought out of the joiner's museum once each, and mirrored beside the host")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(SANCTUARY, 1500.0f);  // the joiner walks up to the host
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	joiner.npcs[STURGES] = LoadedAt(Off(IN_THE_MUSEUM, 200.0f));
	joiner.npcs[JUN] = LoadedAt(Off(IN_THE_MUSEUM, -200.0f));
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON, STURGES, JUN });

	// Three seconds of the host's states, 20 a second: each copy is moved once, not once a state.
	const std::vector<std::pair<std::uint32_t, Spot>> beside{ { PRESTON, Off(SANCTUARY, 300.0f) }, { STURGES, Off(SANCTUARY, -300.0f) }, { JUN, Off(SANCTUARY, 0.0f, 400.0f) } };
	s.HostReports(beside, 20);
	CHECK(joiner.moves == 3);
	for (const auto& [ref, at] : beside) {
		CheckAt(joiner.npcs[ref], at);
		CHECK(joiner.why[ref] == Why::kHere);
		CHECK(joiner.mirrored[ref]);
	}
	CHECK(joiner.bringIns.GaveUp() == 0);
	CHECK(joiner.switches == 0);
}

TEST("npc place: a joiner inside the museum with their own Preston keeps him there, talking to him or not; out in Sanctuary beside the host's, he is brought over")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(IN_THE_MUSEUM, 150.0f);
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	joiner.npcs[PRESTON].talkingToUs = true;
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON });

	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 20);
	CHECK(joiner.moves == 0);
	CHECK(joiner.why[PRESTON] == Why::kNotNearUs);
	CHECK(!joiner.mirrored[PRESTON]);  // ours runs its own AI and talks to us
	CheckAt(joiner.npcs[PRESTON], IN_THE_MUSEUM);

	// The conversation ends; the joiner is still in the museum with him.
	joiner.npcs[PRESTON].talkingToUs = false;
	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 20);
	CHECK(joiner.moves == 0);
	CHECK(!joiner.mirrored[PRESTON]);

	// The joiner fast travels to Sanctuary: the host's Preston stands right there, so theirs is brought over.
	joiner.player = Off(SANCTUARY, 1000.0f);
	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 20);
	CHECK(joiner.moves == 1);
	CheckAt(joiner.npcs[PRESTON], Off(SANCTUARY, 300.0f));
	CHECK(joiner.mirrored[PRESTON]);
}

TEST("npc place: a copy the joiner's own game is using is left where it is: in a scene there, the joiner's companion, or fighting them")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(SANCTUARY, 800.0f);
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	joiner.npcs[PRESTON].inScene = true;
	joiner.npcs[STURGES] = LoadedAt(IN_THE_MUSEUM);
	joiner.npcs[STURGES].companion = true;
	joiner.npcs[JUN] = LoadedAt(IN_THE_MUSEUM);
	joiner.npcs[JUN].fightingUs = true;
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON, STURGES, JUN });

	const std::vector<std::pair<std::uint32_t, Spot>> beside{ { PRESTON, Off(SANCTUARY, 300.0f) }, { STURGES, Off(SANCTUARY, -300.0f) }, { JUN, Off(SANCTUARY, 0.0f, 400.0f) } };
	s.HostReports(beside, 20);
	CHECK(joiner.moves == 0);
	for (const auto& [ref, at] : beside) {
		CHECK(joiner.why[ref] == Why::kBusy);
		CHECK(!joiner.mirrored[ref]);
		CheckAt(joiner.npcs[ref], IN_THE_MUSEUM);
	}

	// Preston's scene ends: now he is brought over. The others stay as they are.
	joiner.npcs[PRESTON].inScene = false;
	s.HostReports(beside, 20);
	CHECK(joiner.moves == 1);
	CHECK(joiner.mirrored[PRESTON]);
	CHECK(!joiner.mirrored[STURGES]);
	CHECK(joiner.why[JUN] == Why::kBusy);
}

TEST("npc place: a story mod's NPC another game runs is not mirrored here while its scene holds our player (no weapons or Pip-Boy until it ends), and is once the scene lets go")
{
	// A story mod's character (a load order of the mod's: 09000004), in its opening scene with our player. A friend
	// joining, or talking to the same NPC in their game, gets their game to run it; mirrored here, its AI would be off,
	// its scene would never end, and the engine would keep our player in it.
	constexpr std::uint32_t DANIEL = 0x09000004;
	Session s;
	auto&   game = s.joiner;
	game.player = Off(SANCTUARY, 100.0f);
	game.npcs[DANIEL] = LoadedAt(Off(SANCTUARY, 250.0f));
	game.npcs[DANIEL].inScene = true;
	game.npcs[DANIEL].holdsUs = true;
	s.HostStands(Off(SANCTUARY, 300.0f));
	s.HostRuns({ DANIEL });

	const std::vector<std::pair<std::uint32_t, Spot>> beside{ { DANIEL, Off(SANCTUARY, 260.0f) } };
	s.HostReports(beside, 10);
	CHECK(game.why[DANIEL] == Why::kHere);  // loaded where theirs is: nothing to move
	CHECK(!game.mirrored[DANIEL]);           // but it runs its own AI, and its scene goes on
	CheckAt(game.npcs[DANIEL], Off(SANCTUARY, 250.0f));

	// Its last line said, the scene leaves only the conversation's speaker holding us: still ours.
	game.npcs[DANIEL].inScene = false;
	s.HostReports(beside, 10);
	CHECK(!game.mirrored[DANIEL]);

	// The conversation over, it is mirrored from the game that runs it.
	game.npcs[DANIEL].holdsUs = false;
	s.HostReports(beside, 10);
	CHECK(game.mirrored[DANIEL]);
	CHECK(game.moves == 0);
}

TEST("npc place: a joiner across Sanctuary from the host's Preston leaves theirs in the museum until they walk up")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(SANCTUARY, 5000.0f);
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON });

	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 10);
	CHECK(joiner.moves == 0);
	CHECK(joiner.why[PRESTON] == Why::kNotNearUs);

	joiner.player = Off(SANCTUARY, 2000.0f);
	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 10);
	CHECK(joiner.moves == 1);
	CHECK(joiner.mirrored[PRESTON]);
}

TEST("npc place: the host's game saying Preston stands beside the joiner, while the host is across the map, moves nothing")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = SANCTUARY;
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	s.HostStands(ACROSS_THE_MAP);
	s.HostRuns({ PRESTON });

	s.HostReports({ { PRESTON, Off(SANCTUARY, 200.0f) } }, 20);
	CHECK(joiner.moves == 0);
	CHECK(joiner.why[PRESTON] == Why::kOwnerAway);
	CheckAt(joiner.npcs[PRESTON], IN_THE_MUSEUM);

	// The host comes over: now it is.
	s.HostStands(Off(SANCTUARY, 100.0f));
	s.HostReports({ { PRESTON, Off(SANCTUARY, 200.0f) } }, 1);
	CHECK(joiner.moves == 1);
}

TEST("npc place: a Preston the joiner's game won't move is tried three times ten seconds apart, then left in the museum on its own AI")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(SANCTUARY, 1000.0f);
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	joiner.stuck.insert(PRESTON);
	joiner.spacing = 1s;
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON });

	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 60);  // a minute of states
	CHECK(joiner.moves == RetryBudget::TRIES);
	CHECK(joiner.bringIns.GaveUp() == 1);
	CHECK(!joiner.mirrored[PRESTON]);
	CheckAt(joiner.npcs[PRESTON], IN_THE_MUSEUM);
}

TEST("npc place: on their own story, a Preston the joiner's story keeps off stays off and unmoved; with a shared story he is switched on, then brought over")
{
	for (const bool shared : { false, true }) {
		Session s;
		auto&   joiner = s.joiner;
		joiner.sharedStory = shared;
		joiner.player = Off(SANCTUARY, 1000.0f);
		joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
		joiner.npcs[PRESTON].off = true;
		joiner.story[PRESTON] = { .unique = true, .essential = true, .questAlias = true };
		s.HostStands(SANCTUARY);
		s.HostRuns({ PRESTON });

		s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 20);
		if (shared) {
			CHECK(joiner.switches == 1);
			CHECK(joiner.moves == 1);
			CHECK(joiner.mirrored[PRESTON]);
			CheckAt(joiner.npcs[PRESTON], Off(SANCTUARY, 300.0f));
		} else {
			CHECK(joiner.switches == 0);
			CHECK(joiner.moves == 0);
			CHECK(joiner.why[PRESTON] == Why::kCannot);
			CHECK(!joiner.mirrored[PRESTON]);
			CheckAt(joiner.npcs[PRESTON], IN_THE_MUSEUM);
		}
	}
}

TEST("npc place: once brought over, Preston going indoors with the host is not pulled after him; ours runs its own AI until he comes out")
{
	Session s;
	auto&   joiner = s.joiner;
	joiner.player = Off(SANCTUARY, 1000.0f);
	joiner.npcs[PRESTON] = LoadedAt(IN_THE_MUSEUM);
	s.HostStands(SANCTUARY);
	s.HostRuns({ PRESTON });
	s.HostReports({ { PRESTON, Off(SANCTUARY, 300.0f) } }, 5);
	REQUIRE(joiner.moves == 1);

	// The host walks into a house with him.
	constexpr std::uint32_t HOUSE = 0x0001F3B2;
	s.HostStands({ HOUSE, 0, 0.0f, 0.0f, 0.0f });
	s.HostReports({ { PRESTON, { HOUSE, 0, 100.0f, 0.0f, 0.0f } } }, 20);
	CHECK(joiner.moves == 1);
	CHECK(joiner.why[PRESTON] == Why::kOwnerIndoors);
	CHECK(!joiner.mirrored[PRESTON]);

	// And back out.
	s.HostStands(SANCTUARY);
	s.HostReports({ { PRESTON, Off(SANCTUARY, 500.0f) } }, 5);
	CHECK(joiner.moves == 1);
	CHECK(joiner.why[PRESTON] == Why::kHere);
	CHECK(joiner.mirrored[PRESTON]);
}

TEST("npc place: each reason ours stays or moves, and when it is mirrored")
{
	const Spot theirs = Off(SANCTUARY, 300.0f);
	const Spot us = Off(SANCTUARY, 1000.0f);
	const auto museum = LoadedAt(IN_THE_MUSEUM);

	CHECK(NpcPlace::Decide(theirs, museum, us, true) == Why::kMove);
	CHECK(NpcPlace::Decide(theirs, LoadedAt(Off(SANCTUARY, 9000.0f)), us, true) == Why::kHere);  // far is the bring-over's case
	NpcPlace::Ours unloaded{ .exists = true, .at = Off(SANCTUARY, 9000.0f) };
	CHECK(NpcPlace::Decide(theirs, unloaded, us, true) == Why::kMove);  // in a cell we have not loaded
	CHECK(NpcPlace::Decide(theirs, museum, us, false) == Why::kOwnerAway);
	CHECK(NpcPlace::Decide(IN_THE_MUSEUM, museum, us, true) == Why::kHere);
	CHECK(NpcPlace::Decide(Off(IN_THE_MUSEUM, 0.0f), LoadedAt(SANCTUARY), us, true) == Why::kOwnerIndoors);
	CHECK(NpcPlace::Decide({ 0, 0, 0.0f, 0.0f, 0.0f }, museum, us, true) == Why::kOwnerIndoors);  // nowhere
	// Our player is in the interior it is in: ours, left in Sanctuary, is moved there.
	const Spot inside = Off(IN_THE_MUSEUM, 0.0f);
	const Spot besideUs = Off(IN_THE_MUSEUM, 200.0f);
	CHECK(NpcPlace::Decide(inside, LoadedAt(SANCTUARY), besideUs, true) == Why::kMove);
	CHECK(NpcPlace::Decide(inside, LoadedAt(SANCTUARY), besideUs, false) == Why::kOwnerAway);
	CHECK(NpcPlace::Decide(inside, LoadedAt(SANCTUARY), Off(IN_THE_MUSEUM, NpcPlace::BRING_IN_RANGE + 1.0f), true) == Why::kNotNearUs);
	CHECK(NpcPlace::Decide(inside, LoadedAt(SANCTUARY), Off({ 0x0001F3B2, 0, 0.0f, 0.0f, 0.0f }, 200.0f), true) == Why::kOwnerIndoors);  // another interior
	CHECK(NpcPlace::Decide(inside, NpcPlace::Ours{ .exists = true, .at = SANCTUARY, .companion = true }, besideUs, true) == Why::kBusy);
	CHECK(NpcPlace::Decide(theirs, museum, IN_THE_MUSEUM, true) == Why::kNotNearUs);
	CHECK(NpcPlace::Decide(theirs, museum, { 0, 0x00000099, us.x, us.y, us.z }, true) == Why::kNotNearUs);  // another world
	CHECK(NpcPlace::Decide(theirs, museum, Off(theirs, NpcPlace::BRING_IN_RANGE + 1.0f), true) == Why::kNotNearUs);
	CHECK(NpcPlace::Decide(theirs, museum, Off(theirs, NpcPlace::BRING_IN_RANGE - 1.0f), true) == Why::kMove);
	CHECK(NpcPlace::Decide(theirs, {}, us, true) == Why::kCannot);
	for (const auto change : { &NpcPlace::Ours::off, &NpcPlace::Ours::unusable }) {
		auto ours = museum;
		ours.*change = true;
		CHECK(NpcPlace::Decide(theirs, ours, us, true) == Why::kCannot);
	}
	for (const auto change : { &NpcPlace::Ours::companion, &NpcPlace::Ours::inScene, &NpcPlace::Ours::fightingUs, &NpcPlace::Ours::talkingToUs, &NpcPlace::Ours::holdsUs }) {
		auto ours = museum;
		ours.*change = true;
		CHECK(NpcPlace::Decide(theirs, ours, us, true) == Why::kBusy);
	}
	// Ours keeps its own AI, whoever runs it elsewhere, while it follows us or holds us; in a scene of its own (one our
	// player isn't in) or fighting us, it is mirrored.
	CHECK(!NpcPlace::StaysWithUs(museum));
	for (const auto change : { &NpcPlace::Ours::companion, &NpcPlace::Ours::talkingToUs, &NpcPlace::Ours::holdsUs }) {
		auto ours = museum;
		ours.*change = true;
		CHECK(NpcPlace::StaysWithUs(ours));
	}
	for (const auto change : { &NpcPlace::Ours::inScene, &NpcPlace::Ours::fightingUs }) {
		auto ours = museum;
		ours.*change = true;
		CHECK(!NpcPlace::StaysWithUs(ours));
	}

	const auto moving = NpcPlace::CopyOf(museum, Why::kMove);
	CHECK(moving.exists && moving.movable && !moving.here && !moving.off);
	const auto here = NpcPlace::CopyOf(LoadedAt(theirs), Why::kHere);
	CHECK(here.here && !here.movable);
	const auto gone = NpcPlace::CopyOf({}, Why::kCannot);
	CHECK(!gone.exists && !gone.here && !gone.movable);

	CHECK(NpcPlace::Follows(theirs, Off(theirs, 100.0f), 0s));
	CHECK(NpcPlace::Follows(theirs, Off(theirs, 100.0f), NpcPlace::LOST_AFTER));
	CHECK(!NpcPlace::Follows(theirs, Off(theirs, 100.0f), NpcPlace::LOST_AFTER + 1ms));
	CHECK(!NpcPlace::Follows(theirs, Off(theirs, NpcPlace::MAX_FOLLOW_DISTANCE + 1.0f), 0s));
	CHECK(!NpcPlace::Follows(IN_THE_MUSEUM, { 0, WORLD, IN_THE_MUSEUM.x, IN_THE_MUSEUM.y, IN_THE_MUSEUM.z }, 0s));  // same numbers, other space

	// And why not, for the log: another space first (ours runs its own AI there whatever else), then a silent owner (a
	// menu, a load, a lost connection), then one whose copy went far (a fast travel, a script).
	using NpcPlace::Lost;
	CHECK(NpcPlace::LostWhy(theirs, Off(theirs, 100.0f), NpcPlace::LOST_AFTER) == Lost::kNo);
	CHECK(NpcPlace::LostWhy(theirs, Off(theirs, 100.0f), NpcPlace::LOST_AFTER + 1ms) == Lost::kSilent);
	CHECK(NpcPlace::LostWhy(theirs, Off(theirs, NpcPlace::MAX_FOLLOW_DISTANCE + 1.0f), 0s) == Lost::kFar);
	CHECK(NpcPlace::LostWhy(theirs, Off(theirs, NpcPlace::MAX_FOLLOW_DISTANCE + 1.0f), NpcPlace::LOST_AFTER + 1ms) == Lost::kSilent);
	CHECK(NpcPlace::LostWhy(IN_THE_MUSEUM, { 0, WORLD, IN_THE_MUSEUM.x, IN_THE_MUSEUM.y, IN_THE_MUSEUM.z }, NpcPlace::LOST_AFTER + 1ms) == Lost::kElsewhere);
	CHECK(std::string_view{ NpcPlace::LostName(Lost::kNo) } == "follows");
	CHECK(std::string_view{ NpcPlace::LostName(Lost::kElsewhere) } == "elsewhere");
	CHECK(std::string_view{ NpcPlace::LostName(Lost::kSilent) } == "silent");
	CHECK(std::string_view{ NpcPlace::LostName(Lost::kFar) } == "far");
}

TEST("npc place: a copy gone silent while its owner reads the Pip-Boy is held still, not let go: out of a fight, for the menu, and not past a minute")
{
	using NpcPlace::Silent;
	using NpcPlace::WhileSilent;
	// The host opens the Pip-Boy with settlers near the joiner: their status names the menu within 2 s, the states
	// stop; after 3 s of silence (LOST_AFTER) the joiner's copies are held where the host's last states put them, for
	// the whole menu, instead of each walking its own way and jumping back when it closes.
	CHECK(WhileSilent(true, false, NpcPlace::LOST_AFTER + 1ms) == Silent::kHold);
	CHECK(WhileSilent(true, false, 10s) == Silent::kHold);
	CHECK(WhileSilent(true, false, NpcPlace::HELD_FOR) == Silent::kHold);
	// A minute on, the status may be stale (their game hung in the menu, their connection gone): let go as before.
	CHECK(WhileSilent(true, false, NpcPlace::HELD_FOR + 1ms) == Silent::kRelease);
	// Silent with no such menu (a load, a bad connection, their game gone): let go, as before.
	CHECK(WhileSilent(false, false, NpcPlace::LOST_AFTER + 1ms) == Silent::kRelease);
	CHECK(WhileSilent(false, false, 10s) == Silent::kRelease);
	// A raider the joiner is fighting, or that fights them: a held one can't fight back, so it runs its own AI here
	// and jumps a little when the host comes back, rather than standing still beside the joiner for the menu.
	CHECK(WhileSilent(true, true, NpcPlace::LOST_AFTER + 1ms) == Silent::kRelease);
	CHECK(WhileSilent(true, true, 10s) == Silent::kRelease);
	CHECK(WhileSilent(false, true, 10s) == Silent::kRelease);
	// A fight starting while held ends the hold; the menu closing (states back) ends it with no release at all.
	CHECK(WhileSilent(true, false, 20s) == Silent::kHold);
	CHECK(WhileSilent(true, true, 20s) == Silent::kRelease);
}
